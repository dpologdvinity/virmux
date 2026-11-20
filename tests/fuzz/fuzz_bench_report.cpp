/**
 * @file fuzz_bench_report.cpp
 * @brief libFuzzer harness for benchmark comparison JSON parsing and
 * rendering.
 *
 * Why this target: `virmux bench report [FILE|-]` reads a user-supplied (often
 * CI-artifact or downloaded) JSON file via parse_comparison_json() and then
 * renders it with format_comparison_table(). Both use hand-rolled numeric
 * extraction and fixed-size snprintf buffers, so crafted numbers (huge,
 * negative, NaN-producing) are the main risk.
 *
 * The parsed result is also re-serialised with format_comparison_json() and
 * parsed again, which must succeed (round-trip property).
 */

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "virmux/bench.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  std::string_view json(reinterpret_cast<const char*>(data), size);
  std::optional<virmux::BenchmarkComparison> comp =
      virmux::parse_comparison_json(json);
  if (!comp) {
    return 0;
  }
  std::string table = virmux::format_comparison_table(*comp);
  std::string reserialised = virmux::format_comparison_json(*comp);
  if (!virmux::parse_comparison_json(reserialised).has_value()) {
    __builtin_trap();
  }
  (void)table;
  return 0;
}
