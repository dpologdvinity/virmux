/**
 * @file test_cli_daemon.cpp
 * @brief Unit tests for CLI daemon subcommands, table formatting, and sandbox
 * hooks.
 */

extern "C" {
#include "unity/unity.h"
}

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "virmux/cli_daemon.hpp"
#include "virmux/daemon.hpp"
#include "virmux/sandbox.hpp"
#include "virmux/session.hpp"

void setUp(void) {}
void tearDown(void) {}

namespace {

std::string get_unique_socket_path() {
  static std::atomic<int> seq{0};
  return "/tmp/virmux_cli_test_" + std::to_string(::getpid()) + "_" +
         std::to_string(seq.fetch_add(1)) + ".sock";
}

}  // namespace

void test_format_session_table_empty(void) {
  std::vector<virmux::SessionInfo> empty_list;
  std::string table = virmux::format_session_table(empty_list);

  TEST_ASSERT_TRUE(
      table.find("S#  | NAME         | STATE   | V-CORES | RAM (RSS) | DEDUP   "
                 "| COMMAND") != std::string::npos);
}

void test_format_session_table_populated(void) {
  std::vector<virmux::SessionInfo> sessions;

  virmux::SessionInfo s1;
  s1.id = 1;
  s1.name = "daemon-task";
  s1.pid = 1234;
  s1.state = virmux::SessionState::RUNNING_NORMAL;
  s1.virtual_cores = 4;
  s1.memory_rss_bytes = 1048576;  // 1.0 MiB
  s1.ksm_dedup_bytes = 524288;    // 512.0 KiB
  s1.command = "virmux-worker --task=1";
  sessions.push_back(s1);

  virmux::SessionInfo s2;
  s2.id = 2;
  s2.name = "frozen-worker";
  s2.pid = 5678;
  s2.state = virmux::SessionState::FROZEN;
  s2.virtual_cores = 2;
  s2.memory_rss_bytes = 1073741824ULL;  // 1.00 GiB
  s2.ksm_dedup_bytes = 0;               // 0 B
  s2.command = "heavy-calc";
  sessions.push_back(s2);

  std::string table = virmux::format_session_table(sessions);

  // Check header
  TEST_ASSERT_TRUE(
      table.find("S#  | NAME         | STATE   | V-CORES | RAM (RSS) | DEDUP   "
                 "| COMMAND") != std::string::npos);

  // Check session 1 content
  TEST_ASSERT_TRUE(table.find("daemon-task") != std::string::npos);
  TEST_ASSERT_TRUE(table.find("RUNNING") != std::string::npos);
  TEST_ASSERT_TRUE(table.find("1.0 MiB") != std::string::npos);
  TEST_ASSERT_TRUE(table.find("512.0 KiB") != std::string::npos);
  TEST_ASSERT_TRUE(table.find("virmux-worker --task=1") != std::string::npos);

  // Check session 2 content
  TEST_ASSERT_TRUE(table.find("frozen-worker") != std::string::npos);
  TEST_ASSERT_TRUE(table.find("FROZEN") != std::string::npos);
  TEST_ASSERT_TRUE(table.find("1.00 GiB") != std::string::npos);
  TEST_ASSERT_TRUE(table.find("heavy-calc") != std::string::npos);
}

void test_format_session_json(void) {
  // 1. Empty list
  std::vector<virmux::SessionInfo> empty_list;
  std::string empty_json = virmux::format_session_json(empty_list);
  TEST_ASSERT_EQUAL_STRING("[]", empty_json.c_str());

  // 2. Populated list
  std::vector<virmux::SessionInfo> sessions;
  virmux::SessionInfo s1;
  s1.id = 1;
  s1.name = "json-task";
  s1.pid = 4321;
  s1.state = virmux::SessionState::RUNNING_NORMAL;
  s1.virtual_cores = 8;
  s1.memory_rss_bytes = 2097152;
  s1.memory_limit_bytes = 1073741824;
  s1.ksm_dedup_bytes = 4096;
  s1.cgroup_path = "/sys/fs/cgroup/test.slice";
  s1.command = "test-json-cmd --flag";
  s1.start_time = std::chrono::system_clock::now();
  sessions.push_back(s1);

  std::string json = virmux::format_session_json(sessions);

  TEST_ASSERT_TRUE(json.front() == '[');
  TEST_ASSERT_TRUE(json.back() == ']');
  TEST_ASSERT_TRUE(json.find("\"id\": 1") != std::string::npos);
  TEST_ASSERT_TRUE(json.find("\"name\": \"json-task\"") != std::string::npos);
  TEST_ASSERT_TRUE(json.find("\"pid\": 4321") != std::string::npos);
  TEST_ASSERT_TRUE(json.find("\"state\": \"RUNNING_NORMAL\"") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(json.find("\"virtual_cores\": 8") != std::string::npos);
  TEST_ASSERT_TRUE(json.find("\"test-json-cmd --flag\"") != std::string::npos);

  // Round-trip parse test with SessionRegistry::from_json
  auto parsed = virmux::SessionRegistry::from_json(json);
  TEST_ASSERT_EQUAL_size_t(1, parsed.size());
  TEST_ASSERT_EQUAL_UINT64(1, parsed[0].id);
  TEST_ASSERT_EQUAL_STRING("json-task", parsed[0].name.c_str());
  TEST_ASSERT_EQUAL_INT(4321, parsed[0].pid);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(virmux::SessionState::RUNNING_NORMAL),
                        static_cast<int>(parsed[0].state));
}

void test_cmd_help_flags(void) {
  char* daemon_args[] = {const_cast<char*>("virmux"),
                         const_cast<char*>("daemon"),
                         const_cast<char*>("--help")};
  TEST_ASSERT_EQUAL_INT(0, virmux::cmd_daemon(3, daemon_args));

  char* list_args[] = {const_cast<char*>("virmux"), const_cast<char*>("list"),
                       const_cast<char*>("-h")};
  TEST_ASSERT_EQUAL_INT(0, virmux::cmd_list(3, list_args));

  char* freeze_args[] = {const_cast<char*>("virmux"),
                         const_cast<char*>("freeze"),
                         const_cast<char*>("--help")};
  TEST_ASSERT_EQUAL_INT(0, virmux::cmd_freeze(3, freeze_args));

  char* thaw_args[] = {const_cast<char*>("virmux"), const_cast<char*>("thaw"),
                       const_cast<char*>("-h")};
  TEST_ASSERT_EQUAL_INT(0, virmux::cmd_thaw(3, thaw_args));

  char* kill_args[] = {const_cast<char*>("virmux"), const_cast<char*>("kill"),
                       const_cast<char*>("--help")};
  TEST_ASSERT_EQUAL_INT(0, virmux::cmd_kill(3, kill_args));
}

void test_cmd_missing_and_invalid_args(void) {
  // Missing session ID
  char* freeze_no_args[] = {const_cast<char*>("virmux"),
                            const_cast<char*>("freeze")};
  TEST_ASSERT_EQUAL_INT(1, virmux::cmd_freeze(2, freeze_no_args));

  char* thaw_no_args[] = {const_cast<char*>("virmux"),
                          const_cast<char*>("thaw")};
  TEST_ASSERT_EQUAL_INT(1, virmux::cmd_thaw(2, thaw_no_args));

  char* kill_no_args[] = {const_cast<char*>("virmux"),
                          const_cast<char*>("kill")};
  TEST_ASSERT_EQUAL_INT(1, virmux::cmd_kill(2, kill_no_args));

  // Invalid session ID (non-numeric)
  char* freeze_invalid[] = {const_cast<char*>("virmux"),
                            const_cast<char*>("freeze"),
                            const_cast<char*>("not_a_number")};
  TEST_ASSERT_EQUAL_INT(1, virmux::cmd_freeze(3, freeze_invalid));

  char* thaw_invalid[] = {const_cast<char*>("virmux"),
                          const_cast<char*>("thaw"),
                          const_cast<char*>("invalid_id")};
  TEST_ASSERT_EQUAL_INT(1, virmux::cmd_thaw(3, thaw_invalid));

  char* kill_invalid[] = {const_cast<char*>("virmux"),
                          const_cast<char*>("kill"), const_cast<char*>("xyz")};
  TEST_ASSERT_EQUAL_INT(1, virmux::cmd_kill(3, kill_invalid));

  // Unrecognized options
  char* daemon_unrec[] = {const_cast<char*>("virmux"),
                          const_cast<char*>("daemon"),
                          const_cast<char*>("--unknown")};
  TEST_ASSERT_EQUAL_INT(1, virmux::cmd_daemon(3, daemon_unrec));

  char* list_unrec[] = {const_cast<char*>("virmux"), const_cast<char*>("list"),
                        const_cast<char*>("--foo")};
  TEST_ASSERT_EQUAL_INT(1, virmux::cmd_list(3, list_unrec));
}

void test_offline_daemon_error_handling(void) {
  std::string fake_socket = "/tmp/virmux_offline_test_nonexistent.sock";
  ::setenv("VIRMUX_SOCKET", fake_socket.c_str(), 1);

  char* list_args[] = {const_cast<char*>("virmux"), const_cast<char*>("list")};
  TEST_ASSERT_EQUAL_INT(1, virmux::cmd_list(2, list_args));

  char* freeze_args[] = {const_cast<char*>("virmux"),
                         const_cast<char*>("freeze"), const_cast<char*>("42")};
  TEST_ASSERT_EQUAL_INT(1, virmux::cmd_freeze(3, freeze_args));

  char* thaw_args[] = {const_cast<char*>("virmux"), const_cast<char*>("thaw"),
                       const_cast<char*>("42")};
  TEST_ASSERT_EQUAL_INT(1, virmux::cmd_thaw(3, thaw_args));

  char* kill_args[] = {const_cast<char*>("virmux"), const_cast<char*>("kill"),
                       const_cast<char*>("42")};
  TEST_ASSERT_EQUAL_INT(1, virmux::cmd_kill(3, kill_args));

  ::unsetenv("VIRMUX_SOCKET");
}

void test_live_daemon_cli_flow(void) {
  std::string test_sock = get_unique_socket_path();
  ::setenv("VIRMUX_SOCKET", test_sock.c_str(), 1);

  // Spawn dummy child process to manage
  pid_t child_pid = ::fork();
  if (child_pid == 0) {
    // Child sleeps until killed
    ::sleep(30);
    ::_exit(0);
  }
  TEST_ASSERT_GREATER_THAN(0, child_pid);

  virmux::DaemonConfig cfg;
  cfg.socket_path = test_sock;
  virmux::SupervisorDaemon daemon(cfg);
  TEST_ASSERT_TRUE(daemon.init());

  std::thread daemon_thread([&]() { daemon.run(); });

  // Wait briefly for daemon thread to start listening
  virmux::DaemonClient client(test_sock);
  int retries = 0;
  while (!client.is_alive(50) && retries++ < 20) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  TEST_ASSERT_TRUE(client.is_alive(200));

  // Register session for child process
  virmux::SessionRegistration reg{
      .name = "cli-test-proc",
      .pid = child_pid,
      .virtual_cores = 2,
      .memory_limit_bytes = 536870912ULL,
      .cgroup_path = "",
      .command = "sleep 30",
  };
  uint64_t session_id = client.register_session(reg, 500);
  TEST_ASSERT_GREATER_THAN_UINT64(0, session_id);

  // Test cmd_list default table
  char* list_argv[] = {const_cast<char*>("virmux"), const_cast<char*>("list")};
  TEST_ASSERT_EQUAL_INT(0, virmux::cmd_list(2, list_argv));

  // Test cmd_list with --json
  char* list_json_argv[] = {const_cast<char*>("virmux"),
                            const_cast<char*>("list"),
                            const_cast<char*>("--json")};
  TEST_ASSERT_EQUAL_INT(0, virmux::cmd_list(3, list_json_argv));

  // Test cmd_freeze with session ID
  std::string id_str = std::to_string(session_id);
  char* freeze_argv[] = {const_cast<char*>("virmux"),
                         const_cast<char*>("freeze"),
                         const_cast<char*>(id_str.c_str())};
  TEST_ASSERT_EQUAL_INT(0, virmux::cmd_freeze(3, freeze_argv));

  // Test cmd_thaw with session ID
  char* thaw_argv[] = {const_cast<char*>("virmux"), const_cast<char*>("thaw"),
                       const_cast<char*>(id_str.c_str())};
  TEST_ASSERT_EQUAL_INT(0, virmux::cmd_thaw(3, thaw_argv));

  // Test cmd_freeze with child PID resolution
  std::string pid_str = std::to_string(child_pid);
  char* freeze_by_pid_argv[] = {const_cast<char*>("virmux"),
                                const_cast<char*>("freeze"),
                                const_cast<char*>(pid_str.c_str())};
  TEST_ASSERT_EQUAL_INT(0, virmux::cmd_freeze(3, freeze_by_pid_argv));

  // Test cmd_thaw with child PID resolution
  char* thaw_by_pid_argv[] = {const_cast<char*>("virmux"),
                              const_cast<char*>("thaw"),
                              const_cast<char*>(pid_str.c_str())};
  TEST_ASSERT_EQUAL_INT(0, virmux::cmd_thaw(3, thaw_by_pid_argv));

  // Test cmd_kill
  char* kill_argv[] = {const_cast<char*>("virmux"), const_cast<char*>("kill"),
                       const_cast<char*>(id_str.c_str())};
  TEST_ASSERT_EQUAL_INT(0, virmux::cmd_kill(3, kill_argv));

  // Wait for child to exit
  int status = 0;
  ::waitpid(child_pid, &status, 0);

  daemon.stop();
  if (daemon_thread.joinable()) {
    daemon_thread.join();
  }

  ::unsetenv("VIRMUX_SOCKET");
  std::error_code ec;
  std::filesystem::remove(test_sock, ec);
}

void test_sandbox_daemon_registration(void) {
  std::string test_sock = get_unique_socket_path();
  ::setenv("VIRMUX_SOCKET", test_sock.c_str(), 1);

  virmux::DaemonConfig cfg;
  cfg.socket_path = test_sock;
  virmux::SupervisorDaemon daemon(cfg);
  TEST_ASSERT_TRUE(daemon.init());

  std::thread daemon_thread([&]() { daemon.run(); });

  virmux::DaemonClient client(test_sock);
  int retries = 0;
  while (!client.is_alive(50) && retries++ < 20) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  TEST_ASSERT_TRUE(client.is_alive(200));

  // Run spawn_command in background thread
  std::thread spawn_thread([&]() {
    virmux::SandboxConfig scfg;
    scfg.virtual_cores = 3;
    char* cmd_args[] = {const_cast<char*>("/bin/sleep"),
                        const_cast<char*>("0.3"), nullptr};
    int rc = virmux::spawn_command(scfg, cmd_args);
    (void)rc;
  });

  // Wait for session to be visible in daemon
  std::vector<virmux::SessionInfo> active_sessions;
  int poll_count = 0;
  while (poll_count++ < 50) {
    active_sessions = client.list_sessions(100);
    if (!active_sessions.empty()) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  TEST_ASSERT_EQUAL_size_t(1, active_sessions.size());
  TEST_ASSERT_EQUAL_INT(3, active_sessions[0].virtual_cores);
  TEST_ASSERT_TRUE(active_sessions[0].command.find("/bin/sleep 0.3") !=
                   std::string::npos);

  if (spawn_thread.joinable()) {
    spawn_thread.join();
  }

  // After command exits, session must be unregistered
  auto after_exit = client.list_sessions(200);
  TEST_ASSERT_EQUAL_size_t(0, after_exit.size());

  daemon.stop();
  if (daemon_thread.joinable()) {
    daemon_thread.join();
  }

  ::unsetenv("VIRMUX_SOCKET");
  std::error_code ec;
  std::filesystem::remove(test_sock, ec);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_format_session_table_empty);
  RUN_TEST(test_format_session_table_populated);
  RUN_TEST(test_format_session_json);
  RUN_TEST(test_cmd_help_flags);
  RUN_TEST(test_cmd_missing_and_invalid_args);
  RUN_TEST(test_offline_daemon_error_handling);
  RUN_TEST(test_live_daemon_cli_flow);
  RUN_TEST(test_sandbox_daemon_registration);
  return UNITY_END();
}
