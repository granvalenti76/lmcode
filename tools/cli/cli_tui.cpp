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
#include <chrono>
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
static const char* COLOR_RED      = "\033[91m";
static const char* COLOR_GREEN    = "\033[92m";
static const char* COLOR_YELLOW   = "\033[93m";
static const char* COLOR_BLUE     = "\033[94m";
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

// Input history (command history, like bash)
static std::vector<std::string> g_input_history;
static int         g_history_index = -1;  // -1 = not navigating history
static const int   MAX_HISTORY = 500;

// Scroll state
static int         g_scroll_offset = 0;  // 0 = at bottom, positive = scrolled back

// Stats line
static std::string g_stats_line;

// Streaming state
// FIX: instead of one big string, we keep a "current line being built"
// and push completed lines to g_output_buffer as they arrive.
// This makes text appear token-by-token AND handles newlines correctly.
static std::string g_stream_current_line;   // line currently being assembled
static bool        g_streaming = false;     // true while model is generating

// Render throttling
static std::atomic<long long> g_last_render_ms{0};
static const int RENDER_THROTTLE_MS = 33;  // ~30 FPS max for render

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

// Render throttle: only render if enough time has passed
static bool should_render() {
    auto now = std::chrono::steady_clock::now();
    long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    long long last = g_last_render_ms.load();
    if (now_ms - last >= RENDER_THROTTLE_MS) {
        g_last_render_ms.store(now_ms);
        return true;
    }
    return false;
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
// - Render is throttled to ~30 FPS to avoid O(n^2) on fast streams.
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
            if (!g_suppress_render && should_render()) render();
        } else {
            g_stream_current_line += *p;
            // Throttled render so user sees tokens appear live without O(n^2)
            if (!g_suppress_render && should_render()) render();
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

    // Reset scroll when user starts typing
    g_scroll_offset = 0;

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
    g_history_index = -1;
    render();

    while (true) {
        char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n <= 0) {
            g_eof_detected = true;  // EOF detected
            break;
        }

        if (c == 27) {
            char seq[4] = {0, 0, 0, 0};  // Zero-init to avoid garbage
            ssize_t n1 = read(STDIN_FILENO, &seq[0], 1);
            if (n1 == 1 && seq[0] == '[') {
                ssize_t n2 = read(STDIN_FILENO, &seq[1], 1);
                if (n2 == 1) {
                    // Mouse events — discard and re-render
                    if (seq[1] == 'M' || seq[1] == '<') {
                        if (seq[1] == 'M') {
                            char mouse_buf[3];
                            if (read(STDIN_FILENO, mouse_buf, 3) < 0) {}
                        } else {
                            for (int i = 2; i < 4; i++) {
                                char b = 0;
                                if (read(STDIN_FILENO, &b, 1) < 0) break;
                                if (b == 'm' || b == 'M') break;
                            }
                        }
                        render();
                        continue;
                    }
                    // Page Up/Down → scroll output
                    if (seq[1] == '5' || seq[1] == '6') {
                        char tilde = 0;
                        if (read(STDIN_FILENO, &tilde, 1) > 0 && tilde == '~') {
                            int term_height = get_term_height();
                            int scroll_amount = term_height - 5;  // One page
                            if (scroll_amount < 1) scroll_amount = 1;
                            if (seq[1] == '5') scroll_output(-scroll_amount);  // Page Up
                            else               scroll_output(scroll_amount);   // Page Down
                            continue;
                        }
                    }
                    // Up/Down arrow → input history
                    if (seq[1] == 'A') {  // Up
                        if (g_history_index < 0 && !g_input_history.empty()) {
                            g_history_index = (int)g_input_history.size() - 1;
                            g_input_buffer = g_input_history.back();
                            g_cursor_pos = g_input_buffer.size();
                            render();
                        } else if (g_history_index > 0) {
                            g_history_index--;
                            g_input_buffer = g_input_history[g_history_index];
                            g_cursor_pos = g_input_buffer.size();
                            render();
                        }
                        continue;
                    }
                    if (seq[1] == 'B') {  // Down
                        if (g_history_index >= 0) {
                            g_history_index++;
                            if (g_history_index >= (int)g_input_history.size()) {
                                g_history_index = -1;
                                g_input_buffer.clear();
                            } else {
                                g_input_buffer = g_input_history[g_history_index];
                            }
                            g_cursor_pos = g_input_buffer.size();
                            render();
                        }
                        continue;
                    }
                    // Left/Right arrow → cursor movement
                    if      (seq[1] == 'C' && g_cursor_pos < g_input_buffer.size()) { g_cursor_pos++; render(); }
                    else if (seq[1] == 'D' && g_cursor_pos > 0)                     { g_cursor_pos--; render(); }
                    else if (seq[1] == '3' && g_cursor_pos < g_input_buffer.size()) {
                        g_input_buffer.erase(g_cursor_pos, 1);
                        render();
                        char tilde = 0;
                        read(STDIN_FILENO, &tilde, 1); // consume '~'
                    }
                }
            }
            continue;
        }

        if (c == 0 || c == 127) {   // Backspace
            if (g_cursor_pos > 0) { g_cursor_pos--; g_input_buffer.erase(g_cursor_pos, 1); render(); }
            continue;
        }
        if (c == '\n' || c == '\r') {
            // Save to history (avoid duplicates)
            if (!g_input_buffer.empty()) {
                if (g_input_history.empty() || g_input_history.back() != g_input_buffer) {
                    g_input_history.push_back(g_input_buffer);
                    if ((int)g_input_history.size() > MAX_HISTORY) {
                        g_input_history.erase(g_input_history.begin());
                    }
                }
            }
            break;
        }
        if (c == 3) {  // Ctrl-C: clear input, signal handler already set g_is_interrupted
            g_input_buffer.clear();
            g_history_index = -1;
            break;
        }
        if (c == 12) { g_output_buffer.clear(); g_scroll_offset = 0; render(); continue; } // Ctrl-L clear
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

    // ── Collect lines with viewport scroll ─────────────────────
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

    // Apply scroll offset: g_scroll_offset = 0 means show bottom
    // g_scroll_offset > 0 means show lines further back
    int scroll_start = total_lines - max_output - g_scroll_offset;
    if (scroll_start < 0) scroll_start = 0;

    // ── Clear & redraw ─────────────────────────────────────────
    // FIX: Don't clear entire screen (causes flickering).
    // Each line is cleared individually with clear_to_end() below.
    printf("%s%s", COLOR_RESET, MOVE_HOME);

    // Expand logical lines → wrapped screen-lines respecting term_width
    std::vector<std::string> screen_lines;
    screen_lines.reserve(total_lines);
    for (int i = scroll_start; i < total_lines; i++) {
        auto wrapped = wrap_line(all_lines[i], term_width);
        for (auto& w : wrapped) screen_lines.push_back(std::move(w));
    }
    int total_screen = (int)screen_lines.size();
    int show_lines = (total_screen > max_output) ? max_output : total_screen;

    int row = 1;
    for (int i = 0; i < total_screen && row <= show_lines; i++, row++) {
        move_cursor(row, 1);
        clear_to_end();
        printf("%s%s", COLOR_RESET, screen_lines[i].c_str());
    }

    // Clear remaining output rows above UI bar
    for (; row <= max_output; row++) {
        move_cursor(row, 1);
        clear_to_end();
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

    // Show scroll indicator if scrolled back
    if (g_scroll_offset > 0) {
        int scroll_pct = total_lines > 0 ? (100 * (total_lines - g_scroll_offset)) / total_lines : 100;
        printf(" %s[scroll %d%%]%s", COLOR_GRAY, scroll_pct, COLOR_RESET);
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

void scroll_output(int lines) {
    if (!g_enabled || !g_initialized) return;

    auto all_lines = g_output_buffer.get_visible_lines(0);
    int total = (int)all_lines.size();
    int term_height = get_term_height();
    int max_output = term_height - 4;  // UI rows
    if (max_output < 1) max_output = 1;

    g_scroll_offset += lines;
    if (g_scroll_offset < 0) g_scroll_offset = 0;
    if (g_scroll_offset > total - max_output) g_scroll_offset = total - max_output;
    if (g_scroll_offset < 0) g_scroll_offset = 0;

    render();
}

void scroll_to_bottom() {
    g_scroll_offset = 0;
    render();
}

bool was_eof() {
    return g_eof_detected;
}

}  // namespace cli_tui
