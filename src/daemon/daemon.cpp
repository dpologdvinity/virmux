/**
 * @file daemon.cpp
 * @brief Implementation of SupervisorDaemon server and DaemonClient IPC.
 */

#include "virmux/daemon.hpp"
#include "virmux/sandbox.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "virmux/cli_stats.hpp"
#include "virmux/memory_manager.hpp"

namespace virmux {

namespace {

/**
 * @brief Replace user home directory in path with ~ to prevent path leaking.
 */
std::string collapse_home_path(const std::string& str) {
  const char* home = std::getenv("HOME");
  if (home == nullptr || *home == '\0' || std::string_view(home) == "/") {
    return str;
  }
  std::string home_str(home);
  if (str.find(home_str) == std::string::npos) {
    return str;
  }
  std::string res = str;
  size_t pos = 0;
  while ((pos = res.find(home_str, pos)) != std::string::npos) {
    res.replace(pos, home_str.length(), "~");
    pos += 1;
  }
  return res;
}

/**
 * @brief Escape characters for JSON string serialization.
 */
std::string escape_json(std::string_view input) {
  std::string output;
  output.reserve(input.size() + 8);
  for (char c : input) {
    switch (c) {
      case '"':
        output += "\\\"";
        break;
      case '\\':
        output += "\\\\";
        break;
      case '\b':
        output += "\\b";
        break;
      case '\f':
        output += "\\f";
        break;
      case '\n':
        output += "\\n";
        break;
      case '\r':
        output += "\\r";
        break;
      case '\t':
        output += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(
              buf, sizeof(buf), "\\u%04x",
              static_cast<unsigned int>(static_cast<unsigned char>(c)));
          output += buf;
        } else {
          output += c;
        }
        break;
    }
  }
  return output;
}

/**
 * @brief Serialize SessionRegistration struct into JSON string.
 */
std::string session_registration_to_json(const SessionRegistration& req) {
  std::ostringstream oss;
  oss << "{\n"
      << "  \"name\": \"" << escape_json(req.name) << "\",\n"
      << "  \"pid\": " << req.pid << ",\n"
      << "  \"virtual_cores\": " << req.virtual_cores << ",\n"
      << "  \"memory_limit_bytes\": " << req.memory_limit_bytes << ",\n"
      << "  \"cgroup_path\": \""
      << escape_json(collapse_home_path(req.cgroup_path)) << "\",\n"
      << "  \"command\": \"" << escape_json(collapse_home_path(req.command))
      << "\"\n"
      << "}";
  return oss.str();
}

/**
 * @brief Parse SessionRegistration fields from JSON string.
 */
SessionRegistration parse_session_registration(std::string_view json) {
  SessionRegistration req;
  size_t idx = 0;

  auto skip_ws = [&]() {
    while (idx < json.size() && (json[idx] == ' ' || json[idx] == '\t' ||
                                 json[idx] == '\r' || json[idx] == '\n')) {
      idx++;
    }
  };

  skip_ws();
  if (idx < json.size() && json[idx] == '{') {
    idx++;
  }

  while (idx < json.size()) {
    skip_ws();
    if (idx < json.size() && json[idx] == '}') {
      break;
    }
    if (idx < json.size() && json[idx] == ',') {
      idx++;
      continue;
    }
    if (idx >= json.size() || json[idx] != '"') {
      idx++;
      continue;
    }
    idx++;  // skip '"'
    size_t key_start = idx;
    while (idx < json.size() && json[idx] != '"') {
      idx++;
    }
    std::string_view key = json.substr(key_start, idx - key_start);
    if (idx < json.size()) {
      idx++;  // skip closing '"'
    }

    skip_ws();
    if (idx < json.size() && json[idx] == ':') {
      idx++;
    }
    skip_ws();

    if (key == "name" || key == "cgroup_path" || key == "command") {
      std::string val;
      if (idx < json.size() && json[idx] == '"') {
        idx++;
        while (idx < json.size() && json[idx] != '"') {
          if (json[idx] == '\\' && idx + 1 < json.size()) {
            char next = json[idx + 1];
            if (next == '"')
              val += '"';
            else if (next == '\\')
              val += '\\';
            else if (next == 'n')
              val += '\n';
            else if (next == 'r')
              val += '\r';
            else if (next == 't')
              val += '\t';
            else
              val += next;
            idx += 2;
          } else {
            val += json[idx++];
          }
        }
        if (idx < json.size()) {
          idx++;
        }
      }
      if (key == "name")
        req.name = std::move(val);
      else if (key == "cgroup_path")
        req.cgroup_path = std::move(val);
      else if (key == "command")
        req.command = std::move(val);
    } else if (key == "pid" || key == "virtual_cores" ||
               key == "memory_limit_bytes") {
      size_t num_start = idx;
      while (idx < json.size() &&
             (std::isdigit(static_cast<unsigned char>(json[idx])) ||
              json[idx] == '-')) {
        idx++;
      }
      std::string_view num_sv = json.substr(num_start, idx - num_start);
      if (key == "pid") {
        int val = 0;
        if (std::from_chars(num_sv.data(), num_sv.data() + num_sv.size(), val)
                .ec == std::errc()) {
          req.pid = val;
        }
      } else if (key == "virtual_cores") {
        int val = 2;
        if (std::from_chars(num_sv.data(), num_sv.data() + num_sv.size(), val)
                .ec == std::errc()) {
          req.virtual_cores = val;
        }
      } else if (key == "memory_limit_bytes") {
        uint64_t val = 0;
        if (std::from_chars(num_sv.data(), num_sv.data() + num_sv.size(), val)
                .ec == std::errc()) {
          req.memory_limit_bytes = val;
        }
      }
    } else {
      if (idx < json.size() && json[idx] == '"') {
        idx++;
        while (idx < json.size() && json[idx] != '"') {
          if (json[idx] == '\\' && idx + 1 < json.size())
            idx += 2;
          else
            idx++;
        }
        if (idx < json.size()) idx++;
      } else {
        while (idx < json.size() && json[idx] != ',' && json[idx] != '}') idx++;
      }
    }
  }
  return req;
}

/**
 * @brief Parse session ID from either plain string or JSON payload.
 */
uint64_t parse_session_id(std::string_view str) {
  while (!str.empty() &&
         std::isspace(static_cast<unsigned char>(str.front()))) {
    str.remove_prefix(1);
  }
  if (str.starts_with('{')) {
    auto pos = str.find("\"id\"");
    if (pos != std::string_view::npos) {
      pos = str.find(':', pos);
      if (pos != std::string_view::npos) {
        str = str.substr(pos + 1);
        while (!str.empty() &&
               (std::isspace(static_cast<unsigned char>(str.front())) ||
                str.front() == '"')) {
          str.remove_prefix(1);
        }
      }
    }
  }
  uint64_t val = 0;
  if (std::from_chars(str.data(), str.data() + str.size(), val).ec ==
      std::errc()) {
    return val;
  }
  return 0;
}

/**
 * @brief Parse session ID and signal from either plain string or JSON payload.
 */
void parse_kill_payload(std::string_view payload, uint64_t& out_id,
                        int& out_sig) {
  out_id = 0;
  out_sig = SIGTERM;

  while (!payload.empty() &&
         std::isspace(static_cast<unsigned char>(payload.front()))) {
    payload.remove_prefix(1);
  }

  if (payload.starts_with('{')) {
    auto id_pos = payload.find("\"id\"");
    if (id_pos != std::string_view::npos) {
      id_pos = payload.find(':', id_pos);
      if (id_pos != std::string_view::npos) {
        auto sub = payload.substr(id_pos + 1);
        while (!sub.empty() &&
               (std::isspace(static_cast<unsigned char>(sub.front())) ||
                sub.front() == '"')) {
          sub.remove_prefix(1);
        }
        uint64_t val = 0;
        if (std::from_chars(sub.data(), sub.data() + sub.size(), val).ec ==
            std::errc()) {
          out_id = val;
        }
      }
    }
    auto sig_pos = payload.find("\"signal\"");
    if (sig_pos != std::string_view::npos) {
      sig_pos = payload.find(':', sig_pos);
      if (sig_pos != std::string_view::npos) {
        auto sub = payload.substr(sig_pos + 1);
        while (!sub.empty() &&
               (std::isspace(static_cast<unsigned char>(sub.front())) ||
                sub.front() == '"')) {
          sub.remove_prefix(1);
        }
        int val = SIGTERM;
        if (std::from_chars(sub.data(), sub.data() + sub.size(), val).ec ==
            std::errc()) {
          out_sig = val;
        }
      }
    }
  } else {
    auto [p1, ec1] = std::from_chars(payload.data(),
                                     payload.data() + payload.size(), out_id);
    if (ec1 == std::errc() && p1 < payload.data() + payload.size()) {
      while (p1 < payload.data() + payload.size() &&
             std::isspace(static_cast<unsigned char>(*p1))) {
        p1++;
      }
      if (p1 < payload.data() + payload.size()) {
        int sig_val = SIGTERM;
        if (std::from_chars(p1, payload.data() + payload.size(), sig_val).ec ==
            std::errc()) {
          out_sig = sig_val;
        }
      }
    }
  }
}

}  // namespace

// ============================================================================
// SupervisorDaemon Implementation
// ============================================================================

SupervisorDaemon::SupervisorDaemon(DaemonConfig config)
    : config_(std::move(config)),
      socket_path_(config_.socket_path.empty() ? resolve_socket_path()
                                               : config_.socket_path) {
  if (::pipe2(wake_pipe_, O_CLOEXEC | O_NONBLOCK) < 0) {
    wake_pipe_[0] = -1;
    wake_pipe_[1] = -1;
  }
}

SupervisorDaemon::~SupervisorDaemon() {
  stop();
  if (wake_pipe_[0] >= 0) {
    ::close(wake_pipe_[0]);
    wake_pipe_[0] = -1;
  }
  if (wake_pipe_[1] >= 0) {
    ::close(wake_pipe_[1]);
    wake_pipe_[1] = -1;
  }
  if (server_fd_ >= 0) {
    ::close(server_fd_);
    server_fd_ = -1;
    ::unlink(socket_path_.c_str());
  }
}

bool SupervisorDaemon::init() {
  if (server_fd_ >= 0) {
    return true;
  }

  // Protect supervisor daemon from kernel OOM killer if permissions permit (-900)
  set_process_oom_score(0, -900);

  stop_requested_.store(false);
  running_.store(false);

  if (wake_pipe_[0] < 0 || wake_pipe_[1] < 0) {
    if (::pipe2(wake_pipe_, O_CLOEXEC | O_NONBLOCK) < 0) {
      return false;
    }
  }

  // Ensure parent directory exists
  std::error_code ec;
  auto parent = std::filesystem::path(socket_path_).parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, ec);
  }

  struct sockaddr_un addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  if (socket_path_.size() >= sizeof(addr.sun_path)) {
    return false;
  }
  std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

  // Stale socket detection: attempt connect with non-blocking probe
  ec.clear();
  if (std::filesystem::exists(socket_path_, ec)) {
    int test_fd =
        ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (test_fd < 0) {
      return false;
    }
    int conn_res = ::connect(test_fd, reinterpret_cast<struct sockaddr*>(&addr),
                             sizeof(addr));
    int saved_errno = errno;
    ::close(test_fd);

    if (conn_res == 0) {
      return false;  // Another daemon is active and responding
    }

    if (saved_errno == ECONNREFUSED || saved_errno == ENOENT) {
      // Stale socket from dead daemon: safe to unlink
      ::unlink(socket_path_.c_str());
    } else {
      // Socket exists but connect failed with transient error (e.g. EAGAIN, EINPROGRESS)
      // Do not unlink active socket
      return false;
    }
  }

  server_fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (server_fd_ < 0) {
    return false;
  }

  mode_t old_umask = ::umask(0077);
  int bind_res = ::bind(server_fd_, reinterpret_cast<struct sockaddr*>(&addr),
                        sizeof(addr));
  ::umask(old_umask);

  if (bind_res < 0) {
    ::close(server_fd_);
    server_fd_ = -1;
    return false;
  }

  // Socket permissions: 0700
  ::chmod(socket_path_.c_str(), 0700);

  if (::listen(server_fd_, 128) < 0) {
    ::close(server_fd_);
    server_fd_ = -1;
    ::unlink(socket_path_.c_str());
    return false;
  }

  return true;
}

void SupervisorDaemon::run() {
  if (stop_requested_.load()) {
    return;
  }

  if (server_fd_ < 0) {
    if (!init()) {
      return;
    }
  }

  running_.store(true);

  std::vector<int> client_fds;
  auto last_telemetry = std::chrono::steady_clock::now();

  while (!stop_requested_.load()) {
    std::vector<struct pollfd> pfds;
    pfds.reserve(2 + client_fds.size());

    // 0: Server listener
    pfds.push_back({server_fd_, POLLIN, 0});
    // 1: Wake pipe
    pfds.push_back({wake_pipe_[0], POLLIN, 0});
    // 2+: Clients
    for (int cfd : client_fds) {
      pfds.push_back({cfd, POLLIN, 0});
    }

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       now - last_telemetry)
                       .count();
    if (elapsed >= config_.telemetry_interval_ms) {
      registry_.refresh_telemetry();
      last_telemetry = std::chrono::steady_clock::now();
      elapsed = 0;
    }

    int timeout_ms =
        std::max(10, config_.telemetry_interval_ms - static_cast<int>(elapsed));
    int ret = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), timeout_ms);

    if (stop_requested_.load()) {
      break;
    }

    if (ret < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }

    // Wake pipe read
    if (pfds[1].revents & POLLIN) {
      char buf[64];
      while (::read(wake_pipe_[0], buf, sizeof(buf)) > 0) {
      }
      if (stop_requested_.load()) {
        break;
      }
    }

    // Process clients that were polled in this iteration
    std::vector<int> active_clients;
    active_clients.reserve(client_fds.size() + 4);

    for (size_t i = 0; i < client_fds.size(); ++i) {
      int cfd = client_fds[i];
      const auto& pfd = pfds[2 + i];

      if (pfd.revents & (POLLERR | POLLNVAL)) {
        ::close(cfd);
        continue;
      }

      if (pfd.revents & POLLIN) {
        uint16_t msg_type = 0;
        std::string payload;
        if (recv_ipc_message(cfd, msg_type, payload, 50)) {
          handle_client_request(cfd, msg_type, payload);
          active_clients.push_back(cfd);
        } else {
          ::close(cfd);
        }
      } else if (pfd.revents & POLLHUP) {
        ::close(cfd);
      } else {
        active_clients.push_back(cfd);
      }
    }

    // Accept new client connections on server_fd_
    if (pfds[0].revents & POLLIN) {
      while (true) {
        int cfd = ::accept4(server_fd_, nullptr, nullptr,
                            SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (cfd < 0) {
          break;
        }
        active_clients.push_back(cfd);
      }
    }

    client_fds = std::move(active_clients);
  }

  for (int cfd : client_fds) {
    ::close(cfd);
  }

  if (server_fd_ >= 0) {
    ::close(server_fd_);
    server_fd_ = -1;
  }
  ::unlink(socket_path_.c_str());

  running_.store(false);
}

void SupervisorDaemon::stop() {
  stop_requested_.store(true);
  if (wake_pipe_[1] >= 0) {
    char b = 'x';
    (void)::write(wake_pipe_[1], &b, 1);
  }
  if (!running_.load()) {
    if (server_fd_ >= 0) {
      ::close(server_fd_);
      server_fd_ = -1;
    }
    ::unlink(socket_path_.c_str());
  }
}

bool SupervisorDaemon::is_running() const { return running_.load(); }

SessionRegistry& SupervisorDaemon::registry() { return registry_; }

const std::string& SupervisorDaemon::socket_path() const {
  return socket_path_;
}

void SupervisorDaemon::handle_client_request(int client_fd, uint16_t msg_type,
                                             const std::string& payload) {
  switch (msg_type) {
    case MSG_PING: {
      send_ipc_message(client_fd, MSG_PONG, "PONG", 500);
      break;
    }
    case MSG_REGISTER_SESSION: {
      SessionRegistration req = parse_session_registration(payload);
      uint64_t id = registry_.register_session(req);
      send_ipc_message(client_fd, MSG_ACK_RESP, std::to_string(id), 500);
      break;
    }
    case MSG_UNREGISTER_SESSION: {
      uint64_t id = parse_session_id(payload);
      if (id > 0 && registry_.unregister_session(id)) {
        send_ipc_message(client_fd, MSG_ACK_RESP, "OK", 500);
      } else {
        send_ipc_message(client_fd, MSG_ERROR_RESP, "Session not found", 500);
      }
      break;
    }
    case MSG_LIST_SESSIONS_REQ: {
      std::string json = registry_.to_json();
      send_ipc_message(client_fd, MSG_LIST_SESSIONS_RESP, json, 1000);
      break;
    }
    case MSG_FREEZE_SESSION_REQ: {
      uint64_t id = parse_session_id(payload);
      if (id > 0 && registry_.freeze_session(id)) {
        send_ipc_message(client_fd, MSG_FREEZE_SESSION_RESP, "OK", 500);
      } else {
        send_ipc_message(client_fd, MSG_ERROR_RESP, "Failed to freeze session",
                         500);
      }
      break;
    }
    case MSG_THAW_SESSION_REQ: {
      uint64_t id = parse_session_id(payload);
      if (id > 0 && registry_.thaw_session(id)) {
        send_ipc_message(client_fd, MSG_THAW_SESSION_RESP, "OK", 500);
      } else {
        send_ipc_message(client_fd, MSG_ERROR_RESP, "Failed to thaw session",
                         500);
      }
      break;
    }
    case MSG_KILL_SESSION_REQ: {
      uint64_t id = 0;
      int sig = SIGTERM;
      parse_kill_payload(payload, id, sig);
      if (id > 0 && registry_.kill_session(id, sig)) {
        send_ipc_message(client_fd, MSG_KILL_SESSION_RESP, "OK", 500);
      } else {
        send_ipc_message(client_fd, MSG_ERROR_RESP, "Failed to kill session",
                         500);
      }
      break;
    }
    case MSG_GET_STATS_REQ: {
      MemoryManager mgr;
      HostMemoryStats host = mgr.get_host_stats();
      KsmStats ksm = mgr.get_ksm_stats();
      std::string stats_json = format_stats_json(host, ksm);
      send_ipc_message(client_fd, MSG_GET_STATS_RESP, stats_json, 1000);
      break;
    }
    default: {
      send_ipc_message(client_fd, MSG_ERROR_RESP, "Unknown message type", 500);
      break;
    }
  }
}

// ============================================================================
// DaemonClient Implementation
// ============================================================================

DaemonClient::DaemonClient(std::string socket_path)
    : socket_path_(socket_path.empty() ? resolve_socket_path()
                                       : std::move(socket_path)) {}

DaemonClient::~DaemonClient() = default;

DaemonClient::DaemonClient(DaemonClient&&) noexcept = default;
DaemonClient& DaemonClient::operator=(DaemonClient&&) noexcept = default;

int DaemonClient::connect_socket(int timeout_ms) const {
  if (socket_path_.empty()) {
    return -1;
  }

  int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return -1;
  }

  struct sockaddr_un addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  if (socket_path_.size() >= sizeof(addr.sun_path)) {
    ::close(fd);
    return -1;
  }
  std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

  if (timeout_ms > 0) {
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  }

  if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) <
      0) {
    ::close(fd);
    return -1;
  }

  return fd;
}

bool DaemonClient::is_alive(int timeout_ms) {
  int fd = connect_socket(timeout_ms);
  if (fd < 0) {
    return false;
  }
  if (!send_ipc_message(fd, MSG_PING, "PING", timeout_ms)) {
    ::close(fd);
    return false;
  }
  uint16_t resp_type = 0;
  std::string resp_payload;
  bool ok = recv_ipc_message(fd, resp_type, resp_payload, timeout_ms);
  ::close(fd);
  return ok && (resp_type == MSG_PONG);
}

uint64_t DaemonClient::register_session(const SessionRegistration& req,
                                        int timeout_ms) {
  int fd = connect_socket(timeout_ms);
  if (fd < 0) {
    return 0;
  }
  std::string json = session_registration_to_json(req);
  if (!send_ipc_message(fd, MSG_REGISTER_SESSION, json, timeout_ms)) {
    ::close(fd);
    return 0;
  }
  uint16_t resp_type = 0;
  std::string resp_payload;
  bool ok = recv_ipc_message(fd, resp_type, resp_payload, timeout_ms);
  ::close(fd);
  if (!ok || resp_type == MSG_ERROR_RESP) {
    return 0;
  }
  return parse_session_id(resp_payload);
}

bool DaemonClient::unregister_session(uint64_t id, int timeout_ms) {
  int fd = connect_socket(timeout_ms);
  if (fd < 0) {
    return false;
  }
  std::string payload = std::to_string(id);
  if (!send_ipc_message(fd, MSG_UNREGISTER_SESSION, payload, timeout_ms)) {
    ::close(fd);
    return false;
  }
  uint16_t resp_type = 0;
  std::string resp_payload;
  bool ok = recv_ipc_message(fd, resp_type, resp_payload, timeout_ms);
  ::close(fd);
  return ok && (resp_type == MSG_ACK_RESP);
}

std::vector<SessionInfo> DaemonClient::list_sessions(int timeout_ms) {
  int fd = connect_socket(timeout_ms);
  if (fd < 0) {
    return {};
  }
  if (!send_ipc_message(fd, MSG_LIST_SESSIONS_REQ, "", timeout_ms)) {
    ::close(fd);
    return {};
  }
  uint16_t resp_type = 0;
  std::string resp_payload;
  bool ok = recv_ipc_message(fd, resp_type, resp_payload, timeout_ms);
  ::close(fd);
  if (!ok || resp_type != MSG_LIST_SESSIONS_RESP) {
    return {};
  }
  return SessionRegistry::from_json(resp_payload);
}

bool DaemonClient::freeze_session(uint64_t id, int timeout_ms) {
  int fd = connect_socket(timeout_ms);
  if (fd < 0) {
    return false;
  }
  std::string payload = std::to_string(id);
  if (!send_ipc_message(fd, MSG_FREEZE_SESSION_REQ, payload, timeout_ms)) {
    ::close(fd);
    return false;
  }
  uint16_t resp_type = 0;
  std::string resp_payload;
  bool ok = recv_ipc_message(fd, resp_type, resp_payload, timeout_ms);
  ::close(fd);
  return ok && (resp_type == MSG_FREEZE_SESSION_RESP);
}

bool DaemonClient::thaw_session(uint64_t id, int timeout_ms) {
  int fd = connect_socket(timeout_ms);
  if (fd < 0) {
    return false;
  }
  std::string payload = std::to_string(id);
  if (!send_ipc_message(fd, MSG_THAW_SESSION_REQ, payload, timeout_ms)) {
    ::close(fd);
    return false;
  }
  uint16_t resp_type = 0;
  std::string resp_payload;
  bool ok = recv_ipc_message(fd, resp_type, resp_payload, timeout_ms);
  ::close(fd);
  return ok && (resp_type == MSG_THAW_SESSION_RESP);
}

bool DaemonClient::kill_session(uint64_t id, int signal, int timeout_ms) {
  int fd = connect_socket(timeout_ms);
  if (fd < 0) {
    return false;
  }
  std::ostringstream oss;
  oss << "{\"id\": " << id << ", \"signal\": " << signal << "}";
  if (!send_ipc_message(fd, MSG_KILL_SESSION_REQ, oss.str(), timeout_ms)) {
    ::close(fd);
    return false;
  }
  uint16_t resp_type = 0;
  std::string resp_payload;
  bool ok = recv_ipc_message(fd, resp_type, resp_payload, timeout_ms);
  ::close(fd);
  return ok && (resp_type == MSG_KILL_SESSION_RESP);
}

std::string DaemonClient::get_stats(int timeout_ms) {
  int fd = connect_socket(timeout_ms);
  if (fd < 0) {
    return "";
  }
  if (!send_ipc_message(fd, MSG_GET_STATS_REQ, "", timeout_ms)) {
    ::close(fd);
    return "";
  }
  uint16_t resp_type = 0;
  std::string resp_payload;
  bool ok = recv_ipc_message(fd, resp_type, resp_payload, timeout_ms);
  ::close(fd);
  if (!ok || resp_type != MSG_GET_STATS_RESP) {
    return "";
  }
  return resp_payload;
}

const std::string& DaemonClient::socket_path() const { return socket_path_; }

}  // namespace virmux
