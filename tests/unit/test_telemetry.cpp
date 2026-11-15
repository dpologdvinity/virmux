/**
 * @file test_telemetry.cpp
 * @brief Unit tests for MemoryTelemetry (process memory, host meminfo, and zram
 * statistics).
 */

extern "C" {
#include "unity/unity.h"
}

#include <sys/stat.h>
#include <unistd.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "virmux/telemetry.hpp"

void setUp(void) {}
void tearDown(void) {}

namespace {

void write_file(const std::filesystem::path& path, const std::string& content) {
  std::ofstream ofs(path);
  ofs << content;
}

struct TempDir {
  std::filesystem::path path;

  explicit TempDir(const char* tmpl = "/tmp/virmux_telem_test_XXXXXX") {
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

void test_process_stats_nonexistent_or_invalid_pid(void) {
  virmux::MemoryTelemetry tele("/tmp/nonexistent_proc_999999",
                               "/tmp/nonexistent_sys_999999");

  // Negative PID
  auto stats_neg = tele.read_process_stats(-100);
  TEST_ASSERT_EQUAL_INT(-100, stats_neg.pid);
  TEST_ASSERT_EQUAL_UINT64(0, stats_neg.rss_bytes);
  TEST_ASSERT_EQUAL_UINT64(0, stats_neg.pss_bytes);
  TEST_ASSERT_EQUAL_UINT64(0, stats_neg.swap_bytes);
  TEST_ASSERT_EQUAL_UINT64(0, stats_neg.swap_pss_bytes);
  TEST_ASSERT_EQUAL_UINT64(0, stats_neg.referenced_bytes);
  TEST_ASSERT_EQUAL_UINT64(0, stats_neg.private_dirty_bytes);

  // Large nonexistent PID
  auto stats_large = tele.read_process_stats(9999999);
  TEST_ASSERT_EQUAL_INT(9999999, stats_large.pid);
  TEST_ASSERT_EQUAL_UINT64(0, stats_large.rss_bytes);
  TEST_ASSERT_EQUAL_UINT64(0, stats_large.pss_bytes);
  TEST_ASSERT_EQUAL_UINT64(0, stats_large.swap_bytes);
  TEST_ASSERT_EQUAL_UINT64(0, stats_large.swap_pss_bytes);
  TEST_ASSERT_EQUAL_UINT64(0, stats_large.referenced_bytes);
  TEST_ASSERT_EQUAL_UINT64(0, stats_large.private_dirty_bytes);
}

void test_process_stats_mock_smaps_rollup(void) {
  TempDir temp_proc;
  TEST_ASSERT_FALSE(temp_proc.path.empty());

  const pid_t test_pid = 12345;
  std::filesystem::path pid_dir = temp_proc.path / std::to_string(test_pid);
  std::filesystem::create_directories(pid_dir);

  const std::string mock_smaps =
      "55a73e6a0000-7ffd551f3000 ---p 00000000 00:00 0                         "
      " [rollup]\n"
      "Rss:                2048 kB\n"
      "Pss:                1024 kB\n"
      "Pss_Dirty:           512 kB\n"
      "Pss_Anon:            512 kB\n"
      "Pss_File:            512 kB\n"
      "Pss_Shmem:             0 kB\n"
      "Shared_Clean:       1024 kB\n"
      "Shared_Dirty:          0 kB\n"
      "Private_Clean:         0 kB\n"
      "Private_Dirty:       512 kB\n"
      "Referenced:         1536 kB\n"
      "Anonymous:           512 kB\n"
      "LazyFree:              0 kB\n"
      "AnonHugePages:         0 kB\n"
      "ShmemPmdMapped:        0 kB\n"
      "FilePmdMapped:         0 kB\n"
      "Shared_Hugetlb:        0 kB\n"
      "Private_Hugetlb:       0 kB\n"
      "Swap:                256 kB\n"
      "SwapPss:             128 kB\n";

  write_file(pid_dir / "smaps_rollup", mock_smaps);

  virmux::MemoryTelemetry tele(temp_proc.path.string(), "/tmp/dummy_sysfs");
  auto stats = tele.read_process_stats(test_pid);

  TEST_ASSERT_EQUAL_INT(test_pid, stats.pid);
  TEST_ASSERT_EQUAL_UINT64(2048ULL * 1024, stats.rss_bytes);
  TEST_ASSERT_EQUAL_UINT64(1024ULL * 1024, stats.pss_bytes);
  TEST_ASSERT_EQUAL_UINT64(256ULL * 1024, stats.swap_bytes);
  TEST_ASSERT_EQUAL_UINT64(128ULL * 1024, stats.swap_pss_bytes);
  TEST_ASSERT_EQUAL_UINT64(1536ULL * 1024, stats.referenced_bytes);
  TEST_ASSERT_EQUAL_UINT64(512ULL * 1024, stats.private_dirty_bytes);
}

void test_process_stats_mock_status_fallback(void) {
  TempDir temp_proc;
  TEST_ASSERT_FALSE(temp_proc.path.empty());

  const pid_t test_pid = 23456;
  std::filesystem::path pid_dir = temp_proc.path / std::to_string(test_pid);
  std::filesystem::create_directories(pid_dir);

  // Note: No smaps_rollup file created, only status file
  const std::string mock_status =
      "Name:\tcat\n"
      "Umask:\t0022\n"
      "State:\tS (sleeping)\n"
      "Tgid:\t23456\n"
      "Ngid:\t0\n"
      "Pid:\t23456\n"
      "PPid:\t1000\n"
      "VmPeak:\t    4140 kB\n"
      "VmSize:\t    4140 kB\n"
      "VmLck:\t       0 kB\n"
      "VmPin:\t       0 kB\n"
      "VmHWM:\t    2048 kB\n"
      "VmRSS:\t    4096 kB\n"
      "RssAnon:\t     256 kB\n"
      "RssFile:\t     768 kB\n"
      "RssShmem:\t       0 kB\n"
      "VmData:\t     312 kB\n"
      "VmStk:\t     132 kB\n"
      "VmExe:\t      28 kB\n"
      "VmLib:\t    1784 kB\n"
      "VmPTE:\t      44 kB\n"
      "VmSwap:\t    1024 kB\n";

  write_file(pid_dir / "status", mock_status);

  virmux::MemoryTelemetry tele(temp_proc.path.string(), "/tmp/dummy_sysfs");
  auto stats = tele.read_process_stats(test_pid);

  TEST_ASSERT_EQUAL_INT(test_pid, stats.pid);
  TEST_ASSERT_EQUAL_UINT64(4096ULL * 1024, stats.rss_bytes);
  TEST_ASSERT_EQUAL_UINT64(1024ULL * 1024, stats.swap_bytes);
  TEST_ASSERT_EQUAL_UINT64(0, stats.pss_bytes);
  TEST_ASSERT_EQUAL_UINT64(0, stats.swap_pss_bytes);
  TEST_ASSERT_EQUAL_UINT64(0, stats.referenced_bytes);
  TEST_ASSERT_EQUAL_UINT64(0, stats.private_dirty_bytes);
}

void test_process_stats_real_self(void) {
  virmux::MemoryTelemetry tele;
  pid_t self_pid = ::getpid();
  auto stats = tele.read_process_stats(self_pid);

  TEST_ASSERT_EQUAL_INT(self_pid, stats.pid);
  TEST_ASSERT_TRUE(stats.rss_bytes > 0);
}

void test_host_stats_mock_meminfo(void) {
  TempDir temp_proc;
  TempDir temp_sysfs;
  TEST_ASSERT_FALSE(temp_proc.path.empty());
  TEST_ASSERT_FALSE(temp_sysfs.path.empty());

  const std::string mock_meminfo =
      "MemTotal:       16384000 kB\n"
      "MemFree:         4096000 kB\n"
      "MemAvailable:    8192000 kB\n"
      "Buffers:          100000 kB\n"
      "Cached:          2048000 kB\n"
      "SwapCached:        50000 kB\n"
      "Active:          8000000 kB\n"
      "Inactive:        4000000 kB\n"
      "SwapTotal:       1024000 kB\n"
      "SwapFree:         512000 kB\n";

  write_file(temp_proc.path / "meminfo", mock_meminfo);

  // Also setup a mock zram device
  std::filesystem::path zram_dir = temp_sysfs.path / "zram0";
  std::filesystem::create_directories(zram_dir);
  write_file(zram_dir / "comp_algorithm", "[lz4] lzo\n");
  write_file(zram_dir / "disksize", "104857600\n");
  write_file(zram_dir / "mm_stat", " 2048000  1024000  1050000 0 0 0 0 0 0\n");

  virmux::MemoryTelemetry tele(temp_proc.path.string(),
                               temp_sysfs.path.string());
  auto host_stats = tele.read_host_stats();

  TEST_ASSERT_EQUAL_UINT64(16384000ULL * 1024, host_stats.mem_total_bytes);
  TEST_ASSERT_EQUAL_UINT64(8192000ULL * 1024, host_stats.mem_available_bytes);
  TEST_ASSERT_EQUAL_UINT64(4096000ULL * 1024, host_stats.mem_free_bytes);
  TEST_ASSERT_EQUAL_UINT64(2048000ULL * 1024, host_stats.cached_bytes);
  TEST_ASSERT_EQUAL_UINT64(1024000ULL * 1024, host_stats.swap_total_bytes);
  TEST_ASSERT_EQUAL_UINT64(512000ULL * 1024, host_stats.swap_free_bytes);

  TEST_ASSERT_EQUAL_INT(1, host_stats.zram_devices.size());
  TEST_ASSERT_EQUAL_STRING("zram0", host_stats.zram_devices[0].name.c_str());
  TEST_ASSERT_EQUAL_STRING("lz4", host_stats.zram_devices[0].algorithm.c_str());
}

void test_host_stats_real(void) {
  virmux::MemoryTelemetry tele;
  auto host_stats = tele.read_host_stats();

  TEST_ASSERT_TRUE(host_stats.mem_total_bytes > 0);
  TEST_ASSERT_TRUE(host_stats.mem_available_bytes > 0);
}

void test_zram_stats_mock_multiple_devices(void) {
  TempDir temp_sysfs;
  TEST_ASSERT_FALSE(temp_sysfs.path.empty());

  // Setup zram0 with active algorithm in brackets and non-zero compression
  std::filesystem::path zram0_dir = temp_sysfs.path / "zram0";
  std::filesystem::create_directories(zram0_dir);
  write_file(zram0_dir / "comp_algorithm", "[lzo-rle] lzo zstd\n");
  write_file(zram0_dir / "disksize", "1073741824\n");
  write_file(zram0_dir / "mm_stat",
             " 209715200   52428800   55000000        0   60000000        0    "
             "    0        0        0\n");

  // Setup zram1 with algorithm without brackets and 0 compressed data size
  // (divide-by-zero check)
  std::filesystem::path zram1_dir = temp_sysfs.path / "zram1";
  std::filesystem::create_directories(zram1_dir);
  write_file(zram1_dir / "comp_algorithm", "zstd\n");
  write_file(zram1_dir / "disksize", "2147483648\n");
  write_file(zram1_dir / "mm_stat",
             "       0        0        0        0        0        0        0   "
             "     0        0\n");

  // Non-zram directory to verify filtering
  std::filesystem::path sda_dir = temp_sysfs.path / "sda";
  std::filesystem::create_directories(sda_dir);
  write_file(sda_dir / "size", "500000000\n");

  virmux::MemoryTelemetry tele("/tmp/dummy_proc", temp_sysfs.path.string());
  auto zrams = tele.read_zram_stats();

  TEST_ASSERT_EQUAL_INT(2, zrams.size());

  // zram0 checks
  TEST_ASSERT_TRUE(zrams[0].present);
  TEST_ASSERT_EQUAL_STRING("zram0", zrams[0].name.c_str());
  TEST_ASSERT_EQUAL_STRING("lzo-rle", zrams[0].algorithm.c_str());
  TEST_ASSERT_EQUAL_UINT64(1073741824ULL, zrams[0].disk_size);
  TEST_ASSERT_EQUAL_UINT64(209715200ULL, zrams[0].orig_data_size);
  TEST_ASSERT_EQUAL_UINT64(52428800ULL, zrams[0].compr_data_size);
  TEST_ASSERT_EQUAL_UINT64(55000000ULL, zrams[0].mem_used_total);
  TEST_ASSERT_TRUE(std::abs(zrams[0].compression_ratio - 4.0) < 0.001);

  // zram1 checks
  TEST_ASSERT_TRUE(zrams[1].present);
  TEST_ASSERT_EQUAL_STRING("zram1", zrams[1].name.c_str());
  TEST_ASSERT_EQUAL_STRING("zstd", zrams[1].algorithm.c_str());
  TEST_ASSERT_EQUAL_UINT64(2147483648ULL, zrams[1].disk_size);
  TEST_ASSERT_EQUAL_UINT64(0ULL, zrams[1].orig_data_size);
  TEST_ASSERT_EQUAL_UINT64(0ULL, zrams[1].compr_data_size);
  TEST_ASSERT_EQUAL_UINT64(0ULL, zrams[1].mem_used_total);
  TEST_ASSERT_TRUE(std::abs(zrams[1].compression_ratio - 1.0) < 0.001);
}

void test_zram_stats_nonexistent_directory(void) {
  virmux::MemoryTelemetry tele("/tmp/dummy_proc",
                               "/tmp/virmux_nonexistent_sysfs_12345");
  auto zrams = tele.read_zram_stats();
  TEST_ASSERT_TRUE(zrams.empty());

  auto host_stats = tele.read_host_stats();
  TEST_ASSERT_TRUE(host_stats.zram_devices.empty());
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_process_stats_nonexistent_or_invalid_pid);
  RUN_TEST(test_process_stats_mock_smaps_rollup);
  RUN_TEST(test_process_stats_mock_status_fallback);
  RUN_TEST(test_process_stats_real_self);
  RUN_TEST(test_host_stats_mock_meminfo);
  RUN_TEST(test_host_stats_real);
  RUN_TEST(test_zram_stats_mock_multiple_devices);
  RUN_TEST(test_zram_stats_nonexistent_directory);
  return UNITY_END();
}
