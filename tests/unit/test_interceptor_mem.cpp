/**
 * @file test_interceptor_mem.cpp
 * @brief Unit tests for dynamic interceptor memory hook (mmap/mmap64 and
 * MADV_MERGEABLE).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#undef _FILE_OFFSET_BITS

extern "C" {
#include "unity/unity.h"
}

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "virmux/intercept.h"

namespace {

/**
 * @brief Parse /proc/self/smaps to check if the VMA at @p addr has the
 * mergeable ('mg') flag.
 */
bool is_vma_mergeable(const void* addr) {
  FILE* fp = std::fopen("/proc/self/smaps", "r");
  if (!fp) {
    return false;
  }

  char prefix[64];
  std::snprintf(prefix, sizeof(prefix), "%lx-",
                reinterpret_cast<uintptr_t>(addr));
  size_t prefix_len = std::strlen(prefix);

  char line[512];
  bool in_vma = false;
  bool has_mg = false;

  while (std::fgets(line, sizeof(line), fp)) {
    if (std::strncmp(line, prefix, prefix_len) == 0) {
      in_vma = true;
    } else if (in_vma) {
      if (std::strncmp(line, "VmFlags:", 8) == 0) {
        const char* p = line + 8;
        while (*p) {
          while (*p == ' ' || *p == '\t') {
            ++p;
          }
          if (*p == '\0' || *p == '\n') {
            break;
          }
          const char* start = p;
          while (*p && *p != ' ' && *p != '\t' && *p != '\n') {
            ++p;
          }
          if ((p - start == 2) && start[0] == 'm' && start[1] == 'g') {
            has_mg = true;
            break;
          }
        }
        break;
      }
      if (std::strchr(line, '-') != nullptr &&
          std::strchr(line, ':') == nullptr) {
        break;
      }
    }
  }

  std::fclose(fp);
  return has_mg;
}

}  // namespace

void setUp(void) {}

void tearDown(void) { unsetenv(VIRMUX_ENV_KSM_AUTO_MERGE); }

void test_mmap_anon_private_auto_merge_enabled(void) {
  setenv(VIRMUX_ENV_KSM_AUTO_MERGE, "1", 1);
  const size_t size = 4096UL * 4;

  void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  TEST_ASSERT_NOT_EQUAL(MAP_FAILED, ptr);
  TEST_ASSERT_NOT_NULL(ptr);

  // Verify memory is readable and writable
  std::memset(ptr, 0xAB, size);
  auto* bytes = static_cast<unsigned char*>(ptr);
  TEST_ASSERT_EQUAL_HEX8(0xAB, bytes[0]);
  TEST_ASSERT_EQUAL_HEX8(0xAB, bytes[size - 1]);

  // Verify proactive MADV_MERGEABLE registration in smaps
  TEST_ASSERT_TRUE(is_vma_mergeable(ptr));

  int unmap_res = munmap(ptr, size);
  TEST_ASSERT_EQUAL_INT(0, unmap_res);
}

void test_mmap64_anon_private_auto_merge_enabled(void) {
  setenv(VIRMUX_ENV_KSM_AUTO_MERGE, "1", 1);
  const size_t size = 4096UL * 2;

  void* ptr = mmap64(nullptr, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  TEST_ASSERT_NOT_EQUAL(MAP_FAILED, ptr);
  TEST_ASSERT_NOT_NULL(ptr);

  std::memset(ptr, 0xCD, size);
  auto* bytes = static_cast<unsigned char*>(ptr);
  TEST_ASSERT_EQUAL_HEX8(0xCD, bytes[0]);
  TEST_ASSERT_EQUAL_HEX8(0xCD, bytes[size - 1]);

  TEST_ASSERT_TRUE(is_vma_mergeable(ptr));

  int unmap_res = munmap(ptr, size);
  TEST_ASSERT_EQUAL_INT(0, unmap_res);
}

void test_mmap_anon_disabled_when_env_zero(void) {
  setenv(VIRMUX_ENV_KSM_AUTO_MERGE, "0", 1);
  const size_t size = 4096;

  void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  TEST_ASSERT_NOT_EQUAL(MAP_FAILED, ptr);
  TEST_ASSERT_NOT_NULL(ptr);

  std::memset(ptr, 0xEF, size);
  TEST_ASSERT_FALSE(is_vma_mergeable(ptr));

  int unmap_res = munmap(ptr, size);
  TEST_ASSERT_EQUAL_INT(0, unmap_res);
}

void test_mmap_anon_disabled_when_env_unset(void) {
  unsetenv(VIRMUX_ENV_KSM_AUTO_MERGE);
  const size_t size = 4096;

  void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  TEST_ASSERT_NOT_EQUAL(MAP_FAILED, ptr);
  TEST_ASSERT_NOT_NULL(ptr);

  std::memset(ptr, 0x12, size);
  TEST_ASSERT_FALSE(is_vma_mergeable(ptr));

  int unmap_res = munmap(ptr, size);
  TEST_ASSERT_EQUAL_INT(0, unmap_res);
}

void test_mmap_file_backed_not_mergeable_even_if_auto_merge_enabled(void) {
  setenv(VIRMUX_ENV_KSM_AUTO_MERGE, "1", 1);

  const char* temp_path = "/tmp/virmux_test_mmap_file.bin";
  int fd = open(temp_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
  TEST_ASSERT_TRUE(fd >= 0);

  const size_t size = 4096;
  char buf[size];
  std::memset(buf, 0x77, sizeof(buf));
  ssize_t written = write(fd, buf, size);
  TEST_ASSERT_EQUAL_INT(static_cast<ssize_t>(size), written);

  void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  TEST_ASSERT_NOT_EQUAL(MAP_FAILED, ptr);
  TEST_ASSERT_NOT_NULL(ptr);

  // File-backed mappings must not be flagged with MADV_MERGEABLE
  TEST_ASSERT_FALSE(is_vma_mergeable(ptr));

  int unmap_res = munmap(ptr, size);
  TEST_ASSERT_EQUAL_INT(0, unmap_res);

  close(fd);
  unlink(temp_path);
}

void test_mmap_reentrancy_and_stress(void) {
  setenv(VIRMUX_ENV_KSM_AUTO_MERGE, "1", 1);
  const size_t size = 4096;

  for (int i = 0; i < 50; ++i) {
    void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    TEST_ASSERT_NOT_EQUAL(MAP_FAILED, ptr);
    auto* b = static_cast<unsigned char*>(ptr);
    b[0] = static_cast<unsigned char>(i);
    int res = munmap(ptr, size);
    TEST_ASSERT_EQUAL_INT(0, res);
  }
}

void test_mmap_invalid_args_fails(void) {
  // mmap with size 0 should fail with EINVAL; the invalid length is the
  // input under test.
  // NOLINTNEXTLINE(clang-analyzer-unix.StdCLibraryFunctions): deliberate
  void* ptr = mmap(nullptr, 0, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  TEST_ASSERT_EQUAL_PTR(MAP_FAILED, ptr);
  TEST_ASSERT_EQUAL_INT(EINVAL, errno);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_mmap_anon_private_auto_merge_enabled);
  RUN_TEST(test_mmap64_anon_private_auto_merge_enabled);
  RUN_TEST(test_mmap_anon_disabled_when_env_zero);
  RUN_TEST(test_mmap_anon_disabled_when_env_unset);
  RUN_TEST(test_mmap_file_backed_not_mergeable_even_if_auto_merge_enabled);
  RUN_TEST(test_mmap_reentrancy_and_stress);
  RUN_TEST(test_mmap_invalid_args_fails);
  return UNITY_END();
}
