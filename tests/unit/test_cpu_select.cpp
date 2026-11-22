/**
 * @file test_cpu_select.cpp
 * @brief Table tests for CPU list parsing/formatting and sandbox CPU
 * selection.
 */

extern "C" {
#include "unity/unity.h"
}

#include <sched.h>

#include <string>
#include <vector>

#include "virmux/cpu_select.hpp"

void setUp(void) {}
void tearDown(void) {}

namespace {

using virmux::CpuList;

/** @brief CPUs 0..n-1. */
CpuList range(int n) {
  CpuList cpus;
  for (int i = 0; i < n; ++i) {
    cpus.push_back(i);
  }
  return cpus;
}

std::string sel(const CpuList& avail, int n, uint64_t rotation,
                const std::vector<double>& load = {}) {
  return virmux::format_cpu_list(virmux::select_cpus(avail, n, rotation, load));
}

}  // namespace

void test_parse_cpu_list_ranges_and_singles(void) {
  const auto cpus = virmux::parse_cpu_list("0-3,8,10-11");
  TEST_ASSERT_TRUE(cpus.has_value());
  const CpuList expected{0, 1, 2, 3, 8, 10, 11};
  TEST_ASSERT_TRUE(expected == *cpus);
}

void test_parse_cpu_list_accepts_sysfs_trailing_newline(void) {
  const auto cpus = virmux::parse_cpu_list("0-9\n");
  TEST_ASSERT_TRUE(cpus.has_value());
  TEST_ASSERT_TRUE(range(10) == *cpus);
}

void test_parse_cpu_list_sorts_and_dedupes(void) {
  const auto cpus = virmux::parse_cpu_list("5,1-2,2");
  TEST_ASSERT_TRUE(cpus.has_value());
  const CpuList expected{1, 2, 5};
  TEST_ASSERT_TRUE(expected == *cpus);
}

void test_parse_cpu_list_rejects_junk(void) {
  const char* junk[] = {"",         "a",       "3-1",  "1,,2", "-1",
                        "1-",       "1-a",     "0x2",  "1 2",  ",",
                        "99999999", "0-99999", "1-2-3"};
  for (const char* j : junk) {
    TEST_ASSERT_FALSE_MESSAGE(virmux::parse_cpu_list(j).has_value(), j);
  }
}

void test_format_cpu_list_compresses_ranges(void) {
  TEST_ASSERT_EQUAL_STRING(
      "0-3,8,10-11", virmux::format_cpu_list({0, 1, 2, 3, 8, 10, 11}).c_str());
  TEST_ASSERT_EQUAL_STRING("5", virmux::format_cpu_list({5}).c_str());
  TEST_ASSERT_EQUAL_STRING("1,3", virmux::format_cpu_list({1, 3}).c_str());
  TEST_ASSERT_EQUAL_STRING("", virmux::format_cpu_list({}).c_str());
}

void test_select_ten_cpus_reserves_two_lowest(void) {
  TEST_ASSERT_EQUAL_STRING("2-3", sel(range(10), 2, 0).c_str());
  TEST_ASSERT_EQUAL_STRING("3-4", sel(range(10), 2, 1).c_str());
  TEST_ASSERT_EQUAL_STRING("2-5", sel(range(10), 4, 0).c_str());
}

void test_select_wraps_around_the_pool(void) {
  // Pool is 2-9 (8 CPUs); rotation 7 starts at CPU 9 and wraps to CPU 2.
  TEST_ASSERT_EQUAL_STRING("2,9", sel(range(10), 2, 7).c_str());
  // Rotation is taken modulo the pool size.
  TEST_ASSERT_EQUAL_STRING("2-3", sel(range(10), 2, 8).c_str());
}

void test_select_four_cpus_reserves_two(void) {
  TEST_ASSERT_EQUAL_STRING("2-3", sel(range(4), 2, 0).c_str());
  TEST_ASSERT_EQUAL_STRING("3", sel(range(4), 1, 1).c_str());
}

void test_select_three_cpus_reserves_nothing(void) {
  TEST_ASSERT_EQUAL_STRING("0-1", sel(range(3), 2, 0).c_str());
  TEST_ASSERT_EQUAL_STRING("0,2", sel(range(3), 2, 2).c_str());
}

void test_select_two_cpus_clamps_request(void) {
  TEST_ASSERT_EQUAL_STRING("0-1", sel(range(2), 4, 0).c_str());
}

void test_select_more_than_pool_uses_reserved_cpus(void) {
  // Asking for more than the unreserved pool lets the reservation yield.
  TEST_ASSERT_EQUAL_STRING("0-9", sel(range(10), 10, 3).c_str());
  TEST_ASSERT_EQUAL_STRING("0-9", sel(range(10), 64, 0).c_str());
}

void test_select_sparse_available_set(void) {
  const auto avail = virmux::parse_cpu_list("0-3,8,10-11");
  TEST_ASSERT_TRUE(avail.has_value());
  // Pool is {2,3,8,10,11}; windows are over pool positions, not CPU ids.
  TEST_ASSERT_EQUAL_STRING("2-3", sel(*avail, 2, 0).c_str());
  TEST_ASSERT_EQUAL_STRING("8,10", sel(*avail, 2, 2).c_str());
  TEST_ASSERT_EQUAL_STRING("2,11", sel(*avail, 2, 4).c_str());
}

void test_select_zero_or_negative_request_gets_one_cpu(void) {
  TEST_ASSERT_EQUAL_STRING("2", sel(range(10), 0, 0).c_str());
  TEST_ASSERT_EQUAL_STRING("2", sel(range(10), -3, 0).c_str());
}

void test_select_empty_available_set(void) {
  TEST_ASSERT_EQUAL_STRING("", sel({}, 2, 0).c_str());
}

void test_select_prefers_least_loaded_window(void) {
  std::vector<double> load(10, 0.0);
  load[2] = 1.0;
  load[3] = 1.0;
  TEST_ASSERT_EQUAL_STRING("4-5", sel(range(10), 2, 0, load).c_str());
  // All-zero load falls back to pure rotation.
  TEST_ASSERT_EQUAL_STRING(
      "5-6", sel(range(10), 2, 3, std::vector<double>(10)).c_str());
}

void test_available_cpus_is_subset_of_affinity(void) {
  const CpuList avail = virmux::available_cpus();
  TEST_ASSERT_TRUE(avail.size() > 0);
  cpu_set_t mask;
  CPU_ZERO(&mask);
  TEST_ASSERT_EQUAL_INT(0, sched_getaffinity(0, sizeof(mask), &mask));
  for (int cpu : avail) {
    TEST_ASSERT_TRUE(CPU_ISSET(cpu, &mask));
  }
  TEST_ASSERT_EQUAL_INT(CPU_COUNT(&mask), static_cast<int>(avail.size()));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_parse_cpu_list_ranges_and_singles);
  RUN_TEST(test_parse_cpu_list_accepts_sysfs_trailing_newline);
  RUN_TEST(test_parse_cpu_list_sorts_and_dedupes);
  RUN_TEST(test_parse_cpu_list_rejects_junk);
  RUN_TEST(test_format_cpu_list_compresses_ranges);
  RUN_TEST(test_select_ten_cpus_reserves_two_lowest);
  RUN_TEST(test_select_wraps_around_the_pool);
  RUN_TEST(test_select_four_cpus_reserves_two);
  RUN_TEST(test_select_three_cpus_reserves_nothing);
  RUN_TEST(test_select_two_cpus_clamps_request);
  RUN_TEST(test_select_more_than_pool_uses_reserved_cpus);
  RUN_TEST(test_select_sparse_available_set);
  RUN_TEST(test_select_zero_or_negative_request_gets_one_cpu);
  RUN_TEST(test_select_empty_available_set);
  RUN_TEST(test_select_prefers_least_loaded_window);
  RUN_TEST(test_available_cpus_is_subset_of_affinity);
  return UNITY_END();
}
