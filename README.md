# lmcode (v0.1-alpha)

**lmcode** is an experimental fork of `llama.cpp` that transforms `llama-cli` into a native **Agentic Coding Environment**. It integrates tool-calling logic and real-time execution statistics directly into the C++ core, enabling autonomous coding workflows without HTTP/REST overhead.

---
<img width="1332" height="742" alt="image" src="https://github.com/user-attachments/assets/82530631-d2e1-41cf-8dc8-9bbc1aa1a7bf" />
<img width="1332" height="742" alt="image" src="https://github.com/user-attachments/assets/d93715a8-0a69-427d-8b90-8e320cfe18f8" />
<img width="1332" height="742" alt="Screenshot 2026-03-26 alle 12 39 49" src="https://github.com/user-attachments/assets/ac15bb1a-1709-4092-89b3-1f2e4ab0ed4f" />
<img width="1332" height="742" alt="Screenshot 2026-03-26 alle 12 42 45" src="https://github.com/user-attachments/assets/304668e5-76ff-486f-836c-629c8b5cf381" />

## 🚀 Installation & Build (Tested on MacBook)

For Apple Silicon (M1/M2/M3/M4) users, CMake ensures Metal acceleration is correctly enabled.

### 1. Compile the Agentic CLI
```bash
cmake --build build --config Release
cmake --build build --target llama-cli -j$(nproc)
```

The executable will be located at: `build/bin/llama-cli`

---

## 🏗 Project Architecture

The implementation is modular and integrated within `tools/cli/` for maximum performance:

* `cli.cpp`: Main inference loop with integrated agentic state machine.
* `cli-tool.cpp/h`: Registry for tool definitions and compact JSON schemas.
* `cli-tool-exec.cpp/h`: Secure execution engine for file system and shell operations.
* `cli-tool-parser.cpp/h`: High-speed extraction of tool-call patterns.
* `cli-stats.cpp/h`: Granular monitoring of context, tokens/s, and tool latency.

---

## 🛠 Integrated Agentic Tools

### File System & Surgical Editing
Designed for precision and context efficiency.

| Tool | Description | Auto-Exec |
| :--- | :--- | :--- |
| `read_file` | Reads file content (safely truncated at 4KB). | ✅ |
| `list_dir` | Lists directory contents for project mapping. | ✅ |
| `search_replace` | **Surgical Edit**: Replaces a specific block of text. | ❌ |
| `insert_line` | Inserts a single line at a specific position. | ❌ |
| `delete_lines` | Removes a range of lines from a file. | ❌ |
| `shell` | Executes whitelisted bash commands. | **Always asks** |

### Chunked Write Chain (Large File Support)
Optimized for handling files exceeding the model's output window:
1.  **`start_file`**: Initialize or truncate a file for writing.
2.  **`append_file`**: Send a chunk (20-50 lines). Returns a hash for tracking.
3.  **`finish_file`**: Finalizes the write and closes the stream.
4.  **`verify_file`**: Compares current file hash against expected value.

---



## ⌨️ CLI Commands & Interface

### Agent Control
- `/thinking`: Toggle visibility of the model's internal reasoning/thoughts.
- `/tools`: Display the list of currently registered tools and their status.
- `/stats`: Show detailed memory, context, and timing metrics.
- `/clear`: Reset chat history and agent state.

### Real-time Status Line
`[██████....] 12.5K/32K tokens | Cache: +8.2K | Tools: 3/20 | 45.2 t/s`

---

## 🛡 Security & Constraints

- **Max Tool Calls**: 20 per turn (prevents infinite loops).
- **Output Truncation**: Tool results are capped at 4KB to preserve context.
- **Strict Blacklist**: Commands like `rm -rf`, `sudo`, or piping to `sh` are hard-blocked.
- **Safety Whitelist**: `ls`, `pwd`, `find`, `cat`, `grep`, `git status`, `diff`.

---
