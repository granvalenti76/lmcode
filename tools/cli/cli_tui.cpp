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
static const char* CLEAR_SCREEN = "\033[2J";
static const char* MOVE_HOME = "\033[H";
static const char* COLOR_GRAY = "\033[90m";
static const char* COLOR_GREEN = "\033[32m";
static const char* COLOR_BOLD = "\033[1m";
static const char* COLOR_RESET = "\033[0m";

// Terminal state
static bool g_enabled = true;
static bool g_initialized = false;
static termios g_initial_state;
static bool g_term_valid = false;

// Input state
static std::string g_input_buffer;
static size_t g_cursor_pos = 0;
static int g_input_row = 0;  // Row where input box is drawn

// Render state
static bool g_needs_redraw = true;

// Streaming output buffer (accumulates tokens before printing)
static std::string g_stream_buffer;
static const int STREAM_FLUSH_INTERVAL = 256;  // Flush every N characters

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

// Move cursor to specific position (1-indexed)
static void move_cursor(int row, int col) {
    printf("\033[%d;%dH", row, col);
}

// Clear from cursor to end of screen
static void clear_to_end() {
    printf("\033[J");
}

// Draw the separator line
static void draw_separator(int row, int width) {
    move_cursor(row, 1);
    printf("%s", COLOR_GRAY);
    for (int i = 0; i < width; i++) {
        printf("─");
    }
    printf("%s", COLOR_RESET);
}

// Draw the input box at the bottom
static void draw_input_box(int term_height, int term_width) {
    g_input_row = term_height - 1;  // Second to last row

    // Draw separator
    draw_separator(g_input_row - 1, term_width);

    // Draw input prompt and content
    move_cursor(g_input_row, 1);
    clear_to_end();
    printf("%s%s> %s", COLOR_BOLD, COLOR_GREEN, COLOR_RESET);
    printf("%s", g_input_buffer.c_str());

    // Position cursor
    move_cursor(g_input_row, 3 + g_cursor_pos);
    fflush(stdout);
}

// Render the output buffer
static void render_output(int /*term_height*/, int /*term_width*/) {
    // Calculate how many lines we can show
    int output_lines = g_input_row - 2;  // Leave room for separator and input
    if (output_lines < 1) output_lines = 1;
    
    // Get visible lines from buffer
    auto lines = g_output_buffer.get_visible_lines(output_lines);
    
    // Clear screen and move home
    printf("%s%s", CLEAR_SCREEN, MOVE_HOME);
    
    // Print each line
    for (size_t i = 0; i < lines.size() && i < static_cast<size_t>(output_lines); i++) {
        move_cursor(1 + i, 1);
        clear_to_end();
        printf("%s", lines[i].c_str());
    }
    
    fflush(stdout);
}

// Handle terminal resize
static void handle_resize(int /*signum*/) {
    if (g_enabled && g_initialized) {
        g_needs_redraw = true;
    }
}

void init() {
    if (g_initialized) return;
    
    // Check if we're running in a terminal
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        g_enabled = false;
        g_initialized = true;
        return;
    }
    
    // Save terminal state
    if (tcgetattr(STDIN_FILENO, &g_initial_state) == 0) {
        g_term_valid = true;
        
        // Set terminal to raw mode (disable canonical mode and echo)
        struct termios raw = g_initial_state;
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }
    
    // Hide cursor
    printf("%s", HIDE_CURSOR);
    fflush(stdout);
    
    // Setup signal handler for resize
    signal(SIGWINCH, handle_resize);
    
    g_initialized = true;
    g_needs_redraw = true;
    
    // Initial render
    render();
}

void cleanup() {
    if (!g_initialized) return;
    
    // Restore terminal state
    if (g_term_valid) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_initial_state);
    }
    
    // Show cursor and move to new line
    printf("%s\n", SHOW_CURSOR);
    fflush(stdout);
    
    g_enabled = false;
    g_initialized = false;
}

void print(const char* fmt, ...) {
    if (!g_enabled || !g_initialized) {
        // Fallback to normal printf
        va_list args;
        va_start(args, fmt);
        vprintf(fmt, args);
        va_end(args);
        fflush(stdout);
        return;
    }
    
    // Format and add to buffer
    va_list args;
    va_start(args, fmt);
    char buffer[4096];
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    
    g_output_buffer.push_line(buffer);
    g_needs_redraw = true;
}

void print_stream(const char* text) {
    if (!g_enabled || !g_initialized) {
        // Fallback to normal printf
        printf("%s", text);
        fflush(stdout);
        return;
    }
    
    g_stream_buffer += text;
    
    // Flush if buffer is large enough
    if (g_stream_buffer.size() >= STREAM_FLUSH_INTERVAL) {
        flush_stream();
    }
}

void flush_stream() {
    if (!g_enabled || !g_initialized) return;
    
    if (!g_stream_buffer.empty()) {
        g_output_buffer.push_line(g_stream_buffer);
        g_stream_buffer.clear();
        g_needs_redraw = true;
        render();
    }
}

std::string read_input() {
    if (!g_enabled || !g_initialized) {
        // Fallback to standard input
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
    g_needs_redraw = true;
    render();  // Show input box
    
    // Read input character by character
    while (true) {
        char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n <= 0) {
            break;
        }
        
        // Handle escape sequences
        if (c == 27) {  // ESC
            char seq[3];
            ssize_t n1 = read(STDIN_FILENO, &seq[0], 1);
            if (n1 == 1 && seq[0] == '[') {
                ssize_t n2 = read(STDIN_FILENO, &seq[1], 1);
                if (n2 == 1) {
                    seq[2] = '\0';
                    
                    switch (seq[1]) {
                        case 'C':  // Right arrow
                            if (g_cursor_pos < g_input_buffer.size()) {
                                g_cursor_pos++;
                                render();
                            }
                            break;
                        case 'D':  // Left arrow
                            if (g_cursor_pos > 0) {
                                g_cursor_pos--;
                                render();
                            }
                            break;
                        case '3':  // Delete key
                            if (g_cursor_pos < g_input_buffer.size()) {
                                g_input_buffer.erase(g_cursor_pos, 1);
                                render();
                            }
                            read(STDIN_FILENO, &seq[2], 1);  // Read trailing '~'
                            break;
                    }
                }
            }
            continue;
        }
        
        // Handle special keys
        if (c == 0 || c == 127) {  // Null or Backspace
            if (g_cursor_pos > 0) {
                g_cursor_pos--;
                g_input_buffer.erase(g_cursor_pos, 1);
                render();
            }
            continue;
        }
        
        // Handle Enter
        if (c == '\n' || c == '\r') {
            break;
        }
        
        // Handle Ctrl+C
        if (c == 3) {
            g_input_buffer.clear();
            break;
        }
        
        // Handle Ctrl+L (clear screen)
        if (c == 12) {
            g_output_buffer.clear();
            render();
            continue;
        }
        
        // Insert character at cursor position
        if (c >= 32 && c < 127) {  // Printable ASCII
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
    
    // Render output
    render_output(term_height, term_width);
    
    // Draw input box
    draw_input_box(term_height, term_width);
    
    g_needs_redraw = false;
}

bool is_enabled() {
    return g_enabled;
}

void set_enabled(bool enabled) {
    g_enabled = enabled;
    if (enabled) {
        g_needs_redraw = true;
    }
}

void force_redraw() {
    g_needs_redraw = true;
    render();
}

}  // namespace cli_tui
