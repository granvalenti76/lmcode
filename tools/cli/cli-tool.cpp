#include "cli-tool.h"

#include <algorithm>
#include <sstream>

void cli_tool_registry::add_tool(const cli_tool& tool) {
    auto it = std::find_if(tools_.begin(), tools_.end(),
        [&tool](const cli_tool& t) { return t.name == tool.name; });

    if (it != tools_.end()) {
        *it = tool;
    } else {
        tools_.push_back(tool);
    }
}

void cli_tool_registry::remove_tool(const std::string& name) {
    tools_.erase(
        std::remove_if(tools_.begin(), tools_.end(),
            [&name](const cli_tool& t) { return t.name == name; }),
        tools_.end());
}

void cli_tool_registry::clear_tools() {
    tools_.clear();
}

const cli_tool* cli_tool_registry::get_tool(const std::string& name) const {
    auto it = std::find_if(tools_.begin(), tools_.end(),
        [&name](const cli_tool& t) { return t.name == name; });
    return it != tools_.end() ? &(*it) : nullptr;
}

std::string cli_tool_registry::list_tools() const {
    std::ostringstream oss;
    oss << "Available tools (" << tools_.size() << "):\n";
    for (const auto& tool : tools_) {
        oss << "  - " << tool.name << ": " << tool.description;
        if (tool.auto_execute) {
            oss << " [auto]";
        }
        oss << "\n";
    }
    return oss.str();
}

json cli_tool_registry::get_tools_json() const {
    json result = json::array();
    for (const auto& tool : tools_) {
        result.push_back({
            {"type", "function"},
            {"function", {
                {"name", tool.name},
                {"description", tool.description},
                {"parameters", json::parse(tool.parameters)}
            }}
        });
    }
    return result;
}

namespace cli_tools {

// JSON schema helpers (avoid raw string literal issues)
static const char* READ_FILE_SCHEMA = R"json({"type":"object","properties":{"path":{"type":"string","description":"Path to the file"}},"required":["path"]})json";
static const char* WRITE_FILE_SCHEMA = R"json({"type":"object","properties":{"path":{"type":"string","description":"Path to the file"},"content":{"type":"string","description":"Content to write"}},"required":["path","content"]})json";
static const char* TOUCH_FILE_SCHEMA = R"json({"type":"object","properties":{"path":{"type":"string","description":"Path to the file to create"}},"required":["path"]})json";
static const char* LIST_DIR_SCHEMA = R"json({"type":"object","properties":{"path":{"type":"string","description":"Path to the directory"}},"required":["path"]})json";
static const char* SHELL_SCHEMA = R"json({"type":"object","properties":{"command":{"type":"string","description":"Shell command to execute"}},"required":["command"]})json";

static const char* START_FILE_SCHEMA = R"json({"type":"object","properties":{"path":{"type":"string","description":"Path to the file to create/truncate"}},"required":["path"]})json";
static const char* APPEND_FILE_SCHEMA = R"json({"type":"object","properties":{"path":{"type":"string","description":"Path to the file"},"content":{"type":"string","description":"Content to append"}},"required":["path","content"]})json";
static const char* FINISH_FILE_SCHEMA = R"json({"type":"object","properties":{"path":{"type":"string","description":"Path to the file to finalize"}},"required":["path"]})json";

static const char* GET_FILE_INFO_SCHEMA = R"json({"type":"object","properties":{"path":{"type":"string","description":"Path to the file"}},"required":["path"]})json";
static const char* VERIFY_FILE_SCHEMA = R"json({"type":"object","properties":{"path":{"type":"string","description":"Path to the file"},"expected_hash":{"type":"string","description":"Expected djb2 hash as returned by append_file or finish_file"}},"required":["path","expected_hash"]})json";

static const char* SEARCH_REPLACE_SCHEMA = R"json({"type":"object","properties":{"path":{"type":"string","description":"Path to the file"},"search":{"type":"string","description":"Text to search for (must be unique)"},"replace":{"type":"string","description":"Text to replace with"}},"required":["path","search","replace"]})json";

static const char* INSERT_LINE_SCHEMA = R"json({"type":"object","properties":{"path":{"type":"string","description":"Path to the file"},"line_number":{"type":"integer","description":"0-indexed line number where to insert (0 = beginning, n = end)"},"content":{"type":"string","description":"Line content to insert"}},"required":["path","line_number","content"]})json";
static const char* REPLACE_RANGE_SCHEMA = R"json({"type":"object","properties":{"path":{"type":"string","description":"Path to the file"},"start_line":{"type":"integer","description":"0-indexed start line (inclusive)"},"end_line":{"type":"integer","description":"0-indexed end line (exclusive)"},"content":{"type":"string","description":"New content to replace the range"}},"required":["path","start_line","end_line","content"]})json";
static const char* DELETE_LINES_SCHEMA = R"json({"type":"object","properties":{"path":{"type":"string","description":"Path to the file"},"start_line":{"type":"integer","description":"0-indexed start line (inclusive)"},"end_line":{"type":"integer","description":"0-indexed end line (exclusive)"}},"required":["path","start_line","end_line"]})json";

static const char* SWIFT_BUILD_SCHEMA = R"json({"type":"object","properties":{"configuration":{"type":"string","enum":["debug","release"],"default":"debug"},"package_path":{"type":"string"}}})json";
static const char* SWIFT_TEST_SCHEMA = R"json({"type":"object","properties":{"configuration":{"type":"string","enum":["debug","release"],"default":"debug"},"filter":{"type":"string"},"package_path":{"type":"string"}}})json";
static const char* SWIFT_RUN_SCHEMA = R"json({"type":"object","properties":{"executable":{"type":"string"},"arguments":{"type":"array","items":{"type":"string"}},"package_path":{"type":"string"}}})json";
static const char* SWIFT_PACKAGE_SCHEMA = R"json({"type":"object","properties":{"command":{"type":"string","enum":["resolve","update","add","remove","edit","unedit","show-dependencies"]},"arguments":{"type":"array","items":{"type":"string"}}},"required":["command"]})json";
static const char* SWIFT_FORMAT_SCHEMA = R"json({"type":"object","properties":{"path":{"type":"string"},"in_place":{"type":"boolean","default":false}},"required":["path"]})json";

std::vector<cli_tool> get_default_tools() {
    return {
        {"read_file", "Read contents of a file", READ_FILE_SCHEMA, true},
        {"touch_file", "Create an empty file (like touch command)", TOUCH_FILE_SCHEMA, true},
        {"write_file", "Write content to a file (asks confirmation if file exists)", WRITE_FILE_SCHEMA, false},
        // --- Chunked write chain: use for files that are too long for a single write_file ---
        // Workflow: start_file → append_file (repeat) → finish_file → verify_file (optional)
        // Use get_file_info before appending to resume a partial write.
        // Recommended chunk size: 20-50 lines (~1-2 KB). Hard limit: 200 lines / 8 KB.
        {"start_file", "Create/truncate a file to begin chunked writing (use with append_file)", START_FILE_SCHEMA, false},
        {"append_file", "Append a chunk (20-50 lines) to a file started with start_file. Returns chunk_hash and cumulative file_size for resume tracking.", APPEND_FILE_SCHEMA, false},
        {"finish_file", "Finalize a chunked write: reports line count, size and total hash", FINISH_FILE_SCHEMA, true},
        {"get_file_info", "Get file size, newline count and hash — use to check state before resuming a partial write", GET_FILE_INFO_SCHEMA, true},
        {"verify_file", "Verify file integrity by comparing its hash against an expected value returned by finish_file", VERIFY_FILE_SCHEMA, true},
        // --- Surgical edits on existing files ---
        {"search_replace", "Search for text and replace it (more robust than line numbers)", SEARCH_REPLACE_SCHEMA, false},
        {"list_dir", "List contents of a directory", LIST_DIR_SCHEMA, true},
        {"shell", "Execute a shell command (always requires confirmation)", SHELL_SCHEMA, false},
        {"insert_line", "Insert a line at a specific position in a file", INSERT_LINE_SCHEMA, false},
        {"replace_range", "Replace a range of lines with new content", REPLACE_RANGE_SCHEMA, false},
        {"delete_lines", "Delete a range of lines from a file", DELETE_LINES_SCHEMA, false}
    };
}

std::vector<cli_tool> get_swift_tools() {
    return {
        {"swift_build", "Build Swift package (swift build)", SWIFT_BUILD_SCHEMA, true},
        {"swift_test", "Run Swift tests (swift test)", SWIFT_TEST_SCHEMA, true},
        {"swift_run", "Run Swift executable (swift run) - requires confirmation", SWIFT_RUN_SCHEMA, false},
        {"swift_package", "Swift package manager operations - requires confirmation", SWIFT_PACKAGE_SCHEMA, false},
        {"swift_format", "Format Swift code (swift format)", SWIFT_FORMAT_SCHEMA, true}
    };
}

}  // namespace cli_tools
