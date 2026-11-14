/**
 * @file cli_cas.hpp
 * @brief Header for CLI cas subcommand and cache formatting.
 */

#pragma once

#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>

#include "virmux/cas.hpp"

namespace virmux {

/**
 * @brief Parses a size string with optional unit suffix into bytes.
 * Supports units like B, K, M, G, T. Suffix matching is case-insensitive.
 * Supports "0" for 0 bytes.
 * @param str Size string representation.
 * @param out_bytes Output variable receiving the parsed byte count.
 * @return True on success, false on invalid format or negative value.
 */
bool parse_size_bytes(std::string_view str, uint64_t& out_bytes);

/**
 * @brief Formats byte size with two decimal precision for CAS output (e.g.
 * "12.40 MiB").
 * @param bytes Number of bytes.
 * @return Formatted size string.
 */
[[nodiscard]] std::string format_cas_bytes(uint64_t bytes);

/**
 * @brief Formats CAS cache statistics into tabular ASCII representation.
 * @param cache_root Root path of the CAS cache.
 * @param stats CAS store statistics.
 * @return Formatted ASCII table string.
 */
[[nodiscard]] std::string format_cas_stats_table(const std::string& cache_root,
                                                 const CasStats& stats);

/**
 * @brief Formats CAS cache statistics into valid JSON.
 * @param cache_root Root path of the CAS cache.
 * @param stats CAS store statistics.
 * @return Formatted JSON string.
 */
[[nodiscard]] std::string format_cas_stats_json(const std::string& cache_root,
                                                const CasStats& stats);

/**
 * @brief Executes the `virmux cas` CLI subcommand.
 * Supports: `virmux cas stats [--json]` and `virmux cas clean [--max-bytes
 * <SIZE>]`.
 * @return 0 on success, non-zero on failure.
 */
int cmd_cas(int argc, char* const argv[]);

/**
 * @brief Prints usage instructions for `virmux cas`.
 * @param os Output stream to write usage text to.
 */
void print_cas_usage(std::ostream& os);

/**
 * @brief Prints usage instructions for `virmux cas stats`.
 * @param os Output stream to write usage text to.
 */
void print_cas_stats_usage(std::ostream& os);

/**
 * @brief Prints usage instructions for `virmux cas clean`.
 * @param os Output stream to write usage text to.
 */
void print_cas_clean_usage(std::ostream& os);

}  // namespace virmux
