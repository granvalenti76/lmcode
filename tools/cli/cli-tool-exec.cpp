#include "cli-tool-exec.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <sys/wait.h>
#include <unistd.h>  // for getpid()

using json = nlohmann::ordered_json;

namespace fs = std::filesystem;

// ============================================================================
// Helper: shell-quote a single argument to prevent injection
// ============================================================================

static std::string shell_quote(const std::string& arg) {
    // Wrap in single quotes, escaping any single quotes inside
    std::string result = "'";
    for (char c : arg) {
        if (c == '\'') result += "'\\''";
        else           result += c;
    }
    result += "'";
    return result;
}

// ============================================================================
// Helper functions implementation
// ============================================================================

namespace cli_tool_exec {

cli_tool_result run_command(
    const std::string& cmd,
    int timeout_seconds,
    size_t max_output_length)
{
    cli_tool_result result;
    result.exit_code = -1;
    result.success = false;

    // Try 'timeout' first (Linux), fallback to 'gtimeout' (macOS with coreutils)
    // If neither exists, run without timeout (less safe but works)
    std::string timeout_cmd = "command -v timeout >/dev/null && echo timeout || "
                              "command -v gtimeout >/dev/null && echo gtimeout || "
                              "echo ''";
    FILE* timeout_pipe = popen(timeout_cmd.c_str(), "r");
    std::string timeout_bin;
    if (timeout_pipe) {
        char buf[64];
        if (fgets(buf, sizeof(buf), timeout_pipe)) {
            timeout_bin = buf;
            // Trim whitespace and newlines
            size_t start = timeout_bin.find_first_not_of(" \t\n\r");
            size_t end = timeout_bin.find_last_not_of(" \t\n\r");
            if (start != std::string::npos && end != std::string::npos) {
                timeout_bin = timeout_bin.substr(start, end - start + 1);
            } else {
                timeout_bin.clear();
            }
            // Validate: only accept 'timeout' or 'gtimeout'
            if (timeout_bin != "timeout" && timeout_bin != "gtimeout") {
                timeout_bin.clear();
            }
        }
        pclose(timeout_pipe);
    }

    // Write command to a temporary script file to preserve multi-line commands (heredocs)
    // Using shell_quote() breaks heredocs because it wraps everything in single quotes
    std::string tmp_script = "/tmp/llama_cli_cmd_" + std::to_string(getpid()) + ".sh";
    {
        std::ofstream script_file(tmp_script);
        if (!script_file) {
            result.error = "Failed to create temporary script file";
            result.exit_code = -1;
            return result;
        }
        script_file << "#!/bin/sh\n" << cmd;
        script_file.close();
        
        if (!script_file) {
            result.error = "Failed to write temporary script file";
            result.exit_code = -1;
            std::remove(tmp_script.c_str());
            return result;
        }
    }

    std::string full_cmd;
    if (!timeout_bin.empty()) {
        // Use timeout command with temp script
        full_cmd = timeout_bin + " " + std::to_string(timeout_seconds) +
                   " sh " + shell_quote(tmp_script) + " 2>&1";
    } else {
        // No timeout available - just run the script
        full_cmd = "sh " + shell_quote(tmp_script) + " 2>&1";
    }

    FILE* pipe = popen(full_cmd.c_str(), "r");
    if (!pipe) {
        std::remove(tmp_script.c_str());
        result.error = "Failed to execute command: " + cmd;
        return result;
    }

    std::ostringstream output;
    std::array<char, 4096> buffer;
    bool truncated = false;

    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        output << buffer.data();
        if (output.str().size() >= max_output_length) {
            truncated = true;
            break;
        }
    }

    // Drain pipe to avoid SIGPIPE if we broke out early
    if (truncated) {
        while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {}
    }

    int raw_exit = pclose(pipe);
    
    // Clean up temp script
    std::remove(tmp_script.c_str());
    
    result.content = output.str();

    // Handle exit codes
    if (raw_exit == -1) {
        result.error = "pclose failed";
        result.exit_code = -1;
    } else if (WIFEXITED(raw_exit)) {
        result.exit_code = WEXITSTATUS(raw_exit);
        if (result.exit_code == 124 || result.exit_code == 125) {
            // timeout/gtimeout exit codes
            result.error = "Command timed out after " + std::to_string(timeout_seconds) + "s";
            result.success = false;
            return result;
        }
    } else if (WIFSIGNALED(raw_exit)) {
        // Process was killed by signal (likely timeout)
        result.error = "Process killed by signal " + std::to_string(WTERMSIG(raw_exit));
        result.exit_code = -1;
    } else {
        result.exit_code = -1;
    }

    result.success = (result.exit_code == 0);

    if (truncated) {
        result.content += "\n... [output truncated at " + std::to_string(max_output_length) + " bytes]";
    }

    return result;
}

cli_tool_result read_file_content(const std::string& path) {
    cli_tool_result result;

    try {
        if (!fs::exists(path)) {
            result.error = "File does not exist: " + path;
            result.exit_code = 1;
            result.success = false;
            return result;
        }
        if (!fs::is_regular_file(path)) {
            result.error = "Not a regular file: " + path;
            result.exit_code = 1;
            result.success = false;
            return result;
        }

        std::ifstream file(path, std::ios::binary);
        if (!file) {
            result.error = "Cannot open file: " + path;
            result.exit_code = 1;
            result.success = false;
            return result;
        }

        std::ostringstream content;
        content << file.rdbuf();
        result.content = content.str();
        result.exit_code = 0;
        result.success = true;
        
        // Log for debugging
        fprintf(stderr, "[read_file] path=%s, size=%zu, success=%d\n", 
                path.c_str(), result.content.size(), result.success ? 1 : 0);
    } catch (const std::exception& e) {
        result.error = std::string("Error reading file: ") + e.what();
        result.exit_code = 1;
        result.success = false;
    }

    return result;
}

cli_tool_result write_file_content(const std::string& path, const std::string& content) {
    cli_tool_result result;

    try {
        bool exists = fs::exists(path);

        // Create parent directories if needed
        auto parent = fs::path(path).parent_path();
        if (!parent.empty() && !fs::exists(parent)) {
            fs::create_directories(parent);
        }

        // Write atomically: write to temp file first, then rename
        auto tmp_path = parent / (fs::path(path).filename().string() + ".tmp_" + std::to_string(getpid()));
        
        std::ofstream file(tmp_path, std::ios::binary);
        if (!file) {
            result.error = "Cannot open temp file for writing: " + tmp_path.string();
            result.exit_code = 1;
            return result;
        }

        file << content;
        file.close();

        if (!file) {
            fs::remove(tmp_path);  // Clean up temp file on error
            result.error = "Write failed (disk full?): " + path;
            result.exit_code = 1;
            return result;
        }

        // Atomic rename
        fs::rename(tmp_path, path);

        result.content = exists ? "File updated: " + path : "File created: " + path;
        result.exit_code = 0;
        result.success = true;
    } catch (const std::exception& e) {
        result.error = std::string("Error writing file: ") + e.what();
        result.exit_code = 1;
    }

    return result;
}

// Start writing a large file (creates/truncates file)
// Use with append_file and finish_file for large files
cli_tool_result start_file(const std::string& path) {
    cli_tool_result result;

    try {
        // Create parent directories if needed
        auto parent = fs::path(path).parent_path();
        if (!parent.empty() && !fs::exists(parent)) {
            fs::create_directories(parent);
        }

        // Create/truncate file
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file) {
            result.error = "Cannot create file: " + path;
            result.exit_code = 1;
            result.success = false;
            return result;
        }

        file.close();

        result.content = "File created (ready for append): " + path;
        result.exit_code = 0;
        result.success = true;

    } catch (const std::exception& e) {
        result.error = std::string("Error in start_file: ") + e.what();
        result.exit_code = 1;
        result.success = false;
    }

    return result;
}

// Helper: calculate simple hash of content for verification
static std::string calculate_hash(const std::string& content) {
    // Simple djb2 hash - fast and good enough for verification
    unsigned long hash = 5381;
    for (char c : content) {
        hash = ((hash << 5) + hash) + static_cast<unsigned char>(c);
    }
    return std::to_string(hash);
}

// Append content to a file (for chunked writing).
// Use after start_file, before finish_file.
//
// Chunk size guidelines:
//   - Recommended: 20-50 lines per call (~1-2 KB)
//   - Hard limit:  200 lines or 8 KB per call
//   - No minimum: empty string is a no-op (returns success)
//
// Returns: bytes written, cumulative file size, and hash of this chunk
// so the caller can verify delivery without reading the whole file.
cli_tool_result append_file(const std::string& path, const std::string& content) {
    cli_tool_result result;

    try {
        if (!fs::exists(path)) {
            result.error = "File does not exist. Call start_file first: " + path;
            result.exit_code = 1;
            result.success = false;
            return result;
        }

        // No-op on empty content - not an error
        if (content.empty()) {
            auto file_size = fs::file_size(path);
            result.content = "No-op (empty chunk). File size: " + std::to_string(file_size) + " bytes.";
            result.exit_code = 0;
            result.success = true;
            return result;
        }

        // Hard limits: protect against accidentally dumping megabytes in one call
        const size_t MAX_CHUNK_BYTES = 8192;   // 8 KB
        const size_t MAX_CHUNK_LINES = 200;

        if (content.size() > MAX_CHUNK_BYTES) {
            result.error = "Chunk too large (" + std::to_string(content.size()) +
                          " bytes, hard limit " + std::to_string(MAX_CHUNK_BYTES) +
                          " bytes / ~" + std::to_string(MAX_CHUNK_LINES) + " lines). "
                          "Split into smaller append_file calls.";
            result.exit_code = 1;
            result.success = false;
            return result;
        }

        // Count newlines (not +1, because a trailing \n does NOT add a phantom line)
        size_t newlines = std::count(content.begin(), content.end(), '\n');
        if (newlines > MAX_CHUNK_LINES) {
            result.error = "Chunk contains " + std::to_string(newlines) +
                          " newlines, hard limit is " + std::to_string(MAX_CHUNK_LINES) +
                          ". Split into smaller append_file calls.";
            result.exit_code = 1;
            result.success = false;
            return result;
        }

        // Append to file
        std::ofstream file(path, std::ios::binary | std::ios::app);
        if (!file) {
            result.error = "Cannot open file for append: " + path;
            result.exit_code = 1;
            result.success = false;
            return result;
        }

        file << content;
        file.close();

        if (!file) {
            result.error = "Append failed (disk full?): " + path;
            result.exit_code = 1;
            result.success = false;
            return result;
        }

        // Report: chunk hash + cumulative file size (useful for resume logic)
        std::string chunk_hash = calculate_hash(content);
        auto cumulative_size   = fs::file_size(path);

        result.content = "Appended " + std::to_string(content.size()) + " bytes (" +
                         std::to_string(newlines) + " newlines) to " + path +
                         " | chunk_hash=" + chunk_hash +
                         " | file_size=" + std::to_string(cumulative_size);
        result.exit_code = 0;
        result.success = true;

    } catch (const std::exception& e) {
        result.error = std::string("Error in append_file: ") + e.what();
        result.exit_code = 1;
        result.success = false;
    }

    return result;
}

// Finish writing a file (optional, just confirms completion)
// Called after all append_file calls
// Verifies file integrity and reports final stats
cli_tool_result finish_file(const std::string& path) {
    cli_tool_result result;

    try {
        if (!fs::exists(path)) {
            result.error = "File does not exist: " + path;
            result.exit_code = 1;
            result.success = false;
            return result;
        }

        // Get file size for confirmation
        auto size = fs::file_size(path);
        
        // Read entire file and calculate total hash
        std::ifstream file(path, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
        file.close();

        size_t lines = std::count(content.begin(), content.end(), '\n') + 1;
        std::string total_hash = calculate_hash(content);

        result.content = "✓ File complete: " + path + " (" + 
                        std::to_string(lines) + " lines, " +
                        std::to_string(size) + " bytes, " +
                        "hash: " + total_hash + ")";
        result.exit_code = 0;
        result.success = true;

    } catch (const std::exception& e) {
        result.error = std::string("Error in finish_file: ") + e.what();
        result.exit_code = 1;
        result.success = false;
    }

    return result;
}

// Get file info: size, line count, hash — useful before resuming a partial write
// or to verify a file is in the expected state before editing.
cli_tool_result get_file_info(const std::string& path) {
    cli_tool_result result;

    try {
        if (!fs::exists(path)) {
            // Report cleanly so the caller can decide to start fresh
            result.content = "NOT_FOUND path=" + path;
            result.exit_code = 0;
            result.success = true;
            return result;
        }
        if (!fs::is_regular_file(path)) {
            result.error = "Not a regular file: " + path;
            result.exit_code = 1;
            result.success = false;
            return result;
        }

        auto size = fs::file_size(path);

        std::ifstream file(path, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
        file.close();

        size_t newlines = std::count(content.begin(), content.end(), '\n');
        std::string hash = calculate_hash(content);

        result.content = "path=" + path +
                         " | size=" + std::to_string(size) +
                         " | newlines=" + std::to_string(newlines) +
                         " | hash=" + hash;
        result.exit_code = 0;
        result.success = true;

    } catch (const std::exception& e) {
        result.error = std::string("Error in get_file_info: ") + e.what();
        result.exit_code = 1;
        result.success = false;
    }

    return result;
}

// Verify file: check that the file's content hash matches an expected value.
// Use after finish_file to confirm the whole write succeeded without corruption.
cli_tool_result verify_file(const std::string& path, const std::string& expected_hash) {
    cli_tool_result result;

    try {
        if (!fs::exists(path)) {
            result.error = "File not found: " + path;
            result.exit_code = 1;
            result.success = false;
            return result;
        }

        std::ifstream file(path, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
        file.close();

        std::string actual_hash = calculate_hash(content);

        if (actual_hash == expected_hash) {
            result.content = "✓ Hash matches: " + path + " (hash=" + actual_hash + ")";
            result.exit_code = 0;
            result.success = true;
        } else {
            result.error = "Hash mismatch for " + path +
                           ": expected=" + expected_hash +
                           " actual=" + actual_hash;
            result.exit_code = 1;
            result.success = false;
        }

    } catch (const std::exception& e) {
        result.error = std::string("Error in verify_file: ") + e.what();
        result.exit_code = 1;
        result.success = false;
    }

    return result;
}

// Search and replace: find a unique block of text and replace it
// More robust than line numbers because it doesn't break when file changes above
cli_tool_result search_replace(const std::string& path, const std::string& search, const std::string& replace) {
    cli_tool_result result;

    try {
        if (!fs::exists(path)) {
            result.error = "File not found: " + path;
            result.exit_code = 1;
            result.success = false;
            return result;
        }
        if (!fs::is_regular_file(path)) {
            result.error = "Not a regular file: " + path;
            result.exit_code = 1;
            result.success = false;
            return result;
        }

        // Read file content
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            result.error = "Cannot open file: " + path;
            result.exit_code = 1;
            result.success = false;
            return result;
        }

        std::ostringstream content;
        content << file.rdbuf();
        std::string original = content.str();
        file.close();

        // Find the search string
        size_t pos = original.find(search);
        if (pos == std::string::npos) {
            result.error = "Search string not found in file. Make sure the text matches exactly (including whitespace and newlines).";
            result.exit_code = 1;
            result.success = false;
            return result;
        }

        // Check for multiple occurrences
        size_t pos2 = original.find(search, pos + 1);
        if (pos2 != std::string::npos) {
            // Count occurrences manually
            int count = 0;
            size_t curr = 0;
            while ((curr = original.find(search, curr)) != std::string::npos) {
                count++;
                curr += search.size();
                if (count > 10) break;  // Cap at 10 for performance
            }
            result.error = "Search string found " + std::to_string(count) + 
                          " times. Make the search string more unique.";
            result.exit_code = 1;
            result.success = false;
            return result;
        }

        // Perform replacement
        std::string new_content = original;
        new_content.replace(pos, search.size(), replace);

        // Write atomically
        auto parent = fs::path(path).parent_path();
        auto tmp_path = parent / (fs::path(path).filename().string() + ".tmp_" + std::to_string(getpid()));

        std::ofstream out(tmp_path, std::ios::binary);
        if (!out) {
            result.error = "Cannot open temp file for writing: " + tmp_path.string();
            result.exit_code = 1;
            result.success = false;
            return result;
        }

        out << new_content;
        out.close();

        if (!out) {
            fs::remove(tmp_path);
            result.error = "Write failed (disk full?): " + path;
            result.exit_code = 1;
            result.success = false;
            return result;
        }

        fs::rename(tmp_path, path);

        // Count lines changed for feedback
        size_t search_lines = std::count(search.begin(), search.end(), '\n') + 1;
        size_t replace_lines = std::count(replace.begin(), replace.end(), '\n') + 1;

        result.content = "Replaced " + std::to_string(search_lines) + " lines with " + 
                        std::to_string(replace_lines) + " lines in " + path;
        result.exit_code = 0;
        result.success = true;

    } catch (const std::exception& e) {
        result.error = std::string("Error in search_replace: ") + e.what();
        result.exit_code = 1;
        result.success = false;
    }

    return result;
}

cli_tool_result list_directory(const std::string& path) {
    cli_tool_result result;

    try {
        std::string actual_path = path.empty() ? "." : path;

        if (!fs::exists(actual_path)) {
            result.error = "Directory does not exist: " + actual_path;
            result.exit_code = 1;
            return result;
        }
        if (!fs::is_directory(actual_path)) {
            result.error = "Not a directory: " + actual_path;
            result.exit_code = 1;
            return result;
        }

        // Collect and sort entries for deterministic output
        std::vector<fs::directory_entry> entries;
        for (const auto& entry : fs::directory_iterator(actual_path)) {
            entries.push_back(entry);
        }
        std::sort(entries.begin(), entries.end(),
            [](const fs::directory_entry& a, const fs::directory_entry& b) {
                // Dirs first, then files, both alphabetical
                bool a_dir = a.is_directory();
                bool b_dir = b.is_directory();
                if (a_dir != b_dir) return a_dir > b_dir;
                return a.path().filename() < b.path().filename();
            });

        std::ostringstream output;
        output << "Contents of " << actual_path << ":\n";

        int file_count = 0, dir_count = 0;
        for (const auto& entry : entries) {
            const auto& name = entry.path().filename().string();
            if (entry.is_directory()) {
                output << "  [DIR]  " << name << "/\n";
                dir_count++;
            } else {
                // Show file size for context
                std::error_code ec;
                auto size = fs::file_size(entry.path(), ec);
                output << "  [FILE] " << name;
                if (!ec) output << "  (" << size << " B)";
                output << "\n";
                file_count++;
            }
        }
        output << "\nTotal: " << (file_count + dir_count) << " items ("
               << file_count << " files, " << dir_count << " directories)";

        result.content = output.str();
        result.exit_code = 0;
        result.success = true;
    } catch (const fs::filesystem_error& e) {
        result.error = std::string("Filesystem error: ") + e.what();
        result.exit_code = 1;
    } catch (const std::exception& e) {
        result.error = std::string("Error listing directory: ") + e.what();
        result.exit_code = 1;
    }

    return result;
}

bool file_exists(const std::string& path) {
    return fs::exists(path);
}

bool is_in_cwd(const std::string& path) {
    try {
        auto abs_path = fs::weakly_canonical(fs::absolute(path));
        auto cwd      = fs::weakly_canonical(fs::current_path());
        // Use lexically_relative: if path is outside cwd the result starts with ".."
        auto rel = abs_path.lexically_relative(cwd);
        auto it  = rel.begin();
        return it == rel.end() || it->string() != "..";
    } catch (...) {
        return false;
    }
}

cli_tool_result insert_line_at(const std::string& path, int line_number, const std::string& content) {
    cli_tool_result result;

    try {
        if (!fs::exists(path)) {
            result.error = "File does not exist: " + path;
            result.exit_code = 1;
            return result;
        }

        // Read all lines
        std::ifstream file(path);
        if (!file) {
            result.error = "Cannot open file: " + path;
            result.exit_code = 1;
            return result;
        }

        std::vector<std::string> lines;
        std::string line;
        while (std::getline(file, line)) {
            lines.push_back(line);
        }
        file.close();

        // Validate line number (0-indexed, allow 0 to lines.size())
        if (line_number < 0 || line_number > static_cast<int>(lines.size())) {
            result.error = "Invalid line number: " + std::to_string(line_number) +
                           " (file has " + std::to_string(lines.size()) + " lines)";
            result.exit_code = 1;
            return result;
        }

        // Insert the new line
        lines.insert(lines.begin() + line_number, content);

        // Write atomically to temp file, then rename
        auto parent = fs::path(path).parent_path();
        auto tmp_path = parent / (fs::path(path).filename().string() + ".tmp_" + std::to_string(getpid()));

        std::ofstream out(tmp_path);
        if (!out) {
            result.error = "Cannot open temp file for writing: " + tmp_path.string();
            result.exit_code = 1;
            return result;
        }

        for (size_t i = 0; i < lines.size(); i++) {
            out << lines[i];
            if (i < lines.size() - 1) out << "\n";
        }
        if (!lines.empty()) out << "\n";  // Trailing newline only if file has content
        out.close();

        if (!out) {
            fs::remove(tmp_path);  // Clean up temp file on error
            result.error = "Write failed (disk full?): " + path;
            result.exit_code = 1;
            return result;
        }

        fs::rename(tmp_path, path);  // Atomic rename

        result.content = "Inserted line at position " + std::to_string(line_number) + " in " + path;
        result.exit_code = 0;
        result.success = true;
    } catch (const std::exception& e) {
        result.error = std::string("Error inserting line: ") + e.what();
        result.exit_code = 1;
    }

    return result;
}

cli_tool_result replace_range(const std::string& path, int start_line, int end_line, const std::string& content) {
    cli_tool_result result;

    try {
        if (!fs::exists(path)) {
            result.error = "File does not exist: " + path;
            result.exit_code = 1;
            return result;
        }

        // Read all lines
        std::ifstream file(path);
        if (!file) {
            result.error = "Cannot open file: " + path;
            result.exit_code = 1;
            return result;
        }

        std::vector<std::string> lines;
        std::string line;
        while (std::getline(file, line)) {
            lines.push_back(line);
        }
        file.close();

        // Validate line numbers
        int total_lines = static_cast<int>(lines.size());
        if (start_line < 0 || start_line > total_lines) {
            result.error = "Invalid start line: " + std::to_string(start_line) + 
                           " (must be 0-" + std::to_string(total_lines) + ")";
            result.exit_code = 1;
            return result;
        }
        if (end_line < start_line || end_line > total_lines) {
            result.error = "Invalid end line: " + std::to_string(end_line) + 
                           " (must be >= start_line and <= " + std::to_string(total_lines) + ")";
            result.exit_code = 1;
            return result;
        }

        // Split content into lines
        std::vector<std::string> new_lines;
        std::istringstream content_stream(content);
        std::string content_line;
        while (std::getline(content_stream, content_line)) {
            new_lines.push_back(content_line);
        }

        // Replace the range: erase [start_line, end_line) and insert new lines
        auto it = lines.erase(lines.begin() + start_line, lines.begin() + end_line);
        lines.insert(it, new_lines.begin(), new_lines.end());

        // Write atomically to temp file, then rename
        auto parent = fs::path(path).parent_path();
        auto tmp_path = parent / (fs::path(path).filename().string() + ".tmp_" + std::to_string(getpid()));

        std::ofstream out(tmp_path);
        if (!out) {
            result.error = "Cannot open temp file for writing: " + tmp_path.string();
            result.exit_code = 1;
            return result;
        }

        for (size_t i = 0; i < lines.size(); i++) {
            out << lines[i];
            if (i < lines.size() - 1) out << "\n";
        }
        if (!lines.empty()) out << "\n";  // Trailing newline only if file has content
        out.close();

        if (!out) {
            fs::remove(tmp_path);  // Clean up temp file on error
            result.error = "Write failed (disk full?): " + path;
            result.exit_code = 1;
            return result;
        }

        fs::rename(tmp_path, path);  // Atomic rename

        result.content = "Replaced lines " + std::to_string(start_line) + "-" +
                         std::to_string(end_line) + " with " + std::to_string(new_lines.size()) +
                         " new lines in " + path;
        result.exit_code = 0;
        result.success = true;
    } catch (const std::exception& e) {
        result.error = std::string("Error replacing range: ") + e.what();
        result.exit_code = 1;
    }

    return result;
}

cli_tool_result delete_lines(const std::string& path, int start_line, int end_line) {
    cli_tool_result result;

    try {
        if (!fs::exists(path)) {
            result.error = "File does not exist: " + path;
            result.exit_code = 1;
            return result;
        }

        // Read all lines
        std::ifstream file(path);
        if (!file) {
            result.error = "Cannot open file: " + path;
            result.exit_code = 1;
            return result;
        }

        std::vector<std::string> lines;
        std::string line;
        while (std::getline(file, line)) {
            lines.push_back(line);
        }
        file.close();

        // Validate line numbers
        int total_lines = static_cast<int>(lines.size());
        if (start_line < 0 || start_line >= total_lines) {
            result.error = "Invalid start line: " + std::to_string(start_line);
            result.exit_code = 1;
            return result;
        }
        if (end_line < start_line || end_line > total_lines) {
            result.error = "Invalid end line: " + std::to_string(end_line) + 
                           " (must be >= start_line and <= " + std::to_string(total_lines) + ")";
            result.exit_code = 1;
            return result;
        }

        // Delete the range
        lines.erase(lines.begin() + start_line, lines.begin() + end_line);

        // Write atomically to temp file, then rename
        auto parent = fs::path(path).parent_path();
        auto tmp_path = parent / (fs::path(path).filename().string() + ".tmp_" + std::to_string(getpid()));
        
        std::ofstream out(tmp_path);
        if (!out) {
            result.error = "Cannot open temp file for writing: " + tmp_path.string();
            result.exit_code = 1;
            return result;
        }

        for (size_t i = 0; i < lines.size(); i++) {
            out << lines[i];
            if (i < lines.size() - 1) out << "\n";
        }
        if (!lines.empty()) out << "\n";
        out.close();

        if (!out) {
            fs::remove(tmp_path);  // Clean up temp file on error
            result.error = "Write failed (disk full?): " + path;
            result.exit_code = 1;
            return result;
        }

        fs::rename(tmp_path, path);  // Atomic rename

        result.content = "Deleted lines " + std::to_string(start_line) + "-" +
                         std::to_string(end_line) + " from " + path +
                         " (remaining: " + std::to_string(lines.size()) + " lines)";
        result.exit_code = 0;
        result.success = true;
    } catch (const std::exception& e) {
        result.error = std::string("Error deleting lines: ") + e.what();
        result.exit_code = 1;
    }

    return result;
}

}  // namespace cli_tool_exec

// ============================================================================
// cli_tool_executor_impl
// ============================================================================

cli_tool_executor_impl::cli_tool_executor_impl(const cli_tool_security_config& config)
    : config_(config) {}

bool cli_tool_executor_impl::is_command_safe(const std::string& cmd) const {
    return get_safety_violation(cmd).empty();
}

std::string cli_tool_executor_impl::get_safety_violation(const std::string& cmd) const {
    // Block shell operators that could bypass security
    // Note: ">" and ">>" are intentionally allowed here because:
    // 1. The shell_whitelist uses regex_match, requiring the ENTIRE command to match safe patterns
    // 2. Heredocs (cat << 'EOF' > file) require ">" redirection, which is explicitly encouraged
    //    in the system prompt for writing files longer than 50 lines
    // 3. The whitelist patterns (e.g., "^cat\\s.*") already restrict which commands can use redirects
    static const std::vector<std::string> dangerous_ops = {
        ";", "&&", "||", "|", "`", "$(", "${", "<("
        // Removed ">>" and ">" - these are safe when combined with whitelist enforcement
    };
    for (const auto& op : dangerous_ops) {
        if (cmd.find(op) != std::string::npos) {
            return "Command contains dangerous shell operator: " + op;
        }
    }

    // Blacklist checked first with actual regex
    for (const auto& pattern : config_.shell_blacklist) {
        try {
            std::regex re(pattern, std::regex::ECMAScript);
            if (std::regex_search(cmd, re)) {
                return "Command matches blacklist pattern: " + pattern;
            }
        } catch (const std::regex_error&) {
            // Fallback to literal match if regex is invalid
            if (cmd.find(pattern) != std::string::npos) {
                return "Command matches blacklist pattern: " + pattern;
            }
        }
    }

    // Whitelist: ENTIRE command must match (use regex_match, not search)
    for (const auto& pattern : config_.shell_whitelist) {
        try {
            std::regex re(pattern, std::regex::ECMAScript);
            if (std::regex_match(cmd, re)) {
                return "";  // Safe
            }
        } catch (const std::regex_error&) {
            // Fallback: check if command starts with pattern
            if (cmd.find(pattern) == 0) {
                return "";
            }
        }
    }

    return "Command not in whitelist";
}

bool cli_tool_executor_impl::requires_confirmation(const cli_tool_call& call) const {
    if (call.name == "read_file" || call.name == "list_dir" ||
        call.name == "touch_file") {
        return false;
    }

    if (call.name == "shell") {
        try {
            auto args = json::parse(call.arguments);
            std::string cmd = args.value("command", std::string(""));
            // Trim
            auto s = cmd.find_first_not_of(" \t\n\r");
            auto e = cmd.find_last_not_of(" \t\n\r");
            if (s == std::string::npos) return true;
            cmd = cmd.substr(s, e - s + 1);

            // Safe read-only commands: no confirmation needed
            static const std::vector<std::string> safe_prefixes = {
                "ls", "pwd", "whoami", "date", "echo", "uname", "wc ",
                "head ", "tail ", "cat ", "grep ", "find "
            };
            for (const auto& prefix : safe_prefixes) {
                if (cmd == prefix || cmd.find(prefix + " ") == 0) {
                    // Extra safety: no redirects
                    if (cmd.find('>') == std::string::npos &&
                        cmd.find(">>") == std::string::npos) {
                        return false;
                    }
                }
            }
        } catch (...) {}
        return true;
    }

    // write_file, swift_run, swift_package, insert_line, replace_range, delete_lines
    // always need confirmation (they modify files)
    if (call.name == "write_file" || call.name == "swift_run" ||
        call.name == "swift_package" || call.name == "insert_line" ||
        call.name == "replace_range" || call.name == "delete_lines") {
        return true;
    }

    return false;
}

cli_tool_result cli_tool_executor_impl::execute(const cli_tool_call& call, bool dry_run) {
    if (dry_run) {
        cli_tool_result result;
        result.tool_call_id = call.id;
        result.content = "[DRY RUN] Would execute: " + call.name + " " + call.arguments;
        result.success = true;
        result.exit_code = 0;
        return result;
    }

    cli_tool_result result;
    result.tool_call_id = call.id;

    try {
        if      (call.name == "read_file")      result = execute_read_file(call);
        else if (call.name == "touch_file")     result = execute_touch_file(call);
        else if (call.name == "write_file")     result = execute_write_file(call);
        else if (call.name == "start_file")     result = execute_start_file(call);
        else if (call.name == "append_file")    result = execute_append_file(call);
        else if (call.name == "finish_file")    result = execute_finish_file(call);
        else if (call.name == "get_file_info")  result = execute_get_file_info(call);
        else if (call.name == "verify_file")    result = execute_verify_file(call);
        else if (call.name == "search_replace") result = execute_search_replace(call);
        else if (call.name == "list_dir")       result = execute_list_dir(call);
        else if (call.name == "shell")          result = execute_shell(call);
        else if (call.name == "insert_line")    result = execute_insert_line(call);
        else if (call.name == "replace_range")  result = execute_replace_range(call);
        else if (call.name == "delete_lines")   result = execute_delete_lines(call);
        else if (call.name == "swift_build")    result = execute_swift_build(call);
        else if (call.name == "swift_test")     result = execute_swift_test(call);
        else if (call.name == "swift_run")      result = execute_swift_run(call);
        else if (call.name == "swift_package")  result = execute_swift_package(call);
        else if (call.name == "swift_format")   result = execute_swift_format(call);
        else {
            result.error = "Unknown tool: " + call.name;
            result.exit_code = -1;
        }
    } catch (const std::exception& e) {
        result.error = std::string("Tool execution error: ") + e.what();
        result.exit_code = -1;
    }

    return result;
}

// ============================================================================
// Argument parsing helpers
// ============================================================================

static json parse_args(const std::string& args_json) {
    if (args_json.empty()) return json::object();
    return json::parse(args_json);
}

static std::string get_arg(const json& args, const std::string& key,
                            const std::string& default_val = "") {
    return args.value(key, default_val);
}

// ============================================================================
// Tool implementations
// ============================================================================

cli_tool_result cli_tool_executor_impl::execute_read_file(const cli_tool_call& call) {
    auto args = parse_args(call.arguments);
    std::string path = get_arg(args, "path");
    if (path.empty()) {
        cli_tool_result r; r.tool_call_id = call.id;
        r.error = "Missing required argument: path"; r.exit_code = -1;
        return r;
    }
    
    // Security check: prevent reading files outside CWD
    if (!cli_tool_exec::is_in_cwd(path)) {
        cli_tool_result r; r.tool_call_id = call.id;
        r.error = "Security: Cannot read files outside current directory"; r.exit_code = -1;
        return r;
    }
    
    auto result = cli_tool_exec::read_file_content(path);
    result.tool_call_id = call.id;
    return result;
}

cli_tool_result cli_tool_executor_impl::execute_touch_file(const cli_tool_call& call) {
    auto args = parse_args(call.arguments);
    std::string path = get_arg(args, "path");
    if (path.empty()) {
        cli_tool_result r; r.tool_call_id = call.id;
        r.error = "Missing required argument: path"; r.exit_code = -1;
        return r;
    }
    
    // Security check: prevent creating files outside CWD
    if (!cli_tool_exec::is_in_cwd(path)) {
        cli_tool_result r; r.tool_call_id = call.id;
        r.error = "Security: Cannot create files outside current directory"; r.exit_code = -1;
        return r;
    }

    cli_tool_result result;
    result.tool_call_id = call.id;
    try {
        bool exists = fs::exists(path);
        std::ofstream file(path, std::ios::app);
        if (!file) {
            result.error = "Cannot create file: " + path;
            result.exit_code = 1; result.success = false;
            return result;
        }
        file.close();
        result.content = exists ? "File touched: " + path : "File created: " + path;
        result.exit_code = 0; result.success = true;
    } catch (const std::exception& e) {
        result.error = std::string("Error: ") + e.what();
        result.exit_code = 1; result.success = false;
    }
    return result;
}

cli_tool_result cli_tool_executor_impl::execute_write_file(const cli_tool_call& call) {
    auto args = parse_args(call.arguments);
    std::string path    = get_arg(args, "path");
    std::string content = get_arg(args, "content", "");
    if (path.empty()) {
        cli_tool_result r; r.tool_call_id = call.id;
        r.error = "Missing required argument: path"; r.exit_code = -1;
        return r;
    }

    // Security check: prevent writing files outside CWD
    if (!cli_tool_exec::is_in_cwd(path)) {
        cli_tool_result r; r.tool_call_id = call.id;
        r.error = "Security: Cannot write files outside current directory"; r.exit_code = -1;
        return r;
    }

    auto result = cli_tool_exec::write_file_content(path, content);
    result.tool_call_id = call.id;
    return result;
}

cli_tool_result cli_tool_executor_impl::execute_start_file(const cli_tool_call& call) {
    auto args = parse_args(call.arguments);
    std::string path = get_arg(args, "path");
    
    if (path.empty()) {
        cli_tool_result r; r.tool_call_id = call.id;
        r.error = "Missing required argument: path"; r.exit_code = -1;
        return r;
    }

    // Security check
    if (!cli_tool_exec::is_in_cwd(path)) {
        cli_tool_result r; r.tool_call_id = call.id;
        r.error = "Security: Cannot create files outside current directory"; r.exit_code = -1;
        return r;
    }

    auto result = cli_tool_exec::start_file(path);
    result.tool_call_id = call.id;
    return result;
}

cli_tool_result cli_tool_executor_impl::execute_append_file(const cli_tool_call& call) {
    auto args = parse_args(call.arguments);
    std::string path = get_arg(args, "path");
    std::string content = get_arg(args, "content");
    
    if (path.empty()) {
        cli_tool_result r; r.tool_call_id = call.id;
        r.error = "Missing required argument: path"; r.exit_code = -1;
        return r;
    }

    // Security check
    if (!cli_tool_exec::is_in_cwd(path)) {
        cli_tool_result r; r.tool_call_id = call.id;
        r.error = "Security: Cannot modify files outside current directory"; r.exit_code = -1;
        return r;
    }

    auto result = cli_tool_exec::append_file(path, content);
    result.tool_call_id = call.id;
    return result;
}

cli_tool_result cli_tool_executor_impl::execute_finish_file(const cli_tool_call& call) {
    auto args = parse_args(call.arguments);
    std::string path = get_arg(args, "path");
    
    if (path.empty()) {
        cli_tool_result r; r.tool_call_id = call.id;
        r.error = "Missing required argument: path"; r.exit_code = -1;
        return r;
    }

    auto result = cli_tool_exec::finish_file(path);
    result.tool_call_id = call.id;
    return result;
}

cli_tool_result cli_tool_executor_impl::execute_search_replace(const cli_tool_call& call) {
    auto args = parse_args(call.arguments);
    std::string path = get_arg(args, "path");
    std::string search = get_arg(args, "search");
    std::string replace = get_arg(args, "replace");
    
    if (path.empty()) {
        cli_tool_result r; r.tool_call_id = call.id;
        r.error = "Missing required argument: path"; r.exit_code = -1;
        return r;
    }
    if (search.empty()) {
        cli_tool_result r; r.tool_call_id = call.id;
        r.error = "Missing required argument: search"; r.exit_code = -1;
        return r;
    }

    // Security check: prevent modifying files outside CWD
    if (!cli_tool_exec::is_in_cwd(path)) {
        cli_tool_result r; r.tool_call_id = call.id;
        r.error = "Security: Cannot modify files outside current directory"; r.exit_code = -1;
        return r;
    }

    auto result = cli_tool_exec::search_replace(path, search, replace);
    result.tool_call_id = call.id;
    return result;
}

cli_tool_result cli_tool_executor_impl::execute_list_dir(const cli_tool_call& call) {
    auto args = parse_args(call.arguments);
    std::string path = get_arg(args, "path", ".");
    auto result = cli_tool_exec::list_directory(path);
    result.tool_call_id = call.id;
    return result;
}

cli_tool_result cli_tool_executor_impl::execute_shell(const cli_tool_call& call) {
    auto args = parse_args(call.arguments);
    std::string command = get_arg(args, "command");
    if (command.empty()) {
        cli_tool_result r; r.tool_call_id = call.id;
        r.error = "Missing required argument: command"; r.exit_code = -1;
        return r;
    }
    auto violation = get_safety_violation(command);
    if (!violation.empty()) {
        cli_tool_result r; r.tool_call_id = call.id;
        r.error = "Security violation: " + violation; r.exit_code = -1;
        return r;
    }
    auto result = cli_tool_exec::run_command(command, config_.timeout_seconds, config_.max_output_length);
    result.tool_call_id = call.id;
    return result;
}

cli_tool_result cli_tool_executor_impl::execute_swift_build(const cli_tool_call& call) {
    auto args = parse_args(call.arguments);
    std::string config       = get_arg(args, "configuration", "debug");
    std::string package_path = get_arg(args, "package_path", "");

    // Use absolute path to swift (/usr/bin/swift on macOS)
    std::string cmd = "/usr/bin/swift build";
    if (config == "release") cmd += " -c release";
    if (!package_path.empty()) cmd += " --package-path " + shell_quote(package_path);

    // Swift build can produce lots of output - increase max_output_length
    // and use longer timeout for compilation
    auto result = cli_tool_exec::run_command(cmd, config_.timeout_seconds * 3, config_.max_output_length * 10);
    result.tool_call_id = call.id;
    return result;
}

cli_tool_result cli_tool_executor_impl::execute_swift_test(const cli_tool_call& call) {
    auto args = parse_args(call.arguments);
    std::string config       = get_arg(args, "configuration", "debug");
    std::string filter       = get_arg(args, "filter", "");
    std::string package_path = get_arg(args, "package_path", "");

    // Use absolute path to swift (/usr/bin/swift on macOS)
    std::string cmd = "/usr/bin/swift test";
    if (config == "release") cmd += " -c release";
    if (!filter.empty())       cmd += " --filter " + shell_quote(filter);
    if (!package_path.empty()) cmd += " --package-path " + shell_quote(package_path);

    // Swift tests can take long and produce lots of output
    auto result = cli_tool_exec::run_command(cmd, config_.timeout_seconds * 5, config_.max_output_length * 10);
    result.tool_call_id = call.id;
    return result;
}

cli_tool_result cli_tool_executor_impl::execute_swift_run(const cli_tool_call& call) {
    auto args        = parse_args(call.arguments);
    std::string exec = get_arg(args, "executable", "");
    std::string pkg  = get_arg(args, "package_path", ".");
    auto arguments   = args.value("arguments", std::vector<std::string>{});

    std::string cmd;

    // Check if executable is a .swift file (script) or a package executable
    if (!exec.empty() && exec.find(".swift") != std::string::npos) {
        // It's a Swift script file - run it directly
        // Use absolute path to swift (/usr/bin/swift on macOS)
        cmd = "/usr/bin/swift " + shell_quote(exec);
        for (const auto& arg : arguments) cmd += " " + shell_quote(arg);
    } else if (!exec.empty()) {
        // It's a package executable - use swift run
        cmd = "/usr/bin/swift run";
        cmd += " " + shell_quote(exec);
        for (const auto& arg : arguments) cmd += " " + shell_quote(arg);
        if (!pkg.empty()) cmd += " --package-path " + shell_quote(pkg);
    } else {
        // No executable specified - just swift run in package path
        cmd = "/usr/bin/swift run";
        if (!pkg.empty()) cmd += " --package-path " + shell_quote(pkg);
    }

    auto result = cli_tool_exec::run_command(cmd, config_.timeout_seconds, config_.max_output_length);
    result.tool_call_id = call.id;
    return result;
}

cli_tool_result cli_tool_executor_impl::execute_swift_package(const cli_tool_call& call) {
    auto args         = parse_args(call.arguments);
    std::string subcmd = get_arg(args, "command", "");
    auto arguments    = args.value("arguments", std::vector<std::string>{});

    if (subcmd.empty()) {
        cli_tool_result r; r.tool_call_id = call.id;
        r.error = "Missing required argument: command"; r.exit_code = -1;
        return r;
    }

    std::string cmd = "swift package " + shell_quote(subcmd);
    for (const auto& arg : arguments) cmd += " " + shell_quote(arg);  // quoted!

    auto result = cli_tool_exec::run_command(cmd, config_.timeout_seconds, config_.max_output_length);
    result.tool_call_id = call.id;
    return result;
}

cli_tool_result cli_tool_executor_impl::execute_swift_format(const cli_tool_call& call) {
    auto args    = parse_args(call.arguments);
    std::string path  = get_arg(args, "path", ".");
    bool in_place     = args.value("in_place", false);

    std::string cmd = "swift format";
    if (in_place) cmd += " --in-place";
    cmd += " " + shell_quote(path);

    auto result = cli_tool_exec::run_command(cmd, config_.timeout_seconds, config_.max_output_length);
    result.tool_call_id = call.id;
    return result;
}

// ============================================================================
// New editing tool implementations
// ============================================================================

cli_tool_result cli_tool_executor_impl::execute_insert_line(const cli_tool_call& call) {
    auto args = parse_args(call.arguments);
    std::string path = get_arg(args, "path");
    int line_number = args.value("line_number", -1);
    std::string content = get_arg(args, "content");

    if (path.empty()) {
        cli_tool_result r; r.tool_call_id = call.id;
        r.error = "Missing required argument: path"; r.exit_code = -1;
        return r;
    }
    if (line_number < 0) {
        cli_tool_result r; r.tool_call_id = call.id;
        r.error = "Missing or invalid required argument: line_number (must be >= 0)"; r.exit_code = -1;
        return r;
    }

    // Security check
    if (!cli_tool_exec::is_in_cwd(path)) {
        cli_tool_result r; r.tool_call_id = call.id;
        r.error = "Security: Cannot modify files outside current directory"; r.exit_code = -1;
        return r;
    }

    auto result = cli_tool_exec::insert_line_at(path, line_number, content);
    result.tool_call_id = call.id;
    return result;
}

cli_tool_result cli_tool_executor_impl::execute_replace_range(const cli_tool_call& call) {
    auto args = parse_args(call.arguments);
    std::string path = get_arg(args, "path");
    int start_line = args.value("start_line", -1);
    int end_line = args.value("end_line", -1);
    std::string content = get_arg(args, "content");

    if (path.empty()) {
        cli_tool_result r; r.tool_call_id = call.id;
        r.error = "Missing required argument: path"; r.exit_code = -1;
        return r;
    }
    if (start_line < 0 || end_line < 0) {
        cli_tool_result r; r.tool_call_id = call.id;
        r.error = "Missing or invalid required argument: start_line or end_line"; r.exit_code = -1;
        return r;
    }

    // Security check
    if (!cli_tool_exec::is_in_cwd(path)) {
        cli_tool_result r; r.tool_call_id = call.id;
        r.error = "Security: Cannot modify files outside current directory"; r.exit_code = -1;
        return r;
    }

    auto result = cli_tool_exec::replace_range(path, start_line, end_line, content);
    result.tool_call_id = call.id;
    return result;
}

cli_tool_result cli_tool_executor_impl::execute_delete_lines(const cli_tool_call& call) {
    auto args = parse_args(call.arguments);
    std::string path = get_arg(args, "path");
    int start_line = args.value("start_line", -1);
    int end_line = args.value("end_line", -1);

    if (path.empty()) {
        cli_tool_result r; r.tool_call_id = call.id;
        r.error = "Missing required argument: path"; r.exit_code = -1;
        return r;
    }
    if (start_line < 0 || end_line < 0) {
        cli_tool_result r; r.tool_call_id = call.id;
        r.error = "Missing or invalid required argument: start_line or end_line"; r.exit_code = -1;
        return r;
    }

    // Security check
    if (!cli_tool_exec::is_in_cwd(path)) {
        cli_tool_result r; r.tool_call_id = call.id;
        r.error = "Security: Cannot modify files outside current directory"; r.exit_code = -1;
        return r;
    }

    auto result = cli_tool_exec::delete_lines(path, start_line, end_line);
    result.tool_call_id = call.id;
    return result;
}

cli_tool_result cli_tool_executor_impl::execute_get_file_info(const cli_tool_call& call) {
    auto args = parse_args(call.arguments);
    std::string path = get_arg(args, "path");
    if (path.empty()) {
        cli_tool_result r; r.tool_call_id = call.id;
        r.error = "Missing required argument: path"; r.exit_code = -1;
        return r;
    }
    auto result = cli_tool_exec::get_file_info(path);
    result.tool_call_id = call.id;
    return result;
}

cli_tool_result cli_tool_executor_impl::execute_verify_file(const cli_tool_call& call) {
    auto args = parse_args(call.arguments);
    std::string path          = get_arg(args, "path");
    std::string expected_hash = get_arg(args, "expected_hash");
    if (path.empty() || expected_hash.empty()) {
        cli_tool_result r; r.tool_call_id = call.id;
        r.error = "Missing required argument: path or expected_hash"; r.exit_code = -1;
        return r;
    }
    auto result = cli_tool_exec::verify_file(path, expected_hash);
    result.tool_call_id = call.id;
    return result;
}

// Factory
std::unique_ptr<cli_tool_executor> create_tool_executor(const cli_tool_security_config& config) {
    return std::make_unique<cli_tool_executor_impl>(config);
}
