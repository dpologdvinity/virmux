/**
 * @file test_interceptor.cpp
 * @brief Unit tests for virmux dynamic interceptor library
 * (libvirmux_intercept.so).
 */

extern "C" {
#include "unity/unity.h"
}

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

void setUp(void) {}

void tearDown(void) {
  unsetenv("VIRMUX_VIRTUAL_CORES");
  unsetenv("VIRMUX_SPOOFED_MEMINFO");
  unsetenv("VIRMUX_SPOOFED_CPUINFO");
  unsetenv("VIRMUX_SPOOFED_CPU_ONLINE");
}

namespace {

/** @brief Creates @p path holding @p content. */
void write_fixture(const char* path, const char* content) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  TEST_ASSERT_TRUE(fd >= 0);
  ssize_t written = write(fd, content, strlen(content));
  TEST_ASSERT_EQUAL_INT(static_cast<ssize_t>(strlen(content)), written);
  close(fd);
}

/** @brief First bytes of @p path read through open(2) with @p flags. */
std::string read_via_open(const char* path, int flags = O_RDONLY) {
  int fd = open(path, flags);
  if (fd < 0) {
    return "<open failed>";
  }
  char buf[256] = {};
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  return n > 0 ? std::string(buf, static_cast<size_t>(n)) : "";
}

}  // namespace

void test_sysconf_returns_spoofed_core_count(void) {
  setenv("VIRMUX_VIRTUAL_CORES", "3", 1);
  long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
  TEST_ASSERT_EQUAL_INT(3, nprocs);
}

void test_interceptor_reentrancy_guard(void) {
  // Calling sysconf repeatedly should not deadlock or crash
  setenv("VIRMUX_VIRTUAL_CORES", "3", 1);
  for (int i = 0; i < 100; ++i) {
    long res = sysconf(_SC_NPROCESSORS_CONF);
    TEST_ASSERT_EQUAL_INT(3, res);
  }
}

void test_get_nprocs_returns_spoofed_core_count(void) {
  setenv("VIRMUX_VIRTUAL_CORES", "4", 1);
  int nprocs = get_nprocs();
  TEST_ASSERT_EQUAL_INT(4, nprocs);
  int nprocs_conf = get_nprocs_conf();
  TEST_ASSERT_EQUAL_INT(4, nprocs_conf);
}

void test_sysconf_passthrough_when_unset(void) {
  unsetenv("VIRMUX_VIRTUAL_CORES");
  long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
  TEST_ASSERT_TRUE(nprocs > 0);
}

void test_sysconf_other_sc_names_passthrough(void) {
  long page_size = sysconf(_SC_PAGESIZE);
  TEST_ASSERT_TRUE(page_size > 0);
}

void test_open_meminfo_redirection(void) {
  const char* fake_meminfo = "/tmp/virmux_test_meminfo.txt";
  const char* content =
      "MemTotal:        2097152 kB\nMemFree:         1572864 kB\n";
  int fd = open(fake_meminfo, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  TEST_ASSERT_TRUE(fd >= 0);
  ssize_t written = write(fd, content, strlen(content));
  TEST_ASSERT_EQUAL_INT(static_cast<ssize_t>(strlen(content)), written);
  close(fd);

  setenv("VIRMUX_SPOOFED_MEMINFO", fake_meminfo, 1);
  int read_fd = open("/proc/meminfo", O_RDONLY);
  TEST_ASSERT_TRUE(read_fd >= 0);

  char buf[128];
  memset(buf, 0, sizeof(buf));
  ssize_t n = read(read_fd, buf, sizeof(buf) - 1);
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_NOT_NULL(strstr(buf, "2097152 kB"));
  close(read_fd);

  unlink(fake_meminfo);
}

void test_open_meminfo_passthrough_when_unset(void) {
  unsetenv("VIRMUX_SPOOFED_MEMINFO");
  int fd = open("/proc/meminfo", O_RDONLY);
  TEST_ASSERT_TRUE(fd >= 0);
  char buf[128];
  memset(buf, 0, sizeof(buf));
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  TEST_ASSERT_TRUE(n > 0);
  // Real /proc/meminfo also contains MemTotal
  TEST_ASSERT_NOT_NULL(strstr(buf, "MemTotal:"));
  close(fd);
}

void test_sysconf_invalid_core_env_falls_back(void) {
  setenv("VIRMUX_VIRTUAL_CORES", "not_a_number", 1);
  long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
  TEST_ASSERT_TRUE(nprocs > 0);
  TEST_ASSERT_NOT_EQUAL(0, nprocs);
}

void test_open_creates_file_with_mode(void) {
  const char* test_path = "/tmp/virmux_test_creat.txt";
  unlink(test_path);
  int fd = open(test_path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
  TEST_ASSERT_TRUE(fd >= 0);
  close(fd);

  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, stat(test_path, &st));
  // Verify file exists and permissions match 0600 mask
  TEST_ASSERT_EQUAL_HEX(0600, st.st_mode & 0777);
  unlink(test_path);
}

void test_openat_meminfo_redirection(void) {
  const char* fake_meminfo = "/tmp/virmux_test_meminfo_openat.txt";
  const char* content =
      "MemTotal:        4194304 kB\nMemFree:         3145728 kB\n";
  int fd = open(fake_meminfo, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  TEST_ASSERT_TRUE(fd >= 0);
  ssize_t written = write(fd, content, strlen(content));
  TEST_ASSERT_EQUAL_INT(static_cast<ssize_t>(strlen(content)), written);
  close(fd);

  setenv("VIRMUX_SPOOFED_MEMINFO", fake_meminfo, 1);
  int read_fd = openat(AT_FDCWD, "/proc/meminfo", O_RDONLY);
  TEST_ASSERT_TRUE(read_fd >= 0);

  char buf[128];
  memset(buf, 0, sizeof(buf));
  ssize_t n = read(read_fd, buf, sizeof(buf) - 1);
  TEST_ASSERT_TRUE(n > 0);
  TEST_ASSERT_NOT_NULL(strstr(buf, "4194304 kB"));
  close(read_fd);

  unlink(fake_meminfo);
}

void test_open_cpuinfo_redirection(void) {
  const char* fake = "/tmp/virmux_test_cpuinfo.txt";
  write_fixture(fake, "processor\t: 7\nmodel name\t: Spoofed\n\n");
  setenv("VIRMUX_SPOOFED_CPUINFO", fake, 1);
  TEST_ASSERT_NOT_NULL(
      strstr(read_via_open("/proc/cpuinfo").c_str(), "Spoofed"));
  unlink(fake);
}

void test_open_cpu_online_redirection(void) {
  const char* fake = "/tmp/virmux_test_cpu_online.txt";
  write_fixture(fake, "2-3\n");
  setenv("VIRMUX_SPOOFED_CPU_ONLINE", fake, 1);
  TEST_ASSERT_EQUAL_STRING(
      "2-3\n", read_via_open("/sys/devices/system/cpu/online").c_str());
  unlink(fake);
}

void test_write_mode_open_not_redirected(void) {
  const char* fake = "/tmp/virmux_test_meminfo_rw.txt";
  write_fixture(fake, "MemTotal:        1024 kB\n");
  setenv("VIRMUX_SPOOFED_MEMINFO", fake, 1);
  // A writable open must reach the real file (and fail there), never the
  // spoofed copy.
  int fd = open("/proc/meminfo", O_WRONLY | O_APPEND);
  if (fd >= 0) {
    (void)!write(fd, "X", 1);
    close(fd);
  }
  fd = openat(AT_FDCWD, "/proc/meminfo", O_RDWR);
  if (fd >= 0) {
    close(fd);
  }
  unsetenv("VIRMUX_SPOOFED_MEMINFO");
  TEST_ASSERT_EQUAL_STRING("MemTotal:        1024 kB\n",
                           read_via_open(fake).c_str());
  unlink(fake);
}

void test_fopen_meminfo_redirection(void) {
  const char* fake = "/tmp/virmux_test_meminfo_fopen.txt";
  write_fixture(fake, "MemTotal:        8192 kB\n");
  setenv("VIRMUX_SPOOFED_MEMINFO", fake, 1);

  FILE* f = fopen("/proc/meminfo", "r");
  TEST_ASSERT_NOT_NULL(f);
  char line[128] = {};
  TEST_ASSERT_NOT_NULL(fgets(line, sizeof(line), f));
  fclose(f);
  TEST_ASSERT_NOT_NULL(strstr(line, "8192 kB"));

  // C++ streams go through fopen too.
  std::ifstream in("/proc/meminfo");
  std::string first;
  std::getline(in, first);
  TEST_ASSERT_NOT_NULL(strstr(first.c_str(), "8192 kB"));

  // Update modes are never redirected.
  FILE* rw = fopen("/proc/meminfo", "r+");
  if (rw != nullptr) {
    fclose(rw);
  }
  unsetenv("VIRMUX_SPOOFED_MEMINFO");
  TEST_ASSERT_EQUAL_STRING("MemTotal:        8192 kB\n",
                           read_via_open(fake).c_str());
  unlink(fake);
}

void test_fopen_passthrough_when_unset(void) {
  FILE* f = fopen("/proc/meminfo", "r");
  TEST_ASSERT_NOT_NULL(f);
  char line[128] = {};
  TEST_ASSERT_NOT_NULL(fgets(line, sizeof(line), f));
  fclose(f);
  TEST_ASSERT_NOT_NULL(strstr(line, "MemTotal:"));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_sysconf_returns_spoofed_core_count);
  RUN_TEST(test_interceptor_reentrancy_guard);
  RUN_TEST(test_get_nprocs_returns_spoofed_core_count);
  RUN_TEST(test_sysconf_passthrough_when_unset);
  RUN_TEST(test_sysconf_other_sc_names_passthrough);
  RUN_TEST(test_sysconf_invalid_core_env_falls_back);
  RUN_TEST(test_open_meminfo_redirection);
  RUN_TEST(test_openat_meminfo_redirection);
  RUN_TEST(test_open_meminfo_passthrough_when_unset);
  RUN_TEST(test_open_creates_file_with_mode);
  RUN_TEST(test_open_cpuinfo_redirection);
  RUN_TEST(test_open_cpu_online_redirection);
  RUN_TEST(test_write_mode_open_not_redirected);
  RUN_TEST(test_fopen_meminfo_redirection);
  RUN_TEST(test_fopen_passthrough_when_unset);
  return UNITY_END();
}
