/**
 * @file test_memory_manager.cpp
 * @brief Unit tests for MemoryManager and VMA scanner.
 */

extern "C" {
#include "unity/unity.h"
}

#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "virmux/memory_manager.hpp"

void setUp(void) {}
void tearDown(void) {}

namespace {

void write_file(const std::filesystem::path& path, const std::string& content) {
  std::ofstream ofs(path);
  ofs << content;
}

struct TempDir {
  std::filesystem::path path;

  explicit TempDir(const char* tmpl = "/tmp/virmux_memman_test_XXXXXX") {
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

void test_vma_scanner_mock_maps(void) {
  TempDir temp_proc;
  TEST_ASSERT_FALSE(temp_proc.path.empty());

  const pid_t test_pid = 1234;
  std::filesystem::path pid_dir = temp_proc.path / std::to_string(test_pid);
  std::filesystem::create_directories(pid_dir);

  const std::string mock_maps =
      "55a73e6a0000-55a73e6b0000 r--p 00000000 08:01 12345                      /usr/bin/foo\n"
      "55a73e6b0000-55a73e6c0000 r-xp 00010000 08:01 12345                      /usr/bin/foo\n"
      "55a73e6c0000-55a73e6d0000 rw-p 00020000 08:01 12345                      /usr/bin/foo\n"
      "55a73e6d0000-55a73e6e0000 rw-p 00000000 00:00 0                          [heap]\n"
      "7f4955a00000-7f4955b00000 rw-p 00000000 00:00 0 \n"
      "7f4955b00000-7f4955c00000 r--p 00000000 00:00 0 \n"
      "7f4955c00000-7f4955d00000 rw-p 00000000 00:00 0                          [anon:custom_arena]\n"
      "7f4955d00123-7f4955e00000 rw-p 00000000 00:00 0                          [anon:misaligned]\n"
      "7f4955e00000-7f4955f00000 r-xp 00000000 00:00 0                          [vdso]\n"
      "7ffd551f3000-7ffd55214000 rw-p 00000000 00:00 0                          [stack]\n";

  write_file(pid_dir / "maps", mock_maps);

  virmux::MemoryManager mgr(temp_proc.path.string());

  // Test anonymous_only = true
  auto anon_vmas = mgr.scan_process_vmas(test_pid, true);
  TEST_ASSERT_EQUAL_INT(5, anon_vmas.size());

  // 1: [heap]
  TEST_ASSERT_EQUAL_HEX64(0x55a73e6d0000ULL, anon_vmas[0].start);
  TEST_ASSERT_EQUAL_HEX64(0x55a73e6e0000ULL, anon_vmas[0].end);
  TEST_ASSERT_TRUE(anon_vmas[0].is_anonymous);
  TEST_ASSERT_TRUE(anon_vmas[0].is_writable);

  // 2: anonymous empty pathname rw-p
  TEST_ASSERT_EQUAL_HEX64(0x7f4955a00000ULL, anon_vmas[1].start);
  TEST_ASSERT_EQUAL_HEX64(0x7f4955b00000ULL, anon_vmas[1].end);
  TEST_ASSERT_TRUE(anon_vmas[1].is_anonymous);
  TEST_ASSERT_TRUE(anon_vmas[1].is_writable);

  // 3: anonymous empty pathname r--p
  TEST_ASSERT_EQUAL_HEX64(0x7f4955b00000ULL, anon_vmas[2].start);
  TEST_ASSERT_EQUAL_HEX64(0x7f4955c00000ULL, anon_vmas[2].end);
  TEST_ASSERT_TRUE(anon_vmas[2].is_anonymous);
  TEST_ASSERT_FALSE(anon_vmas[2].is_writable);

  // 4: [anon:custom_arena] rw-p
  TEST_ASSERT_EQUAL_HEX64(0x7f4955c00000ULL, anon_vmas[3].start);
  TEST_ASSERT_EQUAL_HEX64(0x7f4955d00000ULL, anon_vmas[3].end);
  TEST_ASSERT_TRUE(anon_vmas[3].is_anonymous);
  TEST_ASSERT_TRUE(anon_vmas[3].is_writable);

  // 5: [stack] rw-p
  TEST_ASSERT_EQUAL_HEX64(0x7ffd551f3000ULL, anon_vmas[4].start);
  TEST_ASSERT_EQUAL_HEX64(0x7ffd55214000ULL, anon_vmas[4].end);
  TEST_ASSERT_TRUE(anon_vmas[4].is_anonymous);
  TEST_ASSERT_TRUE(anon_vmas[4].is_writable);

  // Test anonymous_only = false (should include file-backed and vdso, but exclude misaligned)
  auto all_vmas = mgr.scan_process_vmas(test_pid, false);
  TEST_ASSERT_EQUAL_INT(9, all_vmas.size());

  // 0: /usr/bin/foo r--p
  TEST_ASSERT_EQUAL_HEX64(0x55a73e6a0000ULL, all_vmas[0].start);
  TEST_ASSERT_FALSE(all_vmas[0].is_anonymous);
  TEST_ASSERT_FALSE(all_vmas[0].is_writable);

  // 2: /usr/bin/foo rw-p
  TEST_ASSERT_EQUAL_HEX64(0x55a73e6c0000ULL, all_vmas[2].start);
  TEST_ASSERT_FALSE(all_vmas[2].is_anonymous);
  TEST_ASSERT_TRUE(all_vmas[2].is_writable);

  // 7: [vdso] r-xp
  TEST_ASSERT_EQUAL_HEX64(0x7f4955e00000ULL, all_vmas[7].start);
  TEST_ASSERT_FALSE(all_vmas[7].is_anonymous);
  TEST_ASSERT_FALSE(all_vmas[7].is_writable);
}

void test_vma_scanner_nonexistent_or_invalid_pid(void) {
  virmux::MemoryManager mgr("/tmp/virmux_nonexistent_proc_12345");

  auto vmas_neg = mgr.scan_process_vmas(-1);
  TEST_ASSERT_TRUE(vmas_neg.empty());

  auto vmas_zero = mgr.scan_process_vmas(0);
  TEST_ASSERT_TRUE(vmas_zero.empty());

  auto vmas_missing = mgr.scan_process_vmas(999999);
  TEST_ASSERT_TRUE(vmas_missing.empty());
}

void test_vma_scanner_real_self(void) {
  virmux::MemoryManager mgr;
  auto anon_vmas = mgr.scan_process_vmas(::getpid(), true);

  TEST_ASSERT_FALSE(anon_vmas.empty());

  for (const auto& vma : anon_vmas) {
    TEST_ASSERT_TRUE(vma.end > vma.start);
    TEST_ASSERT_EQUAL_UINT64(0, vma.start % 4096);
    TEST_ASSERT_EQUAL_UINT64(0, vma.end % 4096);
    TEST_ASSERT_TRUE(vma.is_anonymous);
  }
}

void test_compact_process_self(void) {
  constexpr size_t kMemSize = 4 * 1024 * 1024;  // 4 MB
  void* addr = ::mmap(nullptr, kMemSize, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  TEST_ASSERT_NOT_NULL(addr);
  TEST_ASSERT_TRUE(addr != MAP_FAILED);

  std::memset(addr, 0xEE, kMemSize);

  virmux::MemoryManager mgr;

  // Test compact with Cold
  auto res_cold = mgr.compact_process(::getpid(), virmux::ReclaimAdvice::Cold);
  TEST_ASSERT_TRUE(res_cold.success);
  TEST_ASSERT_EQUAL_INT(0, res_cold.error_code);
  TEST_ASSERT_TRUE(res_cold.ranges_count > 0);
  TEST_ASSERT_TRUE(res_cold.bytes_advised > 0);

  // Test compact with Pageout
  auto res_pageout = mgr.compact_process(::getpid(), virmux::ReclaimAdvice::Pageout);
  TEST_ASSERT_TRUE(res_pageout.success);
  TEST_ASSERT_EQUAL_INT(0, res_pageout.error_code);
  TEST_ASSERT_TRUE(res_pageout.ranges_count > 0);
  TEST_ASSERT_TRUE(res_pageout.bytes_advised > 0);

  ::munmap(addr, kMemSize);
}

void test_compact_process_invalid_pid(void) {
  virmux::MemoryManager mgr;

  auto res_neg = mgr.compact_process(-1);
  TEST_ASSERT_FALSE(res_neg.success);
  TEST_ASSERT_EQUAL_INT(ESRCH, res_neg.error_code);

  auto res_missing = mgr.compact_process(999999);
  TEST_ASSERT_FALSE(res_missing.success);
  TEST_ASSERT_EQUAL_INT(ESRCH, res_missing.error_code);
}

void test_compact_cgroup_procs_mock(void) {
  TempDir temp_dir;
  TEST_ASSERT_FALSE(temp_dir.path.empty());

  std::filesystem::path procs_file = temp_dir.path / "cgroup.procs";
  std::string content =
      std::to_string(::getpid()) + "\n"
      "-1\n"
      "99999999\n"
      "invalid_entry\n";
  write_file(procs_file, content);

  virmux::MemoryManager mgr;
  size_t compacted = mgr.compact_cgroup_procs(procs_file.string(),
                                              virmux::ReclaimAdvice::Cold);
  TEST_ASSERT_EQUAL_UINT64(1, compacted);
}

void test_compact_cgroup_procs_nonexistent_file(void) {
  virmux::MemoryManager mgr;
  size_t compacted =
      mgr.compact_cgroup_procs("/tmp/virmux_nonexistent_cgroup_procs_999999");
  TEST_ASSERT_EQUAL_UINT64(0, compacted);
}

void test_telemetry_and_ksm_delegation(void) {
  virmux::MemoryManager mgr;

  auto ksm = mgr.get_ksm_stats();
  // Present or not depending on host, but struct initialized
  (void)ksm;

  auto host = mgr.get_host_stats();
  TEST_ASSERT_TRUE(host.mem_total_bytes > 0);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_vma_scanner_mock_maps);
  RUN_TEST(test_vma_scanner_nonexistent_or_invalid_pid);
  RUN_TEST(test_vma_scanner_real_self);
  RUN_TEST(test_compact_process_self);
  RUN_TEST(test_compact_process_invalid_pid);
  RUN_TEST(test_compact_cgroup_procs_mock);
  RUN_TEST(test_compact_cgroup_procs_nonexistent_file);
  RUN_TEST(test_telemetry_and_ksm_delegation);
  return UNITY_END();
}
