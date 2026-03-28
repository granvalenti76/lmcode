#include "cli_tui.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sys/ioctl.h>
#include <unistd.h>
#include <termios.h>
#include <signal.h>

namespace cli_tui {

// ANSI escape codes
static const char* CURSOR_UP = "\033[A";
static const char* CURSOR_DOWN = "\033[B";
static const char* CURSOR_RIGHT = "\033[C";
static const char* CURSOR_LEFT = "\033[D";
static const char* CURSOR_TO_COL = "\033[G";
static const char* CLEAR_LINE = "\033[2K";
static const char* SAVE_CURSOR = "\033[s";
static const char* RESTORE_CURSOR = "\033[u";
static const char* HIDE_CURSOR = "\033[?25l";
static const char* SHOW_CURSOR = "\033[?25h";

// Terminal state
static bool g_enabled = false;
static bool g_initialized = false;
static termios g_initial_state;
static bool g_term_valid = false;

// Input box state
static std::string g_input_buffer;
static size_t g_cursor_pos = 0;
static int g_input_line_row = -1;  // Terminal row where input box is drawn

// Get terminal size
static int get_term_width() {
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0) {
        return w.ws_col;
    }
    return 80;  // Default
}

static int get_term_height() {
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0) {
        return w.ws_row;
    }
    return 24;  // Default
}

// Move cursor to specific row
static void move_to_row(int row) {
    printf("\033[%d;1H", row + 1);  // 1-indexed
    fflush(stdout);
}

// Move cursor to end of current line and clear to end
static void clear_to_end() {
    printf("\033[K");
    fflush(stdout);
}

// Draw the input box at the bottom of the terminal
static void draw_input_box() {
    int term_height = get_term_height();
    int term_width = get_term_width();
    
    // Input box is on the second-to-last line (last line is the separator)
    int input_row = term_height - 2;
    g_input_line_row = input_row;
    
    // Move to input row
    move_to_row(input_row);
    
    // Draw separator line above input
    printf("\033[%d;1H", input_row);  // Move to separator row
    printf("\033[90m%s\033[0m", std::string(term_width, '─').c_str());
    
    // Move to input row and draw prompt + content
    move_to_row(input_row + 1);
    clear_to_end();
    
    // Draw prompt
    printf("\033[1;32m>\033[0m ");  // Green ">"
    
    // Draw input content
    printf("%s", g_input_buffer.c_str());
    
    // Clear rest of line
    clear_to_end();
    
    // Position cursor after the text
    int cursor_col = 2 + g_cursor_pos;  // 2 for "> "
    printf("\033[1;%dH", input_row + 2, cursor_col + 1);  // 1-indexed
    fflush(stdout);
}

// Refresh the input display
static void refresh_input() {
    if (!g_enabled || !g_initialized) return;
    
    draw_input_box();
}

// Handle terminal resize
static void handle_resize(int /*signum*/) {
    if (g_enabled && g_initialized) {
        refresh_input();
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
    
    // Enable TUI by default
    g_enabled = true;
    
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
}

void cleanup() {
    if (!g_initialized) return;
    
    // Restore terminal state
    if (g_term_valid) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_initial_state);
    }
    
    // Show cursor
    printf("%s", SHOW_CURSOR);
    fflush(stdout);
    
    g_enabled = false;
    g_initialized = false;
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
    
    // Draw initial input box
    draw_input_box();
    
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
                        case 'A':  // Up arrow - ignore
                            break;
                        case 'B':  // Down arrow - ignore
                            break;
                        case 'C':  // Right arrow
                            if (g_cursor_pos < g_input_buffer.size()) {
                                g_cursor_pos++;
                                refresh_input();
                            }
                            break;
                        case 'D':  // Left arrow
                            if (g_cursor_pos > 0) {
                                g_cursor_pos--;
                                refresh_input();
                            }
                            break;
                        case '3':  // Delete key
                            if (g_cursor_pos < g_input_buffer.size()) {
                                g_input_buffer.erase(g_cursor_pos, 1);
                                refresh_input();
                            }
                            // Read the trailing '~'
                            read(STDIN_FILENO, &seq[2], 1);
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
                refresh_input();
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
            printf("\033[2J\033[H");
            fflush(stdout);
            refresh_input();
            continue;
        }
        
        // Insert character at cursor position
        if (c >= 32 && c < 127) {  // Printable ASCII
            g_input_buffer.insert(g_cursor_pos, 1, c);
            g_cursor_pos++;
            refresh_input();
        }
    }
    
    // Move cursor down and show normal prompt for the response
    printf("\033[%d;1H", g_input_line_row + 2);
    printf("\n");
    fflush(stdout);
    
    return g_input_buffer;
}

void clear_input_area() {
    if (!g_enabled || !g_initialized || g_input_line_row < 0) return;
    
    // Clear the input line
    move_to_row(g_input_line_row + 1);
    clear_to_end();
    
    // Clear the separator line
    move_to_row(g_input_line_row);
    clear_to_end();
    
    fflush(stdout);
}

bool is_enabled() {
    return g_enabled;
}

void set_enabled(bool enabled) {
    g_enabled = enabled;
}

}  // namespace cli_tui
