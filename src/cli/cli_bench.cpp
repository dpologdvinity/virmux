/**
 * @file cli_bench.cpp
 * @brief Implementation of CLI bench subcommand and benchmark reporting.
 */

#include "virmux/cli_bench.hpp"

#include <signal.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

#include "virmux/bench.hpp"

namespace virmux {

namespace {

static std::atomic<BenchmarkRunner*> s_active_bench_runner{nullptr};

void bench_signal_handler(int /*sig*/) {
  BenchmarkRunner* runner = s_active_bench_runner.load();
  if (runner) {
    runner->stop();
  }
}

void print_bench_usage(std::ostream& out) {
  out << "Usage:\n"
      << "  virmux bench <SUBCOMMAND> [OPTIONS]\n\n"
      << "12-Terminal Gauntlet stress benchmark suite and telemetry reporter.\n\n"
      << "Commands:\n"
      << "  run [OPTIONS]       Execute synthetic workload benchmark gauntlet\n"
      << "  report [OPTIONS]    Format and display benchmark results from JSON file\n\n"
      << "Options:\n"
      << "  -h, --help          Show this help message\n";
}

void print_bench_run_usage(std::ostream& out) {
  out << "Usage:\n"
      << "  virmux bench run [OPTIONS]\n\n"
      << "Execute the 12-terminal gauntlet stress benchmark suite.\n\n"
      << "Options:\n"
      << "  --tasks <N>         Number of concurrent benchmark tasks (default: 12)\n"
      << "  --duration <N>      Run duration in seconds per task (default: 5)\n"
      << "  --quick             Execute fast benchmark suite with reduced load\n"
      << "  --mode <MODE>       Benchmark mode: all, managed, baseline (default: all)\n"
      << "  --output <PATH>     Save benchmark results to JSON file\n"
      << "  --json              Output benchmark results in JSON format\n"
      << "  -h, --help          Show this help message\n";
}

void print_bench_report_usage(std::ostream& out) {
  out << "Usage:\n"
      << "  virmux bench report [OPTIONS] [FILE]\n\n"
      << "Format and display benchmark comparison results from a JSON file.\n\n"
      << "Options:\n"
      << "  --input <PATH>      Path to JSON benchmark results file (or read stdin if omitted)\n"
      << "  -h, --help          Show this help message\n";
}

int handle_bench_run(int argc, char* const argv[], int start_idx) {
  int tasks = 12;
  int duration = 5;
  bool quick = false;
  bool explicit_tasks = false;
  bool explicit_duration = false;
  std::string mode = "all";
  std::string output_path;
  bool json_mode = false;

  for (int i = start_idx; i < argc; ++i) {
    if (argv[i] == nullptr) {
      continue;
    }
    std::string_view arg = argv[i];

    if (arg == "-h" || arg == "--help") {
      print_bench_run_usage(std::cout);
      return 0;
    }

    if (arg == "--quick") {
      quick = true;
    } else if (arg == "--json") {
      json_mode = true;
    } else if (arg == "--tasks") {
      if (i + 1 >= argc || argv[i + 1] == nullptr) {
        std::cerr << "Error: --tasks requires an argument\n";
        return 1;
      }
      tasks = std::atoi(argv[++i]);
      explicit_tasks = true;
    } else if (arg.starts_with("--tasks=")) {
      tasks = std::atoi(std::string(arg.substr(8)).c_str());
      explicit_tasks = true;
    } else if (arg == "--duration") {
      if (i + 1 >= argc || argv[i + 1] == nullptr) {
        std::cerr << "Error: --duration requires an argument\n";
        return 1;
      }
      duration = std::atoi(argv[++i]);
      explicit_duration = true;
    } else if (arg.starts_with("--duration=")) {
      duration = std::atoi(std::string(arg.substr(11)).c_str());
      explicit_duration = true;
    } else if (arg == "--mode") {
      if (i + 1 >= argc || argv[i + 1] == nullptr) {
        std::cerr << "Error: --mode requires an argument\n";
        return 1;
      }
      mode = argv[++i];
    } else if (arg.starts_with("--mode=")) {
      mode = std::string(arg.substr(7));
    } else if (arg == "--output") {
      if (i + 1 >= argc || argv[i + 1] == nullptr) {
        std::cerr << "Error: --output requires an argument\n";
        return 1;
      }
      output_path = argv[++i];
    } else if (arg.starts_with("--output=")) {
      output_path = std::string(arg.substr(9));
    } else {
      std::cerr << "Error: unrecognized option for bench run: '" << arg << "'\n";
      print_bench_run_usage(std::cerr);
      return 1;
    }
  }

  if (tasks <= 0) {
    std::cerr << "Error: --tasks must be a positive integer\n";
    return 1;
  }
  if (duration <= 0) {
    std::cerr << "Error: --duration must be a positive integer\n";
    return 1;
  }
  if (mode != "all" && mode != "managed" && mode != "baseline") {
    std::cerr << "Error: unrecognized mode '" << mode
              << "'. Must be 'all', 'managed', or 'baseline'\n";
    return 1;
  }

  if (quick) {
    if (!explicit_tasks) {
      tasks = 4;
    }
    if (!explicit_duration) {
      duration = 1;
    }
  }

  BenchmarkRunner runner(quick);

  struct sigaction sa{}, old_int{}, old_term{};
  sa.sa_handler = bench_signal_handler;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, &old_int);
  sigaction(SIGTERM, &sa, &old_term);
  s_active_bench_runner.store(&runner);

  struct SignalGuard {
    struct sigaction old_int;
    struct sigaction old_term;
    ~SignalGuard() {
      s_active_bench_runner.store(nullptr);
      sigaction(SIGINT, &old_int, nullptr);
      sigaction(SIGTERM, &old_term, nullptr);
    }
  } sig_guard{old_int, old_term};

  BenchmarkComparison comp;
  if (mode == "all") {
    comp = runner.run_gauntlet(duration, tasks);
  } else if (mode == "baseline") {
    comp.baseline = runner.run_baseline(duration, tasks);
  } else if (mode == "managed") {
    comp.managed = runner.run_managed(duration, tasks);
  }

  if (!output_path.empty()) {
    std::ofstream ofs(output_path);
    if (!ofs) {
      std::cerr << "Error: failed to open output file '" << output_path << "' for writing\n";
      return 1;
    }
    ofs << format_comparison_json(comp) << "\n";
  }

  if (json_mode) {
    std::cout << format_comparison_json(comp) << "\n";
  } else {
    std::cout << format_comparison_table(comp);
  }

  return 0;
}

int handle_bench_report(int argc, char* const argv[], int start_idx) {
  std::string input_path;

  for (int i = start_idx; i < argc; ++i) {
    if (argv[i] == nullptr) {
      continue;
    }
    std::string_view arg = argv[i];

    if (arg == "-h" || arg == "--help") {
      print_bench_report_usage(std::cout);
      return 0;
    }

    if (arg == "--input") {
      if (i + 1 >= argc || argv[i + 1] == nullptr) {
        std::cerr << "Error: --input requires an argument\n";
        return 1;
      }
      input_path = argv[++i];
    } else if (arg.starts_with("--input=")) {
      input_path = std::string(arg.substr(8));
    } else if (!arg.starts_with("-") && input_path.empty()) {
      input_path = std::string(arg);
    } else {
      std::cerr << "Error: unrecognized option for bench report: '" << arg << "'\n";
      print_bench_report_usage(std::cerr);
      return 1;
    }
  }

  std::string content;
  if (input_path.empty() || input_path == "-") {
    std::ostringstream ss;
    ss << std::cin.rdbuf();
    content = ss.str();
  } else {
    std::error_code ec;
    if (!std::filesystem::exists(input_path, ec)) {
      std::cerr << "Error: file not found: " << input_path << "\n";
      return 1;
    }
    std::ifstream ifs(input_path);
    if (!ifs) {
      std::cerr << "Error: failed to open file: " << input_path << "\n";
      return 1;
    }
    std::ostringstream ss;
    ss << ifs.rdbuf();
    content = ss.str();
  }

  auto comp = parse_comparison_json(content);
  if (!comp) {
    std::cerr << "Error: failed to parse benchmark comparison JSON from '"
              << (input_path.empty() ? "stdin" : input_path) << "'\n";
    return 1;
  }

  std::cout << format_comparison_table(*comp);
  return 0;
}

}  // namespace

int cmd_bench(int argc, char* const argv[]) {
  if (argv == nullptr || argc <= 0) {
    return 1;
  }

  int start_idx = 0;
  if (argc > 1 && argv[1] != nullptr && std::string_view(argv[1]) == "bench") {
    start_idx = 2;
  } else if (argc > 0 && argv[0] != nullptr) {
    std::string_view a0(argv[0]);
    if (a0 == "bench" || a0.ends_with("/bench") || a0 == "virmux" ||
        a0.ends_with("/virmux")) {
      start_idx = 1;
    }
  }

  if (start_idx >= argc || argv[start_idx] == nullptr) {
    print_bench_usage(std::cout);
    return 0;
  }

  std::string_view subcmd = argv[start_idx];
  if (subcmd == "-h" || subcmd == "--help" || subcmd == "help") {
    print_bench_usage(std::cout);
    return 0;
  }

  if (subcmd == "run") {
    return handle_bench_run(argc, argv, start_idx + 1);
  }

  if (subcmd == "report") {
    return handle_bench_report(argc, argv, start_idx + 1);
  }

  std::cerr << "Error: unrecognized bench subcommand '" << subcmd << "'\n";
  print_bench_usage(std::cerr);
  return 1;
}

}  // namespace virmux
