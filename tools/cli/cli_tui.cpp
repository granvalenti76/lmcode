#include "cli_tui.h"
#include "output_buffer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <string>
#include <vector>
#include <iostream>
#include <atomic>
#include <sys/ioctl.h>
#include <unistd.h>
#include <termios.h>
#include <signal.h>

namespace cli_tui {

// ANSI escape codes
static const char* HIDE_CURSOR    = "\033[?25l";
static const char* SHOW_CURSOR    = "\033[?25h";
static const char* BLINK_BLOCK_CURSOR = "\033[1 q";
static const char* MOVE_HOME      = "\033[H";
static const char* COLOR_GRAY     = "\033[90m";
static const char* COLOR_RESET    = "\033[0m";

// ─────────────────────────────────────────────────────────────
// Word-wrap helpers
// ─────────────────────────────────────────────────────────────

// Returns the visible (printed) length of a string, skipping ANSI escapes.
static int visible_len(const std::string& s) {
    int len = 0;
    bool in_esc = false;
    for (size_t i = 0; i < s.size(); ) {
        unsigned char c = (unsigned char)s[i];
        if (in_esc) {
            // ESC sequences end at a letter (@ through ~)
            if (c >= 0x40 && c <= 0x7E) in_esc = false;
            i++;
        } else if (c == 0x1B) {
            in_esc = true;
            i++;
        } else if (c >= 0x80) {
            // UTF-8 multi-byte: count as 1 visible char
            if      ((c & 0xF0) == 0xF0) i += 4;
            else if ((c & 0xE0) == 0xE0) i += 3;
            else if ((c & 0xC0) == 0xC0) i += 2;
            else                          i += 1;
            len++;
        } else {
            i++;
            len++;
        }
    }
    return len;
}

// Split a single logical line into screen-width chunks for rendering.
// Preserves ANSI codes (they don't count toward width).
// Each chunk gets COLOR_RESET appended so colors never bleed into the next row.
static std::vector<std::string> wrap_line(const std::string& line, int width) {
    std::vector<std::string> result;
    if (width <= 0) width = 80;

    std::string current;
    int vis = 0;
    bool in_esc = false;

    for (size_t i = 0; i < line.size(); ) {
        unsigned char c = (unsigned char)line[i];

        if (in_esc) {
            current += (char)c;
            if (c >= 0x40 && c <= 0x7E) in_esc = false;
            i++;
        } else if (c == 0x1B) {
            current += (char)c;
            in_esc = true;
            i++;
        } else if (c >= 0x80) {
            // UTF-8 multi-byte char
            size_t char_len = 1;
            if      ((c & 0xF0) == 0xF0) char_len = 4;
            else if ((c & 0xE0) == 0xE0) char_len = 3;
            else if ((c & 0xC0) == 0xC0) char_len = 2;

            if (vis >= width) {
                result.push_back(current + COLOR_RESET);
                current.clear();
                vis = 0;
            }
            for (size_t j = 0; j < char_len && i + j < line.size(); j++)
                current += line[i + j];
            vis++;
            i += char_len;
        } else {
            if (vis >= width) {
                result.push_back(current + COLOR_RESET);
                current.clear();
                vis = 0;
            }
            current += (char)c;
            vis++;
            i++;
        }
    }

    if (!current.empty() || result.empty())
        result.push_back(current + COLOR_RESET);

    return result;
}

// Terminal state
static bool     g_enabled     = false;  // Disabled by default - use /tui on to enable
static bool     g_initialized = false;
static termios  g_initial_state;
static bool     g_term_valid  = false;
static bool     g_suspended   = false;  // Track if terminal is in suspended state

// Resize flag (async-signal-safe: only set flag in handler, check in main loop)
static std::atomic<bool> g_resize_needed{false};

// Bulk print state
static bool g_suppress_render = false;

// Input state
static std::string g_input_buffer;
static size_t      g_cursor_pos = 0;
static bool        g_eof_detected = false;  // Track if EOF was detected in read_input()

// Stats line
static std::string g_stats_line;

// Streaming state
// FIX: instead of one big string, we keep a "current line being built"
// and push completed lines to g_output_buffer as they arrive.
// This makes text appear token-by-token AND handles newlines correctly.
static std::string g_stream_current_line;   // line currently being assembled
static bool        g_streaming = false;     // true while model is generating

// ─────────────────────────────────────────────────────────────
// Terminal helpers
// ─────────────────────────────────────────────────────────────

static int get_term_width() {
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0)
        return w.ws_col > 0 ? w.ws_col : 80;
    return 80;
}

static int get_term_height() {
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0)
        return w.ws_row > 0 ? w.ws_row : 24;
    return 24;
}

static void move_cursor(int row, int col) {
    printf("\033[%d;%dH", row, col);
}

static void clear_to_end() {
    printf("\033[K");
}

// FIX: always end the separator with COLOR_RESET so color never leaks out
static void draw_separator() {
    int width = get_term_width();
    printf("%s", COLOR_GRAY);
    for (int i = 0; i < width; i++) printf("─");
    printf("%s", COLOR_RESET);
    fflush(stdout);   // flush immediately so partial draws don't leave stale color
}

static void handle_resize(int /*signum*/) {
    // FIX: Only set flag in signal handler (printf/render are not async-signal-safe)
    g_resize_needed.store(true);
}

// ─────────────────────────────────────────────────────────────
// Helper: split a string by \n and push each piece to the buffer
// ─────────────────────────────────────────────────────────────
static void push_text_to_buffer(const std::string& text) {
    size_t start = 0;
    while (start < text.size()) {
        size_t nl = text.find('\n', start);
        if (nl == std::string::npos) {
            // No newline: append to last line in buffer (or push new)
            g_output_buffer.push_line(text.substr(start));
            break;
        } else {
            g_output_buffer.push_line(text.substr(start, nl - start));
            start = nl + 1;
        }
    }
}

// ─────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────

void init() {
    if (g_initialized) return;

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        g_enabled     = false;
        g_initialized = true;
        return;
    }

    if (tcgetattr(STDIN_FILENO, &g_initial_state) == 0) {
        g_term_valid = true;
        // Only set raw mode if TUI is enabled
        if (g_enabled) {
            struct termios raw = g_initial_state;
            raw.c_lflag &= ~(ICANON | ECHO);
            raw.c_cc[VMIN]  = 1;
            raw.c_cc[VTIME] = 0;
            tcsetattr(STDIN_FILENO, TCSANOW, &raw);

            printf("%s%s", HIDE_CURSOR, BLINK_BLOCK_CURSOR);
            fflush(stdout);
        }
    }

    signal(SIGWINCH, handle_resize);
    g_initialized = true;
}

// Enable TUI mode (call after init() to switch from console to TUI)
void enable() {
    if (!g_initialized || !g_term_valid || g_enabled) return;

    g_enabled = true;

    // Set raw mode
    struct termios raw = g_initial_state;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    printf("%s%s", HIDE_CURSOR, BLINK_BLOCK_CURSOR);
    fflush(stdout);
}

// Disable TUI mode (call to switch back to console mode)
void disable() {
    if (!g_initialized || !g_enabled) return;

    g_enabled = false;

    // Restore terminal mode
    if (g_term_valid) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_initial_state);
    }

    printf("%s%s", SHOW_CURSOR, COLOR_RESET);
    fflush(stdout);
}

void cleanup() {
    if (!g_initialized) return;

    if (g_term_valid)
        tcsetattr(STDIN_FILENO, TCSANOW, &g_initial_state);

    // FIX: make sure color is reset before restoring cursor
    printf("%s%s\n", COLOR_RESET, SHOW_CURSOR);
    fflush(stdout);

    g_enabled     = false;
    g_initialized = false;
    g_suspended   = false;
}

void suspend() {
    if (!g_initialized || !g_term_valid || g_suspended) return;

    // Restore terminal to normal cooked mode with echo
    tcsetattr(STDIN_FILENO, TCSANOW, &g_initial_state);
    printf("%s%s", SHOW_CURSOR, COLOR_RESET);
    fflush(stdout);
    g_suspended = true;
}

void resume() {
    if (!g_initialized || !g_term_valid || !g_suspended) return;

    // Restore raw mode
    struct termios raw = g_initial_state;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    printf("%s%s", HIDE_CURSOR, BLINK_BLOCK_CURSOR);
    fflush(stdout);
    g_suspended = false;
    render();  // Redraw to restore UI
}

void process_resize() {
    // FIX: Check resize flag and render in main thread (safe)
    if (g_resize_needed.load()) {
        g_resize_needed.store(false);
        if (g_enabled && g_initialized && !g_suspended) {
            render();
        }
    }
}

// FIX: use a dynamic buffer to avoid the 4096-byte truncation
void print(const char* fmt, ...) {
    if (!g_enabled || !g_initialized) {
        va_list args;
        va_start(args, fmt);
        vprintf(fmt, args);
        va_end(args);
        fflush(stdout);
        return;
    }

    // Try stack buffer first, fall back to heap if needed
    char   stack_buf[4096];
    char*  buf     = stack_buf;
    size_t buf_len = sizeof(stack_buf);

    va_list args, args2;
    va_start(args, fmt);
    va_copy(args2, args);
    int needed = vsnprintf(stack_buf, buf_len, fmt, args);
    va_end(args);

    std::string heap_buf;
    if (needed >= (int)buf_len) {
        heap_buf.resize(needed + 1);
        vsnprintf(&heap_buf[0], heap_buf.size(), fmt, args2);
        buf = &heap_buf[0];
    }
    va_end(args2);

    // FIX: split by newlines so each logical line is a separate buffer entry
    push_text_to_buffer(buf);
    if (!g_suppress_render) render();
}

void begin_bulk_print() { g_suppress_render = true; }
void end_bulk_print()   { g_suppress_render = false; render(); }

// FIX: print_stream now processes tokens as they arrive.
// - Appends to g_stream_current_line
// - Whenever a '\n' is encountered, the completed line is committed to
//   g_output_buffer and rendering happens immediately so the user sees
//   text appear token-by-token.
void print_stream(const char* text) {
    if (!g_enabled || !g_initialized) {
        printf("%s", text);
        fflush(stdout);
        return;
    }

    g_streaming = true;

    for (const char* p = text; *p; ++p) {
        if (*p == '\n') {
            // Commit the completed line to the buffer and show it
            g_output_buffer.push_line(g_stream_current_line);
            g_stream_current_line.clear();
            if (!g_suppress_render) render();
        } else {
            g_stream_current_line += *p;
            // Render on every character so the user sees tokens appear live.
            // If this is too slow on your hardware you can render every N chars:
            //   if (g_stream_current_line.size() % 4 == 0) render();
            if (!g_suppress_render) render();
        }
    }
}

// FIX: flush_stream commits whatever partial line is still in the buffer
void flush_stream() {
    if (!g_enabled || !g_initialized) return;

    if (!g_stream_current_line.empty()) {
        g_output_buffer.push_line(g_stream_current_line);
        g_stream_current_line.clear();
    }
    g_streaming = false;
    render();
}

std::string read_input() {
    g_eof_detected = false;  // Reset EOF flag for each call

    if (!g_enabled || !g_initialized) {
        printf("> ");
        fflush(stdout);
        std::string line;
        if (std::getline(std::cin, line)) return line;
        g_eof_detected = true;  // EOF detected
        return "";
    }

    g_input_buffer.clear();
    g_cursor_pos = 0;
    render();

    while (true) {
        char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n <= 0) {
            g_eof_detected = true;  // EOF detected
            break;
        }

        if (c == 27) {
            char seq[4];
            ssize_t n1 = read(STDIN_FILENO, &seq[0], 1);
            if (n1 == 1 && seq[0] == '[') {
                ssize_t n2 = read(STDIN_FILENO, &seq[1], 1);
                if (n2 == 1) {
                    // Mouse events — discard and re-render
                    if (seq[1] == 'M' || seq[1] == '<') {
                        if (seq[1] == 'M') read(STDIN_FILENO, &seq[2], 3);
                        else {
                            for (int i = 2; i < 4; i++) {
                                read(STDIN_FILENO, &seq[i], 1);
                                if (seq[i] == 'm' || seq[i] == 'M') break;
                            }
                        }
                        render();
                        continue;
                    }
                    // Page Up/Down → Home/End of input
                    if (seq[1] == '5' || seq[1] == '6') {
                        read(STDIN_FILENO, &seq[2], 1);
                        if (seq[2] == '~') {
                            if (seq[1] == '5') { g_cursor_pos = 0; render(); }
                            else               { g_cursor_pos = g_input_buffer.size(); render(); }
                            continue;
                        }
                    }
                    if      (seq[1] == 'C' && g_cursor_pos < g_input_buffer.size()) { g_cursor_pos++; render(); }
                    else if (seq[1] == 'D' && g_cursor_pos > 0)                     { g_cursor_pos--; render(); }
                    else if (seq[1] == '3' && g_cursor_pos < g_input_buffer.size()) {
                        g_input_buffer.erase(g_cursor_pos, 1);
                        render();
                        read(STDIN_FILENO, &seq[2], 1); // consume '~'
                    }
                }
            }
            continue;
        }

        if (c == 0 || c == 127) {   // Backspace
            if (g_cursor_pos > 0) { g_cursor_pos--; g_input_buffer.erase(g_cursor_pos, 1); render(); }
            continue;
        }
        if (c == '\n' || c == '\r') break;
        if (c == 3) {  // Ctrl-C: clear input, signal handler already set g_is_interrupted
            g_input_buffer.clear();
            break;
        }
        if (c == 12) { g_output_buffer.clear(); render(); continue; } // Ctrl-L clear
        if (c >= 32 && c < 127) {
            g_input_buffer.insert(g_cursor_pos, 1, c);
            g_cursor_pos++;
            render();
        }
    }
    return g_input_buffer;
}

void render() {
    if (!g_enabled || !g_initialized) return;

    int term_height = get_term_height();
    int term_width  = get_term_width();
    (void)term_width;

    // ── Collect lines ──────────────────────────────────────────
    // During streaming, append the partial current line as a "live" entry
    auto all_lines = g_output_buffer.get_visible_lines(0);
    if (g_streaming && !g_stream_current_line.empty()) {
        all_lines.push_back(g_stream_current_line);
    }
    int total_lines = (int)all_lines.size();

    // 4 fixed UI rows: separator, input, separator, stats
    int ui_lines   = 4;
    int max_output = term_height - ui_lines;
    if (max_output < 1) max_output = 1;

    // ── Clear & redraw ─────────────────────────────────────────
    // FIX: Don't clear entire screen (causes flickering).
    // Each line is cleared individually with clear_to_end() below.
    printf("%s%s", COLOR_RESET, MOVE_HOME);

    // Expand logical lines → wrapped screen-lines respecting term_width
    std::vector<std::string> screen_lines;
    screen_lines.reserve(total_lines);
    for (int i = 0; i < total_lines; i++) {
        auto wrapped = wrap_line(all_lines[i], term_width);
        for (auto& w : wrapped) screen_lines.push_back(std::move(w));
    }
    int total_screen = (int)screen_lines.size();
    int start_idx = (total_screen > max_output) ? (total_screen - max_output) : 0;

    int row = 1;
    for (int i = start_idx; i < total_screen && row <= max_output; i++, row++) {
        move_cursor(row, 1);
        clear_to_end();
        printf("%s%s", COLOR_RESET, screen_lines[i].c_str());
    }

    // ── UI bar ─────────────────────────────────────────────────
    move_cursor(term_height - 3, 1);
    clear_to_end();
    draw_separator();

    move_cursor(term_height - 2, 1);
    clear_to_end();
    // FIX: reset color before the input prompt so it's always white
    printf("%s> %s", COLOR_RESET, g_input_buffer.c_str());
    // Position cursor correctly (accounting for "> " prefix = 2 chars)
    move_cursor(term_height - 2, 3 + (int)g_cursor_pos);

    move_cursor(term_height - 1, 1);
    clear_to_end();
    draw_separator();

    move_cursor(term_height, 1);
    clear_to_end();
    if (!g_stats_line.empty()) {
        printf("%s%s", COLOR_RESET, g_stats_line.c_str());
    }

    fflush(stdout);
}

bool is_enabled()           { return g_enabled; }
void set_enabled(bool enabled) {
    if (enabled && !g_enabled) {
        enable();
    } else if (!enabled && g_enabled) {
        disable();
    }
    if (enabled) render();
}
void force_redraw()         { render(); }

void set_stats_line(const char* text) {
    if (!g_enabled || !g_initialized) return;
    g_stats_line = text ? text : "";
    int term_height = get_term_height();
    move_cursor(term_height - 1, 1);
    clear_to_end();
    draw_separator();
    move_cursor(term_height, 1);
    clear_to_end();
    if (!g_stats_line.empty())
        printf("%s%s", COLOR_RESET, g_stats_line.c_str());
    fflush(stdout);
}

void scroll_output(int /*lines*/) {
    // TODO: implement real scroll by adjusting a viewport offset in output_buffer
}

bool was_eof() {
    return g_eof_detected;
}

}  // namespace cli_tui
