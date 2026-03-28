#include "cli_tui.h"
#include "output_buffer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <string>
#include <vector>
#include <iostream>
#include <sys/ioctl.h>
#include <unistd.h>
#include <termios.h>
#include <signal.h>

namespace cli_tui {

// ANSI escape codes
static const char* HIDE_CURSOR = "\033[?25l";
static const char* SHOW_CURSOR = "\033[?25h";
static const char* BLINK_BLOCK_CURSOR = "\033[1 q";
static const char* CLEAR_SCREEN = "\033[2J";
static const char* MOVE_HOME = "\033[H";
static const char* COLOR_GRAY = "\033[90m";
static const char* COLOR_RESET = "\033[0m";

// Terminal state
static bool g_enabled = true;
static bool g_initialized = false;
static termios g_initial_state;
static bool g_term_valid = false;

// Input state
static std::string g_input_buffer;
static size_t g_cursor_pos = 0;
static int g_input_row = 0;

// Stats line
static std::string g_stats_line;

// Get terminal size
static int get_term_width() {
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0) {
        return w.ws_col > 0 ? w.ws_col : 80;
    }
    return 80;
}

static int get_term_height() {
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0) {
        return w.ws_row > 0 ? w.ws_row : 24;
    }
    return 24;
}

static void move_cursor(int row, int col) {
    printf("\033[%d;%dH", row, col);
}

static void clear_to_end() {
    printf("\033[K");
}

static void draw_separator(int row, int width) {
    move_cursor(row, 1);
    printf("%s", COLOR_GRAY);
    for (int i = 0; i < width; i++) {
        printf("─");
    }
    printf("%s", COLOR_RESET);
}

static void render_output(int term_height, int /*term_width*/) {
    // Layout:
    // Rows 1 to (term_height-4): output
    // Row (term_height-3): separator
    // Row (term_height-2): input
    // Row (term_height-1): separator
    // Row term_height: stats
    
    int output_rows = term_height - 4;
    if (output_rows < 1) output_rows = 1;

    // Get ALL lines from buffer
    auto all_lines = g_output_buffer.get_visible_lines(0);

    // Clear screen
    printf("%s%s", CLEAR_SCREEN, MOVE_HOME);

    int total = all_lines.size();

    // Always show the LAST lines (most recent)
    int line_offset = (total > output_rows) ? (total - output_rows) : 0;

    // Print lines starting from row 1
    for (int i = 0; i < output_rows && (line_offset + i) < total; i++) {
        move_cursor(1 + i, 1);
        clear_to_end();
        printf("%s", all_lines[line_offset + i].c_str());
    }

    fflush(stdout);
}

void render() {
    if (!g_enabled || !g_initialized) return;

    int term_height = get_term_height();
    int term_width = get_term_width();

    // Render output first (rows 1 to term_height-4)
    render_output(term_height, term_width);

    // Separator above input (row term_height-3)
    draw_separator(term_height - 3, term_width);

    // Input box (row term_height-2)
    g_input_row = term_height - 2;
    move_cursor(g_input_row, 1);
    clear_to_end();
    printf("> %s", g_input_buffer.c_str());
    move_cursor(g_input_row, 3 + g_cursor_pos);

    // Separator above stats (row term_height-1)
    draw_separator(term_height - 1, term_width);

    // Stats (row term_height)
    move_cursor(term_height, 1);
    clear_to_end();
    if (!g_stats_line.empty()) {
        printf("%s", g_stats_line.c_str());
    }

    fflush(stdout);
}

static void handle_resize(int /*signum*/) {
    if (g_enabled && g_initialized) {
        render();
    }
}

void init() {
    if (g_initialized) return;
    
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        g_enabled = false;
        g_initialized = true;
        return;
    }
    
    if (tcgetattr(STDIN_FILENO, &g_initial_state) == 0) {
        g_term_valid = true;
        struct termios raw = g_initial_state;
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }
    
    printf("%s%s", HIDE_CURSOR, BLINK_BLOCK_CURSOR);
    fflush(stdout);
    
    signal(SIGWINCH, handle_resize);
    
    g_initialized = true;
}

void cleanup() {
    if (!g_initialized) return;
    
    if (g_term_valid) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_initial_state);
    }
    
    printf("%s\n", SHOW_CURSOR);
    fflush(stdout);
    
    g_enabled = false;
    g_initialized = false;
}

void print(const char* fmt, ...) {
    if (!g_enabled || !g_initialized) {
        va_list args;
        va_start(args, fmt);
        vprintf(fmt, args);
        va_end(args);
        fflush(stdout);
        return;
    }
    
    va_list args;
    va_start(args, fmt);
    char buffer[4096];
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    
    g_output_buffer.push_line(buffer);
    render();
}

void print_stream(const char* text) {
    if (!g_enabled || !g_initialized) {
        printf("%s", text);
        fflush(stdout);
        return;
    }
    
    static std::string stream_buf;
    stream_buf += text;
    
    const int FLUSH_EVERY = 256;
    if (stream_buf.size() >= FLUSH_EVERY) {
        g_output_buffer.push_line(stream_buf);
        stream_buf.clear();
        render();
    }
}

void flush_stream() {
    if (!g_enabled || !g_initialized) return;
    
    static std::string stream_buf;
    if (!stream_buf.empty()) {
        g_output_buffer.push_line(stream_buf);
        stream_buf.clear();
        render();
    }
}

std::string read_input() {
    if (!g_enabled || !g_initialized) {
        printf("> ");
        fflush(stdout);
        std::string line;
        if (std::getline(std::cin, line)) {
            return line;
        }
        return "";
    }
    
    g_input_buffer.clear();
    g_cursor_pos = 0;
    render();
    
    while (true) {
        char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n <= 0) break;
        
        if (c == 27) {
            char seq[4];
            ssize_t n1 = read(STDIN_FILENO, &seq[0], 1);
            if (n1 == 1 && seq[0] == '[') {
                ssize_t n2 = read(STDIN_FILENO, &seq[1], 1);
                if (n2 == 1) {
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
                    
                    if (seq[1] == '5' || seq[1] == '6') {
                        read(STDIN_FILENO, &seq[2], 1);
                        if (seq[2] == '~') {
                            if (seq[1] == '5' && g_cursor_pos > 0) {
                                g_cursor_pos = 0;
                                render();
                            } else if (seq[1] == '6' && g_cursor_pos < g_input_buffer.size()) {
                                g_cursor_pos = g_input_buffer.size();
                                render();
                            }
                            continue;
                        }
                    }
                    
                    if (seq[1] == 'C' && g_cursor_pos < g_input_buffer.size()) {
                        g_cursor_pos++;
                        render();
                    } else if (seq[1] == 'D' && g_cursor_pos > 0) {
                        g_cursor_pos--;
                        render();
                    } else if (seq[1] == '3' && g_cursor_pos < g_input_buffer.size()) {
                        g_input_buffer.erase(g_cursor_pos, 1);
                        render();
                        read(STDIN_FILENO, &seq[2], 1);
                    }
                }
            }
            continue;
        }
        
        if (c == 0 || c == 127) {
            if (g_cursor_pos > 0) {
                g_cursor_pos--;
                g_input_buffer.erase(g_cursor_pos, 1);
                render();
            }
            continue;
        }
        
        if (c == '\n' || c == '\r') break;
        if (c == 3) { g_input_buffer.clear(); break; }
        if (c == 12) { g_output_buffer.clear(); render(); continue; }
        
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
    int term_width = get_term_width();
    
    // Render in order: output first, then UI elements on top
    render_output(term_height, term_width);
    
    // Draw separator above input (row term_height-2)
    draw_separator(term_height - 2, term_width);
    
    // Draw input box (row term_height-1)
    g_input_row = term_height - 1;
    move_cursor(g_input_row, 1);
    clear_to_end();
    printf("> %s", g_input_buffer.c_str());
    move_cursor(g_input_row, 3 + g_cursor_pos);
    
    // Draw separator above stats (row term_height)
    draw_separator(term_height, term_width);
    
    // Stats would go on row term_height+1 but we don't have that row
    // So stats share the bottom area
    
    fflush(stdout);
}

bool is_enabled() { return g_enabled; }

void set_enabled(bool enabled) {
    g_enabled = enabled;
    if (enabled) render();
}

void force_redraw() { render(); }

void set_stats_line(const char* text) {
    if (!g_enabled || !g_initialized) return;
    g_stats_line = text ? text : "";
    int term_height = get_term_height();
    int term_width = get_term_width();
    draw_separator(term_height - 1, term_width);
    draw_stats_line(term_height, term_width);
}

void scroll_output(int /*lines*/) {
    // Disabled - no scrolling, show everything
}

}  // namespace cli_tui
