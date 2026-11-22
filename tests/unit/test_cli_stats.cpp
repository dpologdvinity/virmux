/**
 * @file test_cli_stats.cpp
 * @brief Unit tests for CLI stats and pack subcommands and telemetry
 * formatting.
 */

extern "C" {
#include "unity/unity.h"
}

#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

#include "unit/fake_cgroupfs.hpp"
#include "virmux/cli_stats.hpp"

void setUp(void) {}
void tearDown(void) {}

void test_format_bytes_boundaries(void) {
  TEST_ASSERT_EQUAL_STRING("0 B", virmux::format_bytes(0).c_str());
  TEST_ASSERT_EQUAL_STRING("512 B", virmux::format_bytes(512).c_str());
  TEST_ASSERT_EQUAL_STRING("1023 B", virmux::format_bytes(1023).c_str());
  TEST_ASSERT_EQUAL_STRING("1.0 KiB", virmux::format_bytes(1024).c_str());
  TEST_ASSERT_EQUAL_STRING("1.5 KiB", virmux::format_bytes(1536).c_str());
  TEST_ASSERT_EQUAL_STRING("1.0 MiB", virmux::format_bytes(1048576).c_str());
  TEST_ASSERT_EQUAL_STRING("256.0 MiB",
                           virmux::format_bytes(268435456).c_str());
  TEST_ASSERT_EQUAL_STRING("1.00 GiB",
                           virmux::format_bytes(1073741824).c_str());
  TEST_ASSERT_EQUAL_STRING("3.00 GiB",
                           virmux::format_bytes(3221225472ULL).c_str());
}

void test_format_stats_json(void) {
  virmux::HostMemoryStats host;
  host.mem_total_bytes = 16ULL * 1024 * 1024 * 1024;
  host.mem_available_bytes = 8ULL * 1024 * 1024 * 1024;
  host.mem_free_bytes = 4ULL * 1024 * 1024 * 1024;
  host.cached_bytes = 2ULL * 1024 * 1024 * 1024;
  host.swap_total_bytes = 8ULL * 1024 * 1024 * 1024;
  host.swap_free_bytes = 7ULL * 1024 * 1024 * 1024;

  virmux::ZramDeviceStats zram0;
  zram0.present = true;
  zram0.name = "zram0";
  zram0.algorithm = "lzo-rle";
  zram0.disk_size = 4ULL * 1024 * 1024 * 1024;
  zram0.orig_data_size = 1000000;
  zram0.compr_data_size = 500000;
  zram0.mem_used_total = 520000;
  zram0.compression_ratio = 2.0;
  host.zram_devices.push_back(zram0);

  virmux::KsmStats ksm;
  ksm.present = true;
  ksm.enabled = true;
  ksm.pages_shared = 100;
  ksm.pages_sharing = 400;
  ksm.pages_unshared = 50;
  ksm.pages_volatile = 10;
  ksm.bytes_saved = 400ULL * 4096;

  std::string json = virmux::format_stats_json(host, ksm);

  // Validate outer brackets
  TEST_ASSERT_TRUE(json.front() == '{');
  TEST_ASSERT_TRUE(json.back() == '}');

  // Verify host_memory fields
  TEST_ASSERT_TRUE(json.find("\"host_memory\"") != std::string::npos);
  TEST_ASSERT_TRUE(json.find("\"total_bytes\": 17179869184") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(json.find("\"available_bytes\": 8589934592") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(json.find("\"free_bytes\": 4294967296") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(json.find("\"cached_bytes\": 2147483648") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(json.find("\"swap_total_bytes\": 8589934592") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(json.find("\"swap_free_bytes\": 7516192768") !=
                   std::string::npos);

  // Verify ksm fields
  TEST_ASSERT_TRUE(json.find("\"ksm\"") != std::string::npos);
  TEST_ASSERT_TRUE(json.find("\"present\": true") != std::string::npos);
  TEST_ASSERT_TRUE(json.find("\"enabled\": true") != std::string::npos);
  TEST_ASSERT_TRUE(json.find("\"pages_shared\": 100") != std::string::npos);
  TEST_ASSERT_TRUE(json.find("\"pages_sharing\": 400") != std::string::npos);
  TEST_ASSERT_TRUE(json.find("\"pages_unshared\": 50") != std::string::npos);
  TEST_ASSERT_TRUE(json.find("\"pages_volatile\": 10") != std::string::npos);
  TEST_ASSERT_TRUE(json.find("\"bytes_saved\": 1638400") != std::string::npos);

  // Verify zram fields
  TEST_ASSERT_TRUE(json.find("\"zram\"") != std::string::npos);
  TEST_ASSERT_TRUE(json.find("\"name\": \"zram0\"") != std::string::npos);
  TEST_ASSERT_TRUE(json.find("\"algorithm\": \"lzo-rle\"") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(json.find("\"disk_size_bytes\": 4294967296") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(json.find("\"orig_data_size_bytes\": 1000000") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(json.find("\"compr_data_size_bytes\": 500000") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(json.find("\"mem_used_total_bytes\": 520000") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(json.find("\"compression_ratio\":") != std::string::npos);
}

void test_print_stats_table(void) {
  virmux::HostMemoryStats host;
  host.mem_total_bytes = 16ULL * 1024 * 1024 * 1024;
  host.mem_available_bytes = 8ULL * 1024 * 1024 * 1024;
  virmux::KsmStats ksm;
  ksm.present = false;

  // Verify function runs cleanly without crash
  virmux::print_stats_table(host, ksm);
}

void test_cmd_stats(void) {
  char prog[] = "virmux";
  char cmd[] = "stats";
  char json_flag[] = "--json";
  char help_flag[] = "--help";
  char invalid_flag[] = "--invalid-flag";

  char* argv_json[] = {prog, cmd, json_flag};
  TEST_ASSERT_EQUAL_INT(0, virmux::cmd_stats(3, argv_json));

  char* argv_help[] = {prog, cmd, help_flag};
  TEST_ASSERT_EQUAL_INT(0, virmux::cmd_stats(3, argv_help));

  char* argv_invalid[] = {prog, cmd, invalid_flag};
  TEST_ASSERT_NOT_EQUAL(0, virmux::cmd_stats(3, argv_invalid));
}

void test_cmd_pack(void) {
  char prog[] = "virmux";
  char cmd[] = "pack";
  char help_flag[] = "--help";
  char invalid_flag[] = "--not-a-flag";

  char* argv_help[] = {prog, cmd, help_flag};
  TEST_ASSERT_EQUAL_INT(0, virmux::cmd_pack(3, argv_help));

  char* argv_invalid[] = {prog, cmd, invalid_flag};
  TEST_ASSERT_NOT_EQUAL(0, virmux::cmd_pack(3, argv_invalid));

  std::string pid_str = std::to_string(::getpid());
  char* argv_pid[] = {prog, cmd, pid_str.data()};
  TEST_ASSERT_EQUAL_INT(0, virmux::cmd_pack(3, argv_pid));
}

/**
 * @brief A null or empty argv must be rejected rather than dereferenced, and
 * a null element inside argc must not be turned into a std::string_view.
 */
void test_cmd_stats_pack_reject_null_argv(void) {
  char prog[] = "virmux";
  char stats_cmd[] = "stats";
  char pack_cmd[] = "pack";

  TEST_ASSERT_NOT_EQUAL(0, virmux::cmd_stats(0, nullptr));
  TEST_ASSERT_NOT_EQUAL(0, virmux::cmd_stats(3, nullptr));
  TEST_ASSERT_NOT_EQUAL(0, virmux::cmd_pack(0, nullptr));
  TEST_ASSERT_NOT_EQUAL(0, virmux::cmd_pack(3, nullptr));

  char* stats_null_elem[] = {prog, stats_cmd, nullptr};
  TEST_ASSERT_NOT_EQUAL(0, virmux::cmd_stats(3, stats_null_elem));

  char* pack_null_elem[] = {prog, pack_cmd, nullptr};
  TEST_ASSERT_NOT_EQUAL(0, virmux::cmd_pack(3, pack_null_elem));
}

void test_find_virmux_cgroups_includes_nested_runs(void) {
  virmux::testing::FakeCgroupFs fs;
  const std::string mgr = "user.slice/user-1000.slice/user@1000.service";
  fs.add("", true);
  fs.add("virmux.slice", true);  // legacy root-level slice
  fs.add("user.slice", true);
  fs.add(mgr, true);
  fs.add(mgr + "/app.slice", true);
  fs.add(mgr + "/app.slice/virmuxish.scope", true);  // not ours
  fs.add(mgr + "/virmux", true);
  fs.add(mgr + "/virmux/run-10-0", true);
  fs.add(mgr + "/virmux/run-11-3", true);

  std::vector<std::string> got = virmux::find_virmux_cgroup_procs(fs.root());
  std::sort(got.begin(), got.end());
  std::vector<std::string> want{
      fs.path("virmux.slice") + "/cgroup.procs",
      fs.path(mgr + "/virmux") + "/cgroup.procs",
      fs.path(mgr + "/virmux/run-10-0") + "/cgroup.procs",
      fs.path(mgr + "/virmux/run-11-3") + "/cgroup.procs",
  };
  std::sort(want.begin(), want.end());
  TEST_ASSERT_EQUAL_size_t(want.size(), got.size());
  for (size_t i = 0; i < want.size(); ++i) {
    TEST_ASSERT_EQUAL_STRING(want[i].c_str(), got[i].c_str());
  }
  TEST_ASSERT_TRUE(virmux::find_virmux_cgroup_procs("/nonexistent").empty());
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_format_bytes_boundaries);
  RUN_TEST(test_format_stats_json);
  RUN_TEST(test_print_stats_table);
  RUN_TEST(test_cmd_stats);
  RUN_TEST(test_cmd_pack);
  RUN_TEST(test_cmd_stats_pack_reject_null_argv);
  RUN_TEST(test_find_virmux_cgroups_includes_nested_runs);
  return UNITY_END();
}
