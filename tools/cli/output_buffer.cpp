#include "output_buffer.h"

#include <cstdarg>
#include <cstdio>

output_buffer g_output_buffer;

output_buffer::output_buffer() {
}

void output_buffer::push_line(const std::string& line) {
    std::lock_guard<std::mutex> lock(mutex_);
    lines_.push_back(line);
    
    // Keep buffer size reasonable (max 10000 lines)
    while (lines_.size() > 10000) {
        lines_.pop_front();
        scroll_offset_++;
    }
}

void output_buffer::push_linef(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    
    // Format the string
    char buffer[4096];
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    
    va_end(args);
    
    // Split by newlines and add each line
    std::string line;
    for (const char* p = buffer; *p; ++p) {
        if (*p == '\n') {
            push_line(line);
            line.clear();
        } else {
            line += *p;
        }
    }
    if (!line.empty()) {
        push_line(line);
    }
}

std::vector<std::string> output_buffer::get_visible_lines(int max_lines) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<std::string> result;
    
    if (static_cast<int>(lines_.size()) <= max_lines) {
        // All lines fit
        result.assign(lines_.begin(), lines_.end());
    } else {
        // Take only the last max_lines
        auto start = lines_.end() - max_lines;
        result.assign(start, lines_.end());
    }
    
    return result;
}

size_t output_buffer::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lines_.size();
}

void output_buffer::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    lines_.clear();
    scroll_offset_ = 0;
}
