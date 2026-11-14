/**
 * @file test_browser_trim.cpp
 * @brief Unit tests for headless browser detection and argument injection.
 */

extern "C" {
#include "unity/unity.h"
}

#include "virmux/browser_trim.hpp"

#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char **environ;

void setUp(void) {}
void tearDown(void) {}

void test_browser_binary_detection(void) {
    TEST_ASSERT_TRUE(virmux::is_browser_binary("/usr/bin/chromium"));
    TEST_ASSERT_TRUE(virmux::is_browser_binary("/home/user/.cache/ms-playwright/chromium-1155/chrome-linux/chrome"));
    TEST_ASSERT_FALSE(virmux::is_browser_binary("/usr/bin/gcc"));
    TEST_ASSERT_TRUE(virmux::is_browser_binary("chrome"));
    TEST_ASSERT_TRUE(virmux::is_browser_binary("chromium"));
    TEST_ASSERT_TRUE(virmux::is_browser_binary("/usr/local/bin/headless_shell"));
    TEST_ASSERT_FALSE(virmux::is_browser_binary(nullptr));
    TEST_ASSERT_FALSE(virmux::is_browser_binary(""));
}

void test_browser_flag_injection_preserves_arguments(void) {
    char arg0[] = "chrome";
    char arg1[] = "http://localhost:3000";
    char *argv[] = {arg0, arg1, nullptr};

    auto augmented = virmux::inject_slim_flags(argv);
    TEST_ASSERT_TRUE(augmented.size() > 2);
    TEST_ASSERT_EQUAL_STRING("chrome", augmented[0].c_str());
    TEST_ASSERT_EQUAL_STRING("--disable-gpu", augmented[1].c_str());
    TEST_ASSERT_EQUAL_STRING("--single-process", augmented[2].c_str());
    TEST_ASSERT_EQUAL_STRING("--disable-software-rasterizer", augmented[3].c_str());
    TEST_ASSERT_EQUAL_STRING("http://localhost:3000", augmented[4].c_str());
    TEST_ASSERT_EQUAL_INT(5, augmented.size());
}

void test_browser_flag_injection_null_or_empty(void) {
    auto null_res = virmux::inject_slim_flags(nullptr);
    TEST_ASSERT_TRUE(null_res.empty());

    char *empty_argv[] = {nullptr};
    auto empty_res = virmux::inject_slim_flags(empty_argv);
    TEST_ASSERT_TRUE(empty_res.empty());
}

void test_execve_intercepts_browser_and_injects_flags(void) {
    const char *script_path = "/tmp/virmux_test_chromium_mock.sh";
    const char *out_path = "/tmp/virmux_test_chromium_mock.out";
    unlink(script_path);
    unlink(out_path);

    int fd = open(script_path, O_CREAT | O_WRONLY | O_TRUNC, 0755);
    TEST_ASSERT_TRUE(fd >= 0);
    const char *script_content = "#!/bin/sh\nfor arg in \"$@\"; do echo \"$arg\"; done > /tmp/virmux_test_chromium_mock.out\n";
    ssize_t w = write(fd, script_content, strlen(script_content));
    TEST_ASSERT_EQUAL_INT(static_cast<ssize_t>(strlen(script_content)), w);
    close(fd);

    pid_t pid = fork();
    TEST_ASSERT_TRUE(pid >= 0);
    if (pid == 0) {
        char arg0[] = "/tmp/virmux_test_chromium_mock.sh";
        char arg1[] = "http://localhost:3000";
        char *child_argv[] = {arg0, arg1, nullptr};
        execve(script_path, child_argv, environ);
        _exit(127);
    }

    int status = 0;
    pid_t waited = waitpid(pid, &status, 0);
    TEST_ASSERT_EQUAL_INT(pid, waited);
    TEST_ASSERT_TRUE(WIFEXITED(status));
    TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));

    int out_fd = open(out_path, O_RDONLY);
    TEST_ASSERT_TRUE(out_fd >= 0);
    char buf[1024];
    memset(buf, 0, sizeof(buf));
    ssize_t r = read(out_fd, buf, sizeof(buf) - 1);
    TEST_ASSERT_TRUE(r > 0);
    close(out_fd);

    TEST_ASSERT_NOT_NULL(strstr(buf, "--disable-gpu"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "--single-process"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "--disable-software-rasterizer"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "http://localhost:3000"));

    unlink(script_path);
    unlink(out_path);
}

void test_execve_passthrough_for_non_browser(void) {
    const char *script_path = "/tmp/virmux_test_tool_mock.sh";
    const char *out_path = "/tmp/virmux_test_tool_mock.out";
    unlink(script_path);
    unlink(out_path);

    int fd = open(script_path, O_CREAT | O_WRONLY | O_TRUNC, 0755);
    TEST_ASSERT_TRUE(fd >= 0);
    const char *script_content = "#!/bin/sh\nfor arg in \"$@\"; do echo \"$arg\"; done > /tmp/virmux_test_tool_mock.out\n";
    ssize_t w = write(fd, script_content, strlen(script_content));
    TEST_ASSERT_EQUAL_INT(static_cast<ssize_t>(strlen(script_content)), w);
    close(fd);

    pid_t pid = fork();
    TEST_ASSERT_TRUE(pid >= 0);
    if (pid == 0) {
        char arg0[] = "/tmp/virmux_test_tool_mock.sh";
        char arg1[] = "--verbose";
        char *child_argv[] = {arg0, arg1, nullptr};
        execve(script_path, child_argv, environ);
        _exit(127);
    }

    int status = 0;
    pid_t waited = waitpid(pid, &status, 0);
    TEST_ASSERT_EQUAL_INT(pid, waited);
    TEST_ASSERT_TRUE(WIFEXITED(status));
    TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));

    int out_fd = open(out_path, O_RDONLY);
    TEST_ASSERT_TRUE(out_fd >= 0);
    char buf[1024];
    memset(buf, 0, sizeof(buf));
    ssize_t r = read(out_fd, buf, sizeof(buf) - 1);
    TEST_ASSERT_TRUE(r > 0);
    close(out_fd);

    TEST_ASSERT_NULL(strstr(buf, "--disable-gpu"));
    TEST_ASSERT_NULL(strstr(buf, "--single-process"));
    TEST_ASSERT_NULL(strstr(buf, "--disable-software-rasterizer"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "--verbose"));

    unlink(script_path);
    unlink(out_path);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_browser_binary_detection);
    RUN_TEST(test_browser_flag_injection_preserves_arguments);
    RUN_TEST(test_browser_flag_injection_null_or_empty);
    RUN_TEST(test_execve_intercepts_browser_and_injects_flags);
    RUN_TEST(test_execve_passthrough_for_non_browser);
    return UNITY_END();
}
