/**
 * @file test_interceptor.cpp
 * @brief Unit tests for virmux dynamic interceptor library (libvirmux_intercept.so).
 */

extern "C" {
#include "unity/unity.h"
}

#include <cstdlib>
#include <unistd.h>
#include <sys/sysinfo.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <cstring>

void setUp(void) {}

void tearDown(void) {
    unsetenv("VIRMUX_VIRTUAL_CORES");
    unsetenv("VIRMUX_SPOOFED_MEMINFO");
}

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
    const char *fake_meminfo = "/tmp/virmux_test_meminfo.txt";
    const char *content = "MemTotal:        2097152 kB\nMemFree:         1572864 kB\n";
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
    const char *test_path = "/tmp/virmux_test_creat.txt";
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

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_sysconf_returns_spoofed_core_count);
    RUN_TEST(test_interceptor_reentrancy_guard);
    RUN_TEST(test_get_nprocs_returns_spoofed_core_count);
    RUN_TEST(test_sysconf_passthrough_when_unset);
    RUN_TEST(test_sysconf_other_sc_names_passthrough);
    RUN_TEST(test_sysconf_invalid_core_env_falls_back);
    RUN_TEST(test_open_meminfo_redirection);
    RUN_TEST(test_open_meminfo_passthrough_when_unset);
    RUN_TEST(test_open_creates_file_with_mode);
    return UNITY_END();
}
