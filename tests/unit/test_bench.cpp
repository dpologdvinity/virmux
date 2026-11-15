/**
 * @file test_bench.cpp
 * @brief Unit tests for benchmark workload generators, input latency probe, and
 * telemetry sampler.
 */

extern "C" {
#include "unity/unity.h"
}

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "virmux/bench.hpp"

void setUp(void) {}
void tearDown(void) {}

namespace {

void write_file(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream ofs(path);
  ofs << content;
}

struct TempDir {
  std::filesystem::path path;

  explicit TempDir(const char* tmpl = "/tmp/virmux_bench_test_XXXXXX") {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s", tmpl);
    char* res = ::mkdtemp(buf);
    if (res) {
      path = res;
    }
  }

  ~TempDir() {
    if (!path.empty() && std::filesystem::exists(path)) {
      std::error_code ec;
      std::filesystem::remove_all(path, ec);
    }
  }
};

}  // namespace

void test_workload_type_string_conversions(void) {
  TEST_ASSERT_EQUAL_STRING(
      "browser",
      virmux::workload_type_to_string(virmux::WorkloadType::BROWSER).data());
  TEST_ASSERT_EQUAL_STRING(
      "compiler",
      virmux::workload_type_to_string(virmux::WorkloadType::COMPILER).data());
  TEST_ASSERT_EQUAL_STRING(
      "model",
      virmux::workload_type_to_string(virmux::WorkloadType::MODEL).data());
  TEST_ASSERT_EQUAL_STRING(
      "build",
      virmux::workload_type_to_string(virmux::WorkloadType::BUILD).data());

  TEST_ASSERT_TRUE(virmux::workload_type_from_string("browser") ==
                   virmux::WorkloadType::BROWSER);
  TEST_ASSERT_TRUE(virmux::workload_type_from_string("Browser") ==
                   virmux::WorkloadType::BROWSER);
  TEST_ASSERT_TRUE(virmux::workload_type_from_string("BROWSER") ==
                   virmux::WorkloadType::BROWSER);

  TEST_ASSERT_TRUE(virmux::workload_type_from_string("compiler") ==
                   virmux::WorkloadType::COMPILER);
  TEST_ASSERT_TRUE(virmux::workload_type_from_string("Compiler") ==
                   virmux::WorkloadType::COMPILER);
  TEST_ASSERT_TRUE(virmux::workload_type_from_string("COMPILER") ==
                   virmux::WorkloadType::COMPILER);

  TEST_ASSERT_TRUE(virmux::workload_type_from_string("model") ==
                   virmux::WorkloadType::MODEL);
  TEST_ASSERT_TRUE(virmux::workload_type_from_string("Model") ==
                   virmux::WorkloadType::MODEL);
  TEST_ASSERT_TRUE(virmux::workload_type_from_string("MODEL") ==
                   virmux::WorkloadType::MODEL);

  TEST_ASSERT_TRUE(virmux::workload_type_from_string("build") ==
                   virmux::WorkloadType::BUILD);
  TEST_ASSERT_TRUE(virmux::workload_type_from_string("Build") ==
                   virmux::WorkloadType::BUILD);
  TEST_ASSERT_TRUE(virmux::workload_type_from_string("BUILD") ==
                   virmux::WorkloadType::BUILD);

  bool threw = false;
  try {
    (void)virmux::workload_type_from_string("invalid_workload_name");
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  TEST_ASSERT_TRUE(threw);
}

void test_benchmark_task_config_defaults(void) {
  virmux::BenchmarkTaskConfig cfg;
  TEST_ASSERT_TRUE(cfg.name.empty());
  TEST_ASSERT_TRUE(cfg.type == virmux::WorkloadType::BROWSER);
  TEST_ASSERT_EQUAL_INT(2, cfg.virtual_cores);
  TEST_ASSERT_EQUAL_UINT64(1073741824ULL, cfg.memory_limit_bytes);
  TEST_ASSERT_FALSE(cfg.is_heavy);
  TEST_ASSERT_TRUE(cfg.custom_command.empty());
  TEST_ASSERT_EQUAL_INT(5, cfg.duration_seconds);
  TEST_ASSERT_EQUAL_UINT64(256ULL, cfg.target_alloc_mb);
}

void test_run_synthetic_workload_browser(void) {
  // Test single iteration completion
  int ret = virmux::run_synthetic_workload(virmux::WorkloadType::BROWSER, 0, 1,
                                           nullptr);
  TEST_ASSERT_EQUAL_INT(0, ret);

  // Test immediate stop via flag
  std::atomic<bool> stop_flag{true};
  ret = virmux::run_synthetic_workload(virmux::WorkloadType::BROWSER, 10, 1,
                                       &stop_flag);
  TEST_ASSERT_EQUAL_INT(0, ret);
}

void test_run_synthetic_workload_compiler(void) {
  int ret = virmux::run_synthetic_workload(virmux::WorkloadType::COMPILER, 0, 1,
                                           nullptr);
  TEST_ASSERT_EQUAL_INT(0, ret);

  std::atomic<bool> stop_flag{true};
  ret = virmux::run_synthetic_workload(virmux::WorkloadType::COMPILER, 10, 1,
                                       &stop_flag);
  TEST_ASSERT_EQUAL_INT(0, ret);
}

void test_run_synthetic_workload_model(void) {
  int ret = virmux::run_synthetic_workload(virmux::WorkloadType::MODEL, 0, 1,
                                           nullptr);
  TEST_ASSERT_EQUAL_INT(0, ret);

  std::atomic<bool> stop_flag{true};
  ret = virmux::run_synthetic_workload(virmux::WorkloadType::MODEL, 10, 1,
                                       &stop_flag);
  TEST_ASSERT_EQUAL_INT(0, ret);
}

void test_run_synthetic_workload_build(void) {
  int ret = virmux::run_synthetic_workload(virmux::WorkloadType::BUILD, 0, 1,
                                           nullptr);
  TEST_ASSERT_EQUAL_INT(0, ret);

  std::atomic<bool> stop_flag{true};
  ret = virmux::run_synthetic_workload(virmux::WorkloadType::BUILD, 10, 1,
                                       &stop_flag);
  TEST_ASSERT_EQUAL_INT(0, ret);
}

void test_input_latency_probe_pty_sampling(void) {
  virmux::InputLatencyProbe probe(false);
  TEST_ASSERT_TRUE(probe.is_pty());

  // Test empty probe statistics
  TEST_ASSERT_EQUAL_UINT64(0, probe.sample_count());
  TEST_ASSERT_TRUE(probe.avg_latency_ms() == 0.0);
  TEST_ASSERT_TRUE(probe.max_latency_ms() == 0.0);
  TEST_ASSERT_TRUE(probe.min_latency_ms() == 0.0);
  TEST_ASSERT_TRUE(probe.p99_latency_ms() == 0.0);

  // Measure 3 samples
  for (int i = 0; i < 3; ++i) {
    double sample = probe.measure_sample();
    TEST_ASSERT_TRUE(sample >= 0.0);
  }

  TEST_ASSERT_EQUAL_UINT64(3, probe.sample_count());
  TEST_ASSERT_TRUE(probe.avg_latency_ms() > 0.0);
  TEST_ASSERT_TRUE(probe.max_latency_ms() >= probe.min_latency_ms());
  TEST_ASSERT_TRUE(probe.p99_latency_ms() > 0.0);

  probe.reset();
  TEST_ASSERT_EQUAL_UINT64(0, probe.sample_count());
}

void test_input_latency_probe_pipe_fallback(void) {
  virmux::InputLatencyProbe probe(true);  // Force pipe fallback
  TEST_ASSERT_FALSE(probe.is_pty());

  double sample = probe.measure_sample();
  TEST_ASSERT_TRUE(sample >= 0.0);
  TEST_ASSERT_EQUAL_UINT64(1, probe.sample_count());
  TEST_ASSERT_TRUE(probe.avg_latency_ms() > 0.0);
}

void test_input_latency_probe_background_thread(void) {
  virmux::InputLatencyProbe probe;
  TEST_ASSERT_FALSE(probe.is_running());

  probe.start(10);
  TEST_ASSERT_TRUE(probe.is_running());

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  probe.stop();

  TEST_ASSERT_FALSE(probe.is_running());
  TEST_ASSERT_TRUE(probe.sample_count() >= 2);
}

void test_telemetry_sampler_mock_procfs(void) {
  TempDir temp_proc("/tmp/virmux_bench_proc_XXXXXX");
  TempDir temp_ksm("/tmp/virmux_bench_ksm_XXXXXX");

  const std::string meminfo_content =
      "MemTotal:       16777216 kB\n"
      "MemFree:         4194304 kB\n"
      "MemAvailable:    8388608 kB\n"
      "Cached:          2097152 kB\n"
      "SwapTotal:       2097152 kB\n"
      "SwapFree:        1048576 kB\n";
  write_file(temp_proc.path / "meminfo", meminfo_content);

  write_file(temp_ksm.path / "run", "1\n");
  write_file(temp_ksm.path / "pages_sharing", "1000\n");

  virmux::TelemetrySampler sampler(temp_proc.path.string(),
                                   temp_ksm.path.string());
  TEST_ASSERT_FALSE(sampler.is_running());
  TEST_ASSERT_EQUAL_UINT64(0, sampler.sample_count());

  sampler.sample();
  TEST_ASSERT_EQUAL_UINT64(1, sampler.sample_count());

  TEST_ASSERT_EQUAL_UINT64(16777216ULL * 1024ULL, sampler.mem_total_bytes());
  TEST_ASSERT_EQUAL_UINT64(4194304ULL * 1024ULL, sampler.mem_free_bytes());
  TEST_ASSERT_EQUAL_UINT64(8388608ULL * 1024ULL, sampler.mem_available_bytes());
  TEST_ASSERT_EQUAL_UINT64(2097152ULL * 1024ULL, sampler.swap_total_bytes());
  TEST_ASSERT_EQUAL_UINT64(1048576ULL * 1024ULL, sampler.swap_free_bytes());

  // Host RSS = MemTotal - MemAvailable = (16777216 - 8388608) * 1024
  const uint64_t expected_rss = (16777216ULL - 8388608ULL) * 1024ULL;
  TEST_ASSERT_EQUAL_UINT64(expected_rss, sampler.current_rss_bytes());
  TEST_ASSERT_EQUAL_UINT64(expected_rss, sampler.peak_rss_bytes());

  // Host Swap = SwapTotal - SwapFree = (2097152 - 1048576) * 1024
  const uint64_t expected_swap = (2097152ULL - 1048576ULL) * 1024ULL;
  TEST_ASSERT_EQUAL_UINT64(expected_swap, sampler.current_swap_bytes());
  TEST_ASSERT_EQUAL_UINT64(expected_swap, sampler.peak_swap_bytes());

  // KSM pages_sharing = 1000 -> 1000 * 4096 = 4096000 bytes saved
  TEST_ASSERT_EQUAL_UINT64(1000ULL * 4096ULL, sampler.ksm_bytes_saved());
  TEST_ASSERT_EQUAL_UINT64(1000ULL * 4096ULL, sampler.peak_ksm_bytes_saved());
}

void test_telemetry_sampler_process_tracking(void) {
  TempDir temp_proc("/tmp/virmux_bench_proc_XXXXXX");
  TempDir temp_ksm("/tmp/virmux_bench_ksm_XXXXXX");

  const std::string meminfo_content =
      "MemTotal:       16777216 kB\n"
      "MemFree:         4194304 kB\n"
      "MemAvailable:    8388608 kB\n";
  write_file(temp_proc.path / "meminfo", meminfo_content);

  const std::string status_content =
      "Name:\tmock_worker\n"
      "VmRSS:\t   16384 kB\n"
      "VmSwap:\t    4096 kB\n";
  write_file(temp_proc.path / "4242" / "status", status_content);

  virmux::TelemetrySampler sampler(temp_proc.path.string(),
                                   temp_ksm.path.string());
  sampler.add_target_pid(4242);
  sampler.sample();

  TEST_ASSERT_EQUAL_UINT64(16384ULL * 1024ULL, sampler.current_rss_bytes());
  TEST_ASSERT_EQUAL_UINT64(4096ULL * 1024ULL, sampler.current_swap_bytes());
  TEST_ASSERT_EQUAL_UINT64(16384ULL * 1024ULL, sampler.peak_rss_bytes());
  TEST_ASSERT_EQUAL_UINT64(4096ULL * 1024ULL, sampler.peak_swap_bytes());

  sampler.clear_target_pids();
  sampler.sample();
  // Reverted to host level
  const uint64_t expected_host_rss = (16777216ULL - 8388608ULL) * 1024ULL;
  TEST_ASSERT_EQUAL_UINT64(expected_host_rss, sampler.current_rss_bytes());
}

void test_telemetry_sampler_real_host_and_thread(void) {
  virmux::TelemetrySampler sampler;
  sampler.sample();

  TEST_ASSERT_TRUE(sampler.mem_total_bytes() > 0);
  TEST_ASSERT_TRUE(sampler.current_rss_bytes() > 0);
  TEST_ASSERT_EQUAL_UINT64(1, sampler.sample_count());

  // Test background sampling thread
  sampler.start(10);
  TEST_ASSERT_TRUE(sampler.is_running());
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  sampler.stop();

  TEST_ASSERT_FALSE(sampler.is_running());
  TEST_ASSERT_TRUE(sampler.sample_count() >= 2);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_workload_type_string_conversions);
  RUN_TEST(test_benchmark_task_config_defaults);
  RUN_TEST(test_run_synthetic_workload_browser);
  RUN_TEST(test_run_synthetic_workload_compiler);
  RUN_TEST(test_run_synthetic_workload_model);
  RUN_TEST(test_run_synthetic_workload_build);
  RUN_TEST(test_input_latency_probe_pty_sampling);
  RUN_TEST(test_input_latency_probe_pipe_fallback);
  RUN_TEST(test_input_latency_probe_background_thread);
  RUN_TEST(test_telemetry_sampler_mock_procfs);
  RUN_TEST(test_telemetry_sampler_process_tracking);
  RUN_TEST(test_telemetry_sampler_real_host_and_thread);
  return UNITY_END();
}
