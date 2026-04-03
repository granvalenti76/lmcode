#include "cli-tool-parser.h"
#include "console.h"
#include "chat.h"  // Upstream: common_chat_parse, common_chat_parser_params

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>

// Global counter for unique tool call IDs
namespace cli_tool_parser {
std::atomic<int> g_tool_call_counter{0};
}

namespace cli_tool_parser {

std::vector<cli_tool_call> parse_tool_calls(
    const std::string& content,
    const std::string& /*reasoning_content*/)
{
    std::vector<cli_tool_call> result;

    // Use upstream PEG parser for robust tool call extraction
    // This handles: partial JSON, malformed strings, markdown blocks, etc.
    common_chat_parser_params parser_params;
    parser_params.format = COMMON_CHAT_FORMAT_CONTENT_ONLY;  // We use custom JSON format
    parser_params.parse_tool_calls = true;
    parser_params.debug = g_tool_parser_debug.load();

    // Parse using upstream common_chat_parse
    common_chat_msg msg = common_chat_parse(content, false, parser_params);

    // Extract tool calls from parsed message
    if (!msg.tool_calls.empty()) {
        for (const auto& tool_call : msg.tool_calls) {
            cli_tool_call call;
            call.name = tool_call.name;
            call.arguments = tool_call.arguments;
            call.id = tool_call.id;

            // Generate ID if missing
            if (call.id.empty()) {
                call.id = generate_tool_call_id();
            }

            result.push_back(call);
        }
    }

    // Fallback: if upstream parser found nothing, try legacy JSON regex parsing
    // This handles edge cases where model outputs raw JSON without proper formatting
    if (result.empty()) {
        // Legacy Pattern 1: JSON array [{"name":..., "arguments":...}, ...]
        std::regex array_start_regex(R"(\[\s*\{\s*"name"\s*:)");
        std::smatch match;
        std::string::const_iterator search_start(content.cbegin());

        while (std::regex_search(search_start, content.cend(), match, array_start_regex)) {
            size_t pos = std::distance(content.cbegin(), search_start) + match.position();

            // Extract array with brace-depth tracking (string-aware)
            int depth = 0;
            bool in_string = false;
            bool escape_next = false;
            std::string arr_str;

            for (size_t i = pos; i < content.size(); ++i) {
                char c = content[i];
                if (escape_next) { escape_next = false; arr_str += c; continue; }
                if (c == '\\' && in_string) { escape_next = true; arr_str += c; continue; }
                if (c == '"') { in_string = !in_string; arr_str += c; continue; }
                if (!in_string) {
                    if (c == '[') depth++;
                    else if (c == ']') {
                        depth--;
                        arr_str += c;
                        if (depth == 0) break;
                        continue;
                    }
                }
                arr_str += c;
            }

            if (!arr_str.empty()) {
                try {
                    auto arr = json::parse(arr_str);
                    if (arr.is_array() && !arr.empty()) {
                        for (const auto& item : arr) {
                            if (item.contains("name") && item.contains("arguments")) {
                                cli_tool_call call;
                                call.name = item.value("name", "");
                                call.arguments = item["arguments"].is_string()
                                    ? item["arguments"].get<std::string>()
                                    : item["arguments"].dump();
                                call.id = item.value("id", item.value("tool_call_id", ""));
                                if (call.id.empty()) {
                                    call.id = generate_tool_call_id();
                                }
                                result.push_back(call);
                            }
                        }
                        if (!result.empty()) break;
                    }
                } catch (...) {
                    // Parse failed, continue searching
                }
                search_start = content.cbegin() + pos + arr_str.size();
            } else {
                search_start = match.suffix().first;
            }
        }
    }

    // Fallback Pattern 2: Individual JSON objects {"name":..., "arguments":...}
    if (result.empty()) {
        std::regex obj_start_regex(R"(\{\s*"name"\s*:)");
        std::smatch match;
        std::string::const_iterator search_start(content.cbegin());

        while (std::regex_search(search_start, content.cend(), match, obj_start_regex)) {
            size_t pos = std::distance(content.cbegin(), search_start) + match.position();

            // Extract object with brace-depth tracking (string-aware)
            int depth = 0;
            bool in_string = false;
            bool escape_next = false;
            std::string obj_str;

            for (size_t i = pos; i < content.size(); ++i) {
                char c = content[i];
                if (escape_next) { escape_next = false; obj_str += c; continue; }
                if (c == '\\' && in_string) { escape_next = true; obj_str += c; continue; }
                if (c == '"') { in_string = !in_string; obj_str += c; continue; }
                if (!in_string) {
                    if (c == '{') depth++;
                    else if (c == '}') {
                        depth--;
                        obj_str += c;
                        if (depth == 0) break;
                        continue;
                    }
                }
                obj_str += c;
            }

            if (!obj_str.empty()) {
                try {
                    auto obj = json::parse(obj_str);
                    if (obj.contains("name") && obj.contains("arguments")) {
                        cli_tool_call call;
                        call.name = obj.value("name", "");
                        call.arguments = obj["arguments"].is_string()
                            ? obj["arguments"].get<std::string>()
                            : obj["arguments"].dump();
                        call.id = obj.value("id", obj.value("tool_call_id", ""));
                        if (call.id.empty()) {
                            call.id = generate_tool_call_id();
                        }
                        result.push_back(call);
                    }
                } catch (...) {
                    // Parse failed, continue searching
                }
                search_start = content.cbegin() + pos + obj_str.size();
            } else {
                search_start = match.suffix().first;
            }
        }
    }

    if (!result.empty() && g_tool_parser_debug) {
        console::log("\033[90m[Parser debug: Found %zu tool calls]\033[0m\n", result.size());
    }

    return result;
}

bool has_tool_calls(const std::string& content) {
    return !parse_tool_calls(content).empty();
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
    oss << "### When to Use Each Tool\n\n";
    oss << "**Use `insert_line`, `replace_range`, `delete_lines` when:**\n";
    oss << "- You know the exact line numbers (e.g., \"add at line 5\", \"delete lines 10-15\")\n";
    oss << "- You want to add/remove code without matching text\n";
    oss << "- The file is large and text matching might find multiple occurrences\n\n";
    oss << "**Use `search_replace` when:**\n";
    oss << "- You don't know line numbers\n";
    oss << "- You're replacing a unique text pattern\n";
    oss << "- The change is context-based, not position-based\n\n";
    oss << "**Use `get_line_numbers` to find line numbers before using surgical tools.**\n\n";

    oss << "### Surgical Editing Tools (Line-Based)\n\n";
    oss << "**`insert_line`** — Insert a line at a specific position:\n";
    oss << "```json\n";
    oss << R"({"name": "insert_line", "arguments": {"path": "main.cpp", "line_number": 5, "content": "int new_var = 42;"}})" << "\n";
    oss << "```\n";
    oss << "- `line_number`: 0-indexed (0 = beginning, n = end of file)\n\n";

    oss << "**`replace_range`** — Replace a range of lines:\n";
    oss << "```json\n";
    oss << R"({"name": "replace_range", "arguments": {"path": "main.cpp", "start_line": 10, "end_line": 15, "content": "new_line_1\\nnew_line_2\"}})" << "\n";
    oss << "```\n";
    oss << "- `start_line`: 0-indexed, inclusive\n";
    oss << "- `end_line`: 0-indexed, exclusive (like Python slicing)\n";
    oss << "- `content`: new lines separated by `\\n` (escaped)\n\n";

    oss << "**`delete_lines`** — Delete a range of lines:\n";
    oss << "```json\n";
    oss << R"({"name": "delete_lines", "arguments": {"path": "main.cpp", "start_line": 20, "end_line": 25}})" << "\n";
    oss << "```\n";
    oss << "- `start_line`: 0-indexed, inclusive\n";
    oss << "- `end_line`: 0-indexed, exclusive\n\n";

    oss << "### Text-Based Editing\n\n";
    oss << "**`search_replace`** — Search for text and replace it. Supports fuzzy matching for whitespace, indentation, and escape differences:\n\n";
    oss << "**How it works:** Provide the exact text to replace (`oldString`) and the new text (`newString`).\n";
    oss << "The tool tries 7 matching strategies — from exact match to fuzzy block-anchor matching.\n\n";
    oss << "**Parameters:**\n";
    oss << "- `path`: Absolute path to the file\n";
    oss << "- `oldString`: The text to replace (must match file content, but whitespace/indentation differences are handled automatically)\n";
    oss << "- `newString`: The replacement text\n";
    oss << "- `replaceAll` (optional): Replace ALL occurrences of oldString (default: false). Useful for renaming variables.\n\n";
    oss << "**Example — single replacement:**\n";
    oss << "```json\n";
    oss << R"({"name": "search_replace", "arguments": {"path": "/Users/me/project/main.cpp", "oldString": "int x = 0;", "newString": "int x = 42; // updated"}})" << "\n";
    oss << "```\n\n";
    oss << "**Example — replace all occurrences:**\n";
    oss << "```json\n";
    oss << R"({"name": "search_replace", "arguments": {"path": "/Users/me/project/main.cpp", "oldString": "oldVar", "newString": "newVar", "replaceAll": true}})" << "\n";
    oss << "```\n\n";
    oss << "**Tips:**\n";
    oss << "- Include 3-5 lines of context to make the match unique\n";
    oss << "- The tool handles whitespace, indentation, and escaped character differences automatically\n";
    oss << "- If the match isn't unique, the tool will tell you — add more context\n";
    oss << "- Use `get_line_numbers` or `read_file` first to see the exact content\n\n";

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
    oss << R"({"name": "search_replace", "arguments": {"path": "main.swift", "oldString": "let count = array.count\nif count > 0 {", "newString": "let count = array.count\nif count >= 0 {"}})" << "\n";
    oss << "```\n";
    oss << "[tool result: Replaced 2 lines with 2 lines]\n";
    oss << "Assistant: Fixed the comparison operator.\n\n";

    oss << "User: Add a new function at line 10\n";
    oss << "Assistant:\n";
    oss << "```json\n";
    oss << R"({"name": "insert_line", "arguments": {"path": "main.swift", "line_number": 10, "content": "func newFunction() { print(\"Hello\") }"}})" << "\n";
    oss << "```\n";
    oss << "[tool result: Inserted line at position 10 in main.swift]\n";
    oss << "Assistant: Added the new function at line 10.\n\n";

    oss << "User: Delete the unused function from lines 20-25\n";
    oss << "Assistant:\n";
    oss << "```json\n";
    oss << R"({"name": "delete_lines", "arguments": {"path": "main.swift", "start_line": 20, "end_line": 25}})" << "\n";
    oss << "```\n";
    oss << "[tool result: Deleted lines 20-25 from main.swift (remaining: 95 lines)]\n";
    oss << "Assistant: Deleted the unused function.\n\n";

    oss << "User: Replace lines 30-35 with a new implementation\n";
    oss << "Assistant:\n";
    oss << "```json\n";
    oss << R"({"name": "replace_range", "arguments": {"path": "main.swift", "start_line": 30, "end_line": 35, "content": "// New implementation\\nfunc updated() {\\n    return 42\\n}"}})" << "\n";
    oss << "```\n";
    oss << "[tool result: Replaced lines 30-35 with 3 new lines in main.swift]\n";
    oss << "Assistant: Updated the implementation.\n\n";

    oss << "User: Build the project\n";
    oss << R"({"name": "swift_build", "arguments": {"configuration": "debug"}})" << "\n";
    oss << "[tool result: Build complete!]\n";
    oss << "Assistant: The build succeeded.\n\n";

    oss << "User: Run the Swift script\n";
    oss << R"({"name": "shell", "arguments": {"command": "swift rss_fetcher.swift"}})" << "\n";
    oss << "[tool result: (output or error)]\n";
    oss << "Assistant: The script ran successfully.\n\n";

    // --- File search tools ---
    oss << "## Finding Files\n\n";
    oss << "**`file_glob_search`** — Search for files matching a glob pattern:\n\n";
    oss << "```json\n";
    oss << R"({"name": "file_glob_search", "arguments": {"path": ".", "include": "**/*.cpp"}})" << "\n";
    oss << "```\n";
    oss << "- `path`: Base directory to search in\n";
    oss << "- `include`: Glob pattern (e.g., `**/*.cpp`, `*.h`, `src/**/*`)\n";
    oss << "- `exclude`: Optional glob pattern to exclude files\n\n";

    oss << "**`grep_search`** — Search for a regex pattern in file contents:\n\n";
    oss << "```json\n";
    oss << R"({"name": "grep_search", "arguments": {"path": ".", "pattern": "TODO|FIXME", "return_line_numbers": true}})" << "\n";
    oss << "```\n";
    oss << "- `path`: File or directory to search in\n";
    oss << "- `pattern`: Regular expression to search for\n";
    oss << "- `include`: Optional glob pattern to filter files (default: `**`)\n";
    oss << "- `exclude`: Optional glob pattern to exclude files\n";
    oss << "- `return_line_numbers`: If true, include line numbers in results\n\n";

    oss << "**Example workflow:**\n";
    oss << "1. Use `file_glob_search` to find all `.swift` files\n";
    oss << "2. Use `grep_search` to find all occurrences of a function name\n";
    oss << "3. Use `read_file` to read the relevant files\n";
    oss << "4. Use `search_replace` or `insert_line` to make changes\n\n";

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
    oss << "- **Editing files:** Use `insert_line`/`replace_range`/`delete_lines` when you know line numbers. Use `search_replace` when you don't.\n";
    oss << "- **`search_replace`** uses `oldString`/`newString` (not `search`/`replace`). Set `replaceAll: true` to rename across the whole file.\n";
    oss << "- The `shell` tool does NOT support: >, >>, <<, |, `, $(), ${}. Use only simple commands.\n";

    return oss.str();
}

}  // namespace cli_tool_parser
