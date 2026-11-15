/**
 * @file telemetry.hpp
 * @brief Memory telemetry for process-level consumption, host meminfo, and zram
 * statistics.
 */

#pragma once

#include <sys/types.h>

#include <cstdint>
#include <string>
#include <vector>

namespace virmux {

/**
 * @brief Process-level memory consumption statistics from smaps_rollup or
 * status.
 */
struct ProcessMemoryStats {
  pid_t pid{0};
  uint64_t rss_bytes{0};
  uint64_t pss_bytes{0};
  uint64_t swap_bytes{0};
  uint64_t swap_pss_bytes{0};
  uint64_t referenced_bytes{0};
  uint64_t private_dirty_bytes{0};
};

/**
 * @brief Compressed swap device telemetry for Linux zram block devices.
 */
struct ZramDeviceStats {
  bool present{false};
  std::string name;
  std::string algorithm;
  uint64_t disk_size{0};
  uint64_t orig_data_size{0};
  uint64_t compr_data_size{0};
  uint64_t mem_used_total{0};
  double compression_ratio{1.0};
};

/**
 * @brief Host-wide memory capacity, utilization, and zram pool metrics.
 */
struct HostMemoryStats {
  uint64_t mem_total_bytes{0};
  uint64_t mem_available_bytes{0};
  uint64_t mem_free_bytes{0};
  uint64_t cached_bytes{0};
  uint64_t swap_total_bytes{0};
  uint64_t swap_free_bytes{0};
  std::vector<ZramDeviceStats> zram_devices;
};

/**
 * @brief Telemetry provider querying /proc and /sys/block.
 */
class MemoryTelemetry {
 public:
  /**
   * @brief Construct a new MemoryTelemetry instance.
   * @param procfs_root Procfs root directory path (defaults to "/proc").
   * @param sysfs_block_root Sysfs block root directory path (defaults to
   * "/sys/block").
   */
  explicit MemoryTelemetry(std::string procfs_root = "/proc",
                           std::string sysfs_block_root = "/sys/block");

  /**
   * @brief Read memory statistics for a specific process ID.
   * @param pid Process ID to query.
   * @return Populated ProcessMemoryStats, or zeroed metrics if process not
   * found.
   */
  [[nodiscard]] ProcessMemoryStats read_process_stats(pid_t pid) const;

  /**
   * @brief Read host memory metrics and all active zram device statistics.
   * @return Populated HostMemoryStats.
   */
  [[nodiscard]] HostMemoryStats read_host_stats() const;

  /**
   * @brief Read telemetry metrics for all available zram devices in sysfs block
   * root.
   * @return Vector of ZramDeviceStats sorted alphabetically by device name.
   */
  [[nodiscard]] std::vector<ZramDeviceStats> read_zram_stats() const;

 private:
  std::string procfs_root_;
  std::string sysfs_block_root_;
};

}  // namespace virmux
