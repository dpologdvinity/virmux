/**
 * @file cli_daemon.hpp
 * @brief Header for CLI daemon, list, freeze, thaw, and kill subcommands.
 */

#pragma once

#include <string>
#include <vector>

#include "virmux/daemon.hpp"
#include "virmux/session.hpp"

namespace virmux {

/**
 * @brief Formats a list of sessions into a formatted ASCII table.
 * @param sessions List of session snapshots to format.
 * @return Formatted ASCII table string.
 */
[[nodiscard]] std::string format_session_table(
    const std::vector<SessionInfo>& sessions);

/**
 * @brief Formats a list of sessions into a JSON array string.
 * @param sessions List of session snapshots to format.
 * @return JSON array string.
 */
[[nodiscard]] std::string format_session_json(
    const std::vector<SessionInfo>& sessions);

/**
 * @brief Executes the `virmux daemon` CLI subcommand.
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return 0 on success, non-zero on failure.
 */
int cmd_daemon(int argc, char* const argv[]);

/**
 * @brief Executes the `virmux list` CLI subcommand.
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return 0 on success, non-zero on failure.
 */
int cmd_list(int argc, char* const argv[]);

/**
 * @brief Executes the `virmux freeze` CLI subcommand.
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return 0 on success, non-zero on failure.
 */
int cmd_freeze(int argc, char* const argv[]);

/**
 * @brief Executes the `virmux thaw` CLI subcommand.
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return 0 on success, non-zero on failure.
 */
int cmd_thaw(int argc, char* const argv[]);

/**
 * @brief Executes the `virmux kill` CLI subcommand.
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return 0 on success, non-zero on failure.
 */
int cmd_kill(int argc, char* const argv[]);

}  // namespace virmux
