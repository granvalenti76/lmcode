#pragma once

#include <string>

// Minimal TUI input box for llama-cli
// Shows a fixed input box at the bottom of the terminal

namespace cli_tui {

// Initialize TUI (call once at startup)
void init();

// Cleanup TUI (call before exit)
void cleanup();

// Read a line of input with TUI interface
// Returns the input string
std::string read_input();

// Clear the input area (used after input is submitted)
void clear_input_area();

// Scroll output and redraw input box at bottom
// Call this before printing output that should appear above the input
void scroll_output();

// Hide input box during generation
void hide_for_generation();

// Show input box after generation completes
void show_after_generation();

// Check if TUI is enabled
bool is_enabled();

// Enable/disable TUI
void set_enabled(bool enabled);

}  // namespace cli_tui
