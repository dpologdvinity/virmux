#pragma once

/**
 * @file browser_trim.hpp
 * @brief Headless browser detection and argument injection interface.
 */

#include <string>
#include <vector>

namespace virmux {

/**
 * @brief Check if a binary path or name corresponds to a headless browser.
 *
 * Matches if the path or executable name contains "chrome", "chromium", or
 * "headless_shell".
 *
 * @param path File path or binary name to inspect.
 * @return true if target binary is identified as a browser, false otherwise.
 */
[[nodiscard]] bool is_browser_binary(const char* path) noexcept;

/**
 * @brief Augment argument vector with ultra-slim browser profile flags.
 *
 * Keeps argv[0], inserts slim flags immediately after argv[0], and appends
 * any remaining original arguments.
 *
 * @param argv Null-terminated argument vector.
 * @return Vector of string arguments including injected slim flags.
 */
[[nodiscard]] std::vector<std::string> inject_slim_flags(char* const argv[]);

}  // namespace virmux
