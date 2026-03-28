#pragma once

#include <string>

// TUI (Text User Interface) for llama-cli
// Architecture: View-Controller-Output
//
// - output_buffer: stores all output lines (thread-safe)
// - cli_tui: renders the view (output + input box)
//
// Usage:
//   cli_tui::init() at startup
//   cli_tui::print() instead of console::log()
//   cli_tui::read_input() for user input
//   cli_tui::render() to refresh the display

namespace cli_tui {

// Initialize TUI (call once at startup)
void init();

// Cleanup TUI (call before exit)
void cleanup();

// Print a line to the output buffer (thread-safe)
// This replaces console::log() when TUI is enabled
void print(const char* fmt, ...);

// Bulk print support (for startup logo)
void begin_bulk_print();
void end_bulk_print();

// Print without newline (for streaming output)
// Accumulates in a buffer until flush() is called
void print_stream(const char* text);

// Flush the streaming buffer and trigger render
void flush_stream();

// Read a line of input with TUI interface
// Returns the input string
std::string read_input();

// Render the entire screen (output + input box)
// Call this periodically to refresh the display
void render();

// Check if TUI is enabled
bool is_enabled();

// Enable/disable TUI
void set_enabled(bool enabled);

// Force a full screen redraw (e.g., after terminal resize)
void force_redraw();

// Set the stats line text (displayed below input box)
void set_stats_line(const char* text);

// Scroll output up/down (positive = down, negative = up)
void scroll_output(int lines);

}  // namespace cli_tui
