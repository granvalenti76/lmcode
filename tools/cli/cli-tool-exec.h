#pragma once

#include "cli-tool.h"

#include <string>
#include <vector>
#include <memory>
#include <set>

// Security configuration for tool execution
struct cli_tool_security_config {
    // Shell command whitelist (regex patterns)
    // Note: shell is ONLY for system commands (git, swift, xcodebuild, etc.)
    // File writing must use write_file tool (native C++), NOT shell redirects
    std::vector<std::string> shell_whitelist = {
        "^ls\\s.*", "^ls$",  // ls with args or just ls
        "^grep\\s.*", "^grep$",
        "^find\\s.*", "^find$",
        "^swift\\s.*", "^swift$",  // swift and all subcommands
        "^xcrun\\s.*", "^xcrun$",
        "^xcodebuild\\s.*", "^xcodebuild$",
        "^git\\s.*", "^git$",
        "^pwd$", "^whoami$", "^uname$", "^date$",
        "^wc\\s.*", "^head\\s.*", "^tail\\s.*",
        "^which\\s.*", "^which$",  // which command to find executables
        "^\\./[a-zA-Z0-9_./-]+\\s.*", "^\\./[a-zA-Z0-9_./-]+$"  // local scripts: ./script.sh
    };

    // Shell command blacklist (always blocked)
    // CRITICAL: Block all shell redirections and heredocs
    // File writing must use write_file tool, NOT shell redirects
    std::vector<std::string> shell_blacklist = {
        "rm\\s+-rf", "sudo", "su\\s", "chmod\\s+777",
        "dd\\s+", "mkfs", "mount\\s", "umount\\s",
        "curl.*\\|.*sh", "wget.*\\|.*sh",
        ":(){:|:&};:", "fork bomb",
        ">", ">>",  // Block output redirection (use write_file instead)
        "<<",  // Block heredoc (use write_file instead)
        "\\|",  // Block pipes (use separate commands)
        "`", "$(", "${",  // Block command substitution
        "<(", "<"  // Block process substitution and input redirection
    };

    // Max output length (truncate longer outputs)
    size_t max_output_length = 4096;

    // Execution timeout in seconds
    int timeout_seconds = 30;

    // Require confirmation for file writes outside current directory
    bool restrict_to_cwd = true;

    // Path to snippet library directory (relative to CWD)
    std::string snippets_dir = "snippets";
};

// Concrete tool executor implementation
class cli_tool_executor_impl : public cli_tool_executor {
public:
    cli_tool_executor_impl(const cli_tool_security_config& config = {});

    cli_tool_result execute(const cli_tool_call& call, bool dry_run = false) override;
    bool requires_confirmation(const cli_tool_call& call) const override;

    // Check if a command is safe to execute
    bool is_command_safe(const std::string& cmd) const;
    std::string get_safety_violation(const std::string& cmd) const;

private:
    cli_tool_security_config config_;

    // Track files in active chunked-write sessions (start_file → append_file → finish_file)
    // Prevents append_file from writing to arbitrary existing files
    std::set<std::string> active_write_sessions_;

    // Tool-specific executors
    cli_tool_result execute_read_file(const cli_tool_call& call);
    cli_tool_result execute_touch_file(const cli_tool_call& call);
    cli_tool_result execute_write_file(const cli_tool_call& call);
    cli_tool_result execute_start_file(const cli_tool_call& call);
    cli_tool_result execute_append_file(const cli_tool_call& call);
    cli_tool_result execute_finish_file(const cli_tool_call& call);
    cli_tool_result execute_get_file_info(const cli_tool_call& call);
    cli_tool_result execute_verify_file(const cli_tool_call& call);
    cli_tool_result execute_search_replace(const cli_tool_call& call);
    cli_tool_result execute_get_line_numbers(const cli_tool_call& call);
    cli_tool_result execute_search_regex(const cli_tool_call& call);
    cli_tool_result execute_list_dir(const cli_tool_call& call);
    cli_tool_result execute_shell(const cli_tool_call& call);
    cli_tool_result execute_insert_line(const cli_tool_call& call);
    cli_tool_result execute_replace_range(const cli_tool_call& call);
    cli_tool_result execute_delete_lines(const cli_tool_call& call);
    // File search tools (from upstream llama.cpp)
    cli_tool_result execute_file_glob_search(const cli_tool_call& call);
    cli_tool_result execute_grep_search(const cli_tool_call& call);
    // Swift tools
    cli_tool_result execute_swift_build(const cli_tool_call& call);
    cli_tool_result execute_swift_test(const cli_tool_call& call);
    cli_tool_result execute_swift_run(const cli_tool_call& call);
    cli_tool_result execute_swift_package(const cli_tool_call& call);
    cli_tool_result execute_swift_format(const cli_tool_call& call);
    // Snippet library tools
    cli_tool_result execute_search_snippets(const cli_tool_call& call);
    cli_tool_result execute_load_snippet(const cli_tool_call& call);
};

// Helper functions
namespace cli_tool_exec {
    // Execute command and capture output
    cli_tool_result run_command(
        const std::string& cmd,
        int timeout_seconds,
        size_t max_output_length);

    // Read file content
    cli_tool_result read_file_content(const std::string& path);

    // Write file content
    cli_tool_result write_file_content(const std::string& path, const std::string& content);

    // Start writing a large file (creates/truncates)
    cli_tool_result start_file(const std::string& path);

    // Append content to a file (chunked writing)
    cli_tool_result append_file(const std::string& path, const std::string& content);

    // Finish writing a file (confirms completion)
    cli_tool_result finish_file(const std::string& path);

    // Get file info (size, line count, hash)
    cli_tool_result get_file_info(const std::string& path);

    // Verify file integrity by comparing hash
    cli_tool_result verify_file(const std::string& path, const std::string& expected_hash);

    // Search and replace text in a file
    cli_tool_result search_replace(const std::string& path, const std::string& search, const std::string& replace);

    // Get line numbers for a file (formatted with line numbers)
    cli_tool_result get_line_numbers(const std::string& path);

    // Search and replace using regex patterns
    cli_tool_result search_regex(const std::string& path, const std::string& pattern, const std::string& replace);

    // List directory
    cli_tool_result list_directory(const std::string& path);

    // Check if file exists
    bool file_exists(const std::string& path);

    // Check if path is within current working directory
    bool is_in_cwd(const std::string& path);

    // Insert a line at a specific position in a file
    cli_tool_result insert_line_at(const std::string& path, int line_number, const std::string& content);

    // Replace a range of lines with new content
    cli_tool_result replace_range(const std::string& path, int start_line, int end_line, const std::string& content);

    // Delete a range of lines from a file
    cli_tool_result delete_lines(const std::string& path, int start_line, int end_line);

    // --- File search tools (from upstream llama.cpp) ---

    // Search for files matching a glob pattern
    // base: base directory to search in
    // include: glob pattern (e.g. "**/*.cpp"), default "**"
    // exclude: glob pattern to exclude, default ""
    // max_results: maximum number of results (default 100)
    cli_tool_result file_glob_search(const std::string& base,
                                      const std::string& include,
                                      const std::string& exclude,
                                      size_t max_results = 100);

    // Search for a regex pattern in files
    // path: file or directory to search in
    // pattern: regex pattern
    // include: glob pattern to filter files (default "**")
    // exclude: glob pattern to exclude files
    // return_line_numbers: if true, include line numbers
    // max_results: maximum number of matches (default 100)
    cli_tool_result grep_search(const std::string& path,
                                 const std::string& pattern,
                                 const std::string& include,
                                 const std::string& exclude,
                                 bool return_line_numbers,
                                 size_t max_results = 100);

    // --- Snippet helpers ---

    // Search snippet library: returns matching entries (name + description from first line)
    // snippets_dir: path to the snippets directory (default: "./snippets")
    // query: substring matched case-insensitively against filename (stem) and first-line tag
    // Empty query returns all snippets.
    cli_tool_result search_snippets(const std::string& snippets_dir, const std::string& query);

    // Load snippet: copy snippets_dir/<snippet_name> → dest_path
    // dest_path must be inside CWD.
    // Fails if snippet does not exist; prompts overwrite warning if dest_path already exists.
    cli_tool_result load_snippet(const std::string& snippets_dir,
                                 const std::string& snippet_name,
                                 const std::string& dest_path);
}

// Factory function
std::unique_ptr<cli_tool_executor> create_tool_executor(const cli_tool_security_config& config);
