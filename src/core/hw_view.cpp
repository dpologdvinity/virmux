/**
 * @file hw_view.cpp
 * @brief Derivation of the sandbox's synthetic hardware view.
 */

#include "virmux/hw_view.hpp"

#include <charconv>
#include <cstring>
#include <iterator>
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

std::vector<std::pair<std::string, std::string>> compute_thread_env(
    int n, GetenvFn getenv_fn) {
  static constexpr const char* kVars[] = {
      "OMP_NUM_THREADS",  "OPENBLAS_NUM_THREADS",
      "MKL_NUM_THREADS",  "NUMEXPR_NUM_THREADS",
      "GOMAXPROCS",       "RAYON_NUM_THREADS",
      "CARGO_BUILD_JOBS", "CMAKE_BUILD_PARALLEL_LEVEL",
      "PYTHON_CPU_COUNT",
  };
  const int cap = n < 1 ? 1 : n;
  const std::string cap_str = std::to_string(cap);

  std::vector<std::pair<std::string, std::string>> env;
  env.reserve(std::size(kVars));
  for (const char* name : kVars) {
    const char* user = getenv_fn != nullptr ? getenv_fn(name) : nullptr;
    bool keep = false;
    // Only plain positive decimals qualify: no sign, space or leading zero.
    if (user != nullptr && *user >= '1' && *user <= '9') {
      const char* end = user + std::strlen(user);
      int value = 0;
      const auto [ptr, ec] = std::from_chars(user, end, value);
      keep = ec == std::errc() && ptr == end && value <= cap;
    }
    env.emplace_back(name, keep ? std::string(user) : cap_str);
  }
  return env;
}

}  // namespace virmux
