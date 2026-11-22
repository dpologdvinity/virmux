/**
 * @file test_cgroup.cpp
 * @brief Unit tests for RAII ScopedFd and cgroup v2 slice management.
 */

extern "C" {
#include "unity/unity.h"
}

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>

#include "virmux/cgroup.hpp"
#include "virmux/raii.hpp"

void setUp(void) {}
void tearDown(void) {}

namespace {

/**
 * @brief Creates @p dir and an empty file for each control file in @p files,
 * mimicking the files the kernel populates in a fresh cgroup directory.
 */
void make_fake_cgroup(const std::string& dir,
                      std::initializer_list<const char*> files) {
  std::filesystem::create_directories(dir);
  for (const char* f : files) {
    std::ofstream(dir + "/" + f).close();
  }
}

/** @brief Reads the first whitespace-delimited token of @p path. */
std::string read_token(const std::string& path) {
  std::ifstream in(path);
  std::string val;
  in >> val;
  return val;
}

}  // namespace

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
  // Asserting the documented moved-from state is the point here.
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  TEST_ASSERT_FALSE(fd1.valid());
  TEST_ASSERT_EQUAL_INT(-1, fd1.get());
  TEST_ASSERT_TRUE(fd2.valid());
  TEST_ASSERT_EQUAL_INT(pipe_fds[0], fd2.get());

  // Move assign
  virmux::ScopedFd fd3;
  TEST_ASSERT_FALSE(fd3.valid());
  fd3 = std::move(fd2);
  // Asserting the documented moved-from state is the point here.
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
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
  // Under unprivileged execution, init should not crash; it logs warning and
  // sets valid=false
  bool res = slice.setup("/sys/fs/cgroup/nonexistent_test_slice",
                         1024ULL * 1024 * 500, "2-11");
  TEST_ASSERT_FALSE(res);
  TEST_ASSERT_FALSE(slice.is_valid());
  TEST_ASSERT_FALSE(slice.valid());
  TEST_ASSERT_FALSE(slice.attach_pid(getpid()));
  TEST_ASSERT_FALSE(slice.freeze());
  TEST_ASSERT_FALSE(slice.thaw());
}

void test_cgroup_mock_directory_operations(void) {
  char template_path[] = "/tmp/virmux_cgroup_test_XXXXXX";
  char* tmp_dir = mkdtemp(template_path);
  TEST_ASSERT_NOT_NULL(tmp_dir);

  std::string expected_slice_path = std::string(tmp_dir) + "/session-mock";
  make_fake_cgroup(expected_slice_path, {"cgroup.procs", "cgroup.freeze",
                                         "memory.high", "cpuset.cpus"});

  virmux::CgroupSlice slice("session-mock");
  bool res = slice.setup(tmp_dir, 1024ULL * 1024 * 256, "0-1");
  TEST_ASSERT_TRUE(res);
  TEST_ASSERT_TRUE(slice.is_valid());
  TEST_ASSERT_EQUAL_STRING(expected_slice_path.c_str(), slice.path().c_str());

  TEST_ASSERT_EQUAL_STRING(
      std::to_string(1024 * 1024 * 256).c_str(),
      read_token(expected_slice_path + "/memory.high").c_str());
  TEST_ASSERT_EQUAL_STRING(
      "0-1", read_token(expected_slice_path + "/cpuset.cpus").c_str());

  TEST_ASSERT_TRUE(slice.attach_pid(getpid()));
  TEST_ASSERT_EQUAL_STRING(
      std::to_string(getpid()).c_str(),
      read_token(expected_slice_path + "/cgroup.procs").c_str());

  TEST_ASSERT_TRUE(slice.freeze());
  TEST_ASSERT_EQUAL_STRING(
      "1", read_token(expected_slice_path + "/cgroup.freeze").c_str());
  TEST_ASSERT_TRUE(slice.thaw());
  TEST_ASSERT_EQUAL_STRING(
      "0", read_token(expected_slice_path + "/cgroup.freeze").c_str());

  std::filesystem::remove_all(tmp_dir);
}

void test_cgroup_write_never_creates_missing_control_files(void) {
  char template_path[] = "/tmp/virmux_cgroup_nocreat_XXXXXX";
  char* tmp_dir = mkdtemp(template_path);
  TEST_ASSERT_NOT_NULL(tmp_dir);
  const std::string dir = std::string(tmp_dir) + "/run";
  make_fake_cgroup(dir, {"cgroup.procs"});

  virmux::CgroupSlice slice("run");
  TEST_ASSERT_TRUE(slice.setup(tmp_dir));
  // A controller that is not enabled has no control file; writing it must
  // fail instead of silently creating a regular file.
  TEST_ASSERT_FALSE(slice.set_memory_high(1024));
  TEST_ASSERT_FALSE(std::filesystem::exists(dir + "/memory.high"));
  TEST_ASSERT_FALSE(slice.set_pids_max(10));
  TEST_ASSERT_FALSE(std::filesystem::exists(dir + "/pids.max"));

  std::filesystem::remove_all(tmp_dir);
}

void test_cgroup_valid_requires_writable_procs(void) {
  char template_path[] = "/tmp/virmux_cgroup_noprocs_XXXXXX";
  char* tmp_dir = mkdtemp(template_path);
  TEST_ASSERT_NOT_NULL(tmp_dir);
  // A plain directory is not a cgroup: there is no cgroup.procs to join.
  virmux::CgroupSlice slice("not-a-cgroup");
  TEST_ASSERT_FALSE(slice.setup(tmp_dir));
  TEST_ASSERT_FALSE(slice.is_valid());

  std::filesystem::remove_all(tmp_dir);
}

void test_cgroup_setup_limits_are_independent(void) {
  char template_path[] = "/tmp/virmux_cgroup_indep_XXXXXX";
  char* tmp_dir = mkdtemp(template_path);
  TEST_ASSERT_NOT_NULL(tmp_dir);
  const std::string dir = std::string(tmp_dir) + "/run";
  // cpuset is not delegated (no cpuset.cpus): memory must still apply.
  make_fake_cgroup(dir, {"cgroup.procs", "memory.high"});

  virmux::CgroupSlice slice("run");
  TEST_ASSERT_TRUE(slice.setup(tmp_dir, 4096, "0-1"));
  TEST_ASSERT_TRUE(slice.is_valid());
  TEST_ASSERT_EQUAL_STRING("4096", read_token(dir + "/memory.high").c_str());

  std::filesystem::remove_all(tmp_dir);
}

void test_cgroup_apply_limits_reports_each_limit(void) {
  char template_path[] = "/tmp/virmux_cgroup_apply_XXXXXX";
  char* tmp_dir = mkdtemp(template_path);
  TEST_ASSERT_NOT_NULL(tmp_dir);
  const std::string dir = std::string(tmp_dir) + "/run";
  make_fake_cgroup(dir, {"cgroup.procs", "memory.max", "memory.high",
                         "memory.swap.max", "cpu.max", "pids.max"});

  virmux::CgroupSlice slice("run");
  TEST_ASSERT_TRUE(slice.setup(tmp_dir));

  virmux::CgroupLimits limits;
  limits.memory_max = 64ULL << 20;
  limits.memory_high = 60ULL << 20;
  limits.swap_max = 16ULL << 20;
  limits.cpu_max = "200000 100000";
  limits.cpuset_cpus = "2-3";
  limits.pids_max = 256;
  const auto results = slice.apply_limits(limits);

  TEST_ASSERT_EQUAL_size_t(6, results.size());
  for (const auto& r : results) {
    if (r.file == "cpuset.cpus") {
      TEST_ASSERT_FALSE(r.applied);
      TEST_ASSERT_EQUAL_INT(ENOENT, r.error);
    } else {
      TEST_ASSERT_TRUE_MESSAGE(r.applied, r.file.c_str());
    }
  }
  TEST_ASSERT_EQUAL_STRING("67108864", read_token(dir + "/memory.max").c_str());
  TEST_ASSERT_EQUAL_STRING("62914560",
                           read_token(dir + "/memory.high").c_str());
  TEST_ASSERT_EQUAL_STRING("16777216",
                           read_token(dir + "/memory.swap.max").c_str());
  TEST_ASSERT_EQUAL_STRING("200000", read_token(dir + "/cpu.max").c_str());
  TEST_ASSERT_EQUAL_STRING("256", read_token(dir + "/pids.max").c_str());
  TEST_ASSERT_FALSE(std::filesystem::exists(dir + "/cpuset.cpus"));

  // Unset limits are not attempted at all.
  const auto none = slice.apply_limits(virmux::CgroupLimits{});
  TEST_ASSERT_EQUAL_size_t(0, none.size());

  std::filesystem::remove_all(tmp_dir);
}

void test_cgroup_move_semantics(void) {
  virmux::CgroupSlice slice1("slice-move");
  TEST_ASSERT_EQUAL_STRING("slice-move", slice1.name().c_str());

  virmux::CgroupSlice slice2(std::move(slice1));
  TEST_ASSERT_EQUAL_STRING("slice-move", slice2.name().c_str());
  // Asserting the documented moved-from state is the point here.
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  TEST_ASSERT_FALSE(slice1.is_valid());

  virmux::CgroupSlice slice3;
  slice3 = std::move(slice2);
  TEST_ASSERT_EQUAL_STRING("slice-move", slice3.name().c_str());
  // Asserting the documented moved-from state is the point here.
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  TEST_ASSERT_FALSE(slice2.is_valid());
}

void test_cgroup_uninitialized_slice_operations(void) {
  virmux::CgroupSlice uninit_slice;
  TEST_ASSERT_TRUE(uninit_slice.path().empty());
  TEST_ASSERT_FALSE(uninit_slice.is_valid());

  // Operations on uninitialized slice must safely return false
  TEST_ASSERT_FALSE(uninit_slice.set_memory_high(1024ULL * 1024 * 100));
  TEST_ASSERT_FALSE(uninit_slice.set_cpuset("0-1"));
  TEST_ASSERT_FALSE(uninit_slice.set_pids_max(128));
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
  RUN_TEST(test_cgroup_write_never_creates_missing_control_files);
  RUN_TEST(test_cgroup_valid_requires_writable_procs);
  RUN_TEST(test_cgroup_setup_limits_are_independent);
  RUN_TEST(test_cgroup_apply_limits_reports_each_limit);
  RUN_TEST(test_cgroup_move_semantics);
  RUN_TEST(test_cgroup_uninitialized_slice_operations);
  return UNITY_END();
}
