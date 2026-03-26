#include "cli-tool-parser.h"
#include "console.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>

// Global counter for unique tool call IDs
namespace cli_tool_parser {
std::atomic<int> g_tool_call_counter{0};
}

// Helper: sanitize string for JSON (escape special chars + replace invalid UTF-8)
static std::string sanitize_for_json(const std::string& input) {
    std::string result;
    result.reserve(input.size() * 2);  // May expand due to escaping

    size_t i = 0;
    while (i < input.size()) {
        unsigned char c = input[i];

        // Escape special JSON characters
        switch (c) {
            case '"':  result += "\\\""; i++; continue;
            case '\\': result += "\\\\"; i++; continue;
            case '\b': result += "\\b"; i++; continue;
            case '\f': result += "\\f"; i++; continue;
            case '\n': result += "\\n"; i++; continue;
            case '\r': result += "\\r"; i++; continue;
            case '\t': result += "\\t"; i++; continue;
        }

        // ASCII - pass through
        if (c < 0x80) { result += c; i++; continue; }

        // Multi-byte UTF-8 validation
        int expected_bytes = 0;
        if ((c & 0xE0) == 0xC0) expected_bytes = 2;
        else if ((c & 0xF0) == 0xE0) expected_bytes = 3;
        else if ((c & 0xF8) == 0xF0) expected_bytes = 4;
        else { result += "\xEF\xBF\xBD"; i++; continue; }

        if (i + expected_bytes > input.size()) {
            result += "\xEF\xBF\xBD";
            break;
        }

        bool valid = true;
        for (int j = 1; j < expected_bytes; j++) {
            if ((input[i + j] & 0xC0) != 0x80) { valid = false; break; }
        }

        if (valid) {
            for (int j = 0; j < expected_bytes; j++) result += input[i + j];
            i += expected_bytes;
        } else {
            result += "\xEF\xBF\xBD";
            i++;
        }
    }
    return result;
}

// Helper: sanitize raw newlines/quotes inside JSON string values (state-machine parser)
// This fixes the common LLM mistake of writing unescaped newlines in JSON strings
static std::string sanitize_json_string_values(const std::string& json_str) {
    std::string result;
    result.reserve(json_str.size() * 2);
    
    bool in_string = false;
    bool escape_next = false;
    
    for (size_t i = 0; i < json_str.size(); i++) {
        char c = json_str[i];
        
        if (escape_next) {
            result += c;
            escape_next = false;
            continue;
        }
        
        if (c == '\\' && in_string) {
            result += c;
            escape_next = true;
            continue;
        }
        
        if (c == '"') {
            in_string = !in_string;
            result += c;
            continue;
        }
        
        // Inside a string value, escape raw newlines, carriage returns, and tabs
        if (in_string) {
            if (c == '\n') {
                result += "\\n";
                continue;
            }
            if (c == '\r') {
                result += "\\r";
                continue;
            }
            if (c == '\t') {
                result += "\\t";
                continue;
            }
            // Note: unescaped '"' inside a string is handled above at the
            // `if (c == '"') { in_string = !in_string; ... }` branch — it flips
            // the string state but is already emitted there.  Do NOT add a
            // duplicate handler here; it is unreachable and was a latent bug.
        }
        
        result += c;
    }
    
    return result;
}

// Helper: clean invisible/special UTF-8 characters that LLMs sometimes generate
// Common issues: non-breaking space (\xC2\xA0), zero-width spaces, etc.
static std::string clean_json_string(const std::string& str) {
    std::string result = str;
    
    // Replace non-breaking space (UTF-8: \xC2\xA0) with regular space
    size_t pos;
    while ((pos = result.find("\xC2\xA0")) != std::string::npos) {
        result.replace(pos, 2, " ");
    }
    
    // Replace zero-width space (UTF-8: \xE2\x80\x8B) with nothing
    while ((pos = result.find("\xE2\x80\x8B")) != std::string::npos) {
        result.erase(pos, 3);
    }
    
    // Replace zero-width non-joiner (UTF-8: \xE2\x80\x8C) with nothing
    while ((pos = result.find("\xE2\x80\x8C")) != std::string::npos) {
        result.erase(pos, 3);
    }
    
    // Replace byte order mark (UTF-8: \xEF\xBB\xBF) with nothing
    while ((pos = result.find("\xEF\xBB\xBF")) != std::string::npos) {
        result.erase(pos, 3);
    }
    
    return result;
}

namespace cli_tool_parser {

static std::string trim(const std::string& str) {
    size_t start = str.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    size_t end = str.find_last_not_of(" \t\n\r");
    return str.substr(start, end - start + 1);
}

// Generic balanced-delimiter extractor: works for both {..} objects and [..] arrays
static std::string extract_json_value(const std::string& str, size_t start_pos,
                                      char open_ch, char close_ch) {
    if (start_pos >= str.size() || str[start_pos] != open_ch) return "";

    int depth = 0;
    bool in_string = false;
    bool escape_next = false;

    for (size_t i = start_pos; i < str.size(); ++i) {
        char c = str[i];
        if (escape_next)              { escape_next = false; continue; }
        if (c == '\\' && in_string)  { escape_next = true;  continue; }
        if (c == '"')                 { in_string = !in_string; continue; }
        if (!in_string) {
            if      (c == open_ch)  { depth++; }
            else if (c == close_ch) { depth--; if (depth == 0) return str.substr(start_pos, i - start_pos + 1); }
        }
    }
    return "";  // Unbalanced
}

static std::string extract_json_object(const std::string& str, size_t pos) {
    return extract_json_value(str, pos, '{', '}');
}

// Helper: safely extract arguments from JSON, handling both string and object types
static std::string safe_extract_arguments(const json& obj) {
    if (!obj.contains("arguments")) {
        return "{}";
    }
    const auto& args = obj["arguments"];
    if (args.is_string()) {
        // LLM sometimes JSON-encodes the arguments string (common mistake)
        return args.get<std::string>();
    } else if (args.is_object()) {
        return args.dump();
    } else {
        return "{}";
    }
}

static std::string extract_json_array(const std::string& str, size_t pos) {
    return extract_json_value(str, pos, '[', ']');
}

std::vector<cli_tool_call> parse_tool_calls(
    const std::string& content,
    const std::string& /*reasoning_content*/)
{
    std::vector<cli_tool_call> result;

    // Note: reasoning_content is not used - we parse only from content

    // -------------------------------------------------------------------------
    // Pattern 1: JSON array  [{"name":..., "arguments":...}, ...]
    // -------------------------------------------------------------------------
    {
        // Use regex to find arrays with tool calls - tolerant to whitespace/newlines
        std::regex array_start_regex(R"(\[\s*\{\s*"name"\s*:)");
        std::smatch match;
        std::string::const_iterator search_start(content.cbegin());

        while (std::regex_search(search_start, content.cend(), match, array_start_regex)) {
            size_t pos = std::distance(content.cbegin(), search_start) + match.position();
            std::string arr_str = extract_json_array(content, pos);
            
            if (!arr_str.empty()) {
                try {
                    // Clean invisible characters before parsing
                    std::string cleaned = clean_json_string(arr_str);
                    auto arr = json::parse(cleaned);
                    if (arr.is_array() && !arr.empty()) {
                        bool any_tool = false;
                        for (const auto& item : arr) {
                            if (item.contains("name") && item.contains("arguments")) {
                                any_tool = true;
                                cli_tool_call call;
                                call.name      = item.value("name", "");
                                call.arguments = safe_extract_arguments(item);
                                call.id        = item.value("id", item.value("tool_call_id", ""));
                                if (call.id.empty()) {
                                    call.id = generate_tool_call_id();
                                }
                                result.push_back(call);
                            }
                        }
                        if (any_tool) break;  // Found a tool array, stop
                        result.clear();  // Array had no tools, keep searching
                    }
                } catch (const std::exception& e) {
                    // Try to sanitize and re-parse (handles raw newlines in strings)
                    try {
                        std::string sanitized = sanitize_json_string_values(arr_str);
                        auto arr = json::parse(sanitized);
                        if (arr.is_array() && !arr.empty()) {
                            bool any_tool = false;
                            for (const auto& item : arr) {
                                if (item.contains("name") && item.contains("arguments")) {
                                    any_tool = true;
                                    cli_tool_call call;
                                    call.name      = item.value("name", "");
                                    call.arguments = safe_extract_arguments(item);
                                    call.id        = item.value("id", item.value("tool_call_id", ""));
                                    if (call.id.empty()) {
                                        call.id = generate_tool_call_id();
                                    }
                                    result.push_back(call);
                                }
                            }
                            if (any_tool) break;
                            result.clear();
                        }
                    } catch (const std::exception& e2) {
                        // Sanitization failed - create an error tool call to inform the model
                        if (g_tool_parser_debug) {
                            console::log("\033[90m[Parser debug: JSON array parse error (even after sanitization): %s]\033[0m\n", e2.what());
                        }
                        cli_tool_call error_call;
                        error_call.name = "SYNTAX_ERROR";
                        error_call.id = generate_tool_call_id();
                        error_call.arguments = "{\"error\": \"Invalid JSON in tool call array: " + std::string(e2.what()) + ". Make sure to escape newlines as \\\\n and quotes as \\\\\\\" inside strings.\"}";
                        result.push_back(error_call);
                    }
                }
                // Advance past this array
                search_start = content.cbegin() + pos + arr_str.size();
            } else {
                search_start = match.suffix().first;
            }
        }
    }

    // -------------------------------------------------------------------------
    // Pattern 2: Individual JSON objects  {"name":..., "arguments":...}
    // -------------------------------------------------------------------------
    if (result.empty()) {
        // Use regex to find objects - tolerant to whitespace/newlines
        std::regex obj_start_regex(R"(\{\s*"name"\s*:)");
        std::smatch match;
        std::string::const_iterator search_start(content.cbegin());

        while (std::regex_search(search_start, content.cend(), match, obj_start_regex)) {
            size_t pos = std::distance(content.cbegin(), search_start) + match.position();
            std::string extracted = extract_json_object(content, pos);
            
            if (!extracted.empty()) {
                try {
                    // Clean invisible characters before parsing
                    std::string cleaned = clean_json_string(extracted);
                    auto obj = json::parse(cleaned);
                    if (obj.contains("name") && obj.contains("arguments")) {
                        cli_tool_call call;
                        call.name      = obj.value("name", "");
                        call.arguments = safe_extract_arguments(obj);
                        call.id        = obj.value("id", obj.value("tool_call_id", ""));
                        if (call.id.empty()) {
                            call.id = generate_tool_call_id();
                        }
                        result.push_back(call);
                    }
                } catch (const std::exception& e) {
                    // Try to sanitize and re-parse (handles raw newlines in strings)
                    try {
                        std::string sanitized = sanitize_json_string_values(extracted);
                        auto obj = json::parse(sanitized);
                        if (obj.contains("name") && obj.contains("arguments")) {
                            cli_tool_call call;
                            call.name      = obj.value("name", "");
                            call.arguments = safe_extract_arguments(obj);
                            call.id        = obj.value("id", obj.value("tool_call_id", ""));
                            if (call.id.empty()) {
                                call.id = generate_tool_call_id();
                            }
                            result.push_back(call);
                        }
                    } catch (const std::exception& e2) {
                        // Sanitization failed - create an error tool call to inform the model
                        if (g_tool_parser_debug) {
                            console::log("\033[90m[Parser debug: JSON object parse error (even after sanitization): %s]\033[0m\n", e2.what());
                        }
                        cli_tool_call error_call;
                        error_call.name = "SYNTAX_ERROR";
                        error_call.id = generate_tool_call_id();
                        error_call.arguments = "{\"error\": \"Invalid JSON in tool call: " + std::string(e2.what()) + ". Make sure to escape newlines as \\\\n and quotes as \\\\\\\" inside strings.\"}";
                        result.push_back(error_call);
                    }
                }
                // Always advance past the full object, not just +1
                search_start = content.cbegin() + pos + extracted.size();
            } else {
                // Unbalanced braces at this position, advance
                search_start = match.suffix().first;
            }
        }
    }

    // -------------------------------------------------------------------------
    // Pattern 3: Markdown fenced block  ```tool_call\n{...}\n```
    // -------------------------------------------------------------------------
    if (result.empty()) {
        // More robust: match any JSON-like content between ```tool_call and ```
        std::regex tool_block_regex(R"(```tool_call\s*\n(\{[^`]+?\})\s*\n```)");
        std::smatch match;
        std::string::const_iterator search_start(content.cbegin());

        while (std::regex_search(search_start, content.cend(), match, tool_block_regex)) {
            try {
                // Clean invisible characters before parsing
                std::string cleaned = clean_json_string(match[1].str());
                auto obj = json::parse(cleaned);
                if (obj.contains("name") && obj.contains("arguments")) {
                    cli_tool_call call;
                    call.name      = obj.value("name", "");
                    call.arguments = safe_extract_arguments(obj);
                    call.id        = obj.value("id", "");
                    if (call.id.empty()) {
                        call.id = generate_tool_call_id();
                    }
                    result.push_back(call);
                }
            } catch (const std::exception& e) {
                // Try to sanitize and re-parse (handles raw newlines in strings)
                try {
                    std::string sanitized = sanitize_json_string_values(match[1].str());
                    auto obj = json::parse(sanitized);
                    if (obj.contains("name") && obj.contains("arguments")) {
                        cli_tool_call call;
                        call.name      = obj.value("name", "");
                        call.arguments = safe_extract_arguments(obj);
                        call.id        = obj.value("id", "");
                        if (call.id.empty()) {
                            call.id = generate_tool_call_id();
                        }
                        result.push_back(call);
                    }
                } catch (const std::exception& e2) {
                    // Sanitization failed - create an error tool call to inform the model
                    if (g_tool_parser_debug) {
                        console::log("\033[90m[Parser debug: Markdown block parse error (even after sanitization): %s]\033[0m\n", e2.what());
                    }
                    cli_tool_call error_call;
                    error_call.name = "SYNTAX_ERROR";
                    error_call.id = generate_tool_call_id();
                    error_call.arguments = "{\"error\": \"Invalid JSON in markdown tool call block: " + std::string(e2.what()) + ". Make sure to escape newlines as \\\\n and quotes as \\\\\\\" inside strings.\"}";
                    result.push_back(error_call);
                }
            }
            search_start = match.suffix().first;
        }
    }

    if (!result.empty() && g_tool_parser_debug)
        console::log("\033[90m[Parser debug: Found %zu tool calls]\033[0m\n", result.size());

    return result;
}

bool has_tool_calls(const std::string& content) {
    return !parse_tool_calls(content).empty();
}

cli_tool_call parse_json_tool_call(const std::string& json_str) {
    cli_tool_call result;
    try {
        auto obj    = json::parse(json_str);
        result.name = obj.value("name", "");
        result.arguments = safe_extract_arguments(obj);
        result.id   = obj.value("id", obj.value("tool_call_id", ""));
        if (result.id.empty()) {
            result.id = generate_tool_call_id();
        }
    } catch (...) {}
    return result;
}

std::string format_tool_result(const cli_tool_result& result) {
    // Return plain text - nlohmann::json will handle escaping when serializing
    std::ostringstream oss;
    oss << "Tool: " << result.tool_call_id << "\n";
    if (result.success) {
        oss << "Status: Success\n";
    } else {
        oss << "Status: Failed (exit code: " << result.exit_code << ")\n";
    }
    if (!result.error.empty())   oss << "Error: "  << result.error   << "\n";
    if (!result.content.empty()) oss << "Output:\n" << result.content << "\n";
    return oss.str();
}

std::string format_tool_system_message(const std::vector<cli_tool>& tools) {
    std::ostringstream oss;

    // --- Tool list ---
    oss << "You are a helpful assistant with access to tools for file and shell operations.\n\n";
    oss << "## Available Tools\n\n";
    for (const auto& tool : tools) {
        // Pretty-print parameters: parse JSON schema and show required fields clearly
        std::string param_summary;
        try {
            auto schema = json::parse(tool.parameters);
            if (schema.contains("properties")) {
                std::vector<std::string> required_fields;
                std::vector<std::string> optional_fields;
                auto props = schema["properties"];
                std::vector<std::string> req_names;
                if (schema.contains("required")) {
                    for (const auto& r : schema["required"]) req_names.push_back(r.get<std::string>());
                }
                for (auto it = props.begin(); it != props.end(); ++it) {
                    std::string desc = it.value().value("description", "");
                    std::string type = it.value().value("type", "string");
                    std::string entry = it.key() + " (" + type + ")" + (desc.empty() ? "" : ": " + desc);
                    bool is_req = std::find(req_names.begin(), req_names.end(), it.key()) != req_names.end();
                    if (is_req) required_fields.push_back("  - " + entry);
                    else        optional_fields.push_back("  - [optional] " + entry);
                }
                for (auto& f : required_fields) param_summary += f + "\n";
                for (auto& f : optional_fields) param_summary += f + "\n";
            }
        } catch (...) {
            param_summary = "  " + tool.parameters + "\n";
        }

        oss << "### " << tool.name << "\n";
        oss << tool.description << "\n";
        if (!param_summary.empty()) oss << param_summary;
        oss << "\n";
    }

    // --- How to call tools ---
    oss << "## How to Use Tools\n\n";
    oss << "When you need to use a tool, output a JSON object on its own line:\n\n";
    oss << R"({"name": "tool_name", "arguments": {"param": "value"}})" << "\n\n";
    oss << "The tool will execute and you will receive the result. Then respond normally in plain text.\n\n";

    // --- Shell tool usage ---
    oss << "## Using the Shell Tool\n\n";
    oss << "For the `shell` tool, specify ONLY the command to run. The system automatically wraps it in a shell.\n\n";
    oss << "**CORRECT:** `\"swift rss_fetcher.swift\"`\n";
    oss << "**WRONG:** `\"/bin/sh -c 'swift rss_fetcher.swift' 2>&1\"` (redundant - system already does this)\n\n";
    oss << "Examples:\n";
    oss << "- Run Swift script: `\"swift script.swift\"`\n";
    oss << "- Run command: `\"ls -la\"`\n";
    oss << "- Build: `\"swift build -c release\"`\n";
    oss << "- Git: `\"git status\"`\n\n";
    oss << "**IMPORTANT:** The shell tool does NOT support redirections (>, >>, <<) or pipes (|). Use the `write_file` tool for creating files.\n\n";

    // --- Writing files ---
    oss << "## Writing Files\n\n";
    oss << "### Small Files (<20 lines)\n";
    oss << "Use `write_file` ONLY for very small files (configs, short notes, etc.):\n\n";
    oss << "```json\n";
    oss << R"({"name": "write_file", "arguments": {"path": "config.txt", "content": "setting1=value1\nsetting2=value2"}})" << "\n";
    oss << "```\n\n";
    oss << "**WARNING:** Do NOT use `write_file` for code files or anything longer than a few lines.\n\n";

    oss << "### Medium/Large Files (20+ lines)\n";
    oss << "ALWAYS use chunked writing with `start_file` + `append_file` + `finish_file`.\n\n";
    oss << "**Chunk size:** 20-50 lines per `append_file` call (~1-2 KB). Hard limits: 200 lines or 8 KB per call.\n";
    oss << "Separate lines inside the `content` string with `\\n` (escaped newline).\n\n";
    oss << "**Step 1:** Create/truncate the file\n";
    oss << "```json\n";
    oss << R"({"name": "start_file", "arguments": {"path": "main.py"}})" << "\n";
    oss << "```\n\n";
    oss << "**Step 2:** Append chunks of 20-50 lines at a time\n\n";
    oss << "```json\n";
    oss << R"({"name": "append_file", "arguments": {"path": "main.py", "content": "import os\nimport sys\n\ndef main():\n    pass\n"}})" << "\n";
    oss << "```\n\n";
    oss << "⚠️ **CRITICAL rules for `append_file`:**\n";
    oss << "- Use `\\n` (escaped) to separate lines inside the JSON string — never raw newlines.\n";
    oss << "- Keep each chunk to 20-50 lines. Hard limits: 200 lines / 8 KB per call.\n";
    oss << "- Do NOT try to write an entire large file in a single `append_file` — split it into chunks.\n";
    oss << "- Do NOT use `write_file` for files longer than 5 lines. Always use the chunk chain.\n\n";
    oss << "**✓ Response includes hash:** `Appended N bytes (M newlines) to path | chunk_hash=... | file_size=...`\n\n";
    oss << "**Step 3:** Finish (verifies file integrity)\n";
    oss << "```json\n";
    oss << R"({"name": "finish_file", "arguments": {"path": "main.py"}})" << "\n";
    oss << "```\n\n";
    oss << "**✓ Response includes total hash:** `✓ File complete: main.py (50 lines, 1234 bytes, hash: 9876543210)`\n\n";
    oss << "**Example: Writing a 6-line file in two chunks**\n";
    oss << "```\n";
    oss << "1. start_file(\"app.py\")\n";
    oss << "   → \"File created (ready for append): app.py\"\n";
    oss << "2. append_file(\"app.py\", \"import os\\nimport sys\\n\")\n";
    oss << "   → \"Appended 18 bytes (2 newlines) | chunk_hash=123456 | file_size=18\"\n";
    oss << "3. append_file(\"app.py\", \"\\ndef main():\\n    pass\\n\")\n";
    oss << "   → \"Appended 20 bytes (3 newlines) | chunk_hash=789012 | file_size=38\"\n";
    oss << "4. finish_file(\"app.py\")\n";
    oss << "   → \"✓ File complete: app.py (6 lines, 38 bytes, hash: 999999)\"\n";
    oss << "```\n\n";
    oss << "**❌ WRONG:** `{\"content\": \"line1\\nline2\\n ... 300 lines ...\"}` — chunk too large, will be REJECTED.\n";
    oss << "**✓ CORRECT:** Multiple `append_file` calls of 20-50 lines each.\n\n";
    oss << "**💡 Tip:** Each `append_file` response includes a hash. Use `verify_file` after `finish_file` for extra confidence.\n";

    // --- Editing files with search_replace ---
    oss << "## Editing Existing Files\n\n";
    oss << "Use `search_replace` to modify existing files. This is more robust than line numbers.\n\n";
    oss << "**How it works:** Provide a unique block of text to search for, and the replacement text.\n";
    oss << "The search text must match EXACTLY (including whitespace and newlines).\n\n";
    oss << "Example:\n";
    oss << "```json\n";
    oss << R"({"name": "search_replace", "arguments": {"path": "main.cpp", "search": "int x = 0;", "replace": "int x = 42; // updated"}})" << "\n";
    oss << "```\n\n";
    oss << "**Tip:** Include enough context in the search string to make it unique (3-5 lines usually works).\n\n";

    // --- Concrete examples with realistic values ---
    oss << "## Examples\n\n";
    oss << "User: What files are here?\n";
    oss << R"({"name": "list_dir", "arguments": {"path": "."}})" << "\n";
    oss << "[tool result: Contents of .:\n  [FILE] file1.swift\n  [FILE] file2.swift]\n";
    oss << "Assistant: The directory contains file1.swift and file2.swift.\n\n";

    oss << "User: Create a file called hello.txt with content 'Hello'\n";
    oss << R"({"name": "write_file", "arguments": {"path": "hello.txt", "content": "Hello"}})" << "\n";
    oss << "[tool result: File created: hello.txt]\n";
    oss << "Assistant: Done, hello.txt has been created.\n\n";

    oss << "User: Create a Swift file with 100 lines of code\n";
    oss << "Assistant:\n";
    oss << "```json\n";
    oss << R"({"name": "write_file", "arguments": {"path": "main.swift", "content": "import Foundation\n// ... more code ..."}})" << "\n";
    oss << "```\n";
    oss << "[tool result: File created: main.swift]\n";
    oss << "Assistant: Created main.swift with the Swift code.\n\n";

    oss << "User: Fix the bug in line 42\n";
    oss << "Assistant:\n";
    oss << "```json\n";
    oss << R"({"name": "search_replace", "arguments": {"path": "main.swift", "search": "let count = array.count\nif count > 0 {", "replace": "let count = array.count\nif count >= 0 {"}})" << "\n";
    oss << "```\n";
    oss << "[tool result: Replaced 2 lines with 2 lines in main.swift]\n";
    oss << "Assistant: Fixed the comparison operator.\n\n";

    oss << "User: Build the project\n";
    oss << R"({"name": "swift_build", "arguments": {"configuration": "debug"}})" << "\n";
    oss << "[tool result: Build complete!]\n";
    oss << "Assistant: The build succeeded.\n\n";

    oss << "User: Run the Swift script\n";
    oss << R"({"name": "shell", "arguments": {"command": "swift rss_fetcher.swift"}})" << "\n";
    oss << "[tool result: (output or error)]\n";
    oss << "Assistant: The script ran successfully.\n\n";

    // --- Rules: short and unambiguous ---
    oss << "## Rules\n\n";
    oss << "- Use tools whenever a task requires reading/writing files or running commands.\n";
    oss << "- Output the JSON tool call alone on its line — no extra text before or after it.\n";
    oss << "- After receiving tool results, always respond in plain text to the user.\n";
    oss << "- Do NOT call the same tool with the same arguments twice in a row.\n";
    oss << "- Do NOT explain what you are about to do — just call the tool, then summarize the result.\n";
    oss << "- Use `write_file` ONLY for tiny files (<5 lines). For code or longer content, use `start_file` + `append_file`.\n";
    oss << "- ⚠️ **CRITICAL:** Each `append_file` chunk must be 20-50 lines (~1-2 KB). Use `\\n` (escaped) to separate lines. Hard limits: 200 lines / 8 KB.\n";
    oss << "- Do NOT put an entire large file into a single `append_file` — it will be REJECTED. Split into chunks.\n";
    oss << "- The `shell` tool does NOT support: >, >>, <<, |, `, $(), ${}. Use only simple commands.\n";

    return oss.str();
}

}  // namespace cli_tool_parser
