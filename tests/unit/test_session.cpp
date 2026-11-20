/**
 * @file test_session.cpp
 * @brief Unit tests for SessionRegistry, session lifecycle, telemetry, and JSON
 * serialization.
 */

extern "C" {
#include "unity/unity.h"
}

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "virmux/session.hpp"

void setUp(void) {}
void tearDown(void) {}

/**
 * @brief Test 1: Session registration, lookup by ID/PID, derivation of name,
 * and unregistration.
 */
void test_session_registration_and_lookup(void) {
  virmux::SessionRegistry reg;

  virmux::SessionRegistration r1;
  r1.name = "test-custom";
  r1.pid = 10001;
  r1.virtual_cores = 4;
  r1.memory_limit_bytes = 1024 * 1024 * 512;
  r1.cgroup_path = "/sys/fs/cgroup/virmux.slice/test-custom";
  r1.command = "npm run test";

  uint64_t id1 = reg.register_session(r1);
  TEST_ASSERT_EQUAL_UINT64(1, id1);

  auto s1 = reg.get_session(id1);
  TEST_ASSERT_TRUE(s1.has_value());
  TEST_ASSERT_EQUAL_UINT64(1, s1->id);
  TEST_ASSERT_EQUAL_STRING("test-custom", s1->name.c_str());
  TEST_ASSERT_EQUAL_INT(10001, s1->pid);
  TEST_ASSERT_EQUAL(virmux::SessionState::RUNNING_NORMAL, s1->state);
  TEST_ASSERT_EQUAL_INT(4, s1->virtual_cores);
  TEST_ASSERT_EQUAL_UINT64(1024 * 1024 * 512, s1->memory_limit_bytes);
  TEST_ASSERT_EQUAL_STRING("/sys/fs/cgroup/virmux.slice/test-custom",
                           s1->cgroup_path.c_str());
  TEST_ASSERT_EQUAL_STRING("npm run test", s1->command.c_str());

  auto s1_by_pid = reg.get_session_by_pid(10001);
  TEST_ASSERT_TRUE(s1_by_pid.has_value());
  TEST_ASSERT_EQUAL_UINT64(1, s1_by_pid->id);
  TEST_ASSERT_EQUAL_STRING("test-custom", s1_by_pid->name.c_str());

  // Test name derivation from command binary path
  virmux::SessionRegistration r2;
  r2.pid = 10002;
  r2.virtual_cores = 0;  // Should default to 2
  r2.command = "/usr/bin/python3 script.py --arg val";

  uint64_t id2 = reg.register_session(r2);
  TEST_ASSERT_EQUAL_UINT64(2, id2);
  auto s2 = reg.get_session(id2);
  TEST_ASSERT_TRUE(s2.has_value());
  TEST_ASSERT_EQUAL_STRING("python3", s2->name.c_str());
  TEST_ASSERT_EQUAL_INT(2, s2->virtual_cores);

  // Test name derivation fallback when command is empty
  virmux::SessionRegistration r3;
  r3.pid = 10003;
  uint64_t id3 = reg.register_session(r3);
  TEST_ASSERT_EQUAL_UINT64(3, id3);
  auto s3 = reg.get_session(id3);
  TEST_ASSERT_TRUE(s3.has_value());
  TEST_ASSERT_EQUAL_STRING("session-3", s3->name.c_str());

  // Non-existent lookups
  TEST_ASSERT_FALSE(reg.get_session(9999).has_value());
  TEST_ASSERT_FALSE(reg.get_session_by_pid(999999).has_value());

  // Unregistration
  TEST_ASSERT_TRUE(reg.unregister_session(id2));
  TEST_ASSERT_FALSE(reg.get_session(id2).has_value());
  TEST_ASSERT_FALSE(reg.unregister_session(id2));
}

/**
 * @brief Test 2: Listing sessions and JSON serialization / deserialization
 * round-trip.
 */
void test_session_list_and_json(void) {
  virmux::SessionRegistry reg;

  TEST_ASSERT_EQUAL_UINT32(0, reg.list_sessions().size());
  TEST_ASSERT_EQUAL_STRING("[]", reg.to_json().c_str());
  TEST_ASSERT_EQUAL_UINT32(0, virmux::SessionRegistry::from_json("[]").size());

  virmux::SessionRegistration r1;
  r1.name = "alpha";
  r1.pid = 2001;
  r1.virtual_cores = 4;
  r1.memory_limit_bytes = 2048;
  r1.command = "worker-alpha";

  virmux::SessionRegistration r2;
  r2.name = "beta";
  r2.pid = 2002;
  r2.virtual_cores = 8;
  r2.memory_limit_bytes = 4096;
  r2.command = "worker-beta";

  reg.register_session(r1);
  reg.register_session(r2);

  auto list = reg.list_sessions();
  TEST_ASSERT_EQUAL_UINT32(2, list.size());
  TEST_ASSERT_EQUAL_UINT64(1, list[0].id);
  TEST_ASSERT_EQUAL_STRING("alpha", list[0].name.c_str());
  TEST_ASSERT_EQUAL_UINT64(2, list[1].id);
  TEST_ASSERT_EQUAL_STRING("beta", list[1].name.c_str());

  std::string json = reg.to_json();
  TEST_ASSERT_NOT_NULL(std::strstr(json.c_str(), "\"id\": 1"));
  TEST_ASSERT_NOT_NULL(std::strstr(json.c_str(), "\"name\": \"alpha\""));
  TEST_ASSERT_NOT_NULL(std::strstr(json.c_str(), "\"pid\": 2001"));
  TEST_ASSERT_NOT_NULL(
      std::strstr(json.c_str(), "\"state\": \"RUNNING_NORMAL\""));
  TEST_ASSERT_NOT_NULL(std::strstr(json.c_str(), "\"id\": 2"));
  TEST_ASSERT_NOT_NULL(std::strstr(json.c_str(), "\"name\": \"beta\""));
  TEST_ASSERT_NOT_NULL(std::strstr(json.c_str(), "\"pid\": 2002"));

  auto parsed = virmux::SessionRegistry::from_json(json);
  TEST_ASSERT_EQUAL_UINT32(2, parsed.size());

  TEST_ASSERT_EQUAL_UINT64(1, parsed[0].id);
  TEST_ASSERT_EQUAL_STRING("alpha", parsed[0].name.c_str());
  TEST_ASSERT_EQUAL_INT(2001, parsed[0].pid);
  TEST_ASSERT_EQUAL(virmux::SessionState::RUNNING_NORMAL, parsed[0].state);
  TEST_ASSERT_EQUAL_INT(4, parsed[0].virtual_cores);
  TEST_ASSERT_EQUAL_UINT64(2048, parsed[0].memory_limit_bytes);
  TEST_ASSERT_EQUAL_STRING("worker-alpha", parsed[0].command.c_str());

  TEST_ASSERT_EQUAL_UINT64(2, parsed[1].id);
  TEST_ASSERT_EQUAL_STRING("beta", parsed[1].name.c_str());
  TEST_ASSERT_EQUAL_INT(2002, parsed[1].pid);
  TEST_ASSERT_EQUAL(virmux::SessionState::RUNNING_NORMAL, parsed[1].state);
  TEST_ASSERT_EQUAL_INT(8, parsed[1].virtual_cores);
  TEST_ASSERT_EQUAL_UINT64(4096, parsed[1].memory_limit_bytes);
  TEST_ASSERT_EQUAL_STRING("worker-beta", parsed[1].command.c_str());
}

/**
 * @brief Test 3: Telemetry polling, RSS reading from procfs, and DEAD state
 * transition when child terminates.
 */
void test_session_telemetry_reading(void) {
  int fds[2];
  TEST_ASSERT_EQUAL_INT(0, ::pipe(fds));

  pid_t pid = ::fork();
  TEST_ASSERT_TRUE(pid >= 0);

  if (pid == 0) {
    ::close(fds[1]);
    // Allocate 4 MB of resident anonymous memory
    std::vector<char> buf(4 * 1024 * 1024, static_cast<char>(1));
    buf[1024] = 2;

    // Block until parent signals through pipe
    char signal_byte = 0;
    [[maybe_unused]] ssize_t n = ::read(fds[0], &signal_byte, 1);
    ::close(fds[0]);
    ::_exit(0);
  }

  ::close(fds[0]);

  virmux::SessionRegistry reg;
  virmux::SessionRegistration req;
  req.name = "telemetry-child";
  req.pid = pid;
  req.command = "child-proc";

  uint64_t id = reg.register_session(req);
  TEST_ASSERT_EQUAL_UINT64(1, id);

  // Poll telemetry while child is active
  reg.refresh_telemetry();
  auto s = reg.get_session(id);
  TEST_ASSERT_TRUE(s.has_value());
  TEST_ASSERT_EQUAL(virmux::SessionState::RUNNING_NORMAL, s->state);
  TEST_ASSERT_GREATER_THAN_UINT64(0, s->memory_rss_bytes);

  // Terminate child
  [[maybe_unused]] ssize_t n = ::write(fds[1], "q", 1);
  ::close(fds[1]);

  int status = 0;
  ::waitpid(pid, &status, 0);

  // Refresh telemetry after child terminated
  reg.refresh_telemetry();
  s = reg.get_session(id);
  TEST_ASSERT_TRUE(s.has_value());
  TEST_ASSERT_EQUAL(virmux::SessionState::DEAD, s->state);
  TEST_ASSERT_EQUAL_UINT64(0, s->memory_rss_bytes);
}

/**
 * @brief Test 4: Freeze and thaw using SIGSTOP / SIGCONT fallback.
 */
void test_session_freeze_thaw_fallback(void) {
  pid_t pid = ::fork();
  TEST_ASSERT_TRUE(pid >= 0);

  if (pid == 0) {
    while (true) {
      ::usleep(10000);
    }
    ::_exit(0);
  }

  virmux::SessionRegistry reg;
  virmux::SessionRegistration req;
  req.name = "freeze-target";
  req.pid = pid;
  req.command = "loop-target";

  uint64_t id = reg.register_session(req);
  TEST_ASSERT_EQUAL_UINT64(1, id);

  // Freeze via fallback
  TEST_ASSERT_TRUE(reg.freeze_session(id));
  auto s = reg.get_session(id);
  TEST_ASSERT_TRUE(s.has_value());
  TEST_ASSERT_EQUAL(virmux::SessionState::FROZEN, s->state);

  // Thaw via fallback
  TEST_ASSERT_TRUE(reg.thaw_session(id));
  s = reg.get_session(id);
  TEST_ASSERT_TRUE(s.has_value());
  TEST_ASSERT_EQUAL(virmux::SessionState::RUNNING_NORMAL, s->state);

  // Clean up child process
  ::kill(pid, SIGKILL);
  int status = 0;
  ::waitpid(pid, &status, 0);
}

/**
 * @brief Test 5: Terminate session process with kill_session().
 */
void test_session_kill(void) {
  pid_t pid = ::fork();
  TEST_ASSERT_TRUE(pid >= 0);

  if (pid == 0) {
    while (true) {
      ::usleep(10000);
    }
    ::_exit(0);
  }

  virmux::SessionRegistry reg;
  virmux::SessionRegistration req;
  req.name = "victim";
  req.pid = pid;
  req.command = "victim-proc";

  uint64_t id = reg.register_session(req);
  TEST_ASSERT_EQUAL_UINT64(1, id);

  TEST_ASSERT_TRUE(reg.kill_session(id, SIGKILL));
  auto s = reg.get_session(id);
  TEST_ASSERT_TRUE(s.has_value());
  TEST_ASSERT_EQUAL(virmux::SessionState::DEAD, s->state);

  int status = 0;
  pid_t w = ::waitpid(pid, &status, 0);
  TEST_ASSERT_EQUAL(pid, w);
  TEST_ASSERT_TRUE(WIFSIGNALED(status));
  TEST_ASSERT_EQUAL(SIGKILL, WTERMSIG(status));
}

/**
 * @brief Test 6: String representation conversions for SessionState.
 */
void test_session_state_strings(void) {
  TEST_ASSERT_EQUAL_STRING(
      "INITIALIZING",
      virmux::session_state_to_string(virmux::SessionState::INITIALIZING)
          .data());
  TEST_ASSERT_EQUAL_STRING(
      "RUNNING_BURST",
      virmux::session_state_to_string(virmux::SessionState::RUNNING_BURST)
          .data());
  TEST_ASSERT_EQUAL_STRING(
      "RUNNING_NORMAL",
      virmux::session_state_to_string(virmux::SessionState::RUNNING_NORMAL)
          .data());
  TEST_ASSERT_EQUAL_STRING(
      "WAITING_IO",
      virmux::session_state_to_string(virmux::SessionState::WAITING_IO).data());
  TEST_ASSERT_EQUAL_STRING(
      "FROZEN",
      virmux::session_state_to_string(virmux::SessionState::FROZEN).data());
  TEST_ASSERT_EQUAL_STRING(
      "CHECKPOINTED",
      virmux::session_state_to_string(virmux::SessionState::CHECKPOINTED)
          .data());
  TEST_ASSERT_EQUAL_STRING(
      "DEAD",
      virmux::session_state_to_string(virmux::SessionState::DEAD).data());

  TEST_ASSERT_EQUAL(virmux::SessionState::INITIALIZING,
                    virmux::session_state_from_string("INITIALIZING"));
  TEST_ASSERT_EQUAL(virmux::SessionState::RUNNING_BURST,
                    virmux::session_state_from_string("RUNNING_BURST"));
  TEST_ASSERT_EQUAL(virmux::SessionState::RUNNING_NORMAL,
                    virmux::session_state_from_string("RUNNING_NORMAL"));
  TEST_ASSERT_EQUAL(virmux::SessionState::WAITING_IO,
                    virmux::session_state_from_string("WAITING_IO"));
  TEST_ASSERT_EQUAL(virmux::SessionState::FROZEN,
                    virmux::session_state_from_string("FROZEN"));
  TEST_ASSERT_EQUAL(virmux::SessionState::CHECKPOINTED,
                    virmux::session_state_from_string("CHECKPOINTED"));
  TEST_ASSERT_EQUAL(virmux::SessionState::DEAD,
                    virmux::session_state_from_string("DEAD"));
}

/**
 * @brief Test 7: Error handling for missing sessions in lifecycle methods.
 */
void test_session_error_handling(void) {
  virmux::SessionRegistry reg;

  TEST_ASSERT_FALSE(reg.freeze_session(999));
  TEST_ASSERT_FALSE(reg.thaw_session(999));
  TEST_ASSERT_FALSE(reg.kill_session(999));
}

/**
 * @brief Test 8: Concurrent multithreaded registration and listing.
 */
void test_session_concurrent_access(void) {
  virmux::SessionRegistry reg;
  constexpr int NUM_THREADS = 4;
  constexpr int SESSIONS_PER_THREAD = 25;

  std::vector<std::thread> threads;
  threads.reserve(NUM_THREADS);

  for (int t = 0; t < NUM_THREADS; ++t) {
    threads.emplace_back([&reg, t]() {
      for (int i = 0; i < SESSIONS_PER_THREAD; ++i) {
        virmux::SessionRegistration req;
        req.name = "t" + std::to_string(t) + "-s" + std::to_string(i);
        req.pid = 10000 + t * 100 + i;
        req.command = "concurrent-cmd";
        reg.register_session(req);
        reg.list_sessions();
      }
    });
  }

  for (auto& th : threads) {
    th.join();
  }

  auto list = reg.list_sessions();
  TEST_ASSERT_EQUAL_UINT32(NUM_THREADS * SESSIONS_PER_THREAD, list.size());
}

/**
 * @brief Test 9: Pruning dead sessions after retention window.
 */
void test_session_dead_pruning(void) {
  virmux::SessionRegistry reg;

  virmux::SessionRegistration r1;
  r1.name = "dead-session";
  r1.pid = -1;  // Registered as dead
  r1.command = "exit";
  uint64_t id1 = reg.register_session(r1);

  virmux::SessionRegistration r2;
  r2.name = "live-session";
  r2.pid = ::getpid();
  r2.command = "live-process";
  uint64_t id2 = reg.register_session(r2);

  // Before prune, both sessions exist
  TEST_ASSERT_EQUAL_UINT32(2, reg.list_sessions().size());

  // Pruning with high age threshold (1 hour) does not prune anything yet
  TEST_ASSERT_EQUAL_UINT32(
      0, reg.prune_dead_sessions(std::chrono::milliseconds(3600000)));
  TEST_ASSERT_EQUAL_UINT32(2, reg.list_sessions().size());

  // Immediate prune with 0ms age threshold prunes DEAD session id1
  size_t pruned = reg.prune_dead_sessions(std::chrono::milliseconds(0));
  TEST_ASSERT_EQUAL_UINT32(1, pruned);
  TEST_ASSERT_EQUAL_UINT32(1, reg.list_sessions().size());
  TEST_ASSERT_FALSE(reg.get_session(id1).has_value());
  TEST_ASSERT_TRUE(reg.get_session(id2).has_value());
}

/**
 * @brief Regression (found by fuzz_session_list_json): from_json() must not
 * read json[json.size()] after skipping trailing whitespace.
 *
 * Each view deliberately stops just before a byte that is still inside the
 * backing buffer, so an over-read is observable even in builds without
 * bounds-checked string_view: a phantom '{' yields an extra session and a
 * phantom '"' makes the key scan call substr() past the end (throws).
 */
void test_session_from_json_no_read_past_end(void) {
  const std::string outer = "[ {";
  auto sessions = virmux::SessionRegistry::from_json(
      std::string_view(outer.data(), outer.size() - 1));
  TEST_ASSERT_EQUAL_size_t(0, sessions.size());

  const std::string inner = "[{ \"";
  sessions = virmux::SessionRegistry::from_json(
      std::string_view(inner.data(), inner.size() - 1));
  TEST_ASSERT_EQUAL_size_t(1, sessions.size());
  TEST_ASSERT_EQUAL_UINT64(0, sessions[0].id);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_session_from_json_no_read_past_end);
  RUN_TEST(test_session_registration_and_lookup);
  RUN_TEST(test_session_list_and_json);
  RUN_TEST(test_session_telemetry_reading);
  RUN_TEST(test_session_freeze_thaw_fallback);
  RUN_TEST(test_session_kill);
  RUN_TEST(test_session_state_strings);
  RUN_TEST(test_session_error_handling);
  RUN_TEST(test_session_concurrent_access);
  RUN_TEST(test_session_dead_pruning);
  return UNITY_END();
}
