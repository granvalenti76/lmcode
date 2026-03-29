#pragma once

#include "common.h"
#include "server-task.h"

#include <cstdint>
#include <string>

// Statistics display for CLI
struct cli_stats {
    // Context usage
    uint32_t n_ctx_total = 0;      // Total context size
    uint32_t n_ctx_used = 0;       // Tokens currently in use

    // Token processing
    int32_t n_prompt_processed = 0;  // Total prompt tokens processed
    int32_t n_prompt_cached = 0;     // Tokens from cache (saved)
    int32_t n_generated = 0;         // Generated tokens

    // Memory (in bytes)
    uint64_t memory_model = 0;       // Model memory
    uint64_t memory_kv_cache = 0;    // KV cache memory

    // Tool execution
    int32_t n_tool_calls = 0;        // Tool calls in current turn
    int32_t n_tool_calls_total = 0;  // Total tool calls
    int32_t max_tool_calls = 20;     // Max tool calls per turn

    // Timing (in milliseconds)
    int64_t t_prompt_ms = 0;
    int64_t t_generation_ms = 0;

    // Performance
    double prompt_per_second = 0.0;
    double predicted_per_second = 0.0;
};

// Format stats for display
namespace cli_stats_display {

// Get single-line status string
std::string format_status_line(const cli_stats& stats);

// Get detailed stats display
std::string format_detailed(const cli_stats& stats);

// Get context usage bar (like [████░░░░░░] 39%)
std::string format_context_bar(const cli_stats& stats, int width = 20);

// Update stats from timings
void update_from_timings(cli_stats& stats, const result_timings& timings);

// Get memory usage string (human-readable)
std::string format_memory(uint64_t bytes);

}  // namespace cli_stats_display
