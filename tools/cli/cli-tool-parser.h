#pragma once

#include "cli-tool.h"

#include <string>
#include <vector>
#include <atomic>

// Global debug mode flag for tool parser
extern std::atomic<bool> g_tool_parser_debug;

// Parse tool calls from model output
namespace cli_tool_parser {

// Global counter for unique tool call IDs
extern std::atomic<int> g_tool_call_counter;

// Generate unique tool call ID
inline std::string generate_tool_call_id() {
    return "call_" + std::to_string(++g_tool_call_counter);
}

// Extract tool calls from model response
// Returns empty vector if no tool calls found
std::vector<cli_tool_call> parse_tool_calls(
    const std::string& content,
    const std::string& reasoning_content = "");

// Check if content contains tool calls
bool has_tool_calls(const std::string& content);

// Extract tool call from JSON format
cli_tool_call parse_json_tool_call(const std::string& json_str);

// Generate tool result message for model
std::string format_tool_result(const cli_tool_result& result);

// Generate system message with tool definitions
std::string format_tool_system_message(const std::vector<cli_tool>& tools);

}  // namespace cli_tool_parser
