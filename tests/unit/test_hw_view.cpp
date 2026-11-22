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
#include <map>
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

namespace {

/** @brief Fake environment for compute_thread_env. */
std::map<std::string, std::string> g_env;

const char* fake_getenv(const char* name) {
  const auto it = g_env.find(name);
  return it == g_env.end() ? nullptr : it->second.c_str();
}

std::string env_value(int n, const char* name) {
  for (const auto& [k, v] : virmux::compute_thread_env(n, fake_getenv)) {
    if (k == name) {
      return v;
    }
  }
  return "<missing>";
}

}  // namespace

void test_thread_env_unset_gets_n(void) {
  g_env.clear();
  const auto env = virmux::compute_thread_env(2, fake_getenv);
  const char* expected[] = {"OMP_NUM_THREADS",  "OPENBLAS_NUM_THREADS",
                            "MKL_NUM_THREADS",  "NUMEXPR_NUM_THREADS",
                            "GOMAXPROCS",       "RAYON_NUM_THREADS",
                            "CARGO_BUILD_JOBS", "CMAKE_BUILD_PARALLEL_LEVEL",
                            "PYTHON_CPU_COUNT"};
  TEST_ASSERT_EQUAL_size_t(sizeof(expected) / sizeof(expected[0]), env.size());
  for (const char* name : expected) {
    TEST_ASSERT_EQUAL_STRING_MESSAGE("2", env_value(2, name).c_str(), name);
  }
}

void test_thread_env_keeps_smaller_user_value(void) {
  g_env = {{"GOMAXPROCS", "1"}, {"CARGO_BUILD_JOBS", "2"}};
  TEST_ASSERT_EQUAL_STRING("1", env_value(2, "GOMAXPROCS").c_str());
  TEST_ASSERT_EQUAL_STRING("2", env_value(2, "CARGO_BUILD_JOBS").c_str());
}

void test_thread_env_caps_larger_user_value(void) {
  g_env = {{"GOMAXPROCS", "64"}, {"OMP_NUM_THREADS", "8"}};
  TEST_ASSERT_EQUAL_STRING("2", env_value(2, "GOMAXPROCS").c_str());
  TEST_ASSERT_EQUAL_STRING("2", env_value(2, "OMP_NUM_THREADS").c_str());
}

void test_thread_env_replaces_junk(void) {
  const char* junk[] = {
      "", "abc", "0", "-1", "2x", "4,2", " 1", "1e3", "99999999999999999999"};
  for (const char* j : junk) {
    g_env = {{"RAYON_NUM_THREADS", j}};
    TEST_ASSERT_EQUAL_STRING_MESSAGE(
        "3", env_value(3, "RAYON_NUM_THREADS").c_str(), j);
  }
}

void test_thread_env_leaves_makeflags_alone(void) {
  g_env = {{"MAKEFLAGS", "-j64"}, {"UV_THREADPOOL_SIZE", "64"}};
  TEST_ASSERT_EQUAL_STRING("<missing>", env_value(2, "MAKEFLAGS").c_str());
  TEST_ASSERT_EQUAL_STRING("<missing>",
                           env_value(2, "UV_THREADPOOL_SIZE").c_str());
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_memory_limits_derive_high_and_swap_from_max);
  RUN_TEST(test_memory_limits_zero_means_unlimited);
  RUN_TEST(test_memory_limits_huge_value_does_not_overflow);
  RUN_TEST(test_synthetic_meminfo_matches_cgroup_limits);
  RUN_TEST(test_thread_env_unset_gets_n);
  RUN_TEST(test_thread_env_keeps_smaller_user_value);
  RUN_TEST(test_thread_env_caps_larger_user_value);
  RUN_TEST(test_thread_env_replaces_junk);
  RUN_TEST(test_thread_env_leaves_makeflags_alone);
  return UNITY_END();
}
