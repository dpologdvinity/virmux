/**
 * @file test_idle_autofreeze.cpp
 * @brief Unit tests for CPU tick delta tracking, inactivity auto-freeze, and
 * compaction engine.
 */

extern "C" {
#include "unity/unity.h"
}

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "virmux/daemon.hpp"
#include "virmux/session.hpp"

void setUp(void) {}
void tearDown(void) {}

/**
 * @brief Test 1: CPU ticks reading from procfs stat (field 14 utime + field 15
 * stime).
 */
void test_stat_cpu_ticks_parsing(void) {
  // 1. Synthetic procfs test
  std::string tmp_dir = "/tmp/virmux_stat_test_" + std::to_string(::getpid());
  std::filesystem::create_directories(tmp_dir + "/1234");

  std::ofstream stat_out(tmp_dir + "/1234/stat");
  stat_out << "1234 (testproc) S 1 1234 1234 0 -1 4194304 100 0 0 0 35 65 0 0 "
              "20 0 1 0 1000 10000 100 18446744073709551615 0 0 0 0 0 0 0 0 0 "
              "0 0 0 0 0\n";
  stat_out.close();

  virmux::SessionRegistry reg(tmp_dir);
  virmux::SessionRegistration req;
  req.name = "synthetic-proc";
  req.pid = 1234;
  req.command = "dummy-proc";

  uint64_t id = reg.register_session(req);
  TEST_ASSERT_EQUAL_UINT64(1, id);

  reg.refresh_telemetry();
  auto s = reg.get_session(id);
  TEST_ASSERT_TRUE(s.has_value());
  // 35 (utime) + 65 (stime) = 100 ticks
  TEST_ASSERT_EQUAL_UINT64(100, s->last_cpu_ticks);

  // Update stat file with new tick values: 50 + 75 = 125 ticks
  std::ofstream stat_out2(tmp_dir + "/1234/stat");
  stat_out2 << "1234 (testproc) S 1 1234 1234 0 -1 4194304 100 0 0 0 50 75 0 "
               "0 20 0 1 0 1000 10000 100 18446744073709551615 0 0 0 0 0 0 0 0 "
               "0 0 0 0 0 0\n";
  stat_out2.close();

  reg.refresh_telemetry();
  s = reg.get_session(id);
  TEST_ASSERT_TRUE(s.has_value());
  TEST_ASSERT_EQUAL_UINT64(125, s->last_cpu_ticks);

  std::filesystem::remove_all(tmp_dir);

  // 2. Real process stat reading (/proc/self). PID 1 is not used: in
  // containers it is often an idle placeholder with zero CPU ticks. Burn
  // ~50 ms of CPU so this process has accumulated at least one tick.
  const std::clock_t burn_until = std::clock() + CLOCKS_PER_SEC / 20;
  volatile uint64_t sink = 0;
  while (std::clock() < burn_until) {
    sink = sink + 1;
  }

  virmux::SessionRegistry real_reg;
  virmux::SessionRegistration real_req;
  real_req.name = "self";
  real_req.pid = ::getpid();
  real_req.command = "test_idle_autofreeze";

  uint64_t real_id = real_reg.register_session(real_req);
  real_reg.refresh_telemetry();
  auto real_s = real_reg.get_session(real_id);
  TEST_ASSERT_TRUE(real_s.has_value());
  TEST_ASSERT_GREATER_THAN(0, real_s->last_cpu_ticks);
}

/**
 * @brief Test 2: Auto-freeze disabled keeps idle process in RUNNING_NORMAL
 * state.
 */
void test_auto_freeze_disabled_idle_process(void) {
  pid_t pid = ::fork();
  TEST_ASSERT_TRUE(pid >= 0);

  if (pid == 0) {
    while (true) {
      ::pause();
    }
    ::_exit(0);
  }

  virmux::SessionRegistry reg;
  TEST_ASSERT_FALSE(reg.auto_freeze_enabled());

  virmux::SessionRegistration req;
  req.name = "idle-target-nofreeze";
  req.pid = pid;
  req.command = "idle-worker";

  uint64_t id = reg.register_session(req);
  TEST_ASSERT_EQUAL_UINT64(1, id);

  reg.refresh_telemetry();
  auto s = reg.get_session(id);
  TEST_ASSERT_TRUE(s.has_value());
  TEST_ASSERT_EQUAL(virmux::SessionState::RUNNING_NORMAL, s->state);

  // Wait beyond typical idle threshold
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  reg.refresh_telemetry();

  s = reg.get_session(id);
  TEST_ASSERT_TRUE(s.has_value());
  TEST_ASSERT_EQUAL(virmux::SessionState::RUNNING_NORMAL, s->state);

  ::kill(pid, SIGKILL);
  int st = 0;
  ::waitpid(pid, &st, 0);
}

/**
 * @brief Test 3: Auto-freeze enabled transitions idle process past timeout to
 * FROZEN and invokes callback.
 */
void test_auto_freeze_enabled_idle_process(void) {
  pid_t pid = ::fork();
  TEST_ASSERT_TRUE(pid >= 0);

  if (pid == 0) {
    while (true) {
      ::pause();
    }
    ::_exit(0);
  }

  virmux::SessionRegistry reg;
  reg.set_auto_freeze(true, 100);  // 100 ms timeout for fast unit testing
  TEST_ASSERT_TRUE(reg.auto_freeze_enabled());
  TEST_ASSERT_EQUAL_UINT64(100, reg.idle_timeout_ms());

  bool callback_invoked = false;
  uint64_t callback_id = 0;
  pid_t callback_pid = 0;

  reg.set_on_auto_freeze([&](uint64_t sid, pid_t p) {
    callback_invoked = true;
    callback_id = sid;
    callback_pid = p;
  });

  virmux::SessionRegistration req;
  req.name = "idle-target-autofreeze";
  req.pid = pid;
  req.command = "idle-proc";

  uint64_t id = reg.register_session(req);
  TEST_ASSERT_EQUAL_UINT64(1, id);

  // Initial telemetry read populates last_cpu_ticks
  reg.refresh_telemetry();
  auto s = reg.get_session(id);
  TEST_ASSERT_TRUE(s.has_value());
  TEST_ASSERT_EQUAL(virmux::SessionState::RUNNING_NORMAL, s->state);
  TEST_ASSERT_FALSE(callback_invoked);

  // Sleep beyond idle threshold of 100ms
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  reg.refresh_telemetry();

  s = reg.get_session(id);
  TEST_ASSERT_TRUE(s.has_value());
  TEST_ASSERT_EQUAL(virmux::SessionState::FROZEN, s->state);
  TEST_ASSERT_TRUE(callback_invoked);
  TEST_ASSERT_EQUAL_UINT64(id, callback_id);
  TEST_ASSERT_EQUAL_INT(pid, callback_pid);

  ::kill(pid, SIGKILL);
  int st = 0;
  ::waitpid(pid, &st, 0);
}

/**
 * @brief Test 4: Active busy process continuously updates CPU ticks and is NOT
 * frozen.
 */
void test_active_busy_process_not_frozen(void) {
  pid_t pid = ::fork();
  TEST_ASSERT_TRUE(pid >= 0);

  if (pid == 0) {
    volatile uint64_t counter = 0;
    while (true) {
      counter = counter + 1;
    }
    ::_exit(0);
  }

  virmux::SessionRegistry reg;
  reg.set_auto_freeze(true, 100);  // 100ms timeout

  virmux::SessionRegistration req;
  req.name = "busy-target";
  req.pid = pid;
  req.command = "compute-worker";

  uint64_t id = reg.register_session(req);
  reg.refresh_telemetry();

  auto s = reg.get_session(id);
  TEST_ASSERT_TRUE(s.has_value());
  TEST_ASSERT_EQUAL(virmux::SessionState::RUNNING_NORMAL, s->state);

  // Poll 4 times across 200ms with busy work advancing ticks
  uint64_t previous_ticks = s->last_cpu_ticks;
  for (int i = 0; i < 4; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    reg.refresh_telemetry();

    s = reg.get_session(id);
    TEST_ASSERT_TRUE(s.has_value());
    TEST_ASSERT_EQUAL(virmux::SessionState::RUNNING_NORMAL, s->state);
    TEST_ASSERT_GREATER_OR_EQUAL(previous_ticks, s->last_cpu_ticks);
    previous_ticks = s->last_cpu_ticks;
  }

  ::kill(pid, SIGKILL);
  int st = 0;
  ::waitpid(pid, &st, 0);
}

/**
 * @brief Test 5: Thaw restores session to RUNNING_NORMAL and resets
 * last_active_time.
 */
void test_thaw_restores_and_resets_last_active_time(void) {
  pid_t pid = ::fork();
  TEST_ASSERT_TRUE(pid >= 0);

  if (pid == 0) {
    while (true) {
      ::pause();
    }
    ::_exit(0);
  }

  virmux::SessionRegistry reg;
  virmux::SessionRegistration req;
  req.name = "thaw-target";
  req.pid = pid;
  req.command = "worker-thaw";

  uint64_t id = reg.register_session(req);
  TEST_ASSERT_TRUE(reg.freeze_session(id));

  auto s = reg.get_session(id);
  TEST_ASSERT_TRUE(s.has_value());
  TEST_ASSERT_EQUAL(virmux::SessionState::FROZEN, s->state);

  auto time_before_thaw = std::chrono::steady_clock::now();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  TEST_ASSERT_TRUE(reg.thaw_session(id));

  s = reg.get_session(id);
  TEST_ASSERT_TRUE(s.has_value());
  TEST_ASSERT_EQUAL(virmux::SessionState::RUNNING_NORMAL, s->state);
  TEST_ASSERT_TRUE(s->last_active_time >= time_before_thaw);

  ::kill(pid, SIGKILL);
  int st = 0;
  ::waitpid(pid, &st, 0);
}

/**
 * @brief Test 6: SupervisorDaemon wires auto-freeze configuration and compactor
 * callback.
 */
void test_supervisor_daemon_auto_freeze_wiring(void) {
  virmux::DaemonConfig cfg;
  cfg.auto_freeze_enabled = true;
  cfg.idle_timeout_ms = 250;

  virmux::SupervisorDaemon daemon(cfg);
  TEST_ASSERT_TRUE(daemon.registry().auto_freeze_enabled());
  TEST_ASSERT_EQUAL_UINT64(250, daemon.registry().idle_timeout_ms());
}

/**
 * @brief Test 7: Multithreaded CPU usage tracking from cgroup cpu.stat takes
 * precedence.
 */
void test_cgroup_cpustat_parsing(void) {
  std::string tmp_proc =
      "/tmp/virmux_cpustat_proc_" + std::to_string(::getpid());
  std::string tmp_cg = "/tmp/virmux_cpustat_cg_" + std::to_string(::getpid());
  std::error_code ec;
  std::filesystem::create_directories(tmp_proc + "/4321", ec);
  std::filesystem::create_directories(tmp_cg, ec);

  // Main thread stat has 50 ticks
  std::ofstream stat_out(tmp_proc + "/4321/stat");
  stat_out << "4321 (cgproc) S 1 4321 4321 0 -1 4194304 100 0 0 0 20 30 0 0 "
              "20 0 1 0 1000 10000 100 18446744073709551615 0 0 0 0 0 0 0 0 0 "
              "0 0 0 0 0\n";
  stat_out.close();

  // cgroups cpu.stat reports full slice usage: 987654 usec
  std::ofstream cg_out(tmp_cg + "/cpu.stat");
  cg_out << "usage_usec 987654\nuser_usec 500000\nsystem_usec 487654\n";
  cg_out.close();

  virmux::SessionRegistry reg(tmp_proc);
  virmux::SessionRegistration req;
  req.name = "cg-proc";
  req.pid = 4321;
  req.command = "cg-worker";
  req.cgroup_path = tmp_cg;

  uint64_t id = reg.register_session(req);
  reg.refresh_telemetry();
  auto s = reg.get_session(id);
  TEST_ASSERT_TRUE(s.has_value());
  // cpu.stat usage_usec takes precedence over proc stat ticks
  TEST_ASSERT_EQUAL_UINT64(987654, s->last_cpu_ticks);

  std::filesystem::remove_all(tmp_proc, ec);
  std::filesystem::remove_all(tmp_cg, ec);
}

/**
 * @brief Test 8: touch_session automatically thaws a frozen session.
 */
void test_touch_session_auto_thaws(void) {
  pid_t pid = ::fork();
  TEST_ASSERT_TRUE(pid >= 0);

  if (pid == 0) {
    while (true) {
      ::pause();
    }
    ::_exit(0);
  }

  virmux::SessionRegistry reg;
  virmux::SessionRegistration req;
  req.name = "touch-target";
  req.pid = pid;
  req.command = "worker-touch";

  uint64_t id = reg.register_session(req);
  TEST_ASSERT_TRUE(reg.freeze_session(id));

  auto s = reg.get_session(id);
  TEST_ASSERT_TRUE(s.has_value());
  TEST_ASSERT_EQUAL(virmux::SessionState::FROZEN, s->state);

  // Calling touch_session must auto-thaw to RUNNING_NORMAL
  TEST_ASSERT_TRUE(reg.touch_session(id));

  s = reg.get_session(id);
  TEST_ASSERT_TRUE(s.has_value());
  TEST_ASSERT_EQUAL(virmux::SessionState::RUNNING_NORMAL, s->state);

  ::kill(pid, SIGKILL);
  int st = 0;
  ::waitpid(pid, &st, 0);
}

/**
 * @brief Test 9: Freeze/thaw reach a run cgroup nested in a delegated subtree
 * (`user@<uid>.service/virmux/run-<pid>-<seq>`), not just root-level slices.
 */
void test_freeze_nested_delegated_run_cgroup(void) {
  const std::string base =
      "/tmp/virmux_nested_freeze_" + std::to_string(::getpid());
  const std::string run_cg =
      base + "/user.slice/user-1000.slice/user@1000.service/virmux/run-1-0";
  std::error_code ec;
  std::filesystem::create_directories(run_cg, ec);
  std::ofstream(run_cg + "/cgroup.freeze").close();

  pid_t pid = ::fork();
  TEST_ASSERT_TRUE(pid >= 0);
  if (pid == 0) {
    while (true) {
      ::pause();
    }
  }

  virmux::SessionRegistry reg;
  virmux::SessionRegistration req;
  req.name = "nested-run";
  req.pid = pid;
  req.command = "nested";
  req.cgroup_path = run_cg;
  const uint64_t id = reg.register_session(req);

  auto read_freeze = [&run_cg]() {
    std::ifstream in(run_cg + "/cgroup.freeze");
    std::string v;
    in >> v;
    return v;
  };

  TEST_ASSERT_TRUE(reg.freeze_session(id));
  TEST_ASSERT_EQUAL_STRING("1", read_freeze().c_str());
  const auto frozen = reg.get_session(id);
  TEST_ASSERT_TRUE(frozen.has_value());
  if (frozen.has_value()) {
    TEST_ASSERT_EQUAL(virmux::SessionState::FROZEN, frozen->state);
  }

  TEST_ASSERT_TRUE(reg.thaw_session(id));
  TEST_ASSERT_EQUAL_STRING("0", read_freeze().c_str());
  const auto thawed = reg.get_session(id);
  TEST_ASSERT_TRUE(thawed.has_value());
  if (thawed.has_value()) {
    TEST_ASSERT_EQUAL(virmux::SessionState::RUNNING_NORMAL, thawed->state);
  }

  ::kill(pid, SIGKILL);
  int st = 0;
  ::waitpid(pid, &st, 0);
  std::filesystem::remove_all(base, ec);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_stat_cpu_ticks_parsing);
  RUN_TEST(test_auto_freeze_disabled_idle_process);
  RUN_TEST(test_auto_freeze_enabled_idle_process);
  RUN_TEST(test_active_busy_process_not_frozen);
  RUN_TEST(test_thaw_restores_and_resets_last_active_time);
  RUN_TEST(test_supervisor_daemon_auto_freeze_wiring);
  RUN_TEST(test_cgroup_cpustat_parsing);
  RUN_TEST(test_touch_session_auto_thaws);
  RUN_TEST(test_freeze_nested_delegated_run_cgroup);
  return UNITY_END();
}
