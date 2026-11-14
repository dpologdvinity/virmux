/**
 * @file sandbox.hpp
 * @brief Command sandbox launcher and process isolation controller for virmux.
 */

#pragma once

#include <sys/types.h>

#include <cstdint>
#include <string>

namespace virmux {

class AdmissionGate;

/**
 * @brief Configuration parameters for sandboxed process execution.
 */
struct SandboxConfig {
  int virtual_cores{2};
  uint64_t memory_limit_bytes{2ULL * 1024 * 1024 * 1024};
  std::string cpuset_cores{"2-11"};
  uint64_t max_pids{256};
  bool is_heavy{false};
  bool heavy{false};
  bool ephemeral{false};
  std::string workspace_dir{""};
  std::string scratch_dir{""};
  std::string workspace_path{""};
  AdmissionGate* gate{nullptr};
};

/**
 * @brief Spawn a command inside a virtualized sandbox and propagate its exit
 * code.
 *
 * Configures resource limits via cgroups v2, redirects /proc/meminfo to a
 * synthetic file reflecting the memory limit, injects hardware spoofing and
 * headless browser flags via LD_PRELOAD, and propagates child exit status or
 * signal termination code.
 *
 * @param cfg Sandbox configuration options.
 * @param argv NULL-terminated argument vector for execvp.
 * @return Process exit code (0-255), 128+signum on signal termination, or -1 on
 * error.
 */
int spawn_command(const SandboxConfig& cfg, char* const argv[]);

/**
 * @brief Retrieve reference to the default global AdmissionGate.
 * @return Reference to the AdmissionGate singleton.
 */
AdmissionGate& get_default_admission_gate();

/**
 * @brief Configure process OOM killer adjustment score (/proc/<pid>/oom_score_adj).
 * Clamps score to [-1000, 1000].
 * @param pid Process ID to configure (0 for current calling process).
 * @param score OOM score adjustment value between -1000 and 1000.
 * @return true on success, false on permission or path error without throwing.
 */
bool set_process_oom_score(pid_t pid, int score);

/**
 * @brief Configure OOM killer adjustment score to worker sacrificial priority
 * (+800).
 * @param child_pid Process ID to configure (0 for current calling process).
 */
void set_worker_oom_score(pid_t child_pid = 0);

}  // namespace virmux

/**
 * @brief C-compatible wrapper for virmux::spawn_command.
 */
inline int virmux_spawn_command(const virmux::SandboxConfig& cfg,
                                char* const argv[]) {
  return virmux::spawn_command(cfg, argv);
}

/**
 * @brief C-compatible wrapper for virmux::set_process_oom_score.
 */
inline bool virmux_set_process_oom_score(pid_t pid, int score) {
  return virmux::set_process_oom_score(pid, score);
}

/**
 * @brief C-compatible wrapper for virmux::set_worker_oom_score.
 */
inline void virmux_set_worker_oom_score(pid_t child_pid = 0) {
  virmux::set_worker_oom_score(child_pid);
}
