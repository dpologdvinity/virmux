/**
 * @file test_cli_cas.cpp
 * @brief Unit tests for virmux cas CLI subcommands and formatting.
 */

extern "C" {
#include "unity/unity.h"
}

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "virmux/cas.hpp"
#include "virmux/cli_cas.hpp"

namespace fs = std::filesystem;

namespace {

std::string create_test_temp_dir(const char* prefix) {
  std::string pattern = std::string("/tmp/") + prefix + "_XXXXXX";
  std::vector<char> buf(pattern.begin(), pattern.end());
  buf.push_back('\0');
  char* dir = ::mkdtemp(buf.data());
  TEST_ASSERT_NOT_NULL(dir);
  return std::string(dir);
}

void cleanup_test_dir(const std::string& path) {
  std::error_code ec;
  fs::remove_all(path, ec);
}

}  // namespace

void setUp(void) {}
void tearDown(void) {}

void test_parse_size_bytes(void) {
  uint64_t bytes = 999;

  TEST_ASSERT_TRUE(virmux::parse_size_bytes("0", bytes));
  TEST_ASSERT_EQUAL_UINT64(0ULL, bytes);

  TEST_ASSERT_TRUE(virmux::parse_size_bytes("0B", bytes));
  TEST_ASSERT_EQUAL_UINT64(0ULL, bytes);

  TEST_ASSERT_TRUE(virmux::parse_size_bytes("0M", bytes));
  TEST_ASSERT_EQUAL_UINT64(0ULL, bytes);

  TEST_ASSERT_TRUE(virmux::parse_size_bytes("1024", bytes));
  TEST_ASSERT_EQUAL_UINT64(1024ULL, bytes);

  TEST_ASSERT_TRUE(virmux::parse_size_bytes("1K", bytes));
  TEST_ASSERT_EQUAL_UINT64(1024ULL, bytes);

  TEST_ASSERT_TRUE(virmux::parse_size_bytes("500M", bytes));
  TEST_ASSERT_EQUAL_UINT64(500ULL * 1024ULL * 1024ULL, bytes);

  TEST_ASSERT_TRUE(virmux::parse_size_bytes("1G", bytes));
  TEST_ASSERT_EQUAL_UINT64(1024ULL * 1024ULL * 1024ULL, bytes);

  TEST_ASSERT_TRUE(virmux::parse_size_bytes("2GiB", bytes));
  TEST_ASSERT_EQUAL_UINT64(2ULL * 1024ULL * 1024ULL * 1024ULL, bytes);

  // Invalid sizes
  TEST_ASSERT_FALSE(virmux::parse_size_bytes("", bytes));
  TEST_ASSERT_FALSE(virmux::parse_size_bytes("   ", bytes));
  TEST_ASSERT_FALSE(virmux::parse_size_bytes("-1", bytes));
  TEST_ASSERT_FALSE(virmux::parse_size_bytes("-10M", bytes));
  TEST_ASSERT_FALSE(virmux::parse_size_bytes("abc", bytes));
  TEST_ASSERT_FALSE(virmux::parse_size_bytes("500XYZ", bytes));
  TEST_ASSERT_FALSE(virmux::parse_size_bytes("18446744073709551616", bytes));
  TEST_ASSERT_FALSE(virmux::parse_size_bytes("99999999999999999999G", bytes));
  TEST_ASSERT_FALSE(virmux::parse_size_bytes("nan", bytes));
  TEST_ASSERT_FALSE(virmux::parse_size_bytes("inf", bytes));
}

void test_format_cas_bytes(void) {
  TEST_ASSERT_EQUAL_STRING("0 B", virmux::format_cas_bytes(0).c_str());
  TEST_ASSERT_EQUAL_STRING("512 B", virmux::format_cas_bytes(512).c_str());
  TEST_ASSERT_EQUAL_STRING("1023 B", virmux::format_cas_bytes(1023).c_str());
  TEST_ASSERT_EQUAL_STRING("1.00 KiB", virmux::format_cas_bytes(1024).c_str());
  TEST_ASSERT_EQUAL_STRING("1.50 KiB", virmux::format_cas_bytes(1536).c_str());
  TEST_ASSERT_EQUAL_STRING("1.00 MiB",
                           virmux::format_cas_bytes(1048576).c_str());
  TEST_ASSERT_EQUAL_STRING("12.40 MiB",
                           virmux::format_cas_bytes(13002342).c_str());
  TEST_ASSERT_EQUAL_STRING("1.00 GiB",
                           virmux::format_cas_bytes(1073741824).c_str());
}

void test_format_cas_stats_table(void) {
  virmux::CasStats stats{
      .object_count = 42,
      .total_bytes = 13002342,
      .hits = 128,
      .misses = 5,
  };

  std::string table =
      virmux::format_cas_stats_table("~/.cache/virmux/cas", stats);

  TEST_ASSERT_TRUE(table.find("================================================"
                              "================================") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(table.find("Virmux Content-Addressable Storage (CAS)") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(table.find("Cache Root:     ~/.cache/virmux/cas") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(table.find("Objects:        42") != std::string::npos);
  TEST_ASSERT_TRUE(table.find("Total Size:     12.40 MiB") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(table.find("Cache Hits:     128") != std::string::npos);
  TEST_ASSERT_TRUE(table.find("Cache Misses:   5") != std::string::npos);
}

void test_format_cas_stats_json(void) {
  virmux::CasStats stats{
      .object_count = 42,
      .total_bytes = 13002342,
      .hits = 128,
      .misses = 5,
  };

  std::string json =
      virmux::format_cas_stats_json("~/.cache/virmux/cas", stats);

  // Validate outer brackets and basic valid JSON structure
  TEST_ASSERT_TRUE(json.front() == '{');
  TEST_ASSERT_TRUE(json.back() == '}');
  TEST_ASSERT_TRUE(json.find("\"cas\"") != std::string::npos);
  TEST_ASSERT_TRUE(json.find("\"cache_root\": \"~/.cache/virmux/cas\"") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(json.find("\"object_count\": 42") != std::string::npos);
  TEST_ASSERT_TRUE(json.find("\"total_bytes\": 13002342") != std::string::npos);
  TEST_ASSERT_TRUE(json.find("\"hits\": 128") != std::string::npos);
  TEST_ASSERT_TRUE(json.find("\"misses\": 5") != std::string::npos);
}

void test_cmd_cas_help(void) {
  char prog[] = "virmux";
  char cas_cmd[] = "cas";
  char help_flag[] = "--help";
  char h_flag[] = "-h";
  char stats_sub[] = "stats";
  char clean_sub[] = "clean";

  char* argv_cas_help[] = {prog, cas_cmd, help_flag};
  TEST_ASSERT_EQUAL_INT(0, virmux::cmd_cas(3, argv_cas_help));

  char* argv_cas_h[] = {prog, cas_cmd, h_flag};
  TEST_ASSERT_EQUAL_INT(0, virmux::cmd_cas(3, argv_cas_h));

  char* argv_stats_help[] = {prog, cas_cmd, stats_sub, help_flag};
  TEST_ASSERT_EQUAL_INT(0, virmux::cmd_cas(4, argv_stats_help));

  char* argv_clean_help[] = {prog, cas_cmd, clean_sub, help_flag};
  TEST_ASSERT_EQUAL_INT(0, virmux::cmd_cas(4, argv_clean_help));
}

void test_cmd_cas_stats_and_json(void) {
  std::string temp_dir = create_test_temp_dir("virmux_cas_cli_test");
  ::setenv("VIRMUX_CAS_DIR", temp_dir.c_str(), 1);

  // Put a file into store so we have non-zero stats
  {
    virmux::CasStore store(temp_dir);
    std::string test_file = temp_dir + "/sample.txt";
    std::ofstream ofs(test_file);
    ofs << "hello cas stats";
    ofs.close();
    TEST_ASSERT_TRUE(store.put(
        "aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899",
        test_file));
  }

  char prog[] = "virmux";
  char cas_cmd[] = "cas";
  char stats_sub[] = "stats";
  char json_flag[] = "--json";
  char invalid_flag[] = "--bogus-flag";

  char* argv_stats[] = {prog, cas_cmd, stats_sub};
  TEST_ASSERT_EQUAL_INT(0, virmux::cmd_cas(3, argv_stats));

  char* argv_json[] = {prog, cas_cmd, stats_sub, json_flag};
  TEST_ASSERT_EQUAL_INT(0, virmux::cmd_cas(4, argv_json));

  char* argv_invalid[] = {prog, cas_cmd, stats_sub, invalid_flag};
  TEST_ASSERT_NOT_EQUAL(0, virmux::cmd_cas(4, argv_invalid));

  ::unsetenv("VIRMUX_CAS_DIR");
  cleanup_test_dir(temp_dir);
}

void test_print_cas_usage_streams(void) {
  std::ostringstream ss_usage;
  virmux::print_cas_usage(ss_usage);
  TEST_ASSERT_TRUE(ss_usage.str().find("virmux cas <SUBCOMMAND>") !=
                   std::string::npos);

  std::ostringstream ss_stats;
  virmux::print_cas_stats_usage(ss_stats);
  TEST_ASSERT_TRUE(ss_stats.str().find("virmux cas stats") !=
                   std::string::npos);

  std::ostringstream ss_clean;
  virmux::print_cas_clean_usage(ss_clean);
  TEST_ASSERT_TRUE(ss_clean.str().find("virmux cas clean") !=
                   std::string::npos);
}

void test_cmd_cas_clean(void) {
  std::string temp_dir = create_test_temp_dir("virmux_cas_clean_test");
  ::setenv("VIRMUX_CAS_DIR", temp_dir.c_str(), 1);

  // Put a file into store so we can test pruning
  {
    virmux::CasStore store(temp_dir);
    std::string test_file = temp_dir + "/sample.txt";
    std::ofstream ofs(test_file);
    ofs << "hello cas world";
    ofs.close();
    TEST_ASSERT_TRUE(store.put(
        "11223344556677889900aabbccddeeff11223344556677889900aabbccddeeff",
        test_file));
  }

  char prog[] = "virmux";
  char cas_cmd[] = "cas";
  char clean_sub[] = "clean";
  char max_bytes_flag[] = "--max-bytes";
  char zero_bytes[] = "0";
  char eq_flag[] = "--max-bytes=1G";
  char bad_flag[] = "--invalid-opt";
  char bad_val[] = "invalid_number";

  // Test clean with --max-bytes=1G
  char* argv_clean_1g[] = {prog, cas_cmd, clean_sub, eq_flag};
  TEST_ASSERT_EQUAL_INT(0, virmux::cmd_cas(4, argv_clean_1g));

  // Test clean with --max-bytes 0 (prune all)
  char* argv_clean_zero[] = {prog, cas_cmd, clean_sub, max_bytes_flag,
                             zero_bytes};
  TEST_ASSERT_EQUAL_INT(0, virmux::cmd_cas(5, argv_clean_zero));

  // Test clean with default (no flags, defaults to 0)
  char* argv_clean_default[] = {prog, cas_cmd, clean_sub};
  TEST_ASSERT_EQUAL_INT(0, virmux::cmd_cas(3, argv_clean_default));

  // Test clean with invalid option
  char* argv_clean_bad_opt[] = {prog, cas_cmd, clean_sub, bad_flag};
  TEST_ASSERT_NOT_EQUAL(0, virmux::cmd_cas(4, argv_clean_bad_opt));

  // Test clean with invalid value
  char* argv_clean_bad_val[] = {prog, cas_cmd, clean_sub, max_bytes_flag,
                                bad_val};
  TEST_ASSERT_NOT_EQUAL(0, virmux::cmd_cas(5, argv_clean_bad_val));

  // Test clean with missing value
  char* argv_clean_missing_val[] = {prog, cas_cmd, clean_sub, max_bytes_flag};
  TEST_ASSERT_NOT_EQUAL(0, virmux::cmd_cas(4, argv_clean_missing_val));

  ::unsetenv("VIRMUX_CAS_DIR");
  cleanup_test_dir(temp_dir);
}

void test_cmd_cas_invalid_subcommand(void) {
  char prog[] = "virmux";
  char cas_cmd[] = "cas";
  char bogus_cmd[] = "invalid_subcommand";

  // Null argv pointers
  TEST_ASSERT_NOT_EQUAL(0, virmux::cmd_cas(0, nullptr));
  TEST_ASSERT_NOT_EQUAL(0, virmux::cmd_cas(1, nullptr));

  // No subcommand: "virmux cas"
  char* argv_no_sub[] = {prog, cas_cmd};
  TEST_ASSERT_NOT_EQUAL(0, virmux::cmd_cas(2, argv_no_sub));

  // Invalid subcommand: "virmux cas invalid_subcommand"
  char* argv_invalid[] = {prog, cas_cmd, bogus_cmd};
  TEST_ASSERT_NOT_EQUAL(0, virmux::cmd_cas(3, argv_invalid));

  // Null element in argv
  char* argv_null_elem[] = {prog, cas_cmd, nullptr};
  TEST_ASSERT_NOT_EQUAL(0, virmux::cmd_cas(3, argv_null_elem));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_parse_size_bytes);
  RUN_TEST(test_format_cas_bytes);
  RUN_TEST(test_format_cas_stats_table);
  RUN_TEST(test_format_cas_stats_json);
  RUN_TEST(test_print_cas_usage_streams);
  RUN_TEST(test_cmd_cas_help);
  RUN_TEST(test_cmd_cas_stats_and_json);
  RUN_TEST(test_cmd_cas_clean);
  RUN_TEST(test_cmd_cas_invalid_subcommand);
  return UNITY_END();
}
