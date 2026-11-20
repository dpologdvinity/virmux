/**
 * @file cli_parse.hpp
 * @brief Strict, non-throwing parsing helpers for numeric CLI arguments.
 *
 * These replace `std::atoi`, which silently maps malformed input such as
 * "abc" to 0 and truncates "5abc" to 5, and has undefined behaviour on
 * overflow.
 */

#pragma once

#include <limits>
#include <optional>
#include <string_view>

namespace virmux {

/**
 * @brief Parses a strictly decimal, positive integer in `[1, max_value]`.
 *
 * The whole of @p text must be consumed: leading/trailing whitespace, signs,
 * radix prefixes, fractional parts and trailing garbage are all rejected, as
 * are values that overflow `int` or exceed @p max_value.
 *
 * @param text Candidate argument text (e.g. the value following `--tasks`).
 * @param max_value Inclusive upper bound accepted by the caller.
 * @return The parsed value, or std::nullopt if @p text is not valid.
 */
std::optional<int> parse_positive_int(
    std::string_view text,
    int max_value = std::numeric_limits<int>::max()) noexcept;

}  // namespace virmux
