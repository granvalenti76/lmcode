# LMCode

LMCode is an experimental fork of `llama.cpp`, specifically targeting the `tools/llama-cli` component. It has been developed to enable agentic workflows using low-power local models.

## Overview

The system provides a robust set of tools to transform a standard LLM into an agent capable of interacting with a local environment. It is designed to be lightweight and highly efficient, making it ideal for users working with limited hardware or seeking low-latency performance.

## Key Features

* **Agentic Capabilities:** Includes 31 specialized tools to facilitate complex tasks.
* **Flexible Context Management:**
    * **Full Mode:** Utilizes approximately 6,000 tokens for the full toolset.
    * **Minimal Mode:** Can be run in a highly optimized mode (`read|write|shell|grep`) using only ~351 tokens, making it suitable for extremely constrained environments.
* **Manual Task Orchestration:** Users can manually create Tasks and subtasks. This is particularly useful when the model struggles to follow a complex, multi-step plan autonomously.
* **Safety and Control:**
    * All writing operations require explicit user authorization.
    * Includes a built-in blacklist for shell commands to prevent accidental or harmful executions.
* **Minimalist Design:**
    * No hardcoded system prompts; tool schemas are loaded on-demand to keep the context clean.
    * Maintains identical tuning parameters and compatibility with the original `llama-cli`.

## Design Philosophy

LMCode was conceived to address two main challenges: managing moderate context windows and reducing latency. By providing a compact harness built upon the high-performance `llama.cpp` core, it offers a streamlined way to run agentic workloads locally.

The project aims to stay current, with attempts to sync with the upstream `llama.cpp` repository at least once a week.

## Installation

To install LMCode, clone the repository and build it using CMake:

```bash
git clone https://github.com/granvalenti76/lmcode
cd lmcode
cmake -B build
cmake --build build --target llama-cli -j$(nproc)
```

## Usage

Run the executable with your preferred model and tool configuration. The `--tools` flag is optional and defaults to `all`.

```bash
build/bin/llama-cli -fa on -c 64000 -m ~/.models/gemma-4E4.gguf --tools minimal|empty|all
```

## Chat Interface

### Interactive Commands

| Command | Description |
|---------|-------------|
| `/exit` | Exit the application |
| `/clear` | Clear chat history |
| `/regen` | Regenerate last response |
| `/thinking` | Toggle reasoning mode on/off |
| `/stats` | Show detailed statistics |
| `/tools` | List available tools |
| `/tool add <name>` | Re-add a previously removed tool |
| `/tool remove <name>` | Remove a tool from current session |
| `/tool clear` | Remove all tools |
| `/debug` | Toggle debug mode for tool call logging |
| `/help` | Show help |

### File Commands

| Command | Description |
|---------|-------------|
| `/read <file>` | Add file content to chat |
| `/image <file>` | Add image (if supported) |
| `/audio <file>` | Add audio (if supported) |

### Statistics Display

**Status Line (always visible):**
```
[..........] 12.5K/32K tokens Cache: +8.2K Tools: 3/20 45.2 t/s
```

**Detailed Stats (`/stats`):**
```
=== Statistics ===
Context:     [████████....] 12543/32768 tokens (38%)
Cache save:  +8234 tokens
Generated:   1234 tokens (45.2 t/s)
Memory:      Model: 4.2 GB, KV: 256 MB
Tool calls:  3 (this turn), 15 (total)
Timing:      Prompt: 234ms, Generation: 567ms
```

## Available Tools

### File Operations (15 tools)

| Tool | Description | Auto-Exec |
|------|-------------|-----------|
| `read_file` | Read contents of a file | ✅ |
| `read_file_range` | Read a specific line range from a file (efficient for large files). Returns metadata plus content | ✅ |
| `touch_file` | Create an empty file (like touch command) | ✅ |
| `write_file` | Write content to a file | ❌ (confirms if exists) |
| `start_file` | Create/truncate a file to begin chunked writing (use with `append_file`) | ✅ |
| `append_file` | Append a chunk (20-50 lines) to a file started with `start_file`. Returns chunk_hash and cumulative file_size for resume tracking | ✅ |
| `finish_file` | Finalize a chunked write: reports line count, size and total hash | ✅ |
| `get_file_info` | Get file size, newline count and hash — use to check state before resuming a partial write | ✅ |
| `verify_file` | Verify file integrity by comparing its hash against an expected value | ✅ |
| `search_replace` | Search for text in a file and replace it. Supports fuzzy matching for whitespace, indentation, and escape differences | ✅ |
| `get_line_numbers` | Read file with line numbers prefixed (useful for finding exact positions) | ✅ |
| `get_file_diff` | Get difference between two files in unified diff format | ✅ |
| `insert_line` | Insert a line at a specific position in a file | ✅ |
| `replace_range` | Replace a range of lines with new content | ✅ |
| `replace_lines` | Replace lines `start_line` to `end_line` with new content (direct line-based edit, no string matching) | ✅ |
| `delete_lines` | Delete a range of lines from a file | ✅ |

### Search Tools (4 tools)

| Tool | Description | Auto-Exec |
|------|-------------|-----------|
| `list_dir` | List contents of a directory | ✅ |
| `file_glob_search` | Recursively search for files matching a glob pattern (e.g. `**/*.cpp`) | ✅ |
| `grep_search` | Search for a regex pattern in files under a path | ✅ |
| `search_regex` | Search and replace using regex patterns (more powerful than exact match) | ✅ |

### Task Management (3 tools)

| Tool | Description | Auto-Exec |
|------|-------------|-----------|
| `decompose` | Break a complex task into smaller, actionable subtasks. Call this when a task has multiple independent steps | ✅ |
| `list_tasks` | Show the current task hierarchy and their statuses. Useful to see what still needs to be done | ✅ |
| `set_task_status` | Update the status of a task (DONE, FAILED, IN_PROGRESS). Call this when you complete or fail a subtask | ✅ |

### Snippet Management (2 tools)

| Tool | Description | Auto-Exec |
|------|-------------|-----------|
| `search_snippets` | List all snippets in `./snippets/` directory | ✅ |
| `load_snippet` | Copy a snippet from `./snippets/<snippet_name>` to `./<dest_name>` in the working directory | ❌ (confirms overwrite) |

### Shell & System (1 tool)

| Tool | Description | Auto-Exec |
|------|-------------|-----------|
| `shell` | Execute a shell command | ❌ (always requires confirmation) |

### Swift Development (5 tools)

| Tool | Description | Auto-Exec |
|------|-------------|-----------|
| `swift_build` | Build Swift package (`swift build`) | ✅ |
| `swift_test` | Run Swift tests (`swift test`) | ✅ |
| `swift_run` | Run Swift executable (`swift run`) | ❌ (requires confirmation) |
| `swift_package` | Swift package manager operations | ❌ (requires confirmation) |
| `swift_format` | Format Swift code (`swift format --in-place`) | ✅ |

## Security Model

### Automatic Confirmation (no user prompt)
- Read operations (`read_file`, `list_dir`, `grep_search`, etc.)
- File metadata (`get_file_info`, `get_line_numbers`)
- Search operations (`file_glob_search`, `search_regex`)
- Task management (`decompose`, `list_tasks`, `set_task_status`)
- Swift build & test (`swift_build`, `swift_test`, `swift_format`)

### Always Requires Confirmation
- File write operations (if file already exists)
- Shell command execution (`shell`)
- `swift run` (executes arbitrary code)
- `swift package` (modifies dependencies)
- `load_snippet` (if destination file exists)

### Shell Command Whitelist
Commands permitted in `shell` tool:
- **File:** `ls`, `cat`, `grep`, `find`, `head`, `tail`, `wc`, `diff`
- **Swift:** `swift`, `xcrun`, `xcodebuild`
- **Git:** `git` (status, log, diff only)
- **System:** `pwd`, `whoami`, `uname`, `date`, `echo`

### Shell Command Blacklist (always blocked)
- Destructive: `rm -rf`, `rm -r`, `dd`, `mkfs`, `mount`, `umount`
- Privilege escalation: `sudo`, `su`, `chmod 777`, `chown`
- Remote execution: `curl | sh`, `wget | sh`, `bash -c`, `sh -c`
- Dangerous patterns: `:(){ :|:& };:` (fork bomb), `> /dev/sda`

## Token Budget & Limits

- **Max 20 tool calls per turn** (resets after each user response) – prevents infinite loops with small models
- **Tool definitions:** Sent once and cached
- **Tool output:** Truncated to 4KB max
- **Display output:** Truncated to 500 characters

## Architecture

```
tools/cli/
├── cli.cpp              # Main loop with tool integration
├── cli-tool.h           # Tool data structures
├── cli-tool.cpp         # Tool registry and definitions
├── cli-tool-exec.h      # Tool execution interface
├── cli-tool-exec.cpp    # Execution implementation + security
├── cli-tool-parser.h    # Tool call parser
├── cli-tool-parser.cpp  # Parser implementation
├── cli-stats.h          # Statistics display
└── cli-stats.cpp        # Statistics implementation
```

## Extending Tools

### Adding a Default Tool

In `cli-tool.cpp`:
```cpp
static const char* MY_TOOL_SCHEMA = R"json({
    "name": "my_tool",
    "description": "Does something useful",
    "parameters": {
        "type": "object",
        "properties": {
            "param1": {"type": "string", "description": "First parameter"}
        },
        "required": ["param1"]
    }
})json";

std::vector<cli_tool> get_default_tools() {
    return {
        {"my_tool", "Does something useful", MY_TOOL_SCHEMA, false},
        // ... existing tools
    };
}
```

### Implementing Executor

In `cli-tool-exec.cpp`:
```cpp
cli_tool_result cli_tool_executor_impl::execute_my_tool(const cli_tool_call& call) {
    auto args = parse_args(call.arguments);

    // Implementation...

    cli_tool_result result;
    result.success = true;
    result.content = "Operation completed successfully";
    return result;
}
```

Then register in `cli_tool_executor_impl::execute()` dispatch.

## Technical Notes

- **C++17:** Uses `std::filesystem`, `std::async`
- **Thread-safe:** Tool execution with async + timeout
- **Modular:** Each component is isolated and testable

## Example Session

```
> Create a new Swift package and implement a hello function

[Model executes: swift package init --name Hello]
[Model executes: read_file Sources/Hello/Hello.swift]
[Model executes: write_file Sources/Hello/Hello.swift (with new content)]
[Model executes: swift_build]
[Model generates final response]

> Run the tests

[Model executes: swift_test]
```

## Acknowledgments

This project is a fork of [llama.cpp](https://github.com/ggml-org/llama.cpp), the amazing open‑source inference engine for LLMs created by [Georgi Gerganov](https://github.com/ggerganov) and maintained by the entire llama.cpp community.

Special thanks to the contributors who built the `llama-cli` tool and its rich argument handling, which served as the foundation for our extended version with tool‑call support, Swift developer tooling, and enhanced safety features.



