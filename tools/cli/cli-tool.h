#pragma once

#include "common.h"
#include "nlohmann/json.hpp"

#include <string>
#include <vector>
#include <functional>

using json = nlohmann::ordered_json;

// Tool definition
struct cli_tool {
    std::string name;
    std::string description;
    std::string parameters;  // JSON schema
    bool auto_execute;       // true = esegui senza conferma

    bool operator==(const cli_tool& other) const {
        return name == other.name;
    }
};

// Tool call from model output
struct cli_tool_call {
    std::string name;
    std::string arguments;   // JSON string
    std::string id;
};

// Tool execution result
struct cli_tool_result {
    std::string tool_call_id;
    std::string content;     // stdout/result
    std::string error;       // stderr/error message
    int exit_code;
    bool success;
};

// Tool executor interface
class cli_tool_executor {
public:
    virtual ~cli_tool_executor() = default;
    virtual cli_tool_result execute(const cli_tool_call& call, bool dry_run = false) = 0;
    virtual bool requires_confirmation(const cli_tool_call& call) const = 0;
};

// Tool registry
class cli_tool_registry {
public:
    void add_tool(const cli_tool& tool);
    void remove_tool(const std::string& name);
    void clear_tools();

    const cli_tool* get_tool(const std::string& name) const;
    const std::vector<cli_tool>& get_tools() const { return tools_; }

    std::string list_tools() const;
    json get_tools_json() const;  // Per OpenAI tool format

private:
    std::vector<cli_tool> tools_;
};

// Predefined tool definitions
namespace cli_tools {
    std::vector<cli_tool> get_default_tools();
    std::vector<cli_tool> get_swift_tools();
}
