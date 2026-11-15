/**
 * @file bench.hpp
 * @brief Benchmark workload generators, input latency probe, and telemetry sampler.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "virmux/ksm.hpp"
#include "virmux/telemetry.hpp"

namespace virmux {

/**
 * @brief Synthetic workload category for benchmark tasks.
 */
enum class WorkloadType {
  BROWSER,
  COMPILER,
  MODEL,
  BUILD,
};

/**
 * @brief Convert WorkloadType enum to lowercase string representation.
 */
[[nodiscard]] std::string_view workload_type_to_string(WorkloadType type);

/**
 * @brief Parse string to WorkloadType enum (case-insensitive).
 * @throws std::invalid_argument if string does not match any WorkloadType.
 */
[[nodiscard]] WorkloadType workload_type_from_string(std::string_view sv);

/**
 * @brief Configuration for a single benchmark task in the gauntlet suite.
 */
struct BenchmarkTaskConfig {
  std::string name;
  WorkloadType type{WorkloadType::BROWSER};
  int virtual_cores{2};
  uint64_t memory_limit_bytes{1073741824ULL};
  bool is_heavy{false};
  std::string custom_command;
  int duration_seconds{5};
  uint64_t target_alloc_mb{256};
};

/**
 * @brief Run a synthetic workload of the specified type.
 *
 * @param type Workload type (BROWSER, COMPILER, MODEL, BUILD).
 * @param duration_seconds Duration in seconds (0 = run single iteration).
 * @param target_alloc_mb Memory allocation target in megabytes.
 * @param stop_flag Optional atomic flag to signal early termination.
 * @return 0 on success, non-zero on error.
 */
int run_synthetic_workload(WorkloadType type, int duration_seconds,
                           uint64_t target_alloc_mb,
                           std::atomic<bool>* stop_flag = nullptr);

/**
 * @brief High-resolution input latency probe measuring PTY or pipe round-trip time.
 */
class InputLatencyProbe {
 public:
  /**
   * @brief Construct a new InputLatencyProbe.
   * @param force_pipe_fallback If true, bypass openpty() and use anonymous pipe.
   */
  explicit InputLatencyProbe(bool force_pipe_fallback = false);

  /**
   * @brief Destructor stops sampling thread and closes file descriptors.
   */
  ~InputLatencyProbe();

  InputLatencyProbe(const InputLatencyProbe&) = delete;
  InputLatencyProbe& operator=(const InputLatencyProbe&) = delete;
  InputLatencyProbe(InputLatencyProbe&&) noexcept;
  InputLatencyProbe& operator=(InputLatencyProbe&&) noexcept;

  /**
   * @brief Start background sampling thread measuring latency every interval_ms.
   */
  void start(int interval_ms = 50);

  /**
   * @brief Stop background sampling thread.
   */
  void stop();

  /**
   * @brief Trigger an immediate round-trip latency sample.
   * @return Latency in milliseconds, or -1.0 on error/timeout.
   */
  double measure_sample();

  /**
   * @brief Average round-trip latency across all samples in milliseconds.
   */
  [[nodiscard]] double avg_latency_ms() const;

  /**
   * @brief 99th percentile round-trip latency in milliseconds.
   */
  [[nodiscard]] double p99_latency_ms() const;

  /**
   * @brief Maximum round-trip latency in milliseconds.
   */
  [[nodiscard]] double max_latency_ms() const;

  /**
   * @brief Minimum round-trip latency in milliseconds.
   */
  [[nodiscard]] double min_latency_ms() const;

  /**
   * @brief Total number of recorded samples.
   */
  [[nodiscard]] size_t sample_count() const;

  /**
   * @brief Check whether sampling thread is active.
   */
  [[nodiscard]] bool is_running() const;

  /**
   * @brief Check whether probe is using pseudo-terminal (pty) or pipe fallback.
   */
  [[nodiscard]] bool is_pty() const;

  /**
   * @brief Clear all recorded samples.
   */
  void reset();

 private:
  bool force_pipe_fallback_{false};
  bool is_pty_{false};
  int master_fd_{-1};
  int slave_fd_{-1};

  mutable std::mutex samples_mutex_;
  std::mutex io_mutex_;
  std::condition_variable cv_;
  std::thread worker_;
  std::atomic<bool> running_{false};
  std::vector<double> samples_;

  bool open_descriptors();
  void close_descriptors();
};

/**
 * @brief Real-time telemetry sampler monitoring host and process memory, swap, and KSM stats.
 */
class TelemetrySampler {
 public:
  /**
   * @brief Construct a new TelemetrySampler instance.
   * @param procfs_root Root path for procfs (defaults to "/proc").
   * @param ksm_sysfs_root Root path for KSM sysfs (defaults to "/sys/kernel/mm/ksm").
   */
  explicit TelemetrySampler(std::string procfs_root = "/proc",
                            std::string ksm_sysfs_root = "/sys/kernel/mm/ksm");

  /**
   * @brief Destructor stops background sampling thread.
   */
  ~TelemetrySampler();

  TelemetrySampler(const TelemetrySampler&) = delete;
  TelemetrySampler& operator=(const TelemetrySampler&) = delete;
  TelemetrySampler(TelemetrySampler&&) noexcept;
  TelemetrySampler& operator=(TelemetrySampler&&) noexcept;

  /**
   * @brief Start background sampling thread at specified interval.
   */
  void start(int interval_ms = 50);

  /**
   * @brief Stop background sampling thread.
   */
  void stop();

  /**
   * @brief Perform an immediate synchronous telemetry sample.
   */
  void sample();

  /**
   * @brief Current RSS bytes (sum of tracked PIDs, or host used RAM if none tracked).
   */
  [[nodiscard]] uint64_t current_rss_bytes() const;

  /**
   * @brief Current swap bytes (sum of tracked PIDs, or host used swap if none tracked).
   */
  [[nodiscard]] uint64_t current_swap_bytes() const;

  /**
   * @brief Peak recorded RSS bytes.
   */
  [[nodiscard]] uint64_t peak_rss_bytes() const;

  /**
   * @brief Peak recorded swap bytes.
   */
  [[nodiscard]] uint64_t peak_swap_bytes() const;

  /**
   * @brief Host total physical memory in bytes.
   */
  [[nodiscard]] uint64_t mem_total_bytes() const;

  /**
   * @brief Host free physical memory in bytes.
   */
  [[nodiscard]] uint64_t mem_free_bytes() const;

  /**
   * @brief Host available physical memory in bytes.
   */
  [[nodiscard]] uint64_t mem_available_bytes() const;

  /**
   * @brief Host total swap capacity in bytes.
   */
  [[nodiscard]] uint64_t swap_total_bytes() const;

  /**
   * @brief Host free swap capacity in bytes.
   */
  [[nodiscard]] uint64_t swap_free_bytes() const;

  /**
   * @brief Current bytes saved via KSM deduplication.
   */
  [[nodiscard]] uint64_t ksm_bytes_saved() const;

  /**
   * @brief Peak bytes saved via KSM deduplication.
   */
  [[nodiscard]] uint64_t peak_ksm_bytes_saved() const;

  /**
   * @brief Total number of recorded samples.
   */
  [[nodiscard]] size_t sample_count() const;

  /**
   * @brief Check whether sampling thread is active.
   */
  [[nodiscard]] bool is_running() const;

  /**
   * @brief Set specific process IDs to track memory metrics.
   */
  void set_target_pids(const std::vector<pid_t>& pids);

  /**
   * @brief Add a process ID to tracked processes.
   */
  void add_target_pid(pid_t pid);

  /**
   * @brief Clear tracked process IDs, reverting to host-level metrics.
   */
  void clear_target_pids();

 private:
  std::string procfs_root_;
  std::string ksm_sysfs_root_;
  MemoryTelemetry mem_telemetry_;
  KsmController ksm_controller_;

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::thread worker_;
  std::atomic<bool> running_{false};

  std::vector<pid_t> tracked_pids_;

  uint64_t current_rss_{0};
  uint64_t current_swap_{0};
  uint64_t peak_rss_{0};
  uint64_t peak_swap_{0};

  uint64_t mem_total_{0};
  uint64_t mem_free_{0};
  uint64_t mem_available_{0};
  uint64_t swap_total_{0};
  uint64_t swap_free_{0};

  uint64_t ksm_bytes_saved_{0};
  uint64_t peak_ksm_bytes_saved_{0};
  size_t sample_count_{0};
};

}  // namespace virmux
