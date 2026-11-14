/**
 * @file test_ksm.cpp
 * @brief Unit tests for Kernel Samepage Merging (KSM) controller and sysfs
 * parser.
 */

extern "C" {
#include "unity/unity.h"
}

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "virmux/ksm.hpp"

void setUp(void) {}
void tearDown(void) {}

namespace {

void write_file(const std::filesystem::path& path, const std::string& content) {
  std::ofstream ofs(path);
  ofs << content;
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream ifs(path);
  std::string content;
  std::getline(ifs, content);
  return content;
}

}  // namespace

void test_ksm_stats_default_construction(void) {
  virmux::KsmStats stats;
  TEST_ASSERT_FALSE(stats.present);
  TEST_ASSERT_FALSE(stats.enabled);
  TEST_ASSERT_EQUAL_UINT64(0, stats.pages_shared);
  TEST_ASSERT_EQUAL_UINT64(0, stats.pages_sharing);
  TEST_ASSERT_EQUAL_UINT64(0, stats.pages_unshared);
  TEST_ASSERT_EQUAL_UINT64(0, stats.pages_volatile);
  TEST_ASSERT_EQUAL_UINT32(0, stats.pages_to_scan);
  TEST_ASSERT_EQUAL_UINT32(0, stats.sleep_millisecs);
  TEST_ASSERT_EQUAL_UINT64(0, stats.bytes_saved);
}

void test_ksm_nonexistent_directory(void) {
  virmux::KsmController controller("/tmp/nonexistent_ksm_dir_987654");

  TEST_ASSERT_FALSE(controller.is_available());
  TEST_ASSERT_FALSE(controller.is_enabled());

  virmux::KsmStats stats = controller.read_stats();
  TEST_ASSERT_FALSE(stats.present);
  TEST_ASSERT_FALSE(stats.enabled);
  TEST_ASSERT_EQUAL_UINT64(0, stats.pages_shared);
  TEST_ASSERT_EQUAL_UINT64(0, stats.pages_sharing);
  TEST_ASSERT_EQUAL_UINT64(0, stats.pages_unshared);
  TEST_ASSERT_EQUAL_UINT64(0, stats.pages_volatile);
  TEST_ASSERT_EQUAL_UINT32(0, stats.pages_to_scan);
  TEST_ASSERT_EQUAL_UINT32(0, stats.sleep_millisecs);
  TEST_ASSERT_EQUAL_UINT64(0, stats.bytes_saved);

  // Write operations should fail gracefully without crashing
  TEST_ASSERT_FALSE(controller.enable(true));
  TEST_ASSERT_FALSE(controller.tune(100, 20));
}

void test_ksm_mock_read_stats(void) {
  char dir_template[] = "/tmp/virmux_ksm_read_XXXXXX";
  char* tmp_dir = ::mkdtemp(dir_template);
  TEST_ASSERT_NOT_NULL(tmp_dir);
  std::filesystem::path mock_dir(tmp_dir);

  write_file(mock_dir / "run", "1\n");
  write_file(mock_dir / "pages_shared", "200\n");
  write_file(mock_dir / "pages_sharing", "1000\n");
  write_file(mock_dir / "pages_unshared", "50\n");
  write_file(mock_dir / "pages_volatile", "30\n");
  write_file(mock_dir / "pages_to_scan", "400\n");
  write_file(mock_dir / "sleep_millisecs", "25\n");

  virmux::KsmController controller(mock_dir.string());
  TEST_ASSERT_TRUE(controller.is_available());
  TEST_ASSERT_TRUE(controller.is_enabled());

  virmux::KsmStats stats = controller.read_stats();
  TEST_ASSERT_TRUE(stats.present);
  TEST_ASSERT_TRUE(stats.enabled);
  TEST_ASSERT_EQUAL_UINT64(200, stats.pages_shared);
  TEST_ASSERT_EQUAL_UINT64(1000, stats.pages_sharing);
  TEST_ASSERT_EQUAL_UINT64(50, stats.pages_unshared);
  TEST_ASSERT_EQUAL_UINT64(30, stats.pages_volatile);
  TEST_ASSERT_EQUAL_UINT32(400, stats.pages_to_scan);
  TEST_ASSERT_EQUAL_UINT32(25, stats.sleep_millisecs);

  long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0) {
    page_size = 4096;
  }
  uint64_t expected_bytes = 1000ULL * static_cast<uint64_t>(page_size);
  TEST_ASSERT_EQUAL_UINT64(expected_bytes, stats.bytes_saved);

  std::filesystem::remove_all(mock_dir);
}

void test_ksm_mock_enable_and_tune(void) {
  char dir_template[] = "/tmp/virmux_ksm_write_XXXXXX";
  char* tmp_dir = ::mkdtemp(dir_template);
  TEST_ASSERT_NOT_NULL(tmp_dir);
  std::filesystem::path mock_dir(tmp_dir);

  write_file(mock_dir / "run", "0\n");
  write_file(mock_dir / "pages_to_scan", "100\n");
  write_file(mock_dir / "sleep_millisecs", "20\n");

  virmux::KsmController controller(mock_dir.string());
  TEST_ASSERT_TRUE(controller.is_available());
  TEST_ASSERT_FALSE(controller.is_enabled());

  // Test enable(true)
  TEST_ASSERT_TRUE(controller.enable(true));
  TEST_ASSERT_EQUAL_STRING("1", read_file(mock_dir / "run").c_str());
  TEST_ASSERT_TRUE(controller.is_enabled());

  // Test tune
  TEST_ASSERT_TRUE(controller.tune(500, 15));
  TEST_ASSERT_EQUAL_STRING("500",
                           read_file(mock_dir / "pages_to_scan").c_str());
  TEST_ASSERT_EQUAL_STRING("15",
                           read_file(mock_dir / "sleep_millisecs").c_str());

  // Test enable(false)
  TEST_ASSERT_TRUE(controller.enable(false));
  TEST_ASSERT_EQUAL_STRING("0", read_file(mock_dir / "run").c_str());
  TEST_ASSERT_FALSE(controller.is_enabled());

  std::filesystem::remove_all(mock_dir);
}

void test_ksm_permission_failures_handled_cleanly(void) {
  char dir_template[] = "/tmp/virmux_ksm_perm_XXXXXX";
  char* tmp_dir = ::mkdtemp(dir_template);
  TEST_ASSERT_NOT_NULL(tmp_dir);
  std::filesystem::path mock_dir(tmp_dir);

  write_file(mock_dir / "run", "1\n");
  write_file(mock_dir / "pages_to_scan", "100\n");
  write_file(mock_dir / "sleep_millisecs", "20\n");

  // Make files read-only (chmod 0444)
  ::chmod((mock_dir / "run").c_str(), 0444);
  ::chmod((mock_dir / "pages_to_scan").c_str(), 0444);

  virmux::KsmController controller(mock_dir.string());

  // Operations should return false cleanly without crashing
  TEST_ASSERT_FALSE(controller.enable(false));
  TEST_ASSERT_FALSE(controller.tune(300, 10));

  // Restore write permission so remove_all succeeds
  ::chmod((mock_dir / "run").c_str(), 0644);
  ::chmod((mock_dir / "pages_to_scan").c_str(), 0644);
  std::filesystem::remove_all(mock_dir);
}

void test_ksm_compute_adaptive_pages(void) {
  virmux::KsmController controller;

  // 0 sessions: baseline 100
  TEST_ASSERT_EQUAL_UINT32(100, controller.compute_adaptive_pages(0));

  // 1 session: 100 + 1 * 50 = 150
  TEST_ASSERT_EQUAL_UINT32(150, controller.compute_adaptive_pages(1));

  // 10 sessions: 100 + 10 * 50 = 600
  TEST_ASSERT_EQUAL_UINT32(600, controller.compute_adaptive_pages(10));

  // 38 sessions: 100 + 38 * 50 = 2000 (upper bound)
  TEST_ASSERT_EQUAL_UINT32(2000, controller.compute_adaptive_pages(38));

  // 100 sessions: clamped to 2000
  TEST_ASSERT_EQUAL_UINT32(2000, controller.compute_adaptive_pages(100));

  // 1000 sessions: clamped to 2000
  TEST_ASSERT_EQUAL_UINT32(2000, controller.compute_adaptive_pages(1000));
}

void test_ksm_corrupt_and_missing_metric_files(void) {
  char dir_template[] = "/tmp/virmux_ksm_corrupt_XXXXXX";
  char* tmp_dir = ::mkdtemp(dir_template);
  TEST_ASSERT_NOT_NULL(tmp_dir);
  std::filesystem::path mock_dir(tmp_dir);

  // 'run' exists so directory is considered available, but metric files contain
  // non-numeric text or are empty/missing
  write_file(mock_dir / "run", "1\n");
  write_file(mock_dir / "pages_shared", "corrupt_data\n");
  write_file(mock_dir / "pages_sharing", "\n");
  // pages_unshared, pages_volatile, pages_to_scan, sleep_millisecs omitted

  virmux::KsmController controller(mock_dir.string());
  TEST_ASSERT_TRUE(controller.is_available());
  TEST_ASSERT_TRUE(controller.is_enabled());

  virmux::KsmStats stats = controller.read_stats();
  TEST_ASSERT_TRUE(stats.present);
  TEST_ASSERT_TRUE(stats.enabled);
  TEST_ASSERT_EQUAL_UINT64(0, stats.pages_shared);
  TEST_ASSERT_EQUAL_UINT64(0, stats.pages_sharing);
  TEST_ASSERT_EQUAL_UINT64(0, stats.pages_unshared);
  TEST_ASSERT_EQUAL_UINT64(0, stats.pages_volatile);
  TEST_ASSERT_EQUAL_UINT32(0, stats.pages_to_scan);
  TEST_ASSERT_EQUAL_UINT32(0, stats.sleep_millisecs);
  TEST_ASSERT_EQUAL_UINT64(0, stats.bytes_saved);

  std::filesystem::remove_all(mock_dir);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_ksm_stats_default_construction);
  RUN_TEST(test_ksm_nonexistent_directory);
  RUN_TEST(test_ksm_mock_read_stats);
  RUN_TEST(test_ksm_mock_enable_and_tune);
  RUN_TEST(test_ksm_permission_failures_handled_cleanly);
  RUN_TEST(test_ksm_compute_adaptive_pages);
  RUN_TEST(test_ksm_corrupt_and_missing_metric_files);
  return UNITY_END();
}
