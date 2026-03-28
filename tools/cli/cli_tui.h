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

// Check if TUI is enabled
bool is_enabled();

// Enable/disable TUI
void set_enabled(bool enabled);

}  // namespace cli_tui
