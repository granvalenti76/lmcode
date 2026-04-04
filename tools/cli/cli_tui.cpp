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
#include <mutex>
#include <sys/ioctl.h>
#include <unistd.h>
#include <termios.h>
#include <signal.h>

namespace cli_tui {

// ANSI escape codes
static const char* HIDE_CURSOR      = "\033[?25l";
static const char* SHOW_CURSOR      = "\033[?25h";
static const char* BLINK_BLOCK_CURSOR = "\033[1 q";
static const char* COLOR_GRAY       = "\033[90m";
static const char* COLOR_RED        = "\033[91m";
static const char* COLOR_GREEN      = "\033[92m";
static const char* COLOR_YELLOW     = "\033[93m";
static const char* COLOR_CYAN       = "\033[96m";
static const char* COLOR_BOLD       = "\033[1m";
static const char* COLOR_DIM        = "\033[2m";
static const char* COLOR_RESET      = "\033[0m";

// ─────────────────────────────────────────────────────────────
// Word-wrap helpers
// ─────────────────────────────────────────────────────────────

// Returns the visible (printed) length of a string, skipping ANSI escapes.

// Split a single logical line into screen-width chunks for rendering.
// Preserves ANSI codes (they don't count toward width).
// Each chunk gets COLOR_RESET appended so colors never bleed into the next row.
// Empty lines are preserved (return at least one empty screen line).
// NOTE: UTF-8 wrapping is intentionally NOT done here — the terminal handles it.
static std::vector<std::string> wrap_line(const std::string& line, int width) {
    std::vector<std::string> result;
    (void)width;  // Terminal handles wrapping

    // Handle empty lines explicitly
    if (line.empty()) {
        result.push_back(std::string() + COLOR_RESET);
        return result;
    }

    // Simple: pass through as-is, let terminal handle wrapping
    result.push_back(line + COLOR_RESET);
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

// Tool status (floating, not in buffer)
static std::string g_tool_status;

// Streaming state
// FIX: instead of one big string, we keep a "current line being built"
// and push completed lines to g_output_buffer as they arrive.
// This makes text appear token-by-token AND handles newlines correctly.
static std::string g_stream_current_line;   // line currently being assembled
static std::mutex  g_stream_mutex;           // protects g_stream_current_line
static bool        g_streaming = false;     // true while model is generating

// Render throttling
static std::atomic<long long> g_last_render_ms{0};
static const int RENDER_THROTTLE_MS = 50;  // ~20 FPS max for render

// Dirty region tracking — skip redraw if output hasn't changed
static size_t g_last_line_count = 0;

// Safety flag — if TUI breaks, fallback to console
static std::atomic<bool> g_tui_broken{false};

// Typing indicator state
static bool g_is_typing = false;

// ── Apple HIG: Advanced Animation State ────────────────────────────
static int g_animation_frame = 0;
static int g_render_frame_counter = 0;

// ── Braille spinner (smooth animation) ──────────────────────────────
static const char* SPINNER_FRAMES[] = {
    "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"
};
static const int SPINNER_FRAMES_COUNT = 10;

// ── Status indicators ──────────────────────────────────────────────
enum Status { STATUS_IDLE, STATUS_STREAMING, STATUS_WARNING, STATUS_ERROR };

// ── Get current status based on state ──────────────────────────────
static Status get_current_status() {
    if (g_tui_broken) return STATUS_ERROR;
    if (g_is_typing) return STATUS_STREAMING;
    return STATUS_IDLE;
}

// ── Get animation frame ────────────────────────────────────────────
static int get_animation_frame() {
    g_animation_frame = (g_animation_frame + 1) % SPINNER_FRAMES_COUNT;
    return g_animation_frame;
}

// ── Get status indicator with animation ────────────────────────────
static const char* get_status_indicator() {
    Status status = get_current_status();

    switch (status) {
        case STATUS_STREAMING:
            return SPINNER_FRAMES[get_animation_frame()];
        case STATUS_WARNING:
            return "⚠";
        case STATUS_ERROR:
            return "✗";
        case STATUS_IDLE:
        default:
            return "◯";
    }
}
// ── UTF-8 aware line wrapping ─────────────────────────────
static std::vector<std::string> wrap_line_utf8(const std::string& line, int width) {
    std::vector<std::string> result;
    if (width < 10) width = 10;
    
    std::string current;
    int visible_width = 0;  // Length without ANSI codes
    bool in_ansi = false;
    
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        
        // Detect ANSI escape sequence start
        if (c == '\033' && i + 1 < line.size() && line[i+1] == '[') {
            in_ansi = true;
            current += c;
            continue;
        }
        
        // Detect ANSI escape sequence end
        if (in_ansi && ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '~')) {
            in_ansi = false;
            current += c;
            continue;
        }
        
        if (in_ansi) {
            current += c;
            continue;
        }
        
        // Regular character
        // Check if UTF-8 continuation byte
        unsigned char uc = static_cast<unsigned char>(c);
        bool is_continuation = (uc >= 0x80 && uc < 0xC0);
        
        if (!is_continuation) {
            visible_width++;
        }
        
        current += c;
        
        // Wrap if visible width exceeded
        if (visible_width >= width) {
            result.push_back(current + COLOR_RESET);
            current.clear();
            visible_width = 0;
        }
    }
    
    if (!current.empty()) {
        result.push_back(current + COLOR_RESET);
    }
    
    return result.empty() ? std::vector<std::string>{"."} : result;
}
// ── Get status color based on state ────────────────────────────────
static const char* get_status_color() {
    Status status = get_current_status();

    switch (status) {
        case STATUS_STREAMING:
            return COLOR_CYAN;
        case STATUS_WARNING:
            return COLOR_YELLOW;
        case STATUS_ERROR:
            return COLOR_RED;
        case STATUS_IDLE:
        default:
            return COLOR_GRAY;
    }
}

// ── Smooth blinking cursor (counter-based, no chrono) ──────────────
static bool should_show_cursor() {
    return (g_render_frame_counter / 15) % 2 == 0;
}

// ── Draw elegant divider ─────────────────────────────────────────
static void draw_divider_elegant(int width) {
    printf("%s", COLOR_GRAY);
    for (int i = 0; i < width; i++) {
        if (i > 0 && i % 8 == 0) printf("┈");
        else printf("─");
    }
    printf("%s", COLOR_RESET);
    fflush(stdout);
}

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
// Syntax highlighting for output lines
// ─────────────────────────────────────────────────────────────
static std::string colorize_line(const std::string& line) {
    // Already has ANSI codes — don't double-colorize
    if (line.find("\033[") != std::string::npos) return line;

    // Errors in red
    if (line.find("error") != std::string::npos ||
        line.find("Error") != std::string::npos ||
        line.find("ERROR") != std::string::npos ||
        line.find("failed") != std::string::npos ||
        line.find("Failed") != std::string::npos) {
        return std::string(COLOR_RED) + line + COLOR_RESET;
    }
    // Warnings in yellow
    if (line.find("warning") != std::string::npos ||
        line.find("Warning") != std::string::npos ||
        line.find("truncated") != std::string::npos) {
        return std::string(COLOR_YELLOW) + line + COLOR_RESET;
    }
    // Success in green
    if (line.find("success") != std::string::npos ||
        line.find("Success") != std::string::npos ||
        line.find("✓") != std::string::npos ||
        line.find("created") != std::string::npos ||
        line.find("Created") != std::string::npos ||
        line.find("complete") != std::string::npos) {
        return std::string(COLOR_GREEN) + line + COLOR_RESET;
    }
    // Tool execution in cyan
    if (line.find("[tool:") != std::string::npos ||
        line.find("[ ⚙️") != std::string::npos) {
        return std::string(COLOR_CYAN) + line + COLOR_RESET;
    }
    // File paths in dim
    if (line.find("/") != std::string::npos && line.size() < 80) {
        return std::string(COLOR_DIM) + line + COLOR_RESET;
    }
    // Default
    return line;
}

// Set typing indicator (called from print_stream)
static void set_typing_indicator(bool active) {
    g_is_typing = active;
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
    // Fallback to console if TUI is broken
    if (g_tui_broken || !g_enabled || !g_initialized) {
        va_list args;
        va_start(args, fmt);
        vprintf(fmt, args);
        va_end(args);
        fflush(stdout);
        return;
    }

    try {
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
    } catch (...) {
        g_tui_broken = true;
        va_list args;
        va_start(args, fmt);
        vprintf(fmt, args);
        va_end(args);
        fflush(stdout);
    }
}

void begin_bulk_print() { g_suppress_render = true; }
void end_bulk_print()   { g_suppress_render = false; render(); }

// FIX: print_stream now processes tokens as they arrive.
// - Appends to g_stream_current_line
// - Whenever a '\n' is encountered, the completed line is committed to
//   g_output_buffer and rendering happens immediately so the user sees
//   text appear token-by-token.
// - Render is throttled to ~20 FPS to avoid O(n^2) on fast streams.
// - Thread-safe: mutex protects g_stream_current_line.
// - Batch rendering: render every 10 chars to reduce flicker.
void print_stream(const char* text) {
    if (g_tui_broken || !g_enabled || !g_initialized) {
        printf("%s", text);
        fflush(stdout);
        return;
    }

    g_streaming = true;
    set_typing_indicator(true);

    int render_counter = 0;
    const int BATCH_SIZE = 10;  // Render every 10 chars

    for (const char* p = text; *p; ++p) {
        if (*p == '\n') {
            {
                std::lock_guard<std::mutex> lock(g_stream_mutex);
                g_output_buffer.push_line(g_stream_current_line);
                g_stream_current_line.clear();
            }
            render_counter = 0;
            if (!g_suppress_render && should_render()) render();
        } else {
            {
                std::lock_guard<std::mutex> lock(g_stream_mutex);
                g_stream_current_line += *p;
            }
            render_counter++;
            if (render_counter >= BATCH_SIZE && !g_suppress_render && should_render()) {
                render();
                render_counter = 0;
            }
        }
    }
}

// FIX: flush_stream commits whatever partial line is still in the buffer
void flush_stream() {
    if (!g_enabled || !g_initialized) return;

    {
        std::lock_guard<std::mutex> lock(g_stream_mutex);
        if (!g_stream_current_line.empty()) {
            g_output_buffer.push_line(g_stream_current_line);
            g_stream_current_line.clear();
        }
    }
    g_streaming = false;
    set_typing_indicator(false);
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
        // ✅ FIX: Accept UTF-8 multi-byte (è, à, ù, etc.)
        unsigned char uc = static_cast<unsigned char>(c);
        if ((uc >= 32 && uc <= 126) ||      // ASCII printable
            (uc >= 192 && uc <= 244) ||     // UTF-8 start bytes
            (uc >= 128 && uc < 192)) {      // UTF-8 continuation
            if (g_input_buffer.size() >= 4096) continue;
            g_input_buffer.insert(g_cursor_pos, 1, c);
            g_cursor_pos++;
            render();
        }
    }
    return g_input_buffer;
}

void render() {
    if (!g_enabled || !g_initialized) return;
    g_render_frame_counter++;

    try {
        int term_height = get_term_height();
        int term_width  = get_term_width();

        // 1. DEFINIZIONE SPAZI
        int UI_SIZE = 4;      // Le 4 righe fisse in fondo (divisori, input, stats)
        int MARGIN = 10;      // Il tuo margine di sicurezza
        int RESERVED = UI_SIZE + MARGIN;
        int MAX_TEXT_ROW = term_height - RESERVED; // L'ultima riga utile per il testo

        if (MAX_TEXT_ROW < 1) MAX_TEXT_ROW = 1; // Protezione per finestre piccole

        // 2. PREPARAZIONE TESTO (Calcoliamo le righe REALI/FISICHE)
        auto logical_lines = g_output_buffer.get_visible_lines(0);
        {
            std::lock_guard<std::mutex> lock(g_stream_mutex);
            if (g_streaming && !g_stream_current_line.empty()) {
                logical_lines.push_back(g_stream_current_line);
            }
        }

        std::vector<std::string> physical_rows;
        for (const auto& line : logical_lines) {
            auto wrapped = wrap_line_utf8(line, term_width - 2);
            for (const auto& w : wrapped) {
                physical_rows.push_back(w);
            }
        }

        // 3. CALCOLO DELLO SCROLL (Basato su righe fisiche, non logiche!)
        int total_phys = (int)physical_rows.size();
        int start_idx = total_phys - MAX_TEXT_ROW - g_scroll_offset;
        if (start_idx < 0) start_idx = 0;

        // 4. STAMPA AREA TESTO
        int current_row = 1;
        for (int i = start_idx; i < total_phys && current_row <= MAX_TEXT_ROW; i++) {
            move_cursor(current_row, 1);
            clear_to_end();
            printf("%s%s", COLOR_RESET, physical_rows[i].c_str());
            current_row++;
        }

        // 5. PULIZIA DEL MARGINE (Importante per evitare "cose strane")
        // Puliamo tutto dallo spazio sotto il testo fino a dove inizia l'interfaccia
        int ui_start_row = term_height - 3;
        for (int r = current_row; r < ui_start_row; r++) {
            move_cursor(r, 1);
            clear_to_end();
        }

        // 6. DISEGNO INTERFACCIA (Posizioni fisse)
        move_cursor(term_height - 3, 1);
        draw_divider_elegant(term_width);

        move_cursor(term_height - 2, 1);
        clear_to_end();
        printf("%s%s%s ", get_status_color(), get_status_indicator(), COLOR_RESET);
        printf("%s%s❯%s %s%s", COLOR_CYAN, COLOR_BOLD, COLOR_RESET, COLOR_BOLD, g_input_buffer.c_str());
        if (should_show_cursor()) printf("%s█%s", COLOR_CYAN, COLOR_RESET);
        else printf("%s▏%s", COLOR_GRAY, COLOR_RESET);

        move_cursor(term_height - 1, 1);
        draw_divider_elegant(term_width);

        move_cursor(term_height, 1);
        clear_to_end();
        if (!g_tool_status.empty()) printf("  %s[⚙️  %s]%s", COLOR_CYAN, g_tool_status.c_str(), COLOR_RESET);
        else printf("  %s%s%s", COLOR_GRAY, g_stats_line.empty() ? "✓ Ready" : g_stats_line.c_str(), COLOR_RESET);

        fflush(stdout);
    }
    catch (const std::exception& e) {
        g_tui_broken = true;
        fprintf(stderr, "[TUI Error] %s\n", e.what());
    }
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
    int term_width = get_term_width();
    move_cursor(term_height - 1, 1);
    clear_to_end();
    draw_divider_elegant(term_width);
    move_cursor(term_height, 1);
    clear_to_end();
    if (!g_stats_line.empty())
        printf("%s%s%s", COLOR_DIM, g_stats_line.c_str(), COLOR_RESET);
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

void set_tool_status(const char* status) {
    if (!g_enabled || !g_initialized) return;
    g_tool_status = status ? status : "";
    render();
}

bool was_eof() {
    return g_eof_detected;
}

}  // namespace cli_tui
