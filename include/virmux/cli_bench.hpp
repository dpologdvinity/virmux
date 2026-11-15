/**
 * @file cli_bench.hpp
 * @brief Header for CLI bench subcommand and benchmark reporting.
 */

#pragma once

namespace virmux {

/**
 * @brief Executes the `virmux bench` CLI subcommand.
 *
 * Subcommands:
 *   - `virmux bench run [--tasks 12] [--duration 5] [--quick] [--mode all|managed|baseline] [--output <path>] [--json]`
 *   - `virmux bench report [--input <path>]`
 *   - `virmux bench --help`
 *
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return 0 on success, non-zero on failure.
 */
int cmd_bench(int argc, char* const argv[]);

}  // namespace virmux
