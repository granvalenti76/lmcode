#include "chat.h"
#include "common.h"
#include "arg.h"
#include "console.h"

#include "server-context.h"
#include "server-task.h"

// New modular components
#include "cli-tool.h"
#include "cli-tool-exec.h"
#include "cli-tool-parser.h"
#include "cli-stats.h"

#include <array>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <sstream>
#include <thread>
#include <signal.h>
#include <unordered_map>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#   define NOMINMAX
#endif
#include <windows.h>
#endif

const char * LLAMA_ASCII_LOGO = R"(
▄▄ ▄▄
██ ██
██ ██  ▀▀█▄ ███▄███▄  ▀▀█▄    ▄████ ████▄ ████▄
██ ██ ▄█▀██ ██ ██ ██ ▄█▀██    ██    ██ ██ ██ ██
██ ██ ▀█▄██ ██ ██ ██ ▀█▄██ ██ ▀████ ████▀ ████▀
                                    ██    ██
                                    ▀▀    ▀▀
)";

// Global debug mode flag for tool parser (atomic for thread safety)
std::atomic<bool> g_tool_parser_debug{false};

static std::atomic<bool> g_is_interrupted = false;
static bool should_stop() {
    return g_is_interrupted.load();
}

#if defined (__unix__) || (defined (__APPLE__) && defined (__MACH__)) || defined (_WIN32)
static void signal_handler(int) {
    if (g_is_interrupted.load()) {
        fprintf(stdout, "\033[0m\n");
        fflush(stdout);
        std::exit(130);
    }
    g_is_interrupted.store(true);
}
#endif

// ============================================================================
// CLI Context with tool support
// ============================================================================

struct cli_context {
    server_context ctx_server;
    json messages = json::array();
    std::vector<raw_buffer> input_files;
    task_params defaults;
    bool verbose_prompt;
    int reasoning_budget = -1;
    std::string reasoning_budget_message;
    common_reasoning_format reasoning_format;
    bool enable_thinking = true;

    // Tool support
    cli_tool_registry tool_registry;
    std::unique_ptr<cli_tool_executor> tool_executor;
    int tool_call_limit = 20;  // Max tool calls per turn
    int tool_calls_in_turn = 0;
    int max_conversation_turns = 50;  // Max tool execution turns per conversation
    int conversation_turns = 0;  // Current turn counter
    int failed_tool_calls = 0;  // Consecutive failed tool calls
    int max_failed_tool_calls = 6;  // Max consecutive failures before stopping

    // Plan mode - show plan before executing
    bool plan_mode = false;  // If true, show plan and wait for confirmation

    // Tool cache DISABLED: causes more problems than it solves (stale data, complexity)
    // std::unordered_map<std::string, cli_tool_result> tool_cache;
    // bool use_tool_cache = false;

    // Statistics
    cli_stats stats;

    // Debug mode
    bool debug_mode = false;

    // Accumulates fragments of a JSON tool call that was split across multiple
    // generation turns due to n_predict truncation.  Cleared as soon as the
    // closing delimiter is received and the full JSON is successfully parsed.
    std::string pending_incomplete_json;

    std::atomic<bool> loading_show;

    cli_context(const common_params & params) {
        defaults.sampling    = params.sampling;
        defaults.speculative = params.speculative;
        defaults.n_keep      = params.n_keep;
        defaults.n_predict   = params.n_predict;
        defaults.antiprompt  = params.antiprompt;

        defaults.stream = true;
        defaults.timings_per_token = true;

        defaults.chat_parser_params.reasoning_format = params.reasoning_format;
        reasoning_format = params.reasoning_format;

        verbose_prompt = params.verbose_prompt;
        reasoning_budget = params.reasoning_budget;
        reasoning_budget_message = params.reasoning_budget_message;
        enable_thinking = params.enable_reasoning != 0;

        // Initialize tool executor with security config
        cli_tool_security_config security_config;
        tool_executor = create_tool_executor(security_config);

        // Load default tools
        for (const auto& tool : cli_tools::get_default_tools()) {
            tool_registry.add_tool(tool);
        }
        for (const auto& tool : cli_tools::get_swift_tools()) {
            tool_registry.add_tool(tool);
        }

        // Initialize stats
        stats.max_tool_calls = tool_call_limit;
    }

    // Update context size stats
    void update_context_stats() {
        auto* ctx = ctx_server.get_llama_context();
        if (ctx) {
            stats.n_ctx_total = llama_n_ctx(ctx);

            // Get actual KV cache usage from llama.cpp
            auto* mem = llama_get_memory(ctx);
            if (mem) {
                auto pos_max = llama_memory_seq_pos_max(mem, 0);
                if (pos_max >= 0) {
                    stats.n_ctx_used = pos_max + 1;  // Positions are 0-indexed
                } else {
                    stats.n_ctx_used = 0;
                }
            }
        }
    }

    // Display status line
    void display_status() {
        update_context_stats();
        console::log("\n%s\n", cli_stats_display::format_status_line(stats).c_str());
        console::flush();
    }

    std::string generate_completion(result_timings & out_timings) {
        std::string total_content;
        bool model_has_more_to_say = true;

        while (model_has_more_to_say && tool_calls_in_turn < tool_call_limit) {
            // Update stats from previous timings
            cli_stats_display::update_from_timings(stats, out_timings);
            stats.n_tool_calls = 0;  // Reset for this turn

            server_response_reader rd = ctx_server.get_response_reader();
            auto chat_params = format_chat();
            {
                server_task task = server_task(SERVER_TASK_TYPE_COMPLETION);
                task.id         = rd.get_new_id();
                task.index      = 0;
                task.params     = defaults;
                task.cli_prompt = chat_params.prompt;
                task.cli_files  = input_files;
                task.cli        = true;

                task.params.chat_parser_params.format = chat_params.format;
                task.params.chat_parser_params.generation_prompt = chat_params.generation_prompt;
                if (!chat_params.parser.empty()) {
                    task.params.chat_parser_params.parser.load(chat_params.parser);
                }

                // Reasoning budget
                if (reasoning_budget >= 0 && !chat_params.thinking_end_tag.empty()) {
                    const llama_vocab * vocab = llama_model_get_vocab(
                        llama_get_model(ctx_server.get_llama_context()));

                    task.params.sampling.reasoning_budget_tokens = reasoning_budget;
                    task.params.sampling.generation_prompt = chat_params.generation_prompt;

                    if (!chat_params.thinking_start_tag.empty()) {
                        task.params.sampling.reasoning_budget_start =
                            common_tokenize(vocab, chat_params.thinking_start_tag, false, true);
                    }
                    task.params.sampling.reasoning_budget_end =
                        common_tokenize(vocab, chat_params.thinking_end_tag, false, true);
                    task.params.sampling.reasoning_budget_forced =
                        common_tokenize(vocab, reasoning_budget_message + chat_params.thinking_end_tag, false, true);
                }

                rd.post_task({std::move(task)});
            }

            if (verbose_prompt) {
                console::set_display(DISPLAY_TYPE_PROMPT);
                console::log("%s\n\n", chat_params.prompt.c_str());
                console::set_display(DISPLAY_TYPE_RESET);
            }

            console::spinner::start();
            server_task_result_ptr result = rd.next(should_stop);

            console::spinner::stop();
            std::string curr_content;
            bool is_thinking = false;  // For native reasoning_content tracking

            while (result) {
                if (should_stop()) {
                    break;
                }
                if (result->is_error()) {
                    json err_data = result->to_json();
                    if (err_data.contains("message")) {
                        console::error("Error: %s\n", err_data["message"].get<std::string>().c_str());
                    } else {
                        console::error("Error: %s\n", err_data.dump().c_str());
                    }
                    return total_content;
                }
                auto res_partial = dynamic_cast<server_task_result_cmpl_partial *>(result.get());
                if (res_partial) {
                    out_timings = std::move(res_partial->timings);
                    for (const auto & diff : res_partial->oaicompat_msg_diffs) {
                        if (!diff.content_delta.empty()) {
                            // Add to history always
                            curr_content += diff.content_delta;
                            
                            // Print content (debug mode shows everything including reasoning)
                            console::log("%s", diff.content_delta.c_str());
                            console::flush();
                        }
                        if (!diff.reasoning_content_delta.empty()) {
                            if (g_tool_parser_debug) {
                                console::set_display(DISPLAY_TYPE_REASONING);
                                if (!is_thinking) {
                                    console::log("[Start thinking]\n");
                                }
                                is_thinking = true;
                                console::log("%s", diff.reasoning_content_delta.c_str());
                                console::flush();
                            }
                        }
                    }

                    // Update timings (stats updated after generation completes)
                    cli_stats_display::update_from_timings(stats, out_timings);
                }
                auto res_final = dynamic_cast<server_task_result_cmpl_final *>(result.get());
                if (res_final) {
                    out_timings = std::move(res_final->timings);
                    break;
                }
                result = rd.next(should_stop);
            }

            g_is_interrupted.store(false);

            // Check for tool calls in the response
            if (!curr_content.empty()) {
                auto tool_calls = cli_tool_parser::parse_tool_calls(curr_content);

                // Debug: show raw content if no tool calls detected
                if (tool_calls.empty() && !curr_content.empty() && g_tool_parser_debug) {
                    console::log("\n\033[90m[Debug: No tool calls detected in response]\033[0m\n");
                    console::log("\033[90mRaw response (first 200 chars): %s...\033[0m\n",
                        curr_content.substr(0, std::min(size_t(200), curr_content.size())).c_str());
                }

                // Check for incomplete JSON (truncated tool calls).
                // Uses a string-aware state machine so that { } inside string values
                // are not counted as structural braces (fixes the main false-positive).
                bool has_incomplete_json = false;
                if (curr_content.find("{\"name\":") != std::string::npos ||
                    curr_content.find("[{\"name\":") != std::string::npos) {

                    // Returns true when the tool-call JSON object/array has not been
                    // properly closed yet.  We anchor the search to the actual tool call
                    // pattern ({"name": or [{"name":) so that stray { } characters that
                    // appear in thinking/reasoning text before the JSON are not counted.
                    auto is_actually_incomplete = [](const std::string& content) -> bool {
                        // Find the start of the tool call JSON, not just any {
                        size_t json_start = std::string::npos;

                        // Try array form first: [{"name":
                        size_t arr_pos = content.find("[{\"name\":");
                        if (arr_pos != std::string::npos) {
                            json_start = arr_pos;
                        }
                        // Try object form: {"name":
                        size_t obj_pos = content.find("{\"name\":");
                        if (obj_pos != std::string::npos) {
                            // Pick whichever comes first
                            if (json_start == std::string::npos || obj_pos < json_start) {
                                json_start = obj_pos;
                            }
                        }

                        if (json_start == std::string::npos) return false;

                        // Walk through the JSON with a proper string-aware state machine.
                        // This mirrors the logic in extract_json_value() in cli-tool-parser.cpp
                        // and correctly ignores { } [ ] that appear inside string literals.
                        int depth = 0;
                        bool in_string = false;
                        bool escape_next = false;

                        for (size_t i = json_start; i < content.size(); i++) {
                            char c = content[i];

                            if (escape_next) {
                                escape_next = false;
                                continue;
                            }
                            if (c == '\\' && in_string) {
                                escape_next = true;
                                continue;
                            }
                            if (c == '"') {
                                in_string = !in_string;
                                continue;
                            }

                            if (!in_string) {
                                if (c == '{' || c == '[') {
                                    depth++;
                                } else if (c == '}' || c == ']') {
                                    depth--;
                                    if (depth == 0) {
                                        return false;  // Top-level value closed → complete
                                    }
                                }
                            }
                        }

                        return depth > 0;  // Still open → incomplete
                    };

                    if (is_actually_incomplete(curr_content)) {
                        has_incomplete_json = true;
                        console::log("\033[90m[Incomplete JSON detected, prompting for continuation]\033[0m\n");

                        // Accumulate the partial fragment so the next iteration can
                        // reconstruct the full JSON for parsing.
                        pending_incomplete_json += curr_content;

                        // Do NOT push an assistant message yet — the message is not
                        // complete. We'll push it once we have the full JSON.
                        // Note: total_content is also deferred for the same reason.

                        // Ask the model to emit only the missing closing syntax.
                        messages.push_back({
                            {"role", "user"},
                            {"content", "Your JSON tool call was cut off. Continue EXACTLY from where it was truncated — output ONLY the missing closing syntax (no preamble, no explanation)."}
                        });
                        model_has_more_to_say = true;
                    }
                }

                // If the previous iteration left a pending fragment and this one
                // looks complete, splice them together and re-run the parser on the
                // full reconstructed JSON.
                if (!has_incomplete_json && !pending_incomplete_json.empty()) {
                    std::string full_json = pending_incomplete_json + curr_content;
                    pending_incomplete_json.clear();
                    curr_content = full_json;
                    // Re-parse now that we have the full JSON
                    tool_calls = cli_tool_parser::parse_tool_calls(curr_content);
                    if (g_tool_parser_debug) {
                        console::log("\033[90m[Reconstructed JSON — re-parsed %zu tool call(s)]\033[0m\n",
                            tool_calls.size());
                    }
                }

                // Add assistant message only when the JSON is complete.
                if (!has_incomplete_json) {
                    messages.push_back({
                        {"role", "assistant"},
                        {"content", curr_content}
                    });
                    total_content += curr_content;
                }

                // Execute tool calls only if JSON is complete
                if (!tool_calls.empty() && tool_calls_in_turn < tool_call_limit && !has_incomplete_json) {
                    if (g_tool_parser_debug) {
                        console::log("\n\033[1m\033[36m[Tool calls detected: %zu]\033[0m\n", tool_calls.size());
                    }

                    for (const auto& call : tool_calls) {
                        // Validate tool call before processing
                        if (call.name.empty()) {
                            if (g_tool_parser_debug) {
                                console::log("\033[33m[Skipping invalid tool call: empty name]\033[0m\n");
                            }
                            continue;
                        }
                        
                        // Handle SYNTAX_ERROR pseudo-tool (generated by parser when JSON is malformed)
                        if (call.name == "SYNTAX_ERROR") {
                            console::log("\033[31m[JSON syntax error in tool call]\033[0m\n");
                            // Add error message to context to help model self-correct
                            std::string error_msg = "Your tool call failed JSON validation. Error: " + call.arguments + 
                                ". Please fix the JSON syntax. Common issues: " +
                                "(1) Unescaped newlines in strings (use \\n instead of actual newlines), " +
                                "(2) Unescaped double quotes in strings (use \\\\\" instead of \"), " +
                                "(3) Missing commas or braces.";
                            messages.push_back({
                                {"role", "user"},
                                {"content", error_msg}
                            });
                            failed_tool_calls++;
                            continue;
                        }
                        
                        // Only skip if arguments are truly missing/null (empty object {} is valid for some tools)
                        if (call.arguments.empty() || call.arguments == "null") {
                            if (g_tool_parser_debug) {
                                console::log("\033[33m[Skipping tool call '%s': missing arguments]\033[0m\n",
                                    call.name.c_str());
                            }
                            continue;
                        }

                        if (tool_calls_in_turn >= tool_call_limit) {
                            console::log("\033[31m[Tool call limit reached (%d)]\033[0m\n", tool_call_limit);
                            break;
                        }

                        if (g_tool_parser_debug) {
                            console::log("\033[90m[Calling: %s with args: %s]\033[0m\n",
                                call.name.c_str(), call.arguments.c_str());
                        }

                        try {
                            handle_tool_call(call, curr_content);
                            tool_calls_in_turn++;
                            stats.n_tool_calls = tool_calls_in_turn;
                            stats.n_tool_calls_total++;
                        } catch (const std::exception& e) {
                            console::error("\033[31m[Tool execution error: %s]\033[0m\n", e.what());
                            add_tool_result_to_messages(call.id, std::string("Error: ") + e.what());
                        } catch (...) {
                            console::error("\033[31m[Tool execution error: unknown exception]\033[0m\n");
                            add_tool_result_to_messages(call.id, "Error: unknown exception");
                        }
                    }

                    // Continue loop to process next turn with tool results
                    conversation_turns++;

                    // Check for too many consecutive failures
                    if (failed_tool_calls >= max_failed_tool_calls) {
                        console::log("\033[31m[Too many consecutive failures (%d), stopping]\033[0m\n",
                            max_failed_tool_calls);
                        model_has_more_to_say = false;
                    } else if (conversation_turns >= max_conversation_turns) {
                        console::log("\033[31m[Max conversation turns reached (%d)]\033[0m\n", max_conversation_turns);
                        model_has_more_to_say = false;
                    } else {
                        console::log("\033[36m[Processing tool results... (turn %d/%d)]\033[0m\n",
                            conversation_turns, max_conversation_turns);
                        // Loop continues - model will see tool results and respond
                    }
                } else if (has_incomplete_json) {
                    // CRITICAL FIX: Do NOT set model_has_more_to_say to false here!
                    // The JSON is incomplete, we need to let the model continue writing it.
                    // model_has_more_to_say is already true from the detection logic above.
                    console::log("\033[90m[Waiting for model to complete JSON...]\033[0m\n");
                    // Loop will continue with the user prompt already added to ask for completion
                } else {
                    // No tool calls or limit reached, model is done
                    model_has_more_to_say = false;
                }
            } else {
                // Empty content, model is done
                model_has_more_to_say = false;
            }
        }

        // Display final stats after generation
        display_status();

        return total_content;
    }

    void handle_tool_call(const cli_tool_call& call, std::string& /*response_content*/, bool skip_plan_mode = false) {
        // Validate tool call first
        if (call.name.empty()) {
            console::error("\033[31m[Tool call has empty name, skipping]\033[0m\n");
            return;
        }

        // Plan mode: show plan and wait for confirmation
        if (!skip_plan_mode && plan_mode) {
            console::log("\n");
            console::log("\033[1m\033[33m[📋 PLAN MODE - Review before executing]\033[0m\n");
            console::log("Tool: \033[1m%s\033[0m\n", call.name.c_str());
            console::log("Arguments: %s\n", call.arguments.empty() ? "(empty)" : call.arguments.c_str());
            console::log("\n");
            console::set_display(DISPLAY_TYPE_USER_INPUT);
            console::log("\033[1mExecute this tool? [y/N]: \033[0m");
            console::flush();

            std::string line;
            bool confirmed = false;

            console::cleanup();
            try {
                if (std::getline(std::cin, line)) {
                    size_t start = line.find_first_not_of(" \t\n\r");
                    if (start != std::string::npos) {
                        std::string trimmed = line.substr(start);
                        confirmed = (trimmed == "y" || trimmed == "Y" || trimmed == "yes" || trimmed == "YES");
                    }
                }
            } catch (...) {
                console::error("\n\033[31mError reading confirmation input.\033[0m\n");
            }
            console::init(false, true);

            console::set_display(DISPLAY_TYPE_RESET);

            if (!confirmed) {
                console::log("\n\033[31mTool call cancelled by user.\033[0m\n");
                add_tool_result_to_messages(call.id, "Tool call cancelled by user in plan mode");
                return;
            }
        }

        const cli_tool* tool_def = tool_registry.get_tool(call.name);
        if (!tool_def) {
            console::error("Unknown tool: %s\n", call.name.c_str());
            add_tool_result_to_messages(call.id, "Error: Unknown tool '" + call.name + "'");
            return;
        }

        // Cache DISABLED: always execute tool
        // std::string cache_key = make_cache_key(call);
        // if (use_tool_cache && call.name == "read_file") {
        //     auto it = tool_cache.find(cache_key);
        //     if (it != tool_cache.end()) {
        //         console::log("\033[90m[tool:%s using cached result]\033[0m\n", call.name.c_str());
        //         add_tool_result_to_messages(call.id, cli_tool_parser::format_tool_result(it->second));
        //         return;
        //     }
        // }

        // Show compact status line
        if (g_tool_parser_debug) {
            console::log("\033[90m[tool:%s running]\033[0m\n", call.name.c_str());
        } else {
            console::log("\033[90m[tool:%s working...]\033[0m", call.name.c_str());
            console::flush();
        }

        // Check if confirmation is needed (for non-plan-mode)
        bool needs_confirmation = tool_executor->requires_confirmation(call);

        if (needs_confirmation && !skip_plan_mode && !plan_mode) {
            if (g_tool_parser_debug) {
                console::log("\033[90m[tool:%s waiting for confirmation]\033[0m\n", call.name.c_str());
            }
            console::set_display(DISPLAY_TYPE_USER_INPUT);
            console::log("\033[1mConfirm? [y/N]: \033[0m");
            console::flush();

            std::string line;
            bool confirmed = false;

            console::cleanup();
            try {
                if (std::getline(std::cin, line)) {
                    size_t start = line.find_first_not_of(" \t\n\r");
                    if (start != std::string::npos) {
                        std::string trimmed = line.substr(start);
                        confirmed = (trimmed == "y" || trimmed == "Y" || trimmed == "yes" || trimmed == "YES");
                    }
                }
            } catch (...) {
                console::error("\n\033[31mError reading confirmation input.\033[0m\n");
            }
            console::init(false, true);

            console::set_display(DISPLAY_TYPE_RESET);

            if (!confirmed) {
                console::log("\033[90m[tool:%s cancelled]\033[0m\n", call.name.c_str());
                add_tool_result_to_messages(call.id, "Tool call cancelled by user");
                return;
            }
        }

        // Execute tool with exception handling
        cli_tool_result result;
        try {
            result = tool_executor->execute(call, false);
        } catch (const std::exception& e) {
            console::log("\033[90m[tool:%s exception: %s]\033[0m\n", call.name.c_str(), e.what());
            result.success = false;
            result.error = std::string("Exception: ") + e.what();
            result.exit_code = -1;
        } catch (...) {
            console::log("\033[90m[tool:%s unknown exception]\033[0m\n", call.name.c_str());
            result.success = false;
            result.error = "Unknown exception";
            result.exit_code = -1;
        }

        // Cache DISABLED: always execute tool, never cache results
        // std::string cache_key = make_cache_key(call);
        // if (use_tool_cache && result.success && call.name == "read_file") {
        //     tool_cache[cache_key] = result;
        // }

        // Cache invalidation DISABLED: no cache to invalidate
        // if (result.success && (call.name == "write_file" || call.name == "touch_file" ||
        //                        call.name == "insert_line" || call.name == "replace_range" ||
        //                        call.name == "delete_lines")) {
        //     ...
        // }

        // Show result status
        if (result.success) {
            if (g_tool_parser_debug) {
                console::log("\033[90m[tool:%s done]\033[0m\n", call.name.c_str());
            } else {
                console::log("\r\033[90m[tool:%s done]\033[0m\n", call.name.c_str());
            }
            failed_tool_calls = 0;  // Reset failure counter on success
        } else {
            if (g_tool_parser_debug) {
                console::log("\033[90m[tool:%s failed: %s]\033[0m\n", call.name.c_str(),
                    result.error.empty() ? "unknown error" : result.error.c_str());
            } else {
                console::log("\r\033[90m[tool:%s failed]\033[0m\n", call.name.c_str());
            }
            // Log exit code for debugging
            if (result.exit_code != 0 && result.exit_code != -1) {
                if (g_tool_parser_debug) {
                    console::log("\033[90m[tool:%s exit code: %d]\033[0m\n", call.name.c_str(), result.exit_code);
                }
            }
            // Always show output content, even on failure (important for debugging!)
            if (!result.content.empty()) {
                std::string display_content = result.content;
                if (display_content.size() > 500) {
                    display_content = display_content.substr(0, 500) + "\n... [truncated]";
                }
                if (g_tool_parser_debug) {
                    console::log("\033[90m[tool:%s output: %s]\033[0m\n",
                        call.name.c_str(), display_content.c_str());
                }
            }
            failed_tool_calls++;  // Increment failure counter
        }

        // Show output only if non-empty and not too long
        if (result.success && !result.content.empty()) {
            // Truncate for display
            std::string display_content = result.content;
            if (display_content.size() > 200) {
                display_content = display_content.substr(0, 200) + "\n... [output truncated]";
            }
            if (g_tool_parser_debug) {
                console::log("\033[90m[tool:%s output: %s]\033[0m\n",
                    call.name.c_str(), display_content.c_str());
            }
        }

        // Add result to messages for the model
        add_tool_result_to_messages(call.id, cli_tool_parser::format_tool_result(result));
    }

    void add_tool_result_to_messages(const std::string& tool_call_id, const std::string& result) {
        // nlohmann::json will handle escaping automatically - no manual escape needed!
        std::string safe_result = result;

        // Truncate very long tool results to prevent OOM
        const size_t MAX_TOOL_RESULT = 8192;
        if (safe_result.size() > MAX_TOOL_RESULT) {
            safe_result = safe_result.substr(0, MAX_TOOL_RESULT) + "\n... [truncated]";
        }

        // Use "user" role with format matching our system prompt examples
        // Include tool_call_id so the model can correlate results with calls
        messages.push_back({
            {"role", "user"},
            {"content", "[tool result " + tool_call_id + ": " + safe_result + "]"}
        });
    }

    std::string load_input_file(const std::string & fname, bool is_media) {
        std::error_code ec;
        
        // Check if file exists first
        if (!std::filesystem::exists(fname, ec) || ec) {
            console::error("File does not exist: %s\n", fname.c_str());
            return "";
        }
        
        if (is_media) {
            std::ifstream file(fname, std::ios::binary);
            if (!file) {
                console::error("Cannot open file: %s\n", fname.c_str());
                return "";
            }
            raw_buffer buf;
            buf.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            input_files.push_back(std::move(buf));
            return mtmd_default_marker();
        } else {
            std::ifstream file(fname, std::ios::binary);
            if (!file) {
                console::error("Cannot open file: %s\n", fname.c_str());
                return "";
            }
            std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            if (content.empty()) {
                console::log("Warning: File is empty: %s\n", fname.c_str());
            }
            return content;
        }
    }

    common_chat_params format_chat() {
        auto meta = ctx_server.get_meta();
        auto & chat_params = meta.chat_params;

        // Convert cli_tool to common_chat_tool (for template info only)
        std::vector<common_chat_tool> tools;
        for (const auto& tool : tool_registry.get_tools()) {
            tools.push_back({tool.name, tool.description, tool.parameters});
        }

        common_chat_templates_inputs inputs;
        inputs.messages              = common_chat_msgs_parse_oaicompat(messages);
        // DISABLE native llama.cpp tool handling - we use custom JSON format
        // inputs.tools and tool_choice would inject <|python_tag|> etc.
        // which conflicts with our custom parser
        inputs.tools                 = {};  // Empty - we inject tool docs in system prompt
        inputs.tool_choice           = COMMON_CHAT_TOOL_CHOICE_NONE;
        inputs.json_schema           = "";
        inputs.grammar               = "";
        inputs.use_jinja             = chat_params.use_jinja;
        inputs.parallel_tool_calls   = false;  // We handle sequentially
        inputs.add_generation_prompt = true;
        inputs.reasoning_format      = reasoning_format;
        inputs.force_pure_content    = true;  // Don't use llama.cpp tool parsing
        inputs.enable_thinking       = enable_thinking && common_chat_templates_support_enable_thinking(chat_params.tmpls.get());

        try {
            return common_chat_templates_apply(chat_params.tmpls.get(), inputs);
        } catch (const std::exception& e) {
            console::error("Error applying chat template: %s\n", e.what());
            // Return minimal valid chat params as fallback
            common_chat_params fallback;
            if (!inputs.messages.empty()) {
                fallback.prompt = inputs.messages.back().content;
            } else {
                fallback.prompt = "";
            }
            return fallback;
        }
    }
};

// ============================================================================
// Commands
// ============================================================================

static const std::array<const std::string, 15> cmds = {
    "/audio ",
    "/auto ",
    "/cache",
    "/clear",
    "/debug",
    "/exit",
    "/image ",
    "/plan ",
    "/read ",
    "/regen",
    "/stats",
    "/thinking",
    "/tool ",
    "/tools",
    "/help",
};

// ============================================================================
// Main
// ============================================================================

int main(int argc, char ** argv) {
    common_params params;
    params.verbosity = LOG_LEVEL_ERROR;

    if (!common_params_parse(argc, argv, params, LLAMA_EXAMPLE_CLI)) {
        return 1;
    }

    if (params.conversation_mode == COMMON_CONVERSATION_MODE_DISABLED) {
        console::error("--no-conversation is not supported by llama-cli\n");
        console::error("please use llama-completion instead\n");
    }

    common_init();

    cli_context ctx_cli(params);

    llama_backend_init();
    llama_numa_init(params.numa);

    console::init(params.simple_io, params.use_color);
    atexit([]() { console::cleanup(); });

    console::set_display(DISPLAY_TYPE_RESET);
    console::set_completion_callback([](std::string_view, size_t) {
        return std::vector<std::pair<std::string, size_t>>{};
    });

#if defined (__unix__) || (defined (__APPLE__) && defined (__MACH__))
    struct sigaction sigint_action;
    sigint_action.sa_handler = signal_handler;
    sigemptyset (&sigint_action.sa_mask);
    sigint_action.sa_flags = 0;
    sigaction(SIGINT, &sigint_action, NULL);
    sigaction(SIGTERM, &sigint_action, NULL);
#elif defined (_WIN32)
    auto console_ctrl_handler = +[](DWORD ctrl_type) -> BOOL {
        return (ctrl_type == CTRL_C_EVENT) ? (signal_handler(SIGINT), true) : false;
    };
    SetConsoleCtrlHandler(reinterpret_cast<PHANDLER_ROUTINE>(console_ctrl_handler), true);
#endif

    console::log("\nLoading model... ");
    console::spinner::start();
    if (!ctx_cli.ctx_server.load_model(params)) {
        console::spinner::stop();
        console::error("\nFailed to load the model\n");
        return 1;
    }

    console::spinner::stop();
    console::log("\n");

    std::thread inference_thread([&ctx_cli]() {
        ctx_cli.ctx_server.start_loop();
    });

    auto inf = ctx_cli.ctx_server.get_meta();
    std::string modalities = "text";
    if (inf.has_inp_image) modalities += ", vision";
    if (inf.has_inp_audio) modalities += ", audio";

    auto add_system_prompt = [&]() {
        std::string system_content = params.system_prompt;

        // Add tool definitions to system message
        if (!ctx_cli.tool_registry.get_tools().empty()) {
            if (!system_content.empty()) {
                system_content += "\n\n";
            }
            system_content += cli_tool_parser::format_tool_system_message(ctx_cli.tool_registry.get_tools());
        }

        if (!system_content.empty()) {
            ctx_cli.messages.push_back({
                {"role",    "system"},
                {"content", system_content}
            });
        }
    };
    add_system_prompt();

    console::log("\n");
    console::log("%s\n", LLAMA_ASCII_LOGO);
    console::log("build      : %s\n", inf.build_info.c_str());
    console::log("model      : %s\n", inf.model_name.c_str());
    console::log("modalities : %s\n", modalities.c_str());
    if (!params.system_prompt.empty()) {
        console::log("system     : custom prompt\n");
    }
    console::log("\n");
    console::log("commands:\n");
    console::log("  /exit, Ctrl+C   exit\n");
    console::log("  /regen          regenerate last response\n");
    console::log("  /clear          clear chat history\n");
    console::log("  /cache          clear tool result cache\n");
    console::log("  /plan           toggle plan mode (show plan before executing)\n");
    console::log("  /auto <on|off>  enable/disable auto-execution\n");
    console::log("  /stats          show detailed statistics\n");
    console::log("  /thinking       toggle thinking mode\n");
    console::log("  /debug          toggle debug mode (tool call logging)\n");
    console::log("  /tools          list available tools\n");
    console::log("  /tool <cmd>     tool management (add/remove/clear)\n");
    console::log("  /help           show this help\n");
    if (inf.has_inp_image) {
        console::log("  /image <file>   add image\n");
    }
    if (inf.has_inp_audio) {
        console::log("  /audio <file>   add audio\n");
    }
    console::log("  /read <file>    add text file\n");
    console::log("\n");

    std::string cur_msg;
    while (true) {
        std::string buffer;
        console::set_display(DISPLAY_TYPE_USER_INPUT);
        if (params.prompt.empty()) {
            console::log("\n> ");
            std::string line;
            bool another_line = true;
            do {
                another_line = console::readline(line, params.multiline_input);
                buffer += line;
            } while (another_line);
        } else {
            for (auto & fname : params.image) {
                std::string marker = ctx_cli.load_input_file(fname, true);
                if (marker.empty()) {
                    console::error("file does not exist: '%s'\n", fname.c_str());
                    break;
                }
                console::log("Loaded: '%s'\n", fname.c_str());
                cur_msg += marker;
            }
            buffer = params.prompt;
            if (buffer.size() > 500) {
                console::log("\n> %s ... (truncated)\n", buffer.substr(0, 500).c_str());
            } else {
                console::log("\n> %s\n", buffer.c_str());
            }
            params.prompt.clear();
        }
        console::set_display(DISPLAY_TYPE_RESET);
        console::log("\n");

        if (should_stop()) {
            g_is_interrupted.store(false);
            break;
        }

        if (!buffer.empty() && buffer.back() == '\n') {
            buffer.pop_back();
        }

        if (buffer.empty()) {
            continue;
        }

        bool add_user_msg = true;

        // Process commands
        if (string_starts_with(buffer, "/exit")) {
            break;
        } else if (string_starts_with(buffer, "/regen")) {
            if (ctx_cli.messages.size() >= 2) {
                ctx_cli.messages.erase(ctx_cli.messages.size() - 1);
                add_user_msg = false;
            } else {
                console::error("No message to regenerate.\n");
                continue;
            }
        } else if (string_starts_with(buffer, "/clear")) {
            ctx_cli.messages.clear();
            add_system_prompt();
            ctx_cli.input_files.clear();
            ctx_cli.tool_calls_in_turn = 0;
            ctx_cli.conversation_turns = 0;
            ctx_cli.pending_incomplete_json.clear();
            console::log("Chat cleared.\n");
            continue;
        } else if (string_starts_with(buffer, "/stats")) {
            console::log("%s", cli_stats_display::format_detailed(ctx_cli.stats).c_str());
            continue;
        } else if (string_starts_with(buffer, "/cache")) {
            console::log("Tool cache is disabled.\n");
            continue;
        } else if (string_starts_with(buffer, "/plan")) {
            ctx_cli.plan_mode = !ctx_cli.plan_mode;
            console::log("Plan mode %s.\n", ctx_cli.plan_mode ? "enabled" : "disabled");
            console::log("When enabled, tool calls will be shown for approval before execution.\n");
            continue;
        } else if (string_starts_with(buffer, "/auto")) {
            std::string cmd = string_strip(buffer.substr(5));
            if (cmd == "on" || cmd == "true" || cmd == "1") {
                ctx_cli.plan_mode = false;
                console::log("Auto-execution enabled. Tools will run without confirmation.\n");
            } else if (cmd == "off" || cmd == "false" || cmd == "0") {
                ctx_cli.plan_mode = true;
                console::log("Manual mode enabled. Tool calls will require confirmation.\n");
            } else {
                console::log("Usage: /auto <on|off>\n");
            }
            continue;
        } else if (string_starts_with(buffer, "/thinking")) {
            ctx_cli.enable_thinking = !ctx_cli.enable_thinking;
            console::log("Thinking %s.\n", ctx_cli.enable_thinking ? "enabled" : "disabled");
            continue;
        } else if (string_starts_with(buffer, "/debug")) {
            ctx_cli.debug_mode = !ctx_cli.debug_mode;
            g_tool_parser_debug = ctx_cli.debug_mode;
            console::log("Debug mode %s.\n", ctx_cli.debug_mode ? "enabled" : "disabled");
            continue;
        } else if (string_starts_with(buffer, "/tools")) {
            console::log("%s", ctx_cli.tool_registry.list_tools().c_str());
            continue;
        } else if (string_starts_with(buffer, "/tool ")) {
            std::string cmd = string_strip(buffer.substr(6));
            if (cmd == "clear") {
                ctx_cli.tool_registry.clear_tools();
                console::log("All tools removed.\n");
            } else if (string_starts_with(cmd, "remove ")) {
                std::string name = string_strip(cmd.substr(7));
                ctx_cli.tool_registry.remove_tool(name);
                console::log("Tool '%s' removed.\n", name.c_str());
            } else if (string_starts_with(cmd, "add ")) {
                std::string name = string_strip(cmd.substr(4));

                // Re-add from defaults
                bool found = false;
                for (const auto& tool : cli_tools::get_default_tools()) {
                    if (tool.name == name) {
                        ctx_cli.tool_registry.add_tool(tool);
                        found = true;
                        break;
                    }
                }
                for (const auto& tool : cli_tools::get_swift_tools()) {
                    if (tool.name == name) {
                        ctx_cli.tool_registry.add_tool(tool);
                        found = true;
                        break;
                    }
                }
                console::log(found ? "Tool '%s' added.\n" : "Tool '%s' not found.\n", name.c_str());
            } else {
                console::log("Usage: /tool <add|remove|clear> [name]\n");
            }
            continue;
        } else if (string_starts_with(buffer, "/help")) {
            console::log("See commands above.\n");
            continue;
        } else if ((string_starts_with(buffer, "/image ") && inf.has_inp_image) ||
                   (string_starts_with(buffer, "/audio ") && inf.has_inp_audio)) {
            std::string fname = string_strip(buffer.substr(7));
            std::string marker = ctx_cli.load_input_file(fname, true);
            if (marker.empty()) {
                console::error("file does not exist: '%s'\n", fname.c_str());
                continue;
            }
            cur_msg += marker;
            console::log("Loaded: '%s'\n", fname.c_str());
            continue;
        } else if (string_starts_with(buffer, "/read ")) {
            std::string fname = string_strip(buffer.substr(6));
            std::string marker = ctx_cli.load_input_file(fname, false);
            if (marker.empty()) {
                console::error("file does not exist: '%s'\n", fname.c_str());
                continue;
            }
            if (inf.fim_sep_token != LLAMA_TOKEN_NULL) {
                cur_msg += common_token_to_piece(ctx_cli.ctx_server.get_llama_context(), inf.fim_sep_token, true);
                cur_msg += fname;
                cur_msg.push_back('\n');
            } else {
                cur_msg += "--- File: " + fname + " ---\n";
            }
            cur_msg += marker;
            console::log("Loaded: '%s'\n", fname.c_str());
            continue;
        } else {
            cur_msg += buffer;
        }

        if (add_user_msg) {
            ctx_cli.messages.push_back({
                {"role",    "user"},
                {"content", cur_msg}
            });
            cur_msg.clear();
        }

        ctx_cli.tool_calls_in_turn = 0;  // Reset tool counter for new turn
        ctx_cli.failed_tool_calls = 0;  // Reset failure counter for new turn
        ctx_cli.conversation_turns = 0;  // Reset conversation turns for new user input
        ctx_cli.pending_incomplete_json.clear();  // Discard any leftover fragment from previous turn

        result_timings timings;
        std::string assistant_content = ctx_cli.generate_completion(timings);
        // Note: generate_completion already adds the assistant message to history
        
        console::log("\n");

        if (params.show_timings) {
            console::set_display(DISPLAY_TYPE_INFO);
            console::log("\n");
            console::log("[ Prompt: %.1f t/s | Generation: %.1f t/s ]\n",
                        timings.prompt_per_second, timings.predicted_per_second);
            console::set_display(DISPLAY_TYPE_RESET);
        }

        if (params.single_turn) {
            break;
        }
    }

    console::set_display(DISPLAY_TYPE_RESET);
    console::log("\nExiting...\n");
    ctx_cli.ctx_server.terminate();
    inference_thread.join();

    common_log_set_verbosity_thold(LOG_LEVEL_INFO);
    llama_memory_breakdown_print(ctx_cli.ctx_server.get_llama_context());

    return 0;
}
