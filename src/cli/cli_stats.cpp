/**
 * @file cli_stats.cpp
 * @brief Implementation of CLI stats and pack subcommands and telemetry
 * formatting.
 */

#include "virmux/cli_stats.hpp"

#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace virmux {

namespace {

std::string escape_json_string(const std::string& input) {
  std::string output;
  output.reserve(input.size());
  for (char c : input) {
    if (c == '"') {
      output += "\\\"";
    } else if (c == '\\') {
      output += "\\\\";
    } else if (c == '\b') {
      output += "\\b";
    } else if (c == '\f') {
      output += "\\f";
    } else if (c == '\n') {
      output += "\\n";
    } else if (c == '\r') {
      output += "\\r";
    } else if (c == '\t') {
      output += "\\t";
    } else if (static_cast<unsigned char>(c) < 0x20) {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\u%04x",
                    static_cast<unsigned int>(static_cast<unsigned char>(c)));
      output += buf;
    } else {
      output += c;
    }
  }
  return output;
}

void print_stats_usage(std::ostream& out) {
  out << "Usage:\n"
      << "  virmux stats [OPTIONS]\n\n"
      << "Display live system memory, KSM deduplication, and zram "
         "telemetry.\n\n"
      << "Options:\n"
      << "  --json       Output telemetry in JSON format\n"
      << "  -h, --help   Show this help message\n";
}

void print_pack_usage(std::ostream& out) {
  out << "Usage:\n"
      << "  virmux pack [PID ... | --all]\n\n"
      << "Advise and compact anonymous process memory into swap/zram.\n\n"
      << "Options:\n"
      << "  --all        Compact all processes in active virmux cgroups\n"
      << "  -h, --help   Show this help message\n";
}

}  // namespace

std::string format_bytes(uint64_t bytes) {
  char buf[64];
  constexpr uint64_t kib = 1024ULL;
  constexpr uint64_t mib = 1024ULL * 1024ULL;
  constexpr uint64_t gib = 1024ULL * 1024ULL * 1024ULL;

  if (bytes < kib) {
    std::snprintf(buf, sizeof(buf), "%llu B",
                  static_cast<unsigned long long>(bytes));
  } else if (bytes < mib) {
    std::snprintf(buf, sizeof(buf), "%.1f KiB",
                  static_cast<double>(bytes) / static_cast<double>(kib));
  } else if (bytes < gib) {
    std::snprintf(buf, sizeof(buf), "%.1f MiB",
                  static_cast<double>(bytes) / static_cast<double>(mib));
  } else {
    std::snprintf(buf, sizeof(buf), "%.2f GiB",
                  static_cast<double>(bytes) / static_cast<double>(gib));
  }
  return std::string(buf);
}

std::string format_stats_json(const HostMemoryStats& host,
                              const KsmStats& ksm) {
  std::ostringstream oss;
  oss << "{\n";
  oss << "  \"host_memory\": {\n";
  oss << "    \"total_bytes\": " << host.mem_total_bytes << ",\n";
  oss << "    \"available_bytes\": " << host.mem_available_bytes << ",\n";
  oss << "    \"free_bytes\": " << host.mem_free_bytes << ",\n";
  oss << "    \"cached_bytes\": " << host.cached_bytes << ",\n";
  oss << "    \"swap_total_bytes\": " << host.swap_total_bytes << ",\n";
  oss << "    \"swap_free_bytes\": " << host.swap_free_bytes << "\n";
  oss << "  },\n";
  oss << "  \"ksm\": {\n";
  oss << "    \"present\": " << (ksm.present ? "true" : "false") << ",\n";
  oss << "    \"enabled\": " << (ksm.enabled ? "true" : "false") << ",\n";
  oss << "    \"pages_shared\": " << ksm.pages_shared << ",\n";
  oss << "    \"pages_sharing\": " << ksm.pages_sharing << ",\n";
  oss << "    \"pages_unshared\": " << ksm.pages_unshared << ",\n";
  oss << "    \"pages_volatile\": " << ksm.pages_volatile << ",\n";
  oss << "    \"bytes_saved\": " << ksm.bytes_saved << "\n";
  oss << "  },\n";
  oss << "  \"zram\": [";
  if (!host.zram_devices.empty()) {
    oss << "\n";
    for (size_t i = 0; i < host.zram_devices.size(); ++i) {
      const auto& dev = host.zram_devices[i];
      char ratio_buf[32];
      auto [r_ptr, r_ec] =
          std::to_chars(ratio_buf, ratio_buf + sizeof(ratio_buf) - 1,
                        dev.compression_ratio, std::chars_format::fixed, 2);
      if (r_ec == std::errc{}) {
        *r_ptr = '\0';
      } else {
        std::snprintf(ratio_buf, sizeof(ratio_buf), "%.2f",
                      dev.compression_ratio);
      }
      oss << "    {\n";
      oss << "      \"name\": \"" << escape_json_string(dev.name) << "\",\n";
      oss << "      \"algorithm\": \"" << escape_json_string(dev.algorithm)
          << "\",\n";
      oss << "      \"disk_size_bytes\": " << dev.disk_size << ",\n";
      oss << "      \"orig_data_size_bytes\": " << dev.orig_data_size << ",\n";
      oss << "      \"compr_data_size_bytes\": " << dev.compr_data_size
          << ",\n";
      oss << "      \"mem_used_total_bytes\": " << dev.mem_used_total << ",\n";
      oss << "      \"compression_ratio\": " << ratio_buf << "\n";
      oss << "    }" << (i + 1 < host.zram_devices.size() ? "," : "") << "\n";
    }
    oss << "  ]\n";
  } else {
    oss << "]\n";
  }
  oss << "}";
  return oss.str();
}

void print_stats_table(const HostMemoryStats& host, const KsmStats& ksm) {
  std::cout << "=== Host Physical & Swap Memory ===\n";
  std::cout << "  Total:          " << format_bytes(host.mem_total_bytes)
            << "\n";
  std::cout << "  Available:      " << format_bytes(host.mem_available_bytes)
            << "\n";
  std::cout << "  Free:           " << format_bytes(host.mem_free_bytes)
            << "\n";
  std::cout << "  Cached:         " << format_bytes(host.cached_bytes) << "\n";
  std::cout << "  Swap Total:     " << format_bytes(host.swap_total_bytes)
            << "\n";
  std::cout << "  Swap Free:      " << format_bytes(host.swap_free_bytes)
            << "\n\n";

  std::cout << "=== Kernel Samepage Merging (KSM) ===\n";
  std::cout << "  Present:        " << (ksm.present ? "yes" : "no") << "\n";
  std::cout << "  Enabled:        " << (ksm.enabled ? "yes" : "no") << "\n";
  std::cout << "  Pages Shared:   " << ksm.pages_shared << "\n";
  std::cout << "  Pages Sharing:  " << ksm.pages_sharing << "\n";
  std::cout << "  Pages Unshared: " << ksm.pages_unshared << "\n";
  std::cout << "  Pages Volatile: " << ksm.pages_volatile << "\n";
  std::cout << "  Bytes Saved:    " << format_bytes(ksm.bytes_saved) << "\n\n";

  std::cout << "=== zram In-RAM Compressed Swap ===\n";
  if (host.zram_devices.empty()) {
    std::cout << "  No active zram devices found.\n";
  } else {
    for (const auto& dev : host.zram_devices) {
      std::cout << "  Device:         " << dev.name << "\n";
      std::cout << "  Algorithm:      " << dev.algorithm << "\n";
      std::cout << "  Disk Size:      " << format_bytes(dev.disk_size) << "\n";
      std::cout << "  Original Size:  " << format_bytes(dev.orig_data_size)
                << "\n";
      std::cout << "  Compressed:     " << format_bytes(dev.compr_data_size)
                << "\n";
      std::cout << "  Memory Used:    " << format_bytes(dev.mem_used_total)
                << "\n";
      char ratio_buf[32];
      std::snprintf(ratio_buf, sizeof(ratio_buf), "%.2f:1",
                    dev.compression_ratio);
      std::cout << "  Ratio:          " << ratio_buf << "\n";
    }
  }
}

int cmd_stats(int argc, char* const argv[]) {
  if (argv == nullptr || argc <= 0) {
    return 1;
  }

  int start_idx = 0;
  if (argc > 1 && argv[1] != nullptr && std::string_view(argv[1]) == "stats") {
    start_idx = 2;
  } else if (argv[0] != nullptr) {
    std::string_view a0(argv[0]);
    if (a0 == "stats" || a0.ends_with("/stats") || a0 == "virmux" ||
        a0.ends_with("/virmux")) {
      start_idx = 1;
    }
  }

  bool json_mode = false;
  for (int i = start_idx; i < argc; ++i) {
    if (argv[i] == nullptr) {
      std::cerr << "Error: null argument at position " << i << "\n";
      return 1;
    }
    std::string_view arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      print_stats_usage(std::cout);
      return 0;
    }
    if (arg == "--json") {
      json_mode = true;
    } else {
      std::cerr << "Error: unrecognized option '" << arg << "'\n";
      print_stats_usage(std::cerr);
      return 1;
    }
  }

  MemoryManager mgr;
  HostMemoryStats host = mgr.get_host_stats();
  KsmStats ksm = mgr.get_ksm_stats();

  if (json_mode) {
    std::cout << format_stats_json(host, ksm) << "\n";
  } else {
    print_stats_table(host, ksm);
  }

  return 0;
}

int cmd_pack(int argc, char* const argv[]) {
  if (argv == nullptr || argc <= 0) {
    return 1;
  }

  int start_idx = 0;
  if (argc > 1 && argv[1] != nullptr && std::string_view(argv[1]) == "pack") {
    start_idx = 2;
  } else if (argv[0] != nullptr) {
    std::string_view a0(argv[0]);
    if (a0 == "pack" || a0.ends_with("/pack") || a0 == "virmux" ||
        a0.ends_with("/virmux")) {
      start_idx = 1;
    }
  }

  bool all_mode = false;
  std::vector<pid_t> pids;

  for (int i = start_idx; i < argc; ++i) {
    if (argv[i] == nullptr) {
      std::cerr << "Error: null argument at position " << i << "\n";
      return 1;
    }
    std::string_view arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      print_pack_usage(std::cout);
      return 0;
    }
    if (arg == "--all") {
      all_mode = true;
    } else if (arg.starts_with('-')) {
      std::cerr << "Error: unrecognized option '" << arg << "'\n";
      print_pack_usage(std::cerr);
      return 1;
    } else {
      char* end_ptr = nullptr;
      errno = 0;
      std::string s(arg);
      long val = std::strtol(s.c_str(), &end_ptr, 10);
      if (end_ptr == s.c_str() || *end_ptr != '\0' || errno == ERANGE ||
          val <= 0 || val > std::numeric_limits<pid_t>::max()) {
        std::cerr << "Error: invalid PID '" << arg
                  << "' (must be a positive integer)\n";
        return 1;
      }
      pids.push_back(static_cast<pid_t>(val));
    }
  }

  MemoryManager mgr;
  size_t total_compacted = 0;
  uint64_t total_bytes_advised = 0;

  if (pids.empty() || all_mode) {
    std::error_code ec;
    std::filesystem::path cgroup_root = "/sys/fs/cgroup";
    if (std::filesystem::exists(cgroup_root, ec) &&
        std::filesystem::is_directory(cgroup_root, ec)) {
      std::filesystem::directory_iterator dir_it(
          cgroup_root,
          std::filesystem::directory_options::skip_permission_denied, ec);
      std::filesystem::directory_iterator dir_end;
      while (!ec && dir_it != dir_end) {
        if (dir_it->is_directory(ec)) {
          std::string dir_name = dir_it->path().filename().string();
          if (dir_name.starts_with("virmux")) {
            std::filesystem::path procs_file = dir_it->path() / "cgroup.procs";
            if (std::filesystem::exists(procs_file, ec)) {
              total_compacted += mgr.compact_cgroup_procs(procs_file.string());
            }
            std::filesystem::recursive_directory_iterator sub_it(
                dir_it->path(),
                std::filesystem::directory_options::skip_permission_denied, ec);
            std::filesystem::recursive_directory_iterator sub_end;
            while (!ec && sub_it != sub_end) {
              if (sub_it->is_regular_file(ec) &&
                  sub_it->path().filename() == "cgroup.procs") {
                if (sub_it->path() != procs_file) {
                  total_compacted +=
                      mgr.compact_cgroup_procs(sub_it->path().string());
                }
              }
              sub_it.increment(ec);
            }
            ec.clear();
          }
        }
        dir_it.increment(ec);
      }
    }
  }

  for (pid_t pid : pids) {
    auto res = mgr.compact_process(pid);
    if (res.success) {
      ++total_compacted;
      total_bytes_advised += res.bytes_advised;
    }
  }

  std::cout << "Compacted " << total_compacted << " process(es)";
  if (total_bytes_advised > 0) {
    std::cout << ", advised " << format_bytes(total_bytes_advised);
  }
  std::cout << ".\n";

  return 0;
}

}  // namespace virmux
