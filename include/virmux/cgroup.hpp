#pragma once

#include <sys/types.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace virmux {

/**
 * @brief Resource limits to write into a cgroup. Unset fields are left alone.
 */
struct CgroupLimits {
  std::optional<uint64_t> memory_max;   ///< memory.max (hard ceiling, bytes)
  std::optional<uint64_t> memory_high;  ///< memory.high (throttle point)
  std::optional<uint64_t> swap_max;     ///< memory.swap.max (bytes)
  std::string cpu_max;      ///< cpu.max, e.g. "200000 100000" ("" = unset)
  std::string cpuset_cpus;  ///< cpuset.cpus list, e.g. "2-3" ("" = unset)
  std::optional<uint64_t> pids_max;  ///< pids.max (0 writes "max")
};

/**
 * @brief Inputs for delegation discovery (injectable for tests).
 */
struct CgroupProbe {
  std::string proc_self_cgroup;        ///< Contents of /proc/self/cgroup.
  std::string root{"/sys/fs/cgroup"};  ///< cgroup2 mount point.
  uid_t uid{0};                        ///< Real uid of the caller.

  /** @brief Probe describing the calling process. */
  static CgroupProbe current();
};

/** @brief Where virmux may create its own cgroups. */
enum class DelegationKind {
  kNone,              ///< Nowhere: run without cgroup limits.
  kUserManager,       ///< The systemd user manager's user@<uid>.service tree.
  kWritableAncestor,  ///< Highest writable ancestor (root, containers, CI).
};

/**
 * @brief Result of find_cgroup_delegation().
 */
struct CgroupDelegation {
  DelegationKind kind{DelegationKind::kNone};
  std::string root;    ///< Delegation root D (absolute); "" when kNone.
  std::string self;    ///< Our own cgroup (absolute); "" if unknown.
  std::string reason;  ///< One-line explanation when kNone.

  [[nodiscard]] bool usable() const noexcept {
    return kind != DelegationKind::kNone;
  }
};

/**
 * @brief Find a cgroup subtree the caller may create cgroups in.
 *
 * Reads the cgroup v2 ("0::") line of /proc/self/cgroup and walks up from the
 * caller's cgroup. The nearest `user@<uid>.service` ancestor whose
 * cgroup.procs and cgroup.subtree_control are writable wins (systemd
 * delegates cpu/memory/pids there). Otherwise the highest writable ancestor
 * is used (root, containers). Otherwise there is no delegation, for example a
 * shell in an ssh/console `session-N.scope`, which cannot leave its scope
 * without D-Bus; @ref CgroupDelegation::reason then suggests
 * `systemd-run --user --scope`.
 */
[[nodiscard]] CgroupDelegation find_cgroup_delegation(const CgroupProbe& probe);

/**
 * @brief Outcome of writing one control file.
 */
struct LimitResult {
  std::string file;   ///< Control file name, e.g. "memory.max".
  std::string value;  ///< Value that was (or would have been) written.
  bool applied{false};
  int error{0};  ///< errno of the failed open/write; 0 when applied.
};

/**
 * @brief Controller for Linux cgroups v2 slices.
 *
 * Configures resource limits (memory, cpu, cpuset, pids), attaches processes,
 * and controls freeze/thaw states. Degrades gracefully under unprivileged
 * environments without throwing exceptions or aborting. Control files are
 * only ever written, never created: a missing file means the controller is
 * not enabled for this cgroup.
 */
class CgroupSlice {
 public:
  /**
   * @brief Construct a CgroupSlice with an optional slice name.
   * @param name Optional slice identifier (e.g. "session-1").
   */
  explicit CgroupSlice(std::string name = "");

  /**
   * @brief Destructor; attempts to remove the created cgroup directory if
   * empty.
   */
  ~CgroupSlice();

  // Rule of five: non-copyable, movable
  CgroupSlice(const CgroupSlice&) = delete;
  CgroupSlice& operator=(const CgroupSlice&) = delete;

  CgroupSlice(CgroupSlice&& other) noexcept;
  CgroupSlice& operator=(CgroupSlice&& other) noexcept;

  /**
   * @brief Initialize slice with given name under default virmux base slice.
   * @param slice_name Name of the sub-slice.
   * @return true on success, false on permission/path error (unprivileged
   * fallback).
   */
  bool init(const std::string& slice_name);

  /**
   * @brief Set up slice directory and configure resource limits.
   * @param base_cgroup_path Base cgroup path (e.g.
   * /sys/fs/cgroup/virmux.slice).
   * @param memory_high_bytes Soft memory ceiling in bytes (0 to leave unset).
   * @param cpuset_cpus_str Allowed CPU core list (e.g. "2-11", empty to leave
   * unset).
   * Limits are applied independently; a limit that cannot be written does
   * not make the slice invalid.
   * @return true if the slice is valid (see is_valid()), false on
   * permission/path error (unprivileged fallback).
   */
  bool setup(const std::string& base_cgroup_path,
             uint64_t memory_high_bytes = 0,
             const std::string& cpuset_cpus_str = "");

  /**
   * @brief Write every limit set in @p limits, each independently.
   *
   * A failure (for example a controller that is not delegated, so its control
   * file does not exist) never prevents the remaining limits from applying.
   * @return One result per requested limit, in a fixed order: memory.max,
   * memory.high, memory.swap.max, cpu.max, cpuset.cpus, pids.max.
   */
  std::vector<LimitResult> apply_limits(const CgroupLimits& limits);

  /**
   * @brief Configure memory.max hard limit.
   * @param bytes Memory limit in bytes.
   * @return true on success, false on error.
   */
  bool set_memory_max(uint64_t bytes);

  /**
   * @brief Configure memory.swap.max.
   * @param bytes Swap limit in bytes.
   * @return true on success, false on error.
   */
  bool set_swap_max(uint64_t bytes);

  /**
   * @brief Configure cpu.max bandwidth ("<quota> <period>" or "max <period>").
   * @param value Raw cpu.max value.
   * @return true on success, false on error.
   */
  bool set_cpu_max(const std::string& value);

  /**
   * @brief Configure memory.high soft limit.
   * @param bytes Memory limit in bytes.
   * @return true on success, false on error.
   */
  bool set_memory_high(uint64_t bytes);

  /**
   * @brief Configure cpuset.cpus pinned core range.
   * @param cores_str Range string (e.g. "2-11").
   * @return true on success, false on error.
   */
  bool set_cpuset(const std::string& cores_str);

  /**
   * @brief Configure pids.max limit for fork-bomb containment.
   * @param max_pids Maximum process count ceiling (0 writes "max").
   * @return true on success, false on error.
   */
  bool set_pids_max(uint64_t max_pids);

  /**
   * @brief Attach a process to this cgroup slice (writes to cgroup.procs).
   * @param pid Process ID to attach.
   * @return true on success, false on error.
   */
  bool attach_pid(pid_t pid);

  /**
   * @brief Freeze all processes in this cgroup slice (writes "1" to
   * cgroup.freeze).
   * @return true on success, false on error.
   */
  bool freeze();

  /**
   * @brief Thaw all processes in this cgroup slice (writes "0" to
   * cgroup.freeze).
   * @return true on success, false on error.
   */
  bool thaw();

  /**
   * @brief Absolute path to the cgroup directory.
   * @return Directory path string.
   */
  [[nodiscard]] const std::string& path() const noexcept;

  /**
   * @brief Name of the cgroup slice.
   * @return Slice name string.
   */
  [[nodiscard]] const std::string& name() const noexcept;

  /**
   * @brief Check whether the cgroup slice is valid and functional.
   *
   * Valid means the directory exists and its cgroup.procs is writable, so
   * processes can join it. Limits are reported separately by apply_limits().
   * @return true if processes can be attached, false otherwise.
   */
  [[nodiscard]] bool is_valid() const noexcept;

  /**
   * @brief Alias for is_valid().
   * @return true if valid, false otherwise.
   */
  [[nodiscard]] bool valid() const noexcept;

 private:
  std::string name_;
  std::string path_;
  bool valid_{false};
  bool created_{false};

  int last_errno_{0};

  /**
   * @brief Write @p value to an existing control file (never creates it).
   * Silent: failures are recorded in last_errno_ for the caller to report.
   */
  bool write_file(const std::string& filename, const std::string& value);
};

}  // namespace virmux
