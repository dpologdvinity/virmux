#pragma once

#include <sys/types.h>

#include <cstdint>
#include <string>

namespace virmux {

/**
 * @brief Controller for Linux cgroups v2 slices.
 *
 * Configures resource limits (memory.high, cpuset.cpus), attaches processes,
 * and controls freeze/thaw states. Degrades gracefully under unprivileged
 * environments without throwing exceptions or aborting.
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
   * @return true on success, false on permission/path error (unprivileged
   * fallback).
   */
  bool setup(const std::string& base_cgroup_path,
             uint64_t memory_high_bytes = 0,
             const std::string& cpuset_cpus_str = "");

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
   * @return true if successfully initialized with write permissions, false
   * otherwise.
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

  bool write_file(const std::string& filename, const std::string& value);
};

}  // namespace virmux
