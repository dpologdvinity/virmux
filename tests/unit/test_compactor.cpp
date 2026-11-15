/**
 * @file test_compactor.cpp
 * @brief Unit tests for ProcessCompactor using pidfd_open and process_madvise.
 */

extern "C" {
#include "unity/unity.h"
}

#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <vector>

#include "virmux/compactor.hpp"

void setUp(void) {}
void tearDown(void) {}

void test_compactor_is_supported(void) {
  TEST_ASSERT_TRUE(virmux::ProcessCompactor::is_supported());
}

void test_compactor_self_reclaim_cold_and_pageout(void) {
  constexpr size_t kMemSize = 4 * 1024 * 1024;  // 4 MB
  void* addr = ::mmap(nullptr, kMemSize, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  TEST_ASSERT_NOT_NULL(addr);
  TEST_ASSERT_TRUE(addr != MAP_FAILED);

  std::memset(addr, 0xAB, kMemSize);

  virmux::ProcessCompactor compactor;
  std::vector<std::pair<uintptr_t, size_t>> ranges = {
      {reinterpret_cast<uintptr_t>(addr), kMemSize}};

  // Reclaim with Cold advice
  auto res_cold =
      compactor.reclaim_ranges(::getpid(), ranges, virmux::ReclaimAdvice::Cold);
  TEST_ASSERT_TRUE(res_cold.success);
  TEST_ASSERT_EQUAL_INT(0, res_cold.error_code);
  TEST_ASSERT_EQUAL_UINT64(1, res_cold.ranges_count);
  TEST_ASSERT_EQUAL_UINT64(kMemSize, res_cold.bytes_advised);

  // Reclaim with Pageout advice
  auto res_pageout = compactor.reclaim_ranges(::getpid(), ranges,
                                              virmux::ReclaimAdvice::Pageout);
  TEST_ASSERT_TRUE(res_pageout.success);
  TEST_ASSERT_EQUAL_INT(0, res_pageout.error_code);
  TEST_ASSERT_EQUAL_UINT64(1, res_pageout.ranges_count);
  TEST_ASSERT_EQUAL_UINT64(kMemSize, res_pageout.bytes_advised);

  ::munmap(addr, kMemSize);
}

void test_compactor_reclaim_memory_current_pid(void) {
  virmux::ProcessCompactor compactor;
  auto res = compactor.reclaim_memory(::getpid(), virmux::ReclaimAdvice::Cold);

  TEST_ASSERT_TRUE(res.success);
  TEST_ASSERT_EQUAL_INT(0, res.error_code);
  TEST_ASSERT_TRUE(res.ranges_count > 0);
  TEST_ASSERT_TRUE(res.bytes_advised > 0);
}

void test_compactor_nonexistent_pid(void) {
  virmux::ProcessCompactor compactor;
  std::vector<std::pair<uintptr_t, size_t>> dummy_ranges = {{0x10000, 4096}};

  // Negative PID
  auto res_neg = compactor.reclaim_ranges(-99999, dummy_ranges,
                                          virmux::ReclaimAdvice::Pageout);
  TEST_ASSERT_FALSE(res_neg.success);
  TEST_ASSERT_EQUAL_INT(ESRCH, res_neg.error_code);

  auto res_neg_mem =
      compactor.reclaim_memory(-99999, virmux::ReclaimAdvice::Pageout);
  TEST_ASSERT_FALSE(res_neg_mem.success);
  TEST_ASSERT_EQUAL_INT(ESRCH, res_neg_mem.error_code);

  // Non-existent large positive PID
  auto res_pos = compactor.reclaim_ranges(999999, dummy_ranges,
                                          virmux::ReclaimAdvice::Pageout);
  TEST_ASSERT_FALSE(res_pos.success);
  TEST_ASSERT_EQUAL_INT(ESRCH, res_pos.error_code);

  auto res_pos_mem =
      compactor.reclaim_memory(999999, virmux::ReclaimAdvice::Pageout);
  TEST_ASSERT_FALSE(res_pos_mem.success);
  TEST_ASSERT_EQUAL_INT(ESRCH, res_pos_mem.error_code);
}

void test_compactor_chunking_over_iov_max(void) {
  constexpr size_t kPageSize = 4096;
  constexpr size_t kNumRanges = 1050;
  constexpr size_t kTotalSize = kNumRanges * kPageSize;

  void* addr = ::mmap(nullptr, kTotalSize, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  TEST_ASSERT_NOT_NULL(addr);
  TEST_ASSERT_TRUE(addr != MAP_FAILED);

  std::memset(addr, 0xCD, kTotalSize);

  std::vector<std::pair<uintptr_t, size_t>> ranges;
  ranges.reserve(kNumRanges);
  for (size_t i = 0; i < kNumRanges; ++i) {
    uintptr_t range_addr = reinterpret_cast<uintptr_t>(addr) + (i * kPageSize);
    ranges.emplace_back(range_addr, kPageSize);
  }

  virmux::ProcessCompactor compactor;
  auto res =
      compactor.reclaim_ranges(::getpid(), ranges, virmux::ReclaimAdvice::Cold);

  TEST_ASSERT_TRUE(res.success);
  TEST_ASSERT_EQUAL_INT(0, res.error_code);
  TEST_ASSERT_EQUAL_UINT64(kNumRanges, res.ranges_count);
  TEST_ASSERT_EQUAL_UINT64(kTotalSize, res.bytes_advised);

  ::munmap(addr, kTotalSize);
}

void test_compactor_unaligned_ranges(void) {
  constexpr size_t kAllocSize = 8192;  // 2 pages
  void* addr = ::mmap(nullptr, kAllocSize, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  TEST_ASSERT_NOT_NULL(addr);
  TEST_ASSERT_TRUE(addr != MAP_FAILED);

  std::memset(addr, 0xEF, kAllocSize);

  virmux::ProcessCompactor compactor;
  // Offset by 123 bytes, length 500 bytes (within page 0)
  uintptr_t unaligned_addr = reinterpret_cast<uintptr_t>(addr) + 123;
  std::vector<std::pair<uintptr_t, size_t>> ranges = {{unaligned_addr, 500}};

  auto res =
      compactor.reclaim_ranges(::getpid(), ranges, virmux::ReclaimAdvice::Cold);

  TEST_ASSERT_TRUE(res.success);
  TEST_ASSERT_EQUAL_INT(0, res.error_code);
  TEST_ASSERT_EQUAL_UINT64(1, res.ranges_count);
  TEST_ASSERT_TRUE(res.bytes_advised >= 4096);

  ::munmap(addr, kAllocSize);
}

void test_compactor_empty_and_zero_length_ranges(void) {
  virmux::ProcessCompactor compactor;

  // Empty vector
  auto res_empty = compactor.reclaim_ranges(::getpid(), {});
  TEST_ASSERT_TRUE(res_empty.success);
  TEST_ASSERT_EQUAL_INT(0, res_empty.error_code);
  TEST_ASSERT_EQUAL_UINT64(0, res_empty.ranges_count);
  TEST_ASSERT_EQUAL_UINT64(0, res_empty.bytes_advised);

  // Filtered zero-length ranges
  std::vector<std::pair<uintptr_t, size_t>> zero_ranges = {{0x10000, 0}};
  auto res_zero = compactor.reclaim_ranges(::getpid(), zero_ranges);
  TEST_ASSERT_TRUE(res_zero.success);
  TEST_ASSERT_EQUAL_INT(0, res_zero.error_code);
  TEST_ASSERT_EQUAL_UINT64(0, res_zero.ranges_count);
  TEST_ASSERT_EQUAL_UINT64(0, res_zero.bytes_advised);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_compactor_is_supported);
  RUN_TEST(test_compactor_self_reclaim_cold_and_pageout);
  RUN_TEST(test_compactor_reclaim_memory_current_pid);
  RUN_TEST(test_compactor_nonexistent_pid);
  RUN_TEST(test_compactor_chunking_over_iov_max);
  RUN_TEST(test_compactor_unaligned_ranges);
  RUN_TEST(test_compactor_empty_and_zero_length_ranges);
  return UNITY_END();
}
