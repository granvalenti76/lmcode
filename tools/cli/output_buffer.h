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

private:
    mutable std::mutex mutex_;
    std::deque<std::string> lines_;
    int scroll_offset_ = 0;  // Number of lines that have scrolled off
};

// Global buffer instance
extern output_buffer g_output_buffer;
