/**
 * @file test_cgroup.cpp
 * @brief Unit tests for RAII ScopedFd and cgroup v2 slice management.
 */

extern "C" {
#include "unity/unity.h"
}

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "unit/fake_cgroupfs.hpp"
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
  if (geteuid() == 0) {
    TEST_IGNORE_MESSAGE("root bypasses the read-only base directory");
  }
  // A base directory we cannot write to stands in for a cgroup tree we do
  // not own. (/sys/fs/cgroup itself may be writable, e.g. in CI.)
  char tmpl[] = "/tmp/virmux_cgroup_ro_XXXXXX";
  const char* base = mkdtemp(tmpl);
  TEST_ASSERT_NOT_NULL(base);
  TEST_ASSERT_EQUAL_INT(0, chmod(base, 0555));

  virmux::CgroupSlice slice("test-unprivileged");
  bool res = slice.setup(std::string(base) + "/nonexistent_test_slice",
                         1024ULL * 1024 * 500, "2-11");
  chmod(base, 0755);
  std::filesystem::remove_all(base);
  TEST_ASSERT_FALSE(res);
  TEST_ASSERT_EQUAL_INT(EACCES, slice.last_error());
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

// ---------------------------------------------------------------------------
// Delegation discovery
// ---------------------------------------------------------------------------

namespace {

constexpr const char* kUserMgr = "user.slice/user-1000.slice/user@1000.service";
constexpr const char* kTmuxScope =
    "user.slice/user-1000.slice/user@1000.service/app.slice/tmux-1.scope";

/** @brief Typical desktop layout: a root-owned tree with the user manager's
 * subtree delegated to uid 1000. */
void add_desktop_tree(const virmux::testing::FakeCgroupFs& fs,
                      bool user_mgr_writable, bool everything_writable) {
  const bool mgr = user_mgr_writable || everything_writable;
  fs.add("", everything_writable);
  fs.add("user.slice", everything_writable);
  fs.add("user.slice/user-1000.slice", everything_writable);
  fs.add("user.slice/user-1000.slice/session-3.scope", everything_writable);
  fs.add(kUserMgr, mgr);
  fs.add(std::string(kUserMgr) + "/app.slice", mgr);
  fs.add(kTmuxScope, mgr);
}

virmux::CgroupProbe probe_for(const virmux::testing::FakeCgroupFs& fs,
                              const std::string& rel, uid_t uid) {
  return virmux::CgroupProbe{
      .proc_self_cgroup = "0::/" + rel + "\n",
      .root = fs.root(),
      .uid = uid,
  };
}

}  // namespace

void test_delegation_uses_user_manager_tree(void) {
  virmux::testing::FakeCgroupFs fs;
  add_desktop_tree(fs, true, false);
  const auto d =
      virmux::find_cgroup_delegation(probe_for(fs, kTmuxScope, 1000));
  TEST_ASSERT_TRUE(d.usable());
  TEST_ASSERT_TRUE(d.kind == virmux::DelegationKind::kUserManager);
  TEST_ASSERT_EQUAL_STRING(fs.path(kUserMgr).c_str(), d.root.c_str());
  TEST_ASSERT_EQUAL_STRING(fs.path(kTmuxScope).c_str(), d.self.c_str());
}

void test_delegation_ssh_session_scope_falls_back(void) {
  if (geteuid() == 0) {
    TEST_IGNORE_MESSAGE("root bypasses the read-only modes this test needs");
  }
  virmux::testing::FakeCgroupFs fs;
  add_desktop_tree(fs, true, false);
  const auto d = virmux::find_cgroup_delegation(
      probe_for(fs, "user.slice/user-1000.slice/session-3.scope", 1000));
  TEST_ASSERT_FALSE(d.usable());
  TEST_ASSERT_NOT_NULL(std::strstr(d.reason.c_str(), "session-3.scope"));
  TEST_ASSERT_NOT_NULL(
      std::strstr(d.reason.c_str(), "systemd-run --user --scope"));
}

void test_delegation_cgroup_v1_falls_back(void) {
  virmux::testing::FakeCgroupFs fs(false);
  const virmux::CgroupProbe probe{
      .proc_self_cgroup = "12:memory:/user.slice\n1:name=systemd:/user.slice\n",
      .root = fs.root(),
      .uid = 1000,
  };
  const auto d = virmux::find_cgroup_delegation(probe);
  TEST_ASSERT_FALSE(d.usable());
  TEST_ASSERT_NOT_NULL(std::strstr(d.reason.c_str(), "v2"));

  // Hybrid mode: a 0:: line exists but the mount is not the unified root.
  const auto hybrid =
      virmux::find_cgroup_delegation(probe_for(fs, "user.slice", 1000));
  TEST_ASSERT_FALSE(hybrid.usable());
  TEST_ASSERT_NOT_NULL(std::strstr(hybrid.reason.c_str(), "v2"));
}

void test_delegation_other_users_tree_falls_back(void) {
  if (geteuid() == 0) {
    TEST_IGNORE_MESSAGE("root bypasses the read-only modes this test needs");
  }
  virmux::testing::FakeCgroupFs fs;
  add_desktop_tree(fs, false, false);
  const auto d =
      virmux::find_cgroup_delegation(probe_for(fs, kTmuxScope, 1000));
  TEST_ASSERT_FALSE(d.usable());

  // A different uid never matches the user@<uid>.service name either.
  virmux::testing::FakeCgroupFs fs2;
  add_desktop_tree(fs2, true, false);
  const auto d2 =
      virmux::find_cgroup_delegation(probe_for(fs2, kTmuxScope, 1001));
  TEST_ASSERT_TRUE(d2.kind != virmux::DelegationKind::kUserManager);
}

void test_delegation_fully_writable_tree_prefers_user_manager(void) {
  virmux::testing::FakeCgroupFs fs;
  add_desktop_tree(fs, true, true);  // CI's `chmod -R a+rw /sys/fs/cgroup`
  const auto d =
      virmux::find_cgroup_delegation(probe_for(fs, kTmuxScope, 1000));
  TEST_ASSERT_TRUE(d.kind == virmux::DelegationKind::kUserManager);
  TEST_ASSERT_EQUAL_STRING(fs.path(kUserMgr).c_str(), d.root.c_str());
}

void test_delegation_root_without_user_manager_uses_cgroup_root(void) {
  virmux::testing::FakeCgroupFs fs;
  fs.add("", true);
  fs.add("system.slice", true);
  fs.add("system.slice/ci-runner.service", true);
  const auto d = virmux::find_cgroup_delegation(
      probe_for(fs, "system.slice/ci-runner.service", 0));
  TEST_ASSERT_TRUE(d.kind == virmux::DelegationKind::kWritableAncestor);
  TEST_ASSERT_EQUAL_STRING(fs.root().c_str(), d.root.c_str());
}

void test_delegation_at_cgroup_root_gives_no_user_manager_advice(void) {
  if (geteuid() == 0) {
    TEST_IGNORE_MESSAGE("root bypasses the read-only modes this test needs");
  }
  // Containers and WSL without systemd: we sit at the (read-only) root.
  virmux::testing::FakeCgroupFs fs;
  fs.add("", false);
  const auto d = virmux::find_cgroup_delegation(probe_for(fs, "", 1000));
  TEST_ASSERT_FALSE(d.usable());
  TEST_ASSERT_NULL(std::strstr(d.reason.c_str(), "systemd-run"));
  TEST_ASSERT_NOT_NULL(std::strstr(d.reason.c_str(), "cgroup root"));
}

void test_create_run_reports_mkdir_errno(void) {
  const virmux::CgroupDelegation d{
      .kind = virmux::DelegationKind::kUserManager,
      .root = "/nonexistent/virmux-test-delegation",
      .self = "",
      .reason = "",
  };
  virmux::CgroupSlice slice = virmux::create_run(d, "run-1-0");
  TEST_ASSERT_FALSE(slice.is_valid());
  TEST_ASSERT_EQUAL_INT(ENOENT, slice.last_error());

  // spawn_command assigns into an existing slice: moves keep the errno.
  virmux::CgroupSlice assigned;
  assigned = virmux::create_run(d, "run-1-1");
  TEST_ASSERT_EQUAL_INT(ENOENT, assigned.last_error());
  virmux::CgroupSlice constructed(std::move(assigned));
  TEST_ASSERT_EQUAL_INT(ENOENT, constructed.last_error());
}

void test_describe_limit_failures_optional_files(void) {
  auto fail = [](const char* file, int err) {
    return virmux::LimitResult{
        .file = file, .value = "x", .applied = false, .error = err};
  };
  const virmux::LimitResult ok{
      .file = "memory.max", .value = "1", .applied = true, .error = 0};

  // Not delegated (ENOENT) and covered by other means: silent.
  TEST_ASSERT_EQUAL_STRING(
      "", virmux::describe_limit_failures(
              {ok, fail("cpuset.cpus", ENOENT), fail("memory.swap.max", ENOENT),
               fail("cpu.max", ENOENT)},
              true)
              .c_str());
  // Without pinning nothing else caps CPU use, so a missing cpu.max matters.
  const std::string unpinned =
      virmux::describe_limit_failures({fail("cpu.max", ENOENT)}, false);
  TEST_ASSERT_NOT_NULL(std::strstr(unpinned.c_str(), "cpu.max"));
  // Real errors are always reported.
  const std::string denied =
      virmux::describe_limit_failures({fail("memory.max", EACCES)}, true);
  TEST_ASSERT_NOT_NULL(std::strstr(denied.c_str(), "memory.max"));
  TEST_ASSERT_NOT_NULL(std::strstr(denied.c_str(), std::strerror(EACCES)));
}

void test_process_in_cgroup(void) {
  const virmux::CgroupProbe probe = virmux::CgroupProbe::current();
  const auto d = virmux::find_cgroup_delegation(probe);
  if (d.self.empty()) {
    TEST_IGNORE_MESSAGE("no cgroup v2 here");
  }
  const auto in_own = virmux::process_in_cgroup(getpid(), d.self, probe.root);
  TEST_ASSERT_TRUE(in_own.has_value() && *in_own);
  const auto in_other = virmux::process_in_cgroup(
      getpid(), probe.root + "/virmux/run-nope", probe.root);
  TEST_ASSERT_TRUE(in_other.has_value() && !*in_other);
  // An unreadable /proc/<pid>/cgroup (no such process) is unknown, not "no".
  TEST_ASSERT_FALSE(
      virmux::process_in_cgroup(0x3fffffff, d.self, probe.root).has_value());
}

void test_delegation_missing_proc_self_cgroup_falls_back(void) {
  virmux::testing::FakeCgroupFs fs;
  const virmux::CgroupProbe probe{
      .proc_self_cgroup = "", .root = fs.root(), .uid = 1000};
  TEST_ASSERT_FALSE(virmux::find_cgroup_delegation(probe).usable());
}

// ---------------------------------------------------------------------------
// Run cgroups
// ---------------------------------------------------------------------------

void test_controllers_to_enable_skips_enabled_and_unknown(void) {
  const auto todo = virmux::controllers_to_enable(
      "cpuset cpu io memory pids hugetlb\n", "cpu\n");
  const std::vector<std::string> expected{"cpuset", "memory", "pids"};
  TEST_ASSERT_TRUE(expected == todo);
  TEST_ASSERT_TRUE(virmux::controllers_to_enable("", "").empty());
}

void test_destructor_removes_created_cgroup_with_rmdir(void) {
  virmux::testing::FakeCgroupFs fs;
  fs.add("deleg", true);
  fs.add("deleg/virmux", true);
  const virmux::CgroupDelegation d{
      .kind = virmux::DelegationKind::kUserManager,
      .root = fs.path("deleg"),
      .self = "",
      .reason = "",
  };
  const std::string run_dir = fs.path("deleg/virmux/run-1-0");
  {
    virmux::CgroupSlice slice = virmux::create_run(d, "run-1-0");
    // A plain directory has no kernel-populated cgroup.procs: not valid.
    TEST_ASSERT_FALSE(slice.is_valid());
    TEST_ASSERT_TRUE(std::filesystem::is_directory(run_dir));
  }
  TEST_ASSERT_FALSE(std::filesystem::exists(run_dir));
  // Never removes the shared virmux dir or the delegation root.
  TEST_ASSERT_TRUE(std::filesystem::is_directory(fs.path("deleg/virmux")));
}

void test_create_run_without_delegation_is_invalid(void) {
  virmux::CgroupSlice slice =
      virmux::create_run(virmux::CgroupDelegation{}, "run-1-0");
  TEST_ASSERT_FALSE(slice.is_valid());
}

void test_sweep_removes_only_dead_runs(void) {
  char tmpl[] = "/tmp/virmux_sweep_XXXXXX";
  const char* base = mkdtemp(tmpl);
  TEST_ASSERT_NOT_NULL(base);

  pid_t dead = fork();
  TEST_ASSERT_TRUE(dead >= 0);
  if (dead == 0) {
    _exit(0);
  }
  TEST_ASSERT_EQUAL_INT(dead, waitpid(dead, nullptr, 0));

  const std::string b(base);
  const std::string dead_run = b + "/run-" + std::to_string(dead) + "-0";
  const std::string live_run = b + "/run-" + std::to_string(getpid()) + "-3";
  const std::string busy_dead = b + "/run-" + std::to_string(dead) + "-1";
  std::filesystem::create_directories(dead_run);
  std::filesystem::create_directories(live_run);
  std::filesystem::create_directories(b + "/run-garbage");
  std::filesystem::create_directories(b + "/other");
  // Stands in for a cgroup whose processes outlived virmux (rmdir -> EBUSY).
  std::filesystem::create_directories(busy_dead + "/child");

  TEST_ASSERT_EQUAL_size_t(1, virmux::sweep_stale_runs(b));
  TEST_ASSERT_FALSE(std::filesystem::exists(dead_run));
  TEST_ASSERT_TRUE(std::filesystem::exists(live_run));
  TEST_ASSERT_TRUE(std::filesystem::exists(busy_dead));
  TEST_ASSERT_TRUE(std::filesystem::exists(b + "/run-garbage"));
  TEST_ASSERT_TRUE(std::filesystem::exists(b + "/other"));
  TEST_ASSERT_EQUAL_size_t(0, virmux::sweep_stale_runs(b + "/missing"));

  std::filesystem::remove_all(b);
}

void test_create_run_on_real_delegation(void) {
  const auto d = virmux::find_cgroup_delegation(virmux::CgroupProbe::current());
  if (!d.usable() || d.root.rfind("/sys/fs/cgroup", 0) != 0) {
    TEST_IGNORE_MESSAGE("no delegated cgroup subtree on this machine");
  }
  const std::string name = "run-" + std::to_string(getpid()) + "-test";
  std::string path;
  {
    virmux::CgroupSlice slice = virmux::create_run(d, name);
    TEST_ASSERT_TRUE(slice.is_valid());
    path = slice.path();
    TEST_ASSERT_EQUAL_STRING((d.root + "/virmux/" + name).c_str(),
                             path.c_str());
    // Every controller the delegation root hands down is enabled for runs.
    std::ifstream offered_file(d.root + "/cgroup.subtree_control");
    std::ifstream have_file(path + "/cgroup.controllers");
    std::string have((std::istreambuf_iterator<char>(have_file)), {});
    have = " " + have;
    std::string ctrl;
    while (offered_file >> ctrl) {
      if (ctrl == "cpu" || ctrl == "memory" || ctrl == "pids" ||
          ctrl == "cpuset") {
        TEST_ASSERT_NOT_NULL_MESSAGE(
            std::strstr(have.c_str(), (" " + ctrl).c_str()), ctrl.c_str());
      }
    }
  }
  TEST_ASSERT_FALSE(std::filesystem::exists(path));
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
  RUN_TEST(test_delegation_uses_user_manager_tree);
  RUN_TEST(test_delegation_ssh_session_scope_falls_back);
  RUN_TEST(test_delegation_cgroup_v1_falls_back);
  RUN_TEST(test_delegation_other_users_tree_falls_back);
  RUN_TEST(test_delegation_fully_writable_tree_prefers_user_manager);
  RUN_TEST(test_delegation_root_without_user_manager_uses_cgroup_root);
  RUN_TEST(test_delegation_missing_proc_self_cgroup_falls_back);
  RUN_TEST(test_delegation_at_cgroup_root_gives_no_user_manager_advice);
  RUN_TEST(test_create_run_reports_mkdir_errno);
  RUN_TEST(test_describe_limit_failures_optional_files);
  RUN_TEST(test_process_in_cgroup);
  RUN_TEST(test_controllers_to_enable_skips_enabled_and_unknown);
  RUN_TEST(test_destructor_removes_created_cgroup_with_rmdir);
  RUN_TEST(test_create_run_without_delegation_is_invalid);
  RUN_TEST(test_sweep_removes_only_dead_runs);
  RUN_TEST(test_create_run_on_real_delegation);
  RUN_TEST(test_cgroup_move_semantics);
  RUN_TEST(test_cgroup_uninitialized_slice_operations);
  return UNITY_END();
}
