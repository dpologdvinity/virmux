/**
 * @file telemetry.cpp
 * @brief Implementation of process memory, host meminfo, and zram telemetry
 * parser.
 */

#include "virmux/telemetry.hpp"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <utility>

namespace virmux {

namespace {

/**
 * @brief Trim leading and trailing whitespace from string_view.
 */
std::string trim(std::string_view sv) {
  while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' ||
                         sv.front() == '\r' || sv.front() == '\n')) {
    sv.remove_prefix(1);
  }
  while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' ||
                         sv.back() == '\r' || sv.back() == '\n')) {
    sv.remove_suffix(1);
  }
  return std::string(sv);
}

/**
 * @brief Parse a single colon-separated line formatted as "Key:\s*<val>".
 * @param line Input line from procfs/sysfs.
 * @param key Output extracted key.
 * @param val Output parsed numeric integer.
 * @return True if key and integer value parsed successfully.
 */
bool parse_key_value_kb(const std::string& line, std::string& key,
                        uint64_t& val) {
  auto colon_pos = line.find(':');
  if (colon_pos == std::string::npos) {
    return false;
  }

  std::string_view k_sv(line.data(), colon_pos);
  while (!k_sv.empty() && (k_sv.front() == ' ' || k_sv.front() == '\t')) {
    k_sv.remove_prefix(1);
  }
  while (!k_sv.empty() && (k_sv.back() == ' ' || k_sv.back() == '\t')) {
    k_sv.remove_suffix(1);
  }
  key = std::string(k_sv);

  std::string_view v_sv(line.data() + colon_pos + 1,
                        line.size() - colon_pos - 1);
  while (!v_sv.empty() && (v_sv.front() == ' ' || v_sv.front() == '\t')) {
    v_sv.remove_prefix(1);
  }

  val = 0;
  auto [ptr, ec] = std::from_chars(v_sv.data(), v_sv.data() + v_sv.size(), val);
  return ec == std::errc();
}

/**
 * @brief Safely read the entire contents of a file as a string.
 */
std::string read_file_string(const std::filesystem::path& path) {
  std::ifstream ifs(path);
  if (!ifs.is_open()) {
    return {};
  }
  std::stringstream ss;
  ss << ifs.rdbuf();
  return ss.str();
}

}  // namespace

MemoryTelemetry::MemoryTelemetry(std::string procfs_root,
                                 std::string sysfs_block_root)
    : procfs_root_(std::move(procfs_root)),
      sysfs_block_root_(std::move(sysfs_block_root)) {}

ProcessMemoryStats MemoryTelemetry::read_process_stats(pid_t pid) const {
  ProcessMemoryStats stats;
  stats.pid = pid;

  if (pid <= 0) {
    return stats;
  }

  const std::filesystem::path pid_dir =
      std::filesystem::path(procfs_root_) / std::to_string(pid);
  const std::filesystem::path smaps_path = pid_dir / "smaps_rollup";

  std::ifstream smaps_file(smaps_path);
  if (smaps_file.is_open()) {
    std::string line;
    std::string key;
    uint64_t val_kb = 0;

    while (std::getline(smaps_file, line)) {
      if (!parse_key_value_kb(line, key, val_kb)) {
        continue;
      }

      const uint64_t val_bytes = val_kb * 1024ULL;
      if (key == "Rss") {
        stats.rss_bytes = val_bytes;
      } else if (key == "Pss") {
        stats.pss_bytes = val_bytes;
      } else if (key == "Swap") {
        stats.swap_bytes = val_bytes;
      } else if (key == "SwapPss") {
        stats.swap_pss_bytes = val_bytes;
      } else if (key == "Referenced") {
        stats.referenced_bytes = val_bytes;
      } else if (key == "Private_Dirty") {
        stats.private_dirty_bytes = val_bytes;
      }
    }
    return stats;
  }

  // Fallback: read /proc/[pid]/status
  const std::filesystem::path status_path = pid_dir / "status";
  std::ifstream status_file(status_path);
  if (status_file.is_open()) {
    std::string line;
    std::string key;
    uint64_t val_kb = 0;

    while (std::getline(status_file, line)) {
      if (!parse_key_value_kb(line, key, val_kb)) {
        continue;
      }

      const uint64_t val_bytes = val_kb * 1024ULL;
      if (key == "VmRSS") {
        stats.rss_bytes = val_bytes;
      } else if (key == "VmSwap") {
        stats.swap_bytes = val_bytes;
      }
    }
  }

  return stats;
}

HostMemoryStats MemoryTelemetry::read_host_stats() const {
  HostMemoryStats stats;
  const std::filesystem::path meminfo_path =
      std::filesystem::path(procfs_root_) / "meminfo";

  std::ifstream meminfo_file(meminfo_path);
  if (meminfo_file.is_open()) {
    std::string line;
    std::string key;
    uint64_t val_kb = 0;

    while (std::getline(meminfo_file, line)) {
      if (!parse_key_value_kb(line, key, val_kb)) {
        continue;
      }

      const uint64_t val_bytes = val_kb * 1024ULL;
      if (key == "MemTotal") {
        stats.mem_total_bytes = val_bytes;
      } else if (key == "MemAvailable") {
        stats.mem_available_bytes = val_bytes;
      } else if (key == "MemFree") {
        stats.mem_free_bytes = val_bytes;
      } else if (key == "Cached") {
        stats.cached_bytes = val_bytes;
      } else if (key == "SwapTotal") {
        stats.swap_total_bytes = val_bytes;
      } else if (key == "SwapFree") {
        stats.swap_free_bytes = val_bytes;
      }
    }
  }

  stats.zram_devices = read_zram_stats();
  return stats;
}

std::vector<ZramDeviceStats> MemoryTelemetry::read_zram_stats() const {
  std::vector<ZramDeviceStats> result;

  std::error_code ec;
  std::filesystem::path block_root(sysfs_block_root_);
  if (!std::filesystem::exists(block_root, ec) ||
      !std::filesystem::is_directory(block_root, ec)) {
    return result;
  }

  for (const auto& entry :
       std::filesystem::directory_iterator(block_root, ec)) {
    if (ec) {
      break;
    }

    const std::string dev_name = entry.path().filename().string();
    if (!dev_name.starts_with("zram")) {
      continue;
    }

    // Verify it is a directory or symlink pointing to a directory
    if (!entry.is_directory(ec) || ec) {
      continue;
    }

    ZramDeviceStats dev;
    dev.present = true;
    dev.name = dev_name;

    // Parse comp_algorithm
    const std::filesystem::path algo_path = entry.path() / "comp_algorithm";
    const std::string algo_content = read_file_string(algo_path);
    const auto open_bracket = algo_content.find('[');
    const auto close_bracket = algo_content.find(']');
    if (open_bracket != std::string::npos &&
        close_bracket != std::string::npos && close_bracket > open_bracket) {
      dev.algorithm = algo_content.substr(open_bracket + 1,
                                          close_bracket - open_bracket - 1);
    } else {
      dev.algorithm = trim(algo_content);
    }

    // Parse disksize
    const std::filesystem::path disksize_path = entry.path() / "disksize";
    const std::string disksize_content = trim(read_file_string(disksize_path));
    if (!disksize_content.empty()) {
      uint64_t dsize = 0;
      auto [ptr, conv_ec] = std::from_chars(
          disksize_content.data(),
          disksize_content.data() + disksize_content.size(), dsize);
      if (conv_ec == std::errc()) {
        dev.disk_size = dsize;
      }
    }

    // Parse mm_stat: orig_data_size compr_data_size mem_used_total ...
    const std::filesystem::path mm_stat_path = entry.path() / "mm_stat";
    const std::string mm_stat_content = read_file_string(mm_stat_path);
    if (!mm_stat_content.empty()) {
      std::istringstream iss(mm_stat_content);
      iss >> dev.orig_data_size >> dev.compr_data_size >> dev.mem_used_total;
      if (dev.compr_data_size > 0) {
        dev.compression_ratio = static_cast<double>(dev.orig_data_size) /
                                static_cast<double>(dev.compr_data_size);
      } else {
        dev.compression_ratio = 1.0;
      }
    }

    result.push_back(std::move(dev));
  }

  std::sort(result.begin(), result.end(),
            [](const ZramDeviceStats& a, const ZramDeviceStats& b) {
              return a.name < b.name;
            });

  return result;
}

}  // namespace virmux
