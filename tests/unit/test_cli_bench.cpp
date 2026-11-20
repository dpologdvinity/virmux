/**
 * @file test_cli_bench.cpp
 * @brief Unit tests for CLI bench runner, table/JSON formatting, and
 * calculation helpers.
 */

extern "C" {
#include "unity/unity.h"
}

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "virmux/bench.hpp"
#include "virmux/cli_bench.hpp"

void setUp(void) {}
void tearDown(void) {}

void test_comparison_calculation_helpers(void) {
  virmux::BenchmarkComparison comp;
  comp.baseline.peak_rss_bytes = 1000000ULL;
  comp.managed.peak_rss_bytes = 300000ULL;

  // 70% reduction
  TEST_ASSERT_TRUE(std::abs(comp.rss_reduction_pct() - 70.0) < 0.01);

  // Zero baseline handles gracefully
  comp.baseline.peak_rss_bytes = 0;
  TEST_ASSERT_TRUE(comp.rss_reduction_pct() == 0.0);

  // Managed greater than baseline yields negative reduction
  comp.baseline.peak_rss_bytes = 1000ULL;
  comp.managed.peak_rss_bytes = 1200ULL;
  TEST_ASSERT_TRUE(std::abs(comp.rss_reduction_pct() - (-20.0)) < 0.01);

  // Swap reduction
  comp.baseline.peak_swap_bytes = 500000ULL;
  comp.managed.peak_swap_bytes = 100000ULL;
  TEST_ASSERT_TRUE(std::abs(comp.swap_reduction_pct() - 80.0) < 0.01);

  comp.baseline.peak_swap_bytes = 0;
  TEST_ASSERT_TRUE(comp.swap_reduction_pct() == 0.0);

  // Latency improvement
  comp.baseline.avg_latency_ms = 100.0;
  comp.managed.avg_latency_ms = 2.0;
  TEST_ASSERT_TRUE(std::abs(comp.latency_improvement_x() - 50.0) < 0.01);

  comp.managed.avg_latency_ms = 0.0;
  TEST_ASSERT_TRUE(comp.latency_improvement_x() == 1.0);

  // Wall-clock time improvement
  comp.baseline.wall_time_sec = 24.0;
  comp.managed.wall_time_sec = 6.0;
  TEST_ASSERT_TRUE(std::abs(comp.time_improvement_x() - 4.0) < 0.01);

  comp.managed.wall_time_sec = 0.0;
  TEST_ASSERT_TRUE(comp.time_improvement_x() == 1.0);
}

void test_json_format_and_parse_roundtrip(void) {
  virmux::BenchmarkComparison comp;
  comp.baseline.mode = "baseline";
  comp.baseline.task_count = 12;
  comp.baseline.wall_time_sec = 18.52;
  comp.baseline.peak_rss_bytes = 13369344000ULL;
  comp.baseline.peak_swap_bytes = 2254857830ULL;
  comp.baseline.peak_ksm_bytes_saved = 0ULL;
  comp.baseline.avg_latency_ms = 124.50;
  comp.baseline.p99_latency_ms = 450.00;
  comp.baseline.min_latency_ms = 15.20;
  comp.baseline.max_latency_ms = 520.10;
  comp.baseline.latency_samples = 300;
  comp.baseline.oom_killed_count = 2;
  comp.baseline.successful_tasks = 10;
  comp.baseline.failed_tasks = 2;

  comp.managed.mode = "managed";
  comp.managed.task_count = 12;
  comp.managed.wall_time_sec = 7.12;
  comp.managed.peak_rss_bytes = 4101488640ULL;
  comp.managed.peak_swap_bytes = 0ULL;
  comp.managed.peak_ksm_bytes_saved = 1342177280ULL;
  comp.managed.avg_latency_ms = 2.10;
  comp.managed.p99_latency_ms = 3.50;
  comp.managed.min_latency_ms = 0.85;
  comp.managed.max_latency_ms = 4.12;
  comp.managed.latency_samples = 300;
  comp.managed.oom_killed_count = 0;
  comp.managed.successful_tasks = 12;
  comp.managed.failed_tasks = 0;

  const std::string json = virmux::format_comparison_json(comp);
  TEST_ASSERT_TRUE(!json.empty());
  TEST_ASSERT_TRUE(json.find("\"baseline\"") != std::string::npos);
  TEST_ASSERT_TRUE(json.find("\"managed\"") != std::string::npos);
  TEST_ASSERT_TRUE(json.find("\"summary\"") != std::string::npos);

  auto parsed = virmux::parse_comparison_json(json);
  TEST_ASSERT_TRUE(parsed.has_value());

  TEST_ASSERT_EQUAL_STRING(comp.baseline.mode.c_str(),
                           parsed->baseline.mode.c_str());
  TEST_ASSERT_EQUAL_INT(comp.baseline.task_count, parsed->baseline.task_count);
  TEST_ASSERT_TRUE(std::abs(comp.baseline.wall_time_sec -
                            parsed->baseline.wall_time_sec) < 0.001);
  TEST_ASSERT_EQUAL_UINT64(comp.baseline.peak_rss_bytes,
                           parsed->baseline.peak_rss_bytes);
  TEST_ASSERT_EQUAL_UINT64(comp.baseline.peak_swap_bytes,
                           parsed->baseline.peak_swap_bytes);
  TEST_ASSERT_EQUAL_UINT64(comp.baseline.peak_ksm_bytes_saved,
                           parsed->baseline.peak_ksm_bytes_saved);
  TEST_ASSERT_TRUE(std::abs(comp.baseline.avg_latency_ms -
                            parsed->baseline.avg_latency_ms) < 0.01);
  TEST_ASSERT_TRUE(std::abs(comp.baseline.p99_latency_ms -
                            parsed->baseline.p99_latency_ms) < 0.01);
  TEST_ASSERT_EQUAL_INT(comp.baseline.oom_killed_count,
                        parsed->baseline.oom_killed_count);
  TEST_ASSERT_EQUAL_INT(comp.baseline.successful_tasks,
                        parsed->baseline.successful_tasks);
  TEST_ASSERT_EQUAL_INT(comp.baseline.failed_tasks,
                        parsed->baseline.failed_tasks);

  TEST_ASSERT_EQUAL_STRING(comp.managed.mode.c_str(),
                           parsed->managed.mode.c_str());
  TEST_ASSERT_EQUAL_INT(comp.managed.task_count, parsed->managed.task_count);
  TEST_ASSERT_TRUE(std::abs(comp.managed.wall_time_sec -
                            parsed->managed.wall_time_sec) < 0.001);
  TEST_ASSERT_EQUAL_UINT64(comp.managed.peak_rss_bytes,
                           parsed->managed.peak_rss_bytes);
  TEST_ASSERT_EQUAL_UINT64(comp.managed.peak_swap_bytes,
                           parsed->managed.peak_swap_bytes);
  TEST_ASSERT_EQUAL_UINT64(comp.managed.peak_ksm_bytes_saved,
                           parsed->managed.peak_ksm_bytes_saved);
  TEST_ASSERT_TRUE(std::abs(comp.managed.avg_latency_ms -
                            parsed->managed.avg_latency_ms) < 0.01);
  TEST_ASSERT_TRUE(std::abs(comp.managed.p99_latency_ms -
                            parsed->managed.p99_latency_ms) < 0.01);
  TEST_ASSERT_EQUAL_INT(comp.managed.oom_killed_count,
                        parsed->managed.oom_killed_count);
  TEST_ASSERT_EQUAL_INT(comp.managed.successful_tasks,
                        parsed->managed.successful_tasks);
  TEST_ASSERT_EQUAL_INT(comp.managed.failed_tasks,
                        parsed->managed.failed_tasks);
}

void test_json_parse_invalid(void) {
  TEST_ASSERT_FALSE(virmux::parse_comparison_json("").has_value());
  TEST_ASSERT_FALSE(virmux::parse_comparison_json("invalid json").has_value());
  TEST_ASSERT_FALSE(virmux::parse_comparison_json("{}").has_value());
  TEST_ASSERT_FALSE(
      virmux::parse_comparison_json("{\"random_key\": 123}").has_value());
}

/**
 * @brief Regression (found by fuzz_bench_report): integer fields holding
 * numbers outside int's range must fall back to their default instead of
 * performing an out-of-range double -> int conversion (UB).
 */
void test_json_parse_int_field_out_of_range(void) {
  auto parsed = virmux::parse_comparison_json(
      "{\"baseline\": {\"task_count\": 1e300, \"oom_killed_count\": -3e9,"
      " \"failed_tasks\": 2147483648, \"successful_tasks\": 5}}");
  TEST_ASSERT_TRUE(parsed.has_value());
  TEST_ASSERT_EQUAL_INT(0, parsed->baseline.task_count);
  TEST_ASSERT_EQUAL_INT(0, parsed->baseline.oom_killed_count);
  TEST_ASSERT_EQUAL_INT(0, parsed->baseline.failed_tasks);
  TEST_ASSERT_EQUAL_INT(5, parsed->baseline.successful_tasks);
}

/**
 * @brief Regression (found by fuzz_bench_report): braces inside string
 * values must not terminate the enclosing object early.
 */
void test_json_parse_braces_inside_strings(void) {
  auto parsed = virmux::parse_comparison_json(
      "{\"baseline\": {\"mode\": \"a}b{\", \"task_count\": 3}}");
  TEST_ASSERT_TRUE(parsed.has_value());
  TEST_ASSERT_EQUAL_STRING("a}b{", parsed->baseline.mode.c_str());
  TEST_ASSERT_EQUAL_INT(3, parsed->baseline.task_count);
}

/**
 * @brief Regression (found by fuzz_bench_report): format_comparison_json()
 * must escape string values so its output always parses back to the same
 * mode, whatever bytes the mode contains.
 */
void test_json_roundtrip_special_mode_chars(void) {
  virmux::BenchmarkComparison comp;
  comp.baseline.mode = "we\\ird \"q\" {x}\n\t\x01";
  comp.baseline.task_count = 4;
  comp.managed.mode = "trailing backslash\\";
  comp.managed.task_count = 2;

  auto parsed =
      virmux::parse_comparison_json(virmux::format_comparison_json(comp));
  TEST_ASSERT_TRUE(parsed.has_value());
  TEST_ASSERT_EQUAL_STRING(comp.baseline.mode.c_str(),
                           parsed->baseline.mode.c_str());
  TEST_ASSERT_EQUAL_INT(4, parsed->baseline.task_count);
  TEST_ASSERT_EQUAL_STRING(comp.managed.mode.c_str(),
                           parsed->managed.mode.c_str());
  TEST_ASSERT_EQUAL_INT(2, parsed->managed.task_count);
}

void test_format_comparison_table(void) {
  virmux::BenchmarkComparison comp;
  comp.baseline.mode = "baseline";
  comp.baseline.task_count = 12;
  comp.baseline.wall_time_sec = 25.0;
  comp.baseline.peak_rss_bytes = 10000000000ULL;
  comp.baseline.peak_swap_bytes = 1000000000ULL;
  comp.baseline.avg_latency_ms = 85.0;
  comp.baseline.p99_latency_ms = 200.0;
  comp.baseline.oom_killed_count = 1;
  comp.baseline.successful_tasks = 11;

  comp.managed.mode = "managed";
  comp.managed.task_count = 12;
  comp.managed.wall_time_sec = 10.0;
  comp.managed.peak_rss_bytes = 3000000000ULL;
  comp.managed.peak_swap_bytes = 0ULL;
  comp.managed.peak_ksm_bytes_saved = 500000000ULL;
  comp.managed.avg_latency_ms = 3.5;
  comp.managed.p99_latency_ms = 5.0;
  comp.managed.oom_killed_count = 0;
  comp.managed.successful_tasks = 12;

  const std::string table = virmux::format_comparison_table(comp);
  TEST_ASSERT_TRUE(!table.empty());
  TEST_ASSERT_TRUE(table.find("12-TERMINAL GAUNTLET BENCHMARK REPORT") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(table.find("Peak Host RAM (RSS)") != std::string::npos);
  TEST_ASSERT_TRUE(table.find("Peak Swap Allocated") != std::string::npos);
  TEST_ASSERT_TRUE(table.find("KSM Memory Saved") != std::string::npos);
  TEST_ASSERT_TRUE(table.find("Host Terminal Input Lag") != std::string::npos);
  TEST_ASSERT_TRUE(table.find("Total Wall-Clock Time") != std::string::npos);
  TEST_ASSERT_TRUE(table.find("OOM Termination Count") != std::string::npos);
  TEST_ASSERT_TRUE(table.find("Tasks Completed / Total") != std::string::npos);

  // Test single-mode report
  virmux::BenchmarkComparison single_comp;
  single_comp.managed = comp.managed;
  const std::string single_table = virmux::format_comparison_table(single_comp);
  TEST_ASSERT_TRUE(single_table.find("Virmux Managed") != std::string::npos);

  // Test empty report
  virmux::BenchmarkComparison empty_comp;
  const std::string empty_table = virmux::format_comparison_table(empty_comp);
  TEST_ASSERT_TRUE(empty_table.find("No benchmark results recorded") !=
                   std::string::npos);
}

void test_cli_arg_parsing(void) {
  // General help
  {
    char* argv[] = {const_cast<char*>("virmux"), const_cast<char*>("bench"),
                    const_cast<char*>("--help")};
    TEST_ASSERT_EQUAL_INT(0, virmux::cmd_bench(3, argv));
  }

  // Run help
  {
    char* argv[] = {const_cast<char*>("virmux"), const_cast<char*>("bench"),
                    const_cast<char*>("run"), const_cast<char*>("--help")};
    TEST_ASSERT_EQUAL_INT(0, virmux::cmd_bench(4, argv));
  }

  // Report help
  {
    char* argv[] = {const_cast<char*>("virmux"), const_cast<char*>("bench"),
                    const_cast<char*>("report"), const_cast<char*>("--help")};
    TEST_ASSERT_EQUAL_INT(0, virmux::cmd_bench(4, argv));
  }

  // Unknown subcommand
  {
    char* argv[] = {const_cast<char*>("virmux"), const_cast<char*>("bench"),
                    const_cast<char*>("nonexistent_subcmd")};
    TEST_ASSERT_EQUAL_INT(1, virmux::cmd_bench(3, argv));
  }

  // Unknown option for run
  {
    char* argv[] = {const_cast<char*>("virmux"), const_cast<char*>("bench"),
                    const_cast<char*>("run"),
                    const_cast<char*>("--bad-option")};
    TEST_ASSERT_EQUAL_INT(1, virmux::cmd_bench(4, argv));
  }

  // Invalid mode
  {
    char* argv[] = {const_cast<char*>("virmux"), const_cast<char*>("bench"),
                    const_cast<char*>("run"), const_cast<char*>("--mode"),
                    const_cast<char*>("bogus")};
    TEST_ASSERT_EQUAL_INT(1, virmux::cmd_bench(5, argv));
  }

  // Invalid tasks
  {
    char* argv[] = {const_cast<char*>("virmux"), const_cast<char*>("bench"),
                    const_cast<char*>("run"), const_cast<char*>("--tasks"),
                    const_cast<char*>("0")};
    TEST_ASSERT_EQUAL_INT(1, virmux::cmd_bench(5, argv));
  }

  // Invalid duration
  {
    char* argv[] = {const_cast<char*>("virmux"), const_cast<char*>("bench"),
                    const_cast<char*>("run"), const_cast<char*>("--duration"),
                    const_cast<char*>("-5")};
    TEST_ASSERT_EQUAL_INT(1, virmux::cmd_bench(5, argv));
  }

  // Non-existent report file
  {
    char* argv[] = {
        const_cast<char*>("virmux"), const_cast<char*>("bench"),
        const_cast<char*>("report"), const_cast<char*>("--input"),
        const_cast<char*>("/tmp/virmux_nonexistent_report_12345.json")};
    TEST_ASSERT_EQUAL_INT(1, virmux::cmd_bench(5, argv));
  }
}

void test_quick_runner_execution(void) {
  virmux::BenchmarkRunner runner(true /* quick */);
  TEST_ASSERT_TRUE(runner.is_quick());

  // Test baseline execution with 2 tasks for 1 second
  auto base_res = runner.run_baseline(1, 2);
  TEST_ASSERT_EQUAL_STRING("baseline", base_res.mode.c_str());
  TEST_ASSERT_EQUAL_INT(2, base_res.task_count);
  TEST_ASSERT_TRUE(base_res.successful_tasks >= 1);
  TEST_ASSERT_TRUE(base_res.wall_time_sec > 0.0);

  // Test managed execution with 2 tasks for 1 second
  auto mgd_res = runner.run_managed(1, 2);
  TEST_ASSERT_EQUAL_STRING("managed", mgd_res.mode.c_str());
  TEST_ASSERT_EQUAL_INT(2, mgd_res.task_count);
  TEST_ASSERT_TRUE(mgd_res.successful_tasks >= 1);
  TEST_ASSERT_TRUE(mgd_res.wall_time_sec > 0.0);

  // Test full gauntlet comparison with 2 tasks for 1 second
  auto comp = runner.run_gauntlet(1, 2);
  TEST_ASSERT_EQUAL_STRING("baseline", comp.baseline.mode.c_str());
  TEST_ASSERT_EQUAL_STRING("managed", comp.managed.mode.c_str());
  TEST_ASSERT_EQUAL_INT(2, comp.baseline.task_count);
  TEST_ASSERT_EQUAL_INT(2, comp.managed.task_count);
}

void test_cli_run_and_report_file_roundtrip(void) {
  const std::string tmp_file = "/tmp/virmux_bench_test_report_roundtrip.json";
  std::error_code ec;
  std::filesystem::remove(tmp_file, ec);

  char* argv_run[] = {
      const_cast<char*>("virmux"),     const_cast<char*>("bench"),
      const_cast<char*>("run"),        const_cast<char*>("--quick"),
      const_cast<char*>("--tasks"),    const_cast<char*>("2"),
      const_cast<char*>("--duration"), const_cast<char*>("1"),
      const_cast<char*>("--output"),   const_cast<char*>(tmp_file.c_str()),
  };

  int rc_run = virmux::cmd_bench(10, argv_run);
  TEST_ASSERT_EQUAL_INT(0, rc_run);
  TEST_ASSERT_TRUE(std::filesystem::exists(tmp_file));
  TEST_ASSERT_TRUE(std::filesystem::file_size(tmp_file) > 100);

  // Now report using the file
  char* argv_report[] = {
      const_cast<char*>("virmux"),         const_cast<char*>("bench"),
      const_cast<char*>("report"),         const_cast<char*>("--input"),
      const_cast<char*>(tmp_file.c_str()),
  };

  int rc_report = virmux::cmd_bench(5, argv_report);
  TEST_ASSERT_EQUAL_INT(0, rc_report);

  std::filesystem::remove(tmp_file, ec);
}

void test_runner_stop_cancellation(void) {
  virmux::BenchmarkRunner runner;
  runner.set_quick(false);

  auto start_time = std::chrono::steady_clock::now();

  std::thread stopper([&runner]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    runner.stop();
  });

  // Request 10-second run; should abort early via runner.stop()
  auto result = runner.run_baseline(10, 2);

  if (stopper.joinable()) {
    stopper.join();
  }

  auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start_time)
                        .count();

  // Run should have aborted well before 10 seconds (typically < 1000ms)
  TEST_ASSERT_TRUE(elapsed_ms < 3000);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_comparison_calculation_helpers);
  RUN_TEST(test_json_format_and_parse_roundtrip);
  RUN_TEST(test_json_parse_invalid);
  RUN_TEST(test_json_parse_int_field_out_of_range);
  RUN_TEST(test_json_parse_braces_inside_strings);
  RUN_TEST(test_json_roundtrip_special_mode_chars);
  RUN_TEST(test_format_comparison_table);
  RUN_TEST(test_cli_arg_parsing);
  RUN_TEST(test_quick_runner_execution);
  RUN_TEST(test_cli_run_and_report_file_roundtrip);
  RUN_TEST(test_runner_stop_cancellation);
  return UNITY_END();
}
