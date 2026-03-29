#include "cli-stats.h"
#include "console.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace cli_stats_display {

std::string format_memory(uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit_idx = 0;
    double size = static_cast<double>(bytes);

    while (size >= 1024.0 && unit_idx < 4) {
        size /= 1024.0;
        unit_idx++;
    }

    std::ostringstream oss;
    oss.precision(size >= 100.0 ? 0 : (size >= 10.0 ? 1 : 2));
    oss << std::fixed << size << " " << units[unit_idx];
    return oss.str();
}

std::string format_context_bar(const cli_stats& stats, int width) {
    if (stats.n_ctx_total == 0) {
        return std::string(width, '.');
    }

    float ratio = std::min(1.0f, static_cast<float>(stats.n_ctx_used) / stats.n_ctx_total);
    int filled = static_cast<int>(ratio * width);

    std::ostringstream oss;
    oss << "[";

    for (int i = 0; i < width; ++i) {
        if (i < filled) {
            // Color based on usage
            if (ratio < 0.5f) {
                oss << "\033[32m█\033[0m";  // Green
            } else if (ratio < 0.8f) {
                oss << "\033[33m█\033[0m";  // Yellow
            } else {
                oss << "\033[31m█\033[0m";  // Red
            }
        } else {
            oss << "░";
        }
    }

    oss << "]";
    return oss.str();
}

std::string format_status_line(const cli_stats& stats) {
    std::ostringstream oss;

    // Context usage
    oss << "\033[90m[";  // Gray

    // Context bar
    oss << format_context_bar(stats, 10);
    oss << " ";

    // Context numbers
    oss << stats.n_ctx_used << "/" << stats.n_ctx_total;

    // Cache savings
    if (stats.n_prompt_cached > 0) {
        oss << " \033[32mCache: +" << stats.n_prompt_cached << "\033[0m";
    }

    // Tool calls
    if (stats.n_tool_calls > 0) {
        oss << " \033[36mTools: " << stats.n_tool_calls << "/" << stats.max_tool_calls << "\033[0m";
    }

    // Performance
    if (stats.predicted_per_second > 0) {
        oss << " \033[33m" << stats.predicted_per_second << " t/s\033[0m";
    }

    oss << "]\033[0m";

    return oss.str();
}

std::string format_detailed(const cli_stats& stats) {
    std::ostringstream oss;

    oss << "\n\033[1m=== Statistics ===\033[0m\n";

    // Context
    oss << "Context:     " << format_context_bar(stats, 30) << " ";
    oss << stats.n_ctx_used << "/" << stats.n_ctx_total << " tokens";
    if (stats.n_ctx_total > 0) {
        oss << " (" << (100.0 * stats.n_ctx_used / stats.n_ctx_total) << "%)";
    }
    oss << "\n";

    // Cache
    oss << "Cache save:  ";
    if (stats.n_prompt_cached > 0) {
        oss << "\033[32m+" << stats.n_prompt_cached << " tokens\033[0m";
    } else {
        oss << "0 tokens";
    }
    oss << "\n";

    // Generation
    oss << "Generated:   " << stats.n_generated << " tokens";
    if (stats.t_generation_ms > 0) {
        oss << " (" << stats.predicted_per_second << " t/s)";
    }
    oss << "\n";

    // Memory
    oss << "Memory:      Model: " << format_memory(stats.memory_model);
    oss << ", KV: " << format_memory(stats.memory_kv_cache) << "\n";

    // Tools
    oss << "Tool calls:  " << stats.n_tool_calls << " (this turn), ";
    oss << stats.n_tool_calls_total << " (total)\n";

    // Timing
    oss << "Timing:      Prompt: " << stats.t_prompt_ms << "ms";
    oss << ", Generation: " << stats.t_generation_ms << "ms\n";

    oss << "\n";

    return oss.str();
}

void update_from_timings(cli_stats& stats, const result_timings& timings) {
    stats.n_prompt_processed = timings.prompt_n;
    stats.n_generated = timings.predicted_n;
    stats.t_prompt_ms = static_cast<int64_t>(timings.prompt_ms);
    stats.t_generation_ms = static_cast<int64_t>(timings.predicted_ms);
    stats.prompt_per_second = timings.prompt_per_second;
    stats.predicted_per_second = timings.predicted_per_second;

    // Estimate cached tokens
    if (timings.cache_n > 0) {
        stats.n_prompt_cached = timings.cache_n;
    }
}

}  // namespace cli_stats_display
