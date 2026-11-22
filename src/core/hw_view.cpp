/**
 * @file hw_view.cpp
 * @brief Derivation of the sandbox's synthetic hardware view.
 */

#include "virmux/hw_view.hpp"

#include <sstream>

namespace virmux {

MemoryLimits compute_memory_limits(uint64_t limit_bytes) {
  MemoryLimits m;
  m.max = limit_bytes;
  // max - max/16 is 15/16 of max without the overflow of max*15/16.
  m.high = limit_bytes - limit_bytes / 16;
  m.swap_max = limit_bytes / 4;
  return m;
}

std::string generate_synthetic_meminfo(const MemoryLimits& limits) {
  const uint64_t total_kb = limits.max / 1024;
  const uint64_t free_kb = (total_kb / 4) * 3;
  const uint64_t avail_kb = (total_kb / 8) * 7;
  const uint64_t swap_total_kb = limits.swap_max / 1024;

  std::ostringstream oss;
  oss << "MemTotal:        " << total_kb << " kB\n"
      << "MemFree:         " << free_kb << " kB\n"
      << "MemAvailable:    " << avail_kb << " kB\n"
      << "SwapTotal:       " << swap_total_kb << " kB\n"
      << "SwapFree:        " << swap_total_kb << " kB\n";
  return oss.str();
}

}  // namespace virmux
