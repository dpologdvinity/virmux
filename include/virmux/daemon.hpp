/**
 * @file daemon.hpp
 * @brief Supervisor daemon server and IPC client for session lifecycle
 * management.
 */

#pragma once

#include <sys/types.h>

#include <atomic>
#include <csignal>
#include <cstdint>
#include <string>
#include <vector>

#include "virmux/compactor.hpp"
#include "virmux/ipc.hpp"
#include "virmux/session.hpp"

namespace virmux {

/**
 * @brief Configuration parameters for SupervisorDaemon.
 */
struct DaemonConfig {
  std::string socket_path{resolve_socket_path()};
  int telemetry_interval_ms{1000};
  bool auto_freeze_enabled{false};
  uint64_t idle_timeout_ms{500};
};

/**
 * @brief Background supervisor daemon managing session registry and IPC
 * requests.
 */
class SupervisorDaemon {
 public:
  explicit SupervisorDaemon(DaemonConfig config = {});
  ~SupervisorDaemon();

  SupervisorDaemon(const SupervisorDaemon&) = delete;
  SupervisorDaemon& operator=(const SupervisorDaemon&) = delete;
  SupervisorDaemon(SupervisorDaemon&&) = delete;
  SupervisorDaemon& operator=(SupervisorDaemon&&) = delete;

  /**
   * @brief Initialize Unix domain socket listener and clean up stale sockets.
   * @return true on successful bind and listen, false if another daemon is
   * running or bind fails.
   */
  bool init();

  /**
   * @brief Run event loop using poll/epoll, dispatching IPC requests and
   * telemetry updates. Blocks until stop() is called.
   */
  void run();

  /**
   * @brief Signal the event loop to terminate, close listener, and unlink
   * socket.
   */
  void stop();

  /**
   * @brief Check if the daemon event loop is currently running.
   */
  [[nodiscard]] bool is_running() const;

  /**
   * @brief Access the underlying session registry.
   */
  SessionRegistry& registry();

  /**
   * @brief Access the process memory compactor engine.
   */
  [[nodiscard]] ProcessCompactor& compactor();
  [[nodiscard]] const ProcessCompactor& compactor() const;

  /**
   * @brief Get configured socket path.
   */
  [[nodiscard]] const std::string& socket_path() const;

 private:
  void handle_client_request(int client_fd, uint16_t msg_type,
                             const std::string& payload);

  DaemonConfig config_;
  SessionRegistry registry_;
  ProcessCompactor compactor_;
  std::string socket_path_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_requested_{false};
  int server_fd_{-1};
  int wake_pipe_[2]{-1, -1};
};

/**
 * @brief High-level client for communicating with SupervisorDaemon over IPC.
 */
class DaemonClient {
 public:
  explicit DaemonClient(std::string socket_path = "");
  ~DaemonClient();

  DaemonClient(const DaemonClient&) = delete;
  DaemonClient& operator=(const DaemonClient&) = delete;
  DaemonClient(DaemonClient&&) noexcept;
  DaemonClient& operator=(DaemonClient&&) noexcept;

  /**
   * @brief Check whether the supervisor daemon is alive and responding to
   * pings.
   */
  bool is_alive(int timeout_ms = 500);

  /**
   * @brief Register a new process session with the daemon.
   * @return Assigned unique session ID, or 0 on failure.
   */
  uint64_t register_session(const SessionRegistration& req,
                            int timeout_ms = 1000);

  /**
   * @brief Unregister a session by ID.
   * @return true if successfully unregistered, false on error.
   */
  bool unregister_session(uint64_t id, int timeout_ms = 1000);

  /**
   * @brief Retrieve snapshot of all registered sessions from daemon.
   */
  std::vector<SessionInfo> list_sessions(int timeout_ms = 1000);

  /**
   * @brief Freeze a session by ID.
   */
  bool freeze_session(uint64_t id, int timeout_ms = 1000);

  /**
   * @brief Thaw a frozen session by ID.
   */
  bool thaw_session(uint64_t id, int timeout_ms = 1000);

  /**
   * @brief Send kill signal to session process tree.
   */
  bool kill_session(uint64_t id, int signal = SIGTERM, int timeout_ms = 1000);

  /**
   * @brief Query system and memory telemetry JSON stats from daemon.
   */
  std::string get_stats(int timeout_ms = 1000);

  /**
   * @brief Get client socket path.
   */
  [[nodiscard]] const std::string& socket_path() const;

 private:
  int connect_socket(int timeout_ms) const;

  std::string socket_path_;
};

}  // namespace virmux
