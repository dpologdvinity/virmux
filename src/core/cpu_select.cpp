/**
 * @file cpu_select.cpp
 * @brief CPU list parsing/formatting and sandbox CPU selection.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "virmux/cpu_select.hpp"

#include <sched.h>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>

namespace virmux {

namespace {

/** @brief Strip leading/trailing ASCII whitespace. */
std::string_view trim(std::string_view s) {
  const char* ws = " \t\r\n";
  const size_t begin = s.find_first_not_of(ws);
  if (begin == std::string_view::npos) {
    return {};
  }
  const size_t end = s.find_last_not_of(ws);
  return s.substr(begin, end - begin + 1);
}

/** @brief Parse a decimal CPU id in [0, kMaxCpuId]; the whole view must be
 * consumed. */
std::optional<int> parse_cpu_id(std::string_view s) {
  if (s.empty() || s.front() < '0' || s.front() > '9') {
    return std::nullopt;
  }
  int value = 0;
  const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
  if (ec != std::errc() || ptr != s.data() + s.size() || value > kMaxCpuId) {
    return std::nullopt;
  }
  return value;
}

/** @brief CPUs in the calling thread's affinity mask. */
CpuList affinity_cpus() {
  CpuList cpus;
  cpu_set_t mask;
  CPU_ZERO(&mask);
  if (::sched_getaffinity(0, sizeof(mask), &mask) != 0) {
    return cpus;
  }
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
    if (CPU_ISSET(cpu, &mask)) {
      cpus.push_back(cpu);
    }
  }
  return cpus;
}

}  // namespace

std::optional<CpuList> parse_cpu_list(std::string_view text) {
  text = trim(text);
  if (text.empty()) {
    return std::nullopt;
  }
  CpuList cpus;
  while (true) {
    const size_t comma = text.find(',');
    const std::string_view item = text.substr(0, comma);
    const size_t dash = item.find('-');
    const auto first = parse_cpu_id(item.substr(0, dash));
    if (!first) {
      return std::nullopt;
    }
    int last = *first;
    if (dash != std::string_view::npos) {
      const auto parsed_last = parse_cpu_id(item.substr(dash + 1));
      if (!parsed_last || *parsed_last < *first) {
        return std::nullopt;
      }
      last = *parsed_last;
    }
    for (int cpu = *first; cpu <= last; ++cpu) {
      cpus.push_back(cpu);
    }
    if (comma == std::string_view::npos) {
      break;
    }
    text.remove_prefix(comma + 1);
  }
  std::sort(cpus.begin(), cpus.end());
  cpus.erase(std::unique(cpus.begin(), cpus.end()), cpus.end());
  return cpus;
}

std::string format_cpu_list(const CpuList& cpus) {
  std::string out;
  for (size_t i = 0; i < cpus.size();) {
    size_t j = i;
    while (j + 1 < cpus.size() && cpus[j + 1] == cpus[j] + 1) {
      ++j;
    }
    if (!out.empty()) {
      out += ',';
    }
    out += std::to_string(cpus[i]);
    if (j > i) {
      out += '-';
      out += std::to_string(cpus[j]);
    }
    i = j + 1;
  }
  return out;
}

CpuList available_cpus() {
  CpuList affinity = affinity_cpus();
  std::ifstream in("/sys/devices/system/cpu/online");
  std::ostringstream text;
  text << in.rdbuf();
  const auto online = parse_cpu_list(text.str());
  if (!online) {
    return affinity;
  }
  CpuList both;
  std::set_intersection(online->begin(), online->end(), affinity.begin(),
                        affinity.end(), std::back_inserter(both));
  return both;
}

CpuList select_cpus(const CpuList& avail, int n, uint64_t rotation,
                    const std::vector<double>& load) {
  if (avail.empty()) {
    return {};
  }
  // Keep the two lowest CPUs for the interactive shell on machines that can
  // spare them, unless the request needs them.
  constexpr size_t kReserved = 2;
  constexpr size_t kMinForReserve = 4;
  const size_t want =
      std::clamp<size_t>(n < 1 ? 1 : static_cast<size_t>(n), 1, avail.size());
  auto pool_begin = avail.begin();
  if (avail.size() >= kMinForReserve && want <= avail.size() - kReserved) {
    pool_begin += kReserved;
  }
  const CpuList pool(pool_begin, avail.end());
  const size_t size = pool.size();

  auto cpu_load = [&load](int cpu) {
    const auto idx = static_cast<size_t>(cpu);
    return idx < load.size() ? load[idx] : 0.0;
  };

  // Scan every window start in rotation order; the first least-loaded wins,
  // so with no load information the rotation alone decides.
  const size_t first = static_cast<size_t>(rotation % size);
  size_t best_start = first;
  double best_load = std::numeric_limits<double>::infinity();
  for (size_t k = 0; k < size; ++k) {
    const size_t start = (first + k) % size;
    double total = 0.0;
    for (size_t i = 0; i < want; ++i) {
      total += cpu_load(pool[(start + i) % size]);
    }
    if (total < best_load) {
      best_load = total;
      best_start = start;
    }
  }

  CpuList chosen;
  chosen.reserve(want);
  for (size_t i = 0; i < want; ++i) {
    chosen.push_back(pool[(best_start + i) % size]);
  }
  std::sort(chosen.begin(), chosen.end());
  return chosen;
}

}  // namespace virmux
