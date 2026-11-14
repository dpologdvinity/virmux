/**
 * @file ksm.hpp
 * @brief Kernel Samepage Merging (KSM) controller and sysfs telemetry parser.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace virmux {

/**
 * @brief Telemetry snapshot for Linux Kernel Samepage Merging (KSM).
 */
struct KsmStats {
  bool present{false};
  bool enabled{false};
  uint64_t pages_shared{0};
  uint64_t pages_sharing{0};
  uint64_t pages_unshared{0};
  uint64_t pages_volatile{0};
  uint32_t pages_to_scan{0};
  uint32_t sleep_millisecs{0};
  uint64_t bytes_saved{0};
};

/**
 * @brief Controller for Linux Kernel Samepage Merging (KSM) via sysfs.
 *
 * Interfaces with `/sys/kernel/mm/ksm/` to monitor deduplication metrics,
 * enable/disable KSM scanning, and dynamically tune scanning rate based on
 * active session concurrency.
 */
class KsmController {
 public:
  /**
   * @brief Construct a new KsmController instance.
   * @param sysfs_root Path to KSM sysfs root directory, defaults to
   *                   "/sys/kernel/mm/ksm".
   */
  explicit KsmController(std::string sysfs_root = "/sys/kernel/mm/ksm");

  /**
   * @brief Read full KSM telemetry statistics from sysfs.
   * @return Populated KsmStats struct, or zeroed struct if KSM is unavailable.
   */
  [[nodiscard]] KsmStats read_stats() const;

  /**
   * @brief Check whether KSM is available and supported on the host.
   * @return true if sysfs root directory and readable 'run' file exist.
   */
  [[nodiscard]] bool is_available() const;

  /**
   * @brief Check whether KSM scanning is currently active.
   * @return true if KSM is enabled ('run' == 1), false otherwise.
   */
  [[nodiscard]] bool is_enabled() const;

  /**
   * @brief Enable or disable KSM page scanning.
   * @param activate true to enable ('run' = 1), false to disable ('run' = 0).
   * @return true on success, false if unprivileged, read-only, or failed.
   */
  bool enable(bool activate = true);

  /**
   * @brief Tune KSM scanning parameters.
   * @param pages_to_scan Number of pages to scan before sleeping.
   * @param sleep_millisecs Milliseconds to sleep between scan cycles.
   * @return true if both values were successfully applied, false otherwise.
   */
  bool tune(uint32_t pages_to_scan, uint32_t sleep_millisecs);

  /**
   * @brief Compute adaptive scan rate based on active session count.
   *
   * Baseline is 100 pages, scaling by 50 pages per session up to 2000 pages.
   *
   * @param active_sessions Number of active runtime sessions.
   * @return Target pages_to_scan value clamped between 100 and 2000.
   */
  [[nodiscard]] uint32_t compute_adaptive_pages(size_t active_sessions) const;

 private:
  std::string sysfs_root_;
};

}  // namespace virmux
