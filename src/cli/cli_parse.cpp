/**
 * @file cli_parse.cpp
 * @brief Implementation of strict numeric CLI argument parsing.
 */

#include "virmux/cli_parse.hpp"

#include <charconv>
#include <system_error>

namespace virmux {

std::optional<int> parse_positive_int(std::string_view text,
                                      int max_value) noexcept {
  // std::from_chars accepts a leading '-' for signed types but never
  // whitespace or '+', so only the sign needs rejecting explicitly; the
  // range check below covers it as well.
  if (text.empty() || text.front() == '-') {
    return std::nullopt;
  }

  int value = 0;
  const char* const first = text.data();
  const char* const last = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(first, last, value, 10);
  if (ec != std::errc{} || ptr != last) {
    // Not a number, overflowed int, or trailing characters ("5abc").
    return std::nullopt;
  }
  if (value < 1 || value > max_value) {
    return std::nullopt;
  }
  return value;
}

}  // namespace virmux
