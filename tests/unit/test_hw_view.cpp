/**
 * @file test_hw_view.cpp
 * @brief Unit tests for the sandbox's synthetic hardware view (memory limits,
 * synthetic /proc files and thread-count environment).
 */

extern "C" {
#include "unity/unity.h"
}

#include <cstdint>
#include <cstring>
#include <string>

#include "virmux/hw_view.hpp"

void setUp(void) {}
void tearDown(void) {}

namespace {
constexpr uint64_t kMiB = 1024ULL * 1024;
}  // namespace

void test_memory_limits_derive_high_and_swap_from_max(void) {
  const virmux::MemoryLimits m = virmux::compute_memory_limits(64 * kMiB);
  TEST_ASSERT_EQUAL_UINT64(64 * kMiB, m.max);
  TEST_ASSERT_EQUAL_UINT64(60 * kMiB, m.high);      // 15/16 of max
  TEST_ASSERT_EQUAL_UINT64(16 * kMiB, m.swap_max);  // max/4
}

void test_memory_limits_zero_means_unlimited(void) {
  const virmux::MemoryLimits m = virmux::compute_memory_limits(0);
  TEST_ASSERT_EQUAL_UINT64(0, m.max);
  TEST_ASSERT_EQUAL_UINT64(0, m.high);
  TEST_ASSERT_EQUAL_UINT64(0, m.swap_max);
}

void test_memory_limits_huge_value_does_not_overflow(void) {
  const uint64_t huge = UINT64_MAX - 7;
  const virmux::MemoryLimits m = virmux::compute_memory_limits(huge);
  TEST_ASSERT_EQUAL_UINT64(huge, m.max);
  TEST_ASSERT_TRUE(m.high < m.max);
  TEST_ASSERT_TRUE(m.high > m.max / 2);
}

void test_synthetic_meminfo_matches_cgroup_limits(void) {
  const virmux::MemoryLimits m = virmux::compute_memory_limits(512 * kMiB);
  const std::string info = virmux::generate_synthetic_meminfo(m);
  TEST_ASSERT_NOT_NULL(
      std::strstr(info.c_str(), "MemTotal:        524288 kB\n"));
  TEST_ASSERT_NOT_NULL(
      std::strstr(info.c_str(), "SwapTotal:       131072 kB\n"));
  TEST_ASSERT_NOT_NULL(
      std::strstr(info.c_str(), "SwapFree:        131072 kB\n"));
  TEST_ASSERT_NOT_NULL(std::strstr(info.c_str(), "MemAvailable:"));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_memory_limits_derive_high_and_swap_from_max);
  RUN_TEST(test_memory_limits_zero_means_unlimited);
  RUN_TEST(test_memory_limits_huge_value_does_not_overflow);
  RUN_TEST(test_synthetic_meminfo_matches_cgroup_limits);
  return UNITY_END();
}
