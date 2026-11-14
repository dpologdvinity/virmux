/**
 * @file test_cgroup.cpp
 * @brief Unit tests for RAII ScopedFd and cgroup v2 slice management.
 */

extern "C" {
#include "unity/unity.h"
}

#include "virmux/raii.hpp"
#include "virmux/cgroup.hpp"

#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

void setUp(void) {}
void tearDown(void) {}

void test_scoped_fd_closes_descriptor(void) {
    int pipe_fds[2];
    int res = pipe(pipe_fds);
    TEST_ASSERT_EQUAL_INT(0, res);
    {
        virmux::ScopedFd read_fd(pipe_fds[0]);
        TEST_ASSERT_TRUE(read_fd.valid());
    }
    // Attempting to close again should return EBADF
    TEST_ASSERT_EQUAL_INT(-1, close(pipe_fds[0]));
    close(pipe_fds[1]);
}

void test_scoped_fd_move_and_release(void) {
    int pipe_fds[2];
    int res = pipe(pipe_fds);
    TEST_ASSERT_EQUAL_INT(0, res);

    virmux::ScopedFd fd1(pipe_fds[0]);
    TEST_ASSERT_TRUE(fd1.valid());
    TEST_ASSERT_EQUAL_INT(pipe_fds[0], fd1.get());

    // Move construct
    virmux::ScopedFd fd2(std::move(fd1));
    TEST_ASSERT_FALSE(fd1.valid());
    TEST_ASSERT_EQUAL_INT(-1, fd1.get());
    TEST_ASSERT_TRUE(fd2.valid());
    TEST_ASSERT_EQUAL_INT(pipe_fds[0], fd2.get());

    // Move assign
    virmux::ScopedFd fd3;
    TEST_ASSERT_FALSE(fd3.valid());
    fd3 = std::move(fd2);
    TEST_ASSERT_FALSE(fd2.valid());
    TEST_ASSERT_TRUE(fd3.valid());
    TEST_ASSERT_EQUAL_INT(pipe_fds[0], fd3.get());

    // Release ownership
    int raw = fd3.release();
    TEST_ASSERT_FALSE(fd3.valid());
    TEST_ASSERT_EQUAL_INT(pipe_fds[0], raw);

    // Closing released fd must succeed
    TEST_ASSERT_EQUAL_INT(0, close(raw));
    close(pipe_fds[1]);
}

void test_scoped_fd_reset(void) {
    int pipe_fds[2];
    int res = pipe(pipe_fds);
    TEST_ASSERT_EQUAL_INT(0, res);

    virmux::ScopedFd fd(pipe_fds[0]);
    TEST_ASSERT_TRUE(fd.valid());

    // Self-reset with same fd should NOT close descriptor
    fd.reset(fd.get());
    TEST_ASSERT_TRUE(fd.valid());
    TEST_ASSERT_EQUAL_INT(pipe_fds[0], fd.get());

    // Reset with new fd should close pipe_fds[0]
    fd.reset(pipe_fds[1]);
    TEST_ASSERT_EQUAL_INT(-1, close(pipe_fds[0]));
    TEST_ASSERT_TRUE(fd.valid());
    TEST_ASSERT_EQUAL_INT(pipe_fds[1], fd.get());

    // Reset without args should close pipe_fds[1]
    fd.reset();
    TEST_ASSERT_FALSE(fd.valid());
    TEST_ASSERT_EQUAL_INT(-1, close(pipe_fds[1]));
}

void test_cgroup_unprivileged_fallback_logs_warning(void) {
    virmux::CgroupSlice slice("test-unprivileged");
    // Under unprivileged execution, init should not crash; it logs warning and sets valid=false
    bool res = slice.setup("/sys/fs/cgroup/nonexistent_test_slice", 1024 * 1024 * 500, "2-11");
    TEST_ASSERT_FALSE(res);
    TEST_ASSERT_FALSE(slice.is_valid());
    TEST_ASSERT_FALSE(slice.valid());
    TEST_ASSERT_FALSE(slice.attach_pid(getpid()));
    TEST_ASSERT_FALSE(slice.freeze());
    TEST_ASSERT_FALSE(slice.thaw());
}

void test_cgroup_mock_directory_operations(void) {
    char template_path[] = "/tmp/virmux_cgroup_test_XXXXXX";
    char *tmp_dir = mkdtemp(template_path);
    TEST_ASSERT_NOT_NULL(tmp_dir);

    virmux::CgroupSlice slice("session-mock");
    bool res = slice.setup(tmp_dir, 1024 * 1024 * 256, "0-1");
    TEST_ASSERT_TRUE(res);
    TEST_ASSERT_TRUE(slice.is_valid());

    std::string expected_slice_path = std::string(tmp_dir) + "/session-mock";
    TEST_ASSERT_EQUAL_STRING(expected_slice_path.c_str(), slice.path().c_str());

    // Verify memory.high content
    std::ifstream mem_file(expected_slice_path + "/memory.high");
    TEST_ASSERT_TRUE(mem_file.is_open());
    std::string mem_val;
    mem_file >> mem_val;
    TEST_ASSERT_EQUAL_STRING(std::to_string(1024 * 1024 * 256).c_str(), mem_val.c_str());
    mem_file.close();

    // Verify cpuset.cpus content
    std::ifstream cpu_file(expected_slice_path + "/cpuset.cpus");
    TEST_ASSERT_TRUE(cpu_file.is_open());
    std::string cpu_val;
    cpu_file >> cpu_val;
    TEST_ASSERT_EQUAL_STRING("0-1", cpu_val.c_str());
    cpu_file.close();

    // Test attach_pid
    TEST_ASSERT_TRUE(slice.attach_pid(getpid()));
    std::ifstream proc_file(expected_slice_path + "/cgroup.procs");
    TEST_ASSERT_TRUE(proc_file.is_open());
    std::string proc_val;
    proc_file >> proc_val;
    TEST_ASSERT_EQUAL_STRING(std::to_string(getpid()).c_str(), proc_val.c_str());
    proc_file.close();

    // Test freeze and thaw
    TEST_ASSERT_TRUE(slice.freeze());
    std::ifstream freeze_file(expected_slice_path + "/cgroup.freeze");
    TEST_ASSERT_TRUE(freeze_file.is_open());
    std::string freeze_val;
    freeze_file >> freeze_val;
    TEST_ASSERT_EQUAL_STRING("1", freeze_val.c_str());
    freeze_file.close();

    TEST_ASSERT_TRUE(slice.thaw());
    freeze_file.open(expected_slice_path + "/cgroup.freeze");
    TEST_ASSERT_TRUE(freeze_file.is_open());
    freeze_file >> freeze_val;
    TEST_ASSERT_EQUAL_STRING("0", freeze_val.c_str());
    freeze_file.close();

    // Cleanup mock files
    unlink((expected_slice_path + "/memory.high").c_str());
    unlink((expected_slice_path + "/cpuset.cpus").c_str());
    unlink((expected_slice_path + "/cgroup.procs").c_str());
    unlink((expected_slice_path + "/cgroup.freeze").c_str());
    rmdir(expected_slice_path.c_str());
    rmdir(tmp_dir);
}

void test_cgroup_move_semantics(void) {
    virmux::CgroupSlice slice1("slice-move");
    TEST_ASSERT_EQUAL_STRING("slice-move", slice1.name().c_str());

    virmux::CgroupSlice slice2(std::move(slice1));
    TEST_ASSERT_EQUAL_STRING("slice-move", slice2.name().c_str());
    TEST_ASSERT_FALSE(slice1.is_valid());

    virmux::CgroupSlice slice3;
    slice3 = std::move(slice2);
    TEST_ASSERT_EQUAL_STRING("slice-move", slice3.name().c_str());
    TEST_ASSERT_FALSE(slice2.is_valid());
}

void test_cgroup_uninitialized_slice_operations(void) {
    virmux::CgroupSlice uninit_slice;
    TEST_ASSERT_TRUE(uninit_slice.path().empty());
    TEST_ASSERT_FALSE(uninit_slice.is_valid());

    // Operations on uninitialized slice must safely return false
    TEST_ASSERT_FALSE(uninit_slice.set_memory_high(1024 * 1024 * 100));
    TEST_ASSERT_FALSE(uninit_slice.set_cpuset("0-1"));
    TEST_ASSERT_FALSE(uninit_slice.attach_pid(getpid()));
    TEST_ASSERT_FALSE(uninit_slice.freeze());
    TEST_ASSERT_FALSE(uninit_slice.thaw());
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_scoped_fd_closes_descriptor);
    RUN_TEST(test_scoped_fd_move_and_release);
    RUN_TEST(test_scoped_fd_reset);
    RUN_TEST(test_cgroup_unprivileged_fallback_logs_warning);
    RUN_TEST(test_cgroup_mock_directory_operations);
    RUN_TEST(test_cgroup_move_semantics);
    RUN_TEST(test_cgroup_uninitialized_slice_operations);
    return UNITY_END();
}
