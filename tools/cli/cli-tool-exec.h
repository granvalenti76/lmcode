#pragma once

#include "cli-tool.h"

#include <string>
#include <vector>
#include <memory>

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
    cli_tool_result execute_list_dir(const cli_tool_call& call);
    cli_tool_result execute_shell(const cli_tool_call& call);
    cli_tool_result execute_insert_line(const cli_tool_call& call);
    cli_tool_result execute_replace_range(const cli_tool_call& call);
    cli_tool_result execute_delete_lines(const cli_tool_call& call);
    cli_tool_result execute_swift_build(const cli_tool_call& call);
    cli_tool_result execute_swift_test(const cli_tool_call& call);
    cli_tool_result execute_swift_run(const cli_tool_call& call);
    cli_tool_result execute_swift_package(const cli_tool_call& call);
    cli_tool_result execute_swift_format(const cli_tool_call& call);
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
}

// Factory function
std::unique_ptr<cli_tool_executor> create_tool_executor(const cli_tool_security_config& config);
