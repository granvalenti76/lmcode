# lmcode (v0.1-alpha)

**lmcode** is an experimental fork of `llama.cpp` designed to transform `llama-cli` into a native **Agentic Coding Environment**. By integrating tool-calling logic and real-time execution statistics directly into the C++ core, it enables autonomous coding workflows without the overhead or privacy concerns of external HTTP/REST endpoints.

---

## 🏗 Project Architecture

The implementation is modular and baked into the `tools/cli/` directory for maximum performance:

* `cli.cpp`: Main inference loop with integrated agentic state machine.
* `cli-tool.cpp/h`: Registry for tool definitions and compact JSON schemas.
* `cli-tool-exec.cpp/h`: Secure execution engine for file system and shell operations.
* `cli-tool-parser.cpp/h`: High-speed extraction of tool-call patterns from model output.
* `cli-stats.cpp/h`: Granular monitoring of context, tokens/s, and tool latency.

---

## 🛠 Integrated Agentic Tools

### File System & Surgical Editing
Designed for precision and context efficiency.

| Tool | Description | Auto-Exec |
| :--- | :--- | :--- |
| `read_file` | Reads file content (safely truncated if exceeding 4KB). | ✅ |
| `list_dir` | Lists directory contents for project mapping. | ✅ |
| `search_replace` | **Surgical Edit**: Replaces a specific block of text. More robust than line numbers. | ❌ |
| `insert_line` | Inserts a single line at a specific position. | ❌ |
| `delete_lines` | Removes a range of lines from a file. | ❌ |
| `shell` | Executes whitelisted bash commands. | **Always asks** |

### Chunked Write Chain (Large File Support)
To handle files that are too large for a single inference pass, use this stateful sequence:
1.  **`start_file`**: Initialize or truncate a file for writing.
2.  **`append_file`**: Send a chunk (recommended 20-50 lines). Returns a hash for tracking.
3.  **`finish_file`**: Finalizes the write, closes the stream, and reports total integrity.
4.  **`verify_file`**: Compares current file hash against expected value.

---

## ⌨️ CLI Commands & Interface

### Agent Control
- `/thinking`: Toggle visibility of the model's internal reasoning/thoughts.
- `/tools`: Display the list of currently registered tools and their status.
- `/stats`: Show detailed memory, context, and timing metrics.
- `/clear`: Reset chat history and agent state.

### Real-time Status Line
A persistent overlay during generation:
`[██████....] 12.5K/32K tokens | Cache: +8.2K | Tools: 3/20 | 45.2 t/s`

---

## 🛡 Security & Constraints

### Safety Whitelist
The `shell` tool is restricted to non-destructive commands by default:
- **Nav**: `ls`, `pwd`, `find`.
- **Read**: `cat`, `grep`, `head`, `tail`, `diff`.
- **System**: `git status`, `date`, `uname`.

### Hard Limits
- **Max Tool Calls**: 20 per turn (prevents infinite loops in smaller models).
- **Output Truncation**: Tool results are capped to preserve context space.
- **Strict Blacklist**: Commands like `rm -rf`, `sudo`, or piping to `sh` are hard-blocked.

---

## 🚀 Installation & Build

Compile only the agentic CLI using the provided Makefile:

```bash
make llama-cli
