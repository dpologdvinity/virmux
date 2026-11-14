/**
 * @file main.cpp
 * @brief Command-line interface and run dispatcher for virmux.
 */

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "virmux/cleanse.hpp"
#include "virmux/sandbox.hpp"
#include "virmux/version.hpp"

namespace {

/**
 * @brief Print general help and command usage to the specified output stream.
 * @param out Output stream to write help text to.
 */
void print_general_usage(std::ostream& out) {
  out << "virmux - Lightweight virtualized execution and session "
         "multiplexer\n\n"
      << "Usage:\n"
      << "  virmux [OPTIONS] <COMMAND> [COMMAND OPTIONS]\n\n"
      << "Commands:\n"
      << "  run [OPTIONS] -- <COMMAND...>   Execute a command inside a "
         "resource-isolated sandbox\n\n"
      << "Options:\n"
      << "  -h, --help                      Show this help message\n"
      << "  -v, --version                   Show version information\n";
}

/**
 * @brief Print usage information for the `virmux run` subcommand.
 * @param out Output stream to write help text to.
 */
void print_run_usage(std::ostream& out) {
  out << "Usage:\n"
      << "  virmux run [OPTIONS] -- <COMMAND...>\n\n"
      << "Execute a command inside a resource-isolated sandbox.\n\n"
      << "Options:\n"
      << "  --cores <N>          Virtual CPU core count (default: 2)\n"
      << "  --mem <LIMIT>        Memory ceiling in bytes or with suffix (e.g., "
         "2G, 512M, 1024K; default: 2G)\n"
      << "  --workspace <PATH>   Path to workspace directory (evicts page "
         "cache on exit)\n"
      << "  --heavy              Register as heavy compute task with admission "
         "gate\n"
      << "  -h, --help           Show this help message\n";
}

/**
 * @brief Parse a memory limit string into bytes.
 *
 * Supports plain bytes or unit suffixes like K, KB, KiB, M, MB, MiB, G, GB,
 * GiB, T, TB, TiB. Suffix matching is case-insensitive.
 *
 * @param str Memory limit string representation.
 * @param out_bytes Output variable receiving the parsed byte count.
 * @return True on successful parse, false on syntax or range error.
 */
bool parse_memory_limit(std::string_view str, uint64_t& out_bytes) {
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
  if (end_ptr == s.c_str() || errno == ERANGE || val <= 0.0) {
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
  if (total < 1.0 || total > static_cast<double>(UINT64_MAX)) {
    return false;
  }

  out_bytes = static_cast<uint64_t>(total);
  return true;
}

/**
 * @brief Parse a virtual core count string into an integer.
 *
 * @param str Core count string representation.
 * @param out_cores Output variable receiving the parsed core count.
 * @return True on successful parse, false on syntax or range error.
 */
bool parse_cores(std::string_view str, int& out_cores) {
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
  long val = std::strtol(s.c_str(), &end_ptr, 10);
  if (end_ptr == s.c_str() || *end_ptr != '\0' || errno == ERANGE || val <= 0 ||
      val > 65536) {
    return false;
  }

  out_cores = static_cast<int>(val);
  return true;
}

/**
 * @brief Dispatcher for the `virmux run` command.
 *
 * Parses `--cores`, `--mem`, `--workspace`, `--heavy`, and the command
 * arguments following `--`. Executes `virmux::spawn_command`, optionally
 * cleanses the workspace directory, and returns the exit code.
 *
 * @param argc Argument count from main.
 * @param argv Argument vector from main.
 * @return Child process exit code, or 1 on CLI parsing error.
 */
int handle_run(int argc, char* argv[]) {
  virmux::SandboxConfig cfg{};

  int cmd_start_idx = -1;
  for (int i = 2; i < argc; ++i) {
    std::string_view arg = argv[i];

    if (arg == "--") {
      cmd_start_idx = i + 1;
      break;
    }

    if (arg == "-h" || arg == "--help") {
      print_run_usage(std::cout);
      return 0;
    }

    if (arg == "--cores") {
      if (i + 1 >= argc) {
        std::cerr << "Error: --cores requires an argument\n";
        return 1;
      }
      if (!parse_cores(argv[++i], cfg.virtual_cores)) {
        std::cerr << "Error: invalid value for --cores: '" << argv[i]
                  << "' (must be a positive integer)\n";
        return 1;
      }
    } else if (arg.starts_with("--cores=")) {
      std::string_view val = arg.substr(8);
      if (!parse_cores(val, cfg.virtual_cores)) {
        std::cerr << "Error: invalid value for --cores: '" << val
                  << "' (must be a positive integer)\n";
        return 1;
      }
    } else if (arg == "--mem") {
      if (i + 1 >= argc) {
        std::cerr << "Error: --mem requires an argument\n";
        return 1;
      }
      if (!parse_memory_limit(argv[++i], cfg.memory_limit_bytes)) {
        std::cerr << "Error: invalid memory limit: '" << argv[i] << "'\n";
        return 1;
      }
    } else if (arg.starts_with("--mem=")) {
      std::string_view val = arg.substr(6);
      if (!parse_memory_limit(val, cfg.memory_limit_bytes)) {
        std::cerr << "Error: invalid memory limit: '" << val << "'\n";
        return 1;
      }
    } else if (arg == "--workspace") {
      if (i + 1 >= argc) {
        std::cerr << "Error: --workspace requires an argument\n";
        return 1;
      }
      cfg.workspace_path = argv[++i];
    } else if (arg.starts_with("--workspace=")) {
      cfg.workspace_path = std::string(arg.substr(12));
    } else if (arg == "--heavy") {
      cfg.is_heavy = true;
    } else {
      std::cerr
          << "Error: unrecognized option or missing '--' before command: '"
          << arg << "'\n";
      print_run_usage(std::cerr);
      return 1;
    }
  }

  if (cmd_start_idx == -1 || cmd_start_idx >= argc) {
    std::cerr << "Error: no command specified after '--'\n";
    print_run_usage(std::cerr);
    return 1;
  }

  std::vector<char*> child_argv;
  child_argv.reserve(static_cast<size_t>(argc - cmd_start_idx + 1));
  for (int i = cmd_start_idx; i < argc; ++i) {
    child_argv.push_back(argv[i]);
  }
  child_argv.push_back(nullptr);

  int exit_code = virmux::spawn_command(cfg, child_argv.data());

  if (!cfg.workspace_path.empty()) {
    virmux::cleanse_path(cfg.workspace_path);
  }

  return exit_code;
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc < 2) {
    print_general_usage(std::cout);
    return 0;
  }

  std::string_view first_arg = argv[1];

  if (first_arg == "-h" || first_arg == "--help") {
    print_general_usage(std::cout);
    return 0;
  }

  if (first_arg == "-v" || first_arg == "--version") {
    std::cout << "virmux " << virmux::version_string() << "\n";
    return 0;
  }

  if (first_arg == "help") {
    if (argc > 2 && std::string_view(argv[2]) == "run") {
      print_run_usage(std::cout);
    } else {
      print_general_usage(std::cout);
    }
    return 0;
  }

  if (first_arg == "run") {
    return handle_run(argc, argv);
  }

  std::cerr << "Error: unrecognized command '" << first_arg << "'\n";
  print_general_usage(std::cerr);
  return 1;
}
