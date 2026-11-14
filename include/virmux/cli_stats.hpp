/**
 * @file cli_stats.hpp
 * @brief Header for CLI stats and pack subcommands and telemetry formatting.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "virmux/ksm.hpp"
#include "virmux/memory_manager.hpp"
#include "virmux/telemetry.hpp"

namespace virmux {

/**
 * @brief Formats a byte size into a human-readable string (e.g. "1.5 GiB",
 * "256.0 MiB").
 */
[[nodiscard]] std::string format_bytes(uint64_t bytes);

/**
 * @brief Generates a valid JSON string representing host memory, KSM, and zram
 * telemetry.
 */
[[nodiscard]] std::string format_stats_json(const HostMemoryStats& host,
                                            const KsmStats& ksm);

/**
 * @brief Prints an ASCII table representation of memory telemetry to stdout.
 */
void print_stats_table(const HostMemoryStats& host, const KsmStats& ksm);

/**
 * @brief Executes the `virmux stats` CLI subcommand.
 * @return 0 on success, non-zero on failure.
 */
int cmd_stats(int argc, char* const argv[]);

/**
 * @brief Executes the `virmux pack` CLI subcommand.
 * @return 0 on success, non-zero on failure.
 */
int cmd_pack(int argc, char* const argv[]);

}  // namespace virmux
