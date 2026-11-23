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
 * @brief cgroup.procs files of every virmux-managed cgroup under
 * @p cgroup_root.
 *
 * Finds `virmux*` cgroups at the root (legacy and root-run layouts) and every
 * nested `virmux` directory, e.g. `user@1000.service/virmux/run-<pid>-<seq>`
 * in a systemd user manager, including all of their descendants.
 * @param cgroup_root cgroup2 mount point (normally /sys/fs/cgroup).
 * @return Absolute cgroup.procs paths; unreadable subtrees are skipped.
 */
[[nodiscard]] std::vector<std::string> find_virmux_cgroup_procs(
    const std::string& cgroup_root);

/**
 * @brief Executes the `virmux pack` CLI subcommand.
 * @return 0 on success, non-zero on failure.
 */
int cmd_pack(int argc, char* const argv[]);

}  // namespace virmux
