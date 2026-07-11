#pragma once

#include <string>
#include <deque>
#include <mutex>
#include <vector>

// Thread-safe circular output buffer for TUI
// Stores lines of output and provides visible window for rendering

class output_buffer {
public:
    output_buffer();
    ~output_buffer() = default;
    
    // Add a line to the buffer (thread-safe)
    void push_line(const std::string& line);
    
    // Add formatted line (thread-safe)
    void push_linef(const char* fmt, ...);
    
    // Get the last N lines for rendering (thread-safe)
    // If max_lines <= 0, returns ALL lines
    std::vector<std::string> get_visible_lines(int max_lines) const;
    
    // Get total line count
    size_t size() const;
    
    // Clear the buffer
    void clear();
    
    // Get scroll offset (how many lines have scrolled off screen)
    int scroll_offset() const { return scroll_offset_; }

    // Get the current viewport offset (for scrolling back through history)
    int viewport_offset() const { return viewport_offset_; }

    // Set the viewport offset (negative = scroll back, 0 = at bottom)
    void set_viewport_offset(int offset) { viewport_offset_ = offset; }

    // Get lines for a specific viewport window
    // viewport_offset: 0 = bottom (latest), positive = scroll back
    // max_lines: how many lines to return
    std::vector<std::string> get_viewport_lines(int max_lines) const;

    // Bounded buffer limits
    static constexpr size_t MAX_BUFFER_LINES = 5000;
    static constexpr size_t MAX_LINE_LENGTH = 16384;  // Increased from 8192 for long lines

private:
    mutable std::mutex mutex_;
    std::deque<std::string> lines_;
    int scroll_offset_ = 0;  // Number of lines that have scrolled off
    int viewport_offset_ = 0;  // User scroll offset (0 = at bottom)
};

// Global buffer instance
extern output_buffer g_output_buffer;
