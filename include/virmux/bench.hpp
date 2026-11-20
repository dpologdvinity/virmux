/**
 * @file bench.hpp
 * @brief Benchmark workload generators, input latency probe, and telemetry
 * sampler.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
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
 * @brief High-resolution input latency probe measuring PTY or pipe round-trip
 * time.
 */
class InputLatencyProbe {
 public:
  /**
   * @brief Construct a new InputLatencyProbe.
   * @param force_pipe_fallback If true, bypass openpty() and use anonymous
   * pipe.
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
   * @brief Start background sampling thread measuring latency every
   * interval_ms.
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
 * @brief Real-time telemetry sampler monitoring host and process memory, swap,
 * and KSM stats.
 */
class TelemetrySampler {
 public:
  /**
   * @brief Construct a new TelemetrySampler instance.
   * @param procfs_root Root path for procfs (defaults to "/proc").
   * @param ksm_sysfs_root Root path for KSM sysfs (defaults to
   * "/sys/kernel/mm/ksm").
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
   * @brief Current RSS bytes (sum of tracked PIDs, or host used RAM if none
   * tracked).
   */
  [[nodiscard]] uint64_t current_rss_bytes() const;

  /**
   * @brief Current swap bytes (sum of tracked PIDs, or host used swap if none
   * tracked).
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

/**
 * @brief Telemetry and performance metrics recorded during a benchmark run.
 */
struct BenchmarkResult {
  std::string mode{"baseline"};
  int task_count{0};
  double wall_time_sec{0.0};
  uint64_t peak_rss_bytes{0};
  uint64_t peak_swap_bytes{0};
  uint64_t peak_ksm_bytes_saved{0};
  double avg_latency_ms{0.0};
  double p99_latency_ms{0.0};
  double min_latency_ms{0.0};
  double max_latency_ms{0.0};
  size_t latency_samples{0};
  int oom_killed_count{0};
  int successful_tasks{0};
  int failed_tasks{0};
};

/**
 * @brief Side-by-side comparison of unmanaged baseline vs Virmux-managed
 * benchmark results.
 */
struct BenchmarkComparison {
  BenchmarkResult baseline;
  BenchmarkResult managed;

  /**
   * @brief Percentage reduction in peak RSS memory usage ((1 - managed /
   * baseline) * 100).
   */
  [[nodiscard]] double rss_reduction_pct() const noexcept {
    if (baseline.peak_rss_bytes == 0) {
      return 0.0;
    }
    return (1.0 - static_cast<double>(managed.peak_rss_bytes) /
                      static_cast<double>(baseline.peak_rss_bytes)) *
           100.0;
  }

  /**
   * @brief Percentage reduction in peak swap memory usage ((1 - managed /
   * baseline) * 100).
   */
  [[nodiscard]] double swap_reduction_pct() const noexcept {
    if (baseline.peak_swap_bytes == 0) {
      return 0.0;
    }
    return (1.0 - static_cast<double>(managed.peak_swap_bytes) /
                      static_cast<double>(baseline.peak_swap_bytes)) *
           100.0;
  }

  /**
   * @brief Ratio of baseline input latency to managed input latency (baseline /
   * managed).
   */
  [[nodiscard]] double latency_improvement_x() const noexcept {
    if (managed.avg_latency_ms <= 0.0001) {
      return 1.0;
    }
    return baseline.avg_latency_ms / managed.avg_latency_ms;
  }

  /**
   * @brief Ratio of baseline completion time to managed completion time
   * (baseline / managed).
   */
  [[nodiscard]] double time_improvement_x() const noexcept {
    if (managed.wall_time_sec <= 0.0001) {
      return 1.0;
    }
    return baseline.wall_time_sec / managed.wall_time_sec;
  }
};

/**
 * @brief Format side-by-side ASCII/ANSI terminal comparison table.
 */
[[nodiscard]] std::string format_comparison_table(
    const BenchmarkComparison& comp);

/**
 * @brief Format benchmark comparison into structured JSON string.
 */
[[nodiscard]] std::string format_comparison_json(
    const BenchmarkComparison& comp);

/**
 * @brief Parse structured JSON string into BenchmarkComparison instance.
 * @return BenchmarkComparison on success, or std::nullopt on syntax error.
 */
[[nodiscard]] std::optional<BenchmarkComparison> parse_comparison_json(
    std::string_view json);

/**
 * @brief Benchmark runner orchestrating synthetic workload tasks in baseline
 * and managed modes.
 *
 * Each suite forks its workload processes (which run without exec'ing) before
 * starting any of its own threads, so call the run_* methods from a thread
 * that is the only one in the process. Cancel via request_stop() from a
 * signal handler (as the CLI does) rather than from a helper thread that is
 * alive while a suite starts.
 */
class BenchmarkRunner {
 public:
  /**
   * @brief Construct a BenchmarkRunner instance.
   * @param quick If true, executes quick iterations with reduced load.
   */
  explicit BenchmarkRunner(bool quick = false);
  ~BenchmarkRunner();

  BenchmarkRunner(const BenchmarkRunner&) = delete;
  BenchmarkRunner& operator=(const BenchmarkRunner&) = delete;
  BenchmarkRunner(BenchmarkRunner&&) noexcept;
  BenchmarkRunner& operator=(BenchmarkRunner&&) noexcept;

  /**
   * @brief Enable or disable quick mode.
   */
  void set_quick(bool quick) noexcept { quick_ = quick; }

  /**
   * @brief Check whether quick mode is enabled.
   */
  [[nodiscard]] bool is_quick() const noexcept { return quick_; }

  /**
   * @brief Asynchronously request stop (sets atomic flag, async-signal-safe).
   */
  void request_stop() noexcept;

  /**
   * @brief Signal the runner to terminate any running tasks immediately.
   */
  void stop();

  /**
   * @brief Run unmanaged baseline benchmark.
   * @param duration_sec Run duration in seconds.
   * @param tasks Number of concurrent tasks.
   * @return BenchmarkResult containing recorded metrics.
   */
  BenchmarkResult run_baseline(int duration_sec = 5, int tasks = 12);

  /**
   * @brief Run Virmux-managed benchmark.
   * @param duration_sec Run duration in seconds.
   * @param tasks Number of concurrent tasks.
   * @return BenchmarkResult containing recorded metrics.
   */
  BenchmarkResult run_managed(int duration_sec = 5, int tasks = 12);

  /**
   * @brief Run full gauntlet comparison: unmanaged baseline vs Virmux managed.
   * @param duration_sec Run duration in seconds.
   * @param tasks Number of concurrent tasks.
   * @return BenchmarkComparison with baseline and managed results.
   */
  BenchmarkComparison run_gauntlet(int duration_sec = 5, int tasks = 12);

 private:
  bool quick_{false};
  std::atomic<bool> stop_flag_{false};
  std::vector<pid_t> active_pids_;
  mutable std::mutex pids_mutex_;

  std::vector<BenchmarkTaskConfig> build_tasks(int count,
                                               int duration_sec) const;
  BenchmarkResult execute_suite(const std::vector<BenchmarkTaskConfig>& tasks,
                                bool managed, int duration_sec);
  void reap_children();
};

}  // namespace virmux
