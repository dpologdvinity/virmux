/**
 * @file test_fault_isolation.cpp
 * @brief Unit tests for OOM score adjustment and fork-bomb / pids limit
 * protection.
 */

extern "C" {
#include "unity/unity.h"
}

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <string>

#include "virmux/cgroup.hpp"
#include "virmux/sandbox.hpp"

void setUp(void) {}
void tearDown(void) {}

void test_oom_score_positive_write_and_verify(void) {
  // Test writing positive score (+800) in child process to avoid altering test
  // runner process
  pid_t pid = fork();
  TEST_ASSERT_TRUE(pid >= 0);
  if (pid == 0) {
    bool ok = virmux::set_process_oom_score(0, 800);
    if (!ok) {
      _exit(1);
    }
    std::ifstream f("/proc/self/oom_score_adj");
    if (!f.is_open()) {
      _exit(2);
    }
    int read_val = -1;
    f >> read_val;
    f.close();
    if (read_val != 800) {
      _exit(3);
    }
    _exit(0);
  }

  int status = 0;
  pid_t w = waitpid(pid, &status, 0);
  TEST_ASSERT_EQUAL_INT(pid, w);
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));
}

void test_oom_score_negative_unprivileged_graceful(void) {
  // Writing negative score (-900) should handle EPERM/EACCES gracefully without
  // throwing or crashing
  bool res = virmux::set_process_oom_score(0, -900);
  if (geteuid() != 0) {
    TEST_ASSERT_FALSE(res);
  }
}

void test_oom_score_invalid_pid_graceful(void) {
  // Testing invalid/nonexistent PID gracefully returns false
  bool res = virmux::set_process_oom_score(99999999, 500);
  TEST_ASSERT_FALSE(res);
}

void test_oom_score_clamping(void) {
  pid_t pid = fork();
  TEST_ASSERT_TRUE(pid >= 0);
  if (pid == 0) {
    // 1500 should be clamped to 1000
    bool ok = virmux::set_process_oom_score(0, 1500);
    if (!ok) {
      _exit(1);
    }
    std::ifstream f("/proc/self/oom_score_adj");
    if (!f.is_open()) {
      _exit(2);
    }
    int read_val = -1;
    f >> read_val;
    f.close();
    if (read_val != 1000) {
      _exit(3);
    }
    _exit(0);
  }

  int status = 0;
  pid_t w = waitpid(pid, &status, 0);
  TEST_ASSERT_EQUAL_INT(pid, w);
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));
}

void test_cgroup_set_pids_max_mock(void) {
  char template_path[] = "/tmp/virmux_cgroup_pids_XXXXXX";
  char* tmp_dir = mkdtemp(template_path);
  TEST_ASSERT_NOT_NULL(tmp_dir);

  // Pre-create the files the kernel would populate in a real cgroup.
  const std::string mock_dir = std::string(tmp_dir) + "/pids-mock";
  mkdir(mock_dir.c_str(), 0755);
  std::ofstream(mock_dir + "/cgroup.procs").close();
  std::ofstream(mock_dir + "/pids.max").close();

  virmux::CgroupSlice slice("pids-mock");
  bool res = slice.setup(tmp_dir, 0, "");
  TEST_ASSERT_TRUE(res);
  TEST_ASSERT_TRUE(slice.is_valid());

  std::string slice_path = std::string(tmp_dir) + "/pids-mock";

  // Test set_pids_max with specific numeric ceiling
  TEST_ASSERT_TRUE(slice.set_pids_max(256));
  std::ifstream pids_file(slice_path + "/pids.max");
  TEST_ASSERT_TRUE(pids_file.is_open());
  std::string val;
  pids_file >> val;
  pids_file.close();
  TEST_ASSERT_EQUAL_STRING("256", val.c_str());

  // Test set_pids_max with 0 ("max")
  TEST_ASSERT_TRUE(slice.set_pids_max(0));
  pids_file.open(slice_path + "/pids.max");
  TEST_ASSERT_TRUE(pids_file.is_open());
  pids_file >> val;
  pids_file.close();
  TEST_ASSERT_EQUAL_STRING("max", val.c_str());

  // Cleanup
  unlink((slice_path + "/pids.max").c_str());
  unlink((slice_path + "/cgroup.procs").c_str());
  rmdir(slice_path.c_str());
  rmdir(tmp_dir);
}

void test_cgroup_uninitialized_set_pids_max(void) {
  virmux::CgroupSlice uninit_slice;
  TEST_ASSERT_FALSE(uninit_slice.set_pids_max(128));
}

void test_sandbox_child_execution_with_max_pids(void) {
  virmux::SandboxConfig cfg{
      .virtual_cores = 1,
      .max_pids = 512,
  };
  char arg0[] = "/bin/sh";
  char arg1[] = "-c";
  char arg2[] = "exit 0";
  char* argv[] = {arg0, arg1, arg2, nullptr};

  int exit_code = virmux::spawn_command(cfg, argv);
  TEST_ASSERT_EQUAL_INT(0, exit_code);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_oom_score_positive_write_and_verify);
  RUN_TEST(test_oom_score_negative_unprivileged_graceful);
  RUN_TEST(test_oom_score_invalid_pid_graceful);
  RUN_TEST(test_oom_score_clamping);
  RUN_TEST(test_cgroup_set_pids_max_mock);
  RUN_TEST(test_cgroup_uninitialized_set_pids_max);
  RUN_TEST(test_sandbox_child_execution_with_max_pids);
  return UNITY_END();
}
