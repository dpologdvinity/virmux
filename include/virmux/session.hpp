#pragma once

#include <sys/types.h>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace virmux {

/**
 * @brief Lifecycle states for a managed virmux session.
 */
enum class SessionState {
  INITIALIZING,
  RUNNING_BURST,
  RUNNING_NORMAL,
  WAITING_IO,
  FROZEN,
  DEAD
};

/**
 * @brief Convert SessionState enum to string representation.
 */
std::string_view session_state_to_string(SessionState s);

/**
 * @brief Convert string representation to SessionState enum.
 */
SessionState session_state_from_string(std::string_view str);

/**
 * @brief Telemetry snapshot and metadata for a managed session.
 */
struct SessionInfo {
  uint64_t id{0};
  std::string name;
  pid_t pid{0};
  SessionState state{SessionState::INITIALIZING};
  int virtual_cores{2};
  uint64_t memory_rss_bytes{0};
  uint64_t memory_limit_bytes{0};
  uint64_t ksm_dedup_bytes{0};
  std::string cgroup_path;
  std::string command;
  std::chrono::system_clock::time_point start_time;
  uint64_t last_cpu_ticks{0};
  std::chrono::steady_clock::time_point last_active_time{
      std::chrono::steady_clock::now()};
};

/**
 * @brief Input payload for registering a session.
 */
struct SessionRegistration {
  std::string name;
  pid_t pid{0};
  int virtual_cores{2};
  uint64_t memory_limit_bytes{0};
  std::string cgroup_path;
  std::string command;
};

/**
 * @brief Thread-safe session registry and lifecycle manager.
 */
class SessionRegistry {
 public:
  explicit SessionRegistry(std::string procfs_root = "/proc");
  ~SessionRegistry();

  SessionRegistry(const SessionRegistry&) = delete;
  SessionRegistry& operator=(const SessionRegistry&) = delete;
  SessionRegistry(SessionRegistry&&) = delete;
  SessionRegistry& operator=(SessionRegistry&&) = delete;

  /**
   * @brief Register a new session.
   *
   * Assigns incremental ID, derives name from command basename if empty,
   * sets state to RUNNING_NORMAL.
   *
   * @param req Registration parameters.
   * @return Unique session ID.
   */
  uint64_t register_session(const SessionRegistration& req);

  /**
   * @brief Unregister a session by ID.
   * @param id Session ID.
   * @return true if found and removed, false otherwise.
   */
  bool unregister_session(uint64_t id);

  /**
   * @brief Look up session by ID.
   * @param id Session ID.
   * @return SessionInfo if found, std::nullopt otherwise.
   */
  std::optional<SessionInfo> get_session(uint64_t id) const;

  /**
   * @brief Look up session by PID.
   * @param pid Process ID.
   * @return SessionInfo if found, std::nullopt otherwise.
   */
  std::optional<SessionInfo> get_session_by_pid(pid_t pid) const;

  /**
   * @brief Retrieve snapshot of all registered sessions.
   * @return List of SessionInfo objects.
   */
  std::vector<SessionInfo> list_sessions() const;

  /**
   * @brief Refresh process telemetry (liveness, RSS, KSM dedup) across all
   * sessions.
   */
  void refresh_telemetry();

  /**
   * @brief Configure transparent inactivity auto-freeze engine.
   * @param enabled Whether auto-freeze is active.
   * @param idle_timeout_ms Inactivity idle threshold in milliseconds (default: 500).
   */
  void set_auto_freeze(bool enabled, uint64_t idle_timeout_ms = 500);

  /**
   * @brief Check if auto-freeze is enabled.
   */
  [[nodiscard]] bool auto_freeze_enabled() const;

  /**
   * @brief Get configured inactivity timeout in milliseconds.
   */
  [[nodiscard]] uint64_t idle_timeout_ms() const;

  /**
   * @brief Register callback invoked when a session is auto-frozen due to inactivity.
   * @param cb Callback function taking session ID and process PID.
   */
  void set_on_auto_freeze(std::function<void(uint64_t id, pid_t pid)> cb);

  /**
   * @brief Prune sessions in DEAD state that have exceeded retention duration.
   * @param max_age Retention duration threshold (0 to prune all DEAD sessions immediately).
   * @return Number of pruned sessions.
   */
  size_t prune_dead_sessions(
      std::chrono::milliseconds max_age = std::chrono::milliseconds(5000));

  /**
   * @brief Freeze session via cgroup.freeze or SIGSTOP fallback.
   * @param id Session ID.
   * @return true if frozen, false on failure or session dead/missing.
   */
  bool freeze_session(uint64_t id);

  /**
   * @brief Thaw session via cgroup.freeze or SIGCONT fallback.
   * @param id Session ID.
   * @return true if thawed, false on failure or session dead/missing.
   */
  bool thaw_session(uint64_t id);

  /**
   * @brief Kill session via signal.
   * @param id Session ID.
   * @param signal POSIX signal (default SIGTERM).
   * @return true if signal sent or already dead, false if session missing.
   */
  bool kill_session(uint64_t id, int signal = SIGTERM);

  /**
   * @brief Serialize session list to JSON array.
   * @return JSON string.
   */
  std::string to_json() const;

  /**
   * @brief Deserialize JSON array into SessionInfo vector.
   * @param json JSON string view.
   * @return Vector of SessionInfo objects.
   */
  static std::vector<SessionInfo> from_json(std::string_view json);

 private:
  mutable std::mutex mutex_;
  std::string procfs_root_;
  uint64_t next_id_{1};
  std::vector<SessionInfo> sessions_;
  std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> dead_since_;
  bool auto_freeze_enabled_{false};
  uint64_t idle_timeout_ms_{500};
  std::function<void(uint64_t id, pid_t pid)> on_auto_freeze_;
};

}  // namespace virmux
