/**
 * @file cli_daemon.cpp
 * @brief Implementation of CLI daemon, list, freeze, thaw, and kill
 * subcommands.
 */

#include "virmux/cli_daemon.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "virmux/cli_stats.hpp"

namespace virmux {

namespace {

static std::atomic<SupervisorDaemon*> g_active_daemon{nullptr};

static void daemon_signal_handler(int /*sig*/) {
  auto* d = g_active_daemon.load();
  if (d != nullptr) {
    d->stop();
  }
}

std::string escape_json_str(const std::string& input) {
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

std::string format_state_col(SessionState state) {
  switch (state) {
    case SessionState::INITIALIZING:
      return "INIT";
    case SessionState::RUNNING_BURST:
      return "BURST";
    case SessionState::RUNNING_NORMAL:
      return "RUNNING";
    case SessionState::WAITING_IO:
      return "WAITING";
    case SessionState::FROZEN:
      return "FROZEN";
    case SessionState::DEAD:
      return "DEAD";
  }
  return "UNKNOWN";
}

int parse_subcmd_start(int argc, char* const argv[], std::string_view subcmd) {
  if (argc > 1 && argv != nullptr && argv[1] != nullptr &&
      std::string_view(argv[1]) == subcmd) {
    return 2;
  }
  if (argc > 0 && argv != nullptr && argv[0] != nullptr) {
    std::string_view a0(argv[0]);
    if (a0 == subcmd || a0.ends_with("/" + std::string(subcmd)) ||
        a0 == "virmux" || a0.ends_with("/virmux")) {
      return 1;
    }
  }
  return 0;
}

bool parse_session_or_pid(std::string_view str, uint64_t& out_val) {
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
  for (char c : str) {
    if (!std::isdigit(static_cast<unsigned char>(c))) {
      return false;
    }
  }
  char* end_ptr = nullptr;
  errno = 0;
  unsigned long long v = std::strtoull(std::string(str).c_str(), &end_ptr, 10);
  if (errno != 0 || end_ptr == nullptr || *end_ptr != '\0' || v == 0) {
    return false;
  }
  out_val = static_cast<uint64_t>(v);
  return true;
}

uint64_t resolve_target_session_id(DaemonClient& client, uint64_t val) {
  auto sessions = client.list_sessions(500);
  for (const auto& s : sessions) {
    if (s.id == val) {
      return s.id;
    }
  }
  for (const auto& s : sessions) {
    if (s.pid == static_cast<pid_t>(val)) {
      return s.id;
    }
  }
  return val;
}

void print_daemon_usage(std::ostream& out) {
  out << "Usage:\n"
      << "  virmux daemon [OPTIONS]\n\n"
      << "Run the virmux background supervisor daemon.\n\n"
      << "Options:\n"
      << "  --foreground    Run daemon in foreground (default: background "
         "daemon)\n"
      << "  -h, --help      Show this help message\n";
}

void print_list_usage(std::ostream& out) {
  out << "Usage:\n"
      << "  virmux list [OPTIONS]\n\n"
      << "List active virtualized sessions managed by the supervisor "
         "daemon.\n\n"
      << "Options:\n"
      << "  --json       Output session list in JSON format\n"
      << "  -h, --help   Show this help message\n";
}

void print_freeze_usage(std::ostream& out) {
  out << "Usage:\n"
      << "  virmux freeze <SESSION_ID | PID>\n\n"
      << "Freeze an active virtualized session.\n\n"
      << "Options:\n"
      << "  -h, --help   Show this help message\n";
}

void print_thaw_usage(std::ostream& out) {
  out << "Usage:\n"
      << "  virmux thaw <SESSION_ID | PID>\n\n"
      << "Thaw a frozen virtualized session.\n\n"
      << "Options:\n"
      << "  -h, --help   Show this help message\n";
}

void print_kill_usage(std::ostream& out) {
  out << "Usage:\n"
      << "  virmux kill <SESSION_ID | PID>\n\n"
      << "Terminate a virtualized session and its process tree.\n\n"
      << "Options:\n"
      << "  -h, --help   Show this help message\n";
}

}  // namespace

std::string format_session_table(const std::vector<SessionInfo>& sessions) {
  std::ostringstream oss;
  oss << "S#  | NAME         | STATE   | V-CORES | RAM (RSS) | DEDUP   | "
         "COMMAND\n";
  oss << "----+--------------+---------+---------+-----------+---------+-------"
         "---------\n";
  for (const auto& s : sessions) {
    char buf[512];
    std::string state_str = format_state_col(s.state);
    std::string rss_str = format_bytes(s.memory_rss_bytes);
    std::string dedup_str = format_bytes(s.ksm_dedup_bytes);
    std::snprintf(buf, sizeof(buf),
                  "%-4llu| %-13.13s| %-8s| %-8d| %-10s| %-8s| %s\n",
                  static_cast<unsigned long long>(s.id), s.name.c_str(),
                  state_str.c_str(), s.virtual_cores, rss_str.c_str(),
                  dedup_str.c_str(), s.command.c_str());
    oss << buf;
  }
  return oss.str();
}

std::string format_session_json(const std::vector<SessionInfo>& sessions) {
  if (sessions.empty()) {
    return "[]";
  }

  std::ostringstream oss;
  oss << "[\n";
  for (size_t i = 0; i < sessions.size(); ++i) {
    const auto& s = sessions[i];
    auto start_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             s.start_time.time_since_epoch())
                             .count();
    oss << "  {\n"
        << "    \"id\": " << s.id << ",\n"
        << "    \"name\": \"" << escape_json_str(s.name) << "\",\n"
        << "    \"pid\": " << s.pid << ",\n"
        << "    \"state\": \"" << session_state_to_string(s.state) << "\",\n"
        << "    \"virtual_cores\": " << s.virtual_cores << ",\n"
        << "    \"memory_rss_bytes\": " << s.memory_rss_bytes << ",\n"
        << "    \"memory_limit_bytes\": " << s.memory_limit_bytes << ",\n"
        << "    \"ksm_dedup_bytes\": " << s.ksm_dedup_bytes << ",\n"
        << "    \"cgroup_path\": \"" << escape_json_str(s.cgroup_path)
        << "\",\n"
        << "    \"command\": \"" << escape_json_str(s.command) << "\",\n"
        << "    \"start_time\": " << start_time_ms << "\n"
        << "  }" << (i + 1 < sessions.size() ? ",\n" : "\n");
  }
  oss << "]";
  return oss.str();
}

int cmd_daemon(int argc, char* const argv[]) {
  int start_idx = parse_subcmd_start(argc, argv, "daemon");
  bool foreground = false;

  for (int i = start_idx; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      print_daemon_usage(std::cout);
      return 0;
    }
    if (arg == "--foreground") {
      foreground = true;
    } else {
      std::cerr << "Error: unrecognized option '" << arg << "'\n";
      print_daemon_usage(std::cerr);
      return 1;
    }
  }

  if (foreground) {
    SupervisorDaemon daemon;
    if (!daemon.init()) {
      std::cerr << "Error: failed to initialize supervisor daemon (already "
                   "running?)\n";
      return 1;
    }

    g_active_daemon.store(&daemon);
    struct sigaction sa{};
    sa.sa_handler = daemon_signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    daemon.run();
    g_active_daemon.store(nullptr);
    return 0;
  }

  // Without --foreground: daemonize into background
  DaemonClient probe;
  if (probe.is_alive(200)) {
    std::cerr << "Error: supervisor daemon is already running\n";
    return 1;
  }

  pid_t pid = ::fork();
  if (pid < 0) {
    std::perror("fork");
    return 1;
  }

  if (pid > 0) {
    // In parent process: print background PID and return 0
    std::cout << "Supervisor daemon started in background (PID " << pid
              << ")\n";
    return 0;
  }

  // In child process
  ::setsid();
  int null_fd = ::open("/dev/null", O_RDWR);
  if (null_fd >= 0) {
    ::dup2(null_fd, STDIN_FILENO);
    ::dup2(null_fd, STDOUT_FILENO);
    ::dup2(null_fd, STDERR_FILENO);
    if (null_fd > STDERR_FILENO) {
      ::close(null_fd);
    }
  }

  SupervisorDaemon daemon;
  if (!daemon.init()) {
    ::_exit(1);
  }

  g_active_daemon.store(&daemon);
  struct sigaction sa{};
  sa.sa_handler = daemon_signal_handler;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  daemon.run();
  g_active_daemon.store(nullptr);
  ::_exit(0);
}

int cmd_list(int argc, char* const argv[]) {
  int start_idx = parse_subcmd_start(argc, argv, "list");
  bool json_mode = false;

  for (int i = start_idx; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      print_list_usage(std::cout);
      return 0;
    }
    if (arg == "--json") {
      json_mode = true;
    } else {
      std::cerr << "Error: unrecognized option '" << arg << "'\n";
      print_list_usage(std::cerr);
      return 1;
    }
  }

  DaemonClient client;
  if (!client.is_alive(500)) {
    std::cerr << "Error: supervisor daemon is not running or unreachable\n";
    return 1;
  }

  auto sessions = client.list_sessions(1000);
  if (json_mode) {
    std::cout << format_session_json(sessions) << "\n";
  } else {
    std::cout << format_session_table(sessions);
  }

  return 0;
}

int cmd_freeze(int argc, char* const argv[]) {
  int start_idx = parse_subcmd_start(argc, argv, "freeze");
  std::string_view target_arg;

  for (int i = start_idx; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      print_freeze_usage(std::cout);
      return 0;
    }
    if (arg.starts_with("-")) {
      std::cerr << "Error: unrecognized option '" << arg << "'\n";
      print_freeze_usage(std::cerr);
      return 1;
    }
    if (target_arg.empty()) {
      target_arg = arg;
    } else {
      std::cerr << "Error: unexpected extra argument '" << arg << "'\n";
      print_freeze_usage(std::cerr);
      return 1;
    }
  }

  if (target_arg.empty()) {
    std::cerr << "Error: missing session ID or PID\n";
    print_freeze_usage(std::cerr);
    return 1;
  }

  uint64_t val = 0;
  if (!parse_session_or_pid(target_arg, val)) {
    std::cerr << "Error: invalid session ID or PID: '" << target_arg << "'\n";
    return 1;
  }

  DaemonClient client;
  if (!client.is_alive(500)) {
    std::cerr << "Error: supervisor daemon is not running or unreachable\n";
    return 1;
  }

  uint64_t session_id = resolve_target_session_id(client, val);
  if (!client.freeze_session(session_id)) {
    std::cerr << "Error: failed to freeze session " << session_id << "\n";
    return 1;
  }

  std::cout << "Session " << session_id << " frozen successfully.\n";
  return 0;
}

int cmd_thaw(int argc, char* const argv[]) {
  int start_idx = parse_subcmd_start(argc, argv, "thaw");
  std::string_view target_arg;

  for (int i = start_idx; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      print_thaw_usage(std::cout);
      return 0;
    }
    if (arg.starts_with("-")) {
      std::cerr << "Error: unrecognized option '" << arg << "'\n";
      print_thaw_usage(std::cerr);
      return 1;
    }
    if (target_arg.empty()) {
      target_arg = arg;
    } else {
      std::cerr << "Error: unexpected extra argument '" << arg << "'\n";
      print_thaw_usage(std::cerr);
      return 1;
    }
  }

  if (target_arg.empty()) {
    std::cerr << "Error: missing session ID or PID\n";
    print_thaw_usage(std::cerr);
    return 1;
  }

  uint64_t val = 0;
  if (!parse_session_or_pid(target_arg, val)) {
    std::cerr << "Error: invalid session ID or PID: '" << target_arg << "'\n";
    return 1;
  }

  DaemonClient client;
  if (!client.is_alive(500)) {
    std::cerr << "Error: supervisor daemon is not running or unreachable\n";
    return 1;
  }

  uint64_t session_id = resolve_target_session_id(client, val);
  if (!client.thaw_session(session_id)) {
    std::cerr << "Error: failed to thaw session " << session_id << "\n";
    return 1;
  }

  std::cout << "Session " << session_id << " thawed successfully.\n";
  return 0;
}

int cmd_kill(int argc, char* const argv[]) {
  int start_idx = parse_subcmd_start(argc, argv, "kill");
  std::string_view target_arg;

  for (int i = start_idx; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      print_kill_usage(std::cout);
      return 0;
    }
    if (arg.starts_with("-")) {
      std::cerr << "Error: unrecognized option '" << arg << "'\n";
      print_kill_usage(std::cerr);
      return 1;
    }
    if (target_arg.empty()) {
      target_arg = arg;
    } else {
      std::cerr << "Error: unexpected extra argument '" << arg << "'\n";
      print_kill_usage(std::cerr);
      return 1;
    }
  }

  if (target_arg.empty()) {
    std::cerr << "Error: missing session ID or PID\n";
    print_kill_usage(std::cerr);
    return 1;
  }

  uint64_t val = 0;
  if (!parse_session_or_pid(target_arg, val)) {
    std::cerr << "Error: invalid session ID or PID: '" << target_arg << "'\n";
    return 1;
  }

  DaemonClient client;
  if (!client.is_alive(500)) {
    std::cerr << "Error: supervisor daemon is not running or unreachable\n";
    return 1;
  }

  uint64_t session_id = resolve_target_session_id(client, val);
  if (!client.kill_session(session_id)) {
    std::cerr << "Error: failed to kill session " << session_id << "\n";
    return 1;
  }

  std::cout << "Session " << session_id << " killed successfully.\n";
  return 0;
}

}  // namespace virmux
