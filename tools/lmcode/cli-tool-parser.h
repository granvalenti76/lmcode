#pragma once

#include "cli-tool.h"  // Includes chat.h upstream

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
// Uses upstream common_chat_parse() for robust parsing
std::vector<cli_tool_call> parse_tool_calls(
    const std::string& content,
    const std::string& reasoning_content = "");

// Check if content contains tool calls
bool has_tool_calls(const std::string& content);

// Generate tool result message for model
std::string format_tool_result(const cli_tool_result& result);

// Generate system message with tool definitions
std::string format_tool_system_message(const std::vector<cli_tool>& tools);

}  // namespace cli_tool_parser
