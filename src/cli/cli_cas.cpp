/**
 * @file cli_cas.cpp
 * @brief Implementation of CLI cas subcommand and cache formatting.
 */

#include "virmux/cli_cas.hpp"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

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

std::string collapse_home_path(const std::string& path) {
  const char* home = std::getenv("HOME");
  if (home != nullptr && *home != '\0') {
    std::string home_str(home);
    if (path == home_str) {
      return "~";
    }
    if (path.starts_with(home_str + "/")) {
      return "~" + path.substr(home_str.size());
    }
  }
  return path;
}

}  // namespace

bool parse_size_bytes(std::string_view str, uint64_t& out_bytes) {
  while (!str.empty() &&
         std::isspace(static_cast<unsigned char>(str.front()))) {
    str.remove_prefix(1);
  }
  while (!str.empty() && std::isspace(static_cast<unsigned char>(str.back()))) {
    str.remove_suffix(1);
  }
  if (str.empty()) {
    return false;
  }

  std::string s(str);
  char* end_ptr = nullptr;
  errno = 0;
  double val = std::strtod(s.c_str(), &end_ptr);
  if (end_ptr == s.c_str() || errno == ERANGE || !std::isfinite(val) ||
      val < 0.0) {
    return false;
  }

  std::string suffix = end_ptr;
  size_t start = suffix.find_first_not_of(" \t");
  if (start != std::string::npos) {
    suffix = suffix.substr(start);
  } else {
    suffix.clear();
  }

  for (char& c : suffix) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }

  uint64_t multiplier = 1ULL;
  if (suffix.empty() || suffix == "b" || suffix == "bytes") {
    multiplier = 1ULL;
  } else if (suffix == "k" || suffix == "kb" || suffix == "kib") {
    multiplier = 1024ULL;
  } else if (suffix == "m" || suffix == "mb" || suffix == "mib") {
    multiplier = 1024ULL * 1024ULL;
  } else if (suffix == "g" || suffix == "gb" || suffix == "gib") {
    multiplier = 1024ULL * 1024ULL * 1024ULL;
  } else if (suffix == "t" || suffix == "tb" || suffix == "tib") {
    multiplier = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
  } else {
    return false;
  }

  double total = val * static_cast<double>(multiplier);
  if (!std::isfinite(total) || total < 0.0 ||
      total >= static_cast<double>(UINT64_MAX)) {
    return false;
  }

  out_bytes = static_cast<uint64_t>(total);
  return true;
}

std::string format_cas_bytes(uint64_t bytes) {
  char buf[64];
  constexpr uint64_t kib = 1024ULL;
  constexpr uint64_t mib = 1024ULL * 1024ULL;
  constexpr uint64_t gib = 1024ULL * 1024ULL * 1024ULL;
  constexpr uint64_t tib = 1024ULL * 1024ULL * 1024ULL * 1024ULL;

  if (bytes < kib) {
    std::snprintf(buf, sizeof(buf), "%llu B",
                  static_cast<unsigned long long>(bytes));
  } else if (bytes < mib) {
    std::snprintf(buf, sizeof(buf), "%.2f KiB",
                  static_cast<double>(bytes) / static_cast<double>(kib));
  } else if (bytes < gib) {
    std::snprintf(buf, sizeof(buf), "%.2f MiB",
                  static_cast<double>(bytes) / static_cast<double>(mib));
  } else if (bytes < tib) {
    std::snprintf(buf, sizeof(buf), "%.2f GiB",
                  static_cast<double>(bytes) / static_cast<double>(gib));
  } else {
    std::snprintf(buf, sizeof(buf), "%.2f TiB",
                  static_cast<double>(bytes) / static_cast<double>(tib));
  }
  return std::string(buf);
}

std::string format_cas_stats_table(const std::string& cache_root,
                                   const CasStats& stats) {
  std::ostringstream oss;
  std::string display_root = collapse_home_path(cache_root);
  oss << "====================================================================="
         "===========\n"
      << "Virmux Content-Addressable Storage (CAS)\n"
      << "====================================================================="
         "===========\n"
      << std::left << std::setw(16) << "Cache Root:" << display_root << "\n"
      << std::left << std::setw(16) << "Objects:" << stats.object_count << "\n"
      << std::left << std::setw(16)
      << "Total Size:" << format_cas_bytes(stats.total_bytes) << "\n"
      << std::left << std::setw(16) << "Cache Hits:" << stats.hits << "\n"
      << std::left << std::setw(16) << "Cache Misses:" << stats.misses << "\n"
      << "====================================================================="
         "===========\n";
  return oss.str();
}

std::string format_cas_stats_json(const std::string& cache_root,
                                  const CasStats& stats) {
  std::ostringstream oss;
  std::string display_root = collapse_home_path(cache_root);
  oss << "{\n"
      << "  \"cache_root\": \"" << escape_json_string(display_root) << "\",\n"
      << "  \"object_count\": " << stats.object_count << ",\n"
      << "  \"total_bytes\": " << stats.total_bytes << ",\n"
      << "  \"hits\": " << stats.hits << ",\n"
      << "  \"misses\": " << stats.misses << ",\n"
      << "  \"cas\": {\n"
      << "    \"cache_root\": \"" << escape_json_string(display_root) << "\",\n"
      << "    \"object_count\": " << stats.object_count << ",\n"
      << "    \"total_bytes\": " << stats.total_bytes << ",\n"
      << "    \"hits\": " << stats.hits << ",\n"
      << "    \"misses\": " << stats.misses << "\n"
      << "  }\n"
      << "}";
  return oss.str();
}

void print_cas_usage(std::ostream& os) {
  os << "Usage:\n"
     << "  virmux cas <SUBCOMMAND> [OPTIONS]\n\n"
     << "Content-Addressable Storage (CAS) build cache manager.\n\n"
     << "Subcommands:\n"
     << "  stats [OPTIONS]                Display CAS cache statistics\n"
     << "  clean [--max-bytes <SIZE>]     Prune cache objects exceeding size "
        "ceiling\n\n"
     << "Options:\n"
     << "  -h, --help                     Show this help message\n";
}

void print_cas_stats_usage(std::ostream& os) {
  os << "Usage:\n"
     << "  virmux cas stats [OPTIONS]\n\n"
     << "Display Content-Addressable Storage (CAS) cache statistics.\n\n"
     << "Options:\n"
     << "  --json       Output telemetry in JSON format\n"
     << "  -h, --help   Show this help message\n";
}

void print_cas_clean_usage(std::ostream& os) {
  os << "Usage:\n"
     << "  virmux cas clean [OPTIONS]\n\n"
     << "Prune objects from the Content-Addressable Storage (CAS) cache.\n\n"
     << "Options:\n"
     << "  --max-bytes <SIZE>   Maximum cache size to retain (e.g., 500M, 1G, "
        "0; default: 0)\n"
     << "  -h, --help           Show this help message\n";
}

int cmd_cas(int argc, char* const argv[]) {
  if (argv == nullptr || argc <= 0) {
    return 1;
  }

  int start_idx = 0;
  if (argc > 1 && argv[1] != nullptr && std::string_view(argv[1]) == "cas") {
    start_idx = 2;
  } else if (argc > 0 && argv[0] != nullptr) {
    std::string_view a0(argv[0]);
    if (a0 == "cas" || a0.ends_with("/cas") || a0 == "virmux" ||
        a0.ends_with("/virmux")) {
      start_idx = 1;
    }
  }

  if (start_idx >= argc || argv[start_idx] == nullptr) {
    std::cerr << "Error: no subcommand specified for 'cas'\n";
    print_cas_usage(std::cerr);
    return 1;
  }

  std::string_view subcmd = argv[start_idx];
  if (subcmd == "-h" || subcmd == "--help" || subcmd == "help") {
    print_cas_usage(std::cout);
    return 0;
  }

  if (subcmd == "stats") {
    bool json_mode = false;
    for (int i = start_idx + 1; i < argc; ++i) {
      if (argv[i] == nullptr) {
        continue;
      }
      std::string_view arg = argv[i];
      if (arg == "-h" || arg == "--help") {
        print_cas_stats_usage(std::cout);
        return 0;
      }
      if (arg == "--json") {
        json_mode = true;
      } else {
        std::cerr << "Error: unrecognized option '" << arg
                  << "' for 'cas stats'\n";
        print_cas_stats_usage(std::cerr);
        return 1;
      }
    }

    CasStore store;
    CasStats stats = store.get_stats();
    if (json_mode) {
      std::cout << format_cas_stats_json(store.cache_root(), stats) << "\n";
    } else {
      std::cout << format_cas_stats_table(store.cache_root(), stats);
    }
    return 0;
  }

  if (subcmd == "clean") {
    uint64_t max_bytes = 0ULL;
    for (int i = start_idx + 1; i < argc; ++i) {
      if (argv[i] == nullptr) {
        continue;
      }
      std::string_view arg = argv[i];
      if (arg == "-h" || arg == "--help") {
        print_cas_clean_usage(std::cout);
        return 0;
      }
      if (arg == "--max-bytes") {
        if (i + 1 >= argc || argv[i + 1] == nullptr) {
          std::cerr << "Error: --max-bytes requires an argument\n";
          print_cas_clean_usage(std::cerr);
          return 1;
        }
        if (!parse_size_bytes(argv[++i], max_bytes)) {
          std::cerr << "Error: invalid value for --max-bytes: '" << argv[i]
                    << "'\n";
          print_cas_clean_usage(std::cerr);
          return 1;
        }
      } else if (arg.starts_with("--max-bytes=")) {
        std::string_view val = arg.substr(12);
        if (!parse_size_bytes(val, max_bytes)) {
          std::cerr << "Error: invalid value for --max-bytes: '" << val
                    << "'\n";
          print_cas_clean_usage(std::cerr);
          return 1;
        }
      } else {
        std::cerr << "Error: unrecognized option '" << arg
                  << "' for 'cas clean'\n";
        print_cas_clean_usage(std::cerr);
        return 1;
      }
    }

    CasStore store;
    size_t pruned = store.prune(max_bytes);
    std::cout << "Pruned " << pruned << " object(s) from CAS cache.\n";
    return 0;
  }

  std::cerr << "Error: unrecognized subcommand '" << subcmd << "' for 'cas'\n";
  print_cas_usage(std::cerr);
  return 1;
}

}  // namespace virmux
