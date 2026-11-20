/**
 * @file fuzz_size_bytes.cpp
 * @brief libFuzzer harness for parse_size_bytes().
 *
 * Why this target: `virmux cas clean --max-bytes=<SIZE>` converts a
 * user-supplied string to a byte budget through strtod() plus a unit
 * multiplier. Float-to-integer conversion of out-of-range values is undefined
 * behaviour, so this checks the range guards hold for every spelling strtod
 * accepts (hex floats, exponents, inf/nan, embedded NULs, whitespace).
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "virmux/cli_cas.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  std::string_view str(reinterpret_cast<const char*>(data), size);
  uint64_t bytes = 0;
  if (virmux::parse_size_bytes(str, bytes)) {
    // Formatting the accepted value is what `cas clean` does next.
    (void)virmux::format_cas_bytes(bytes);
  }
  return 0;
}
