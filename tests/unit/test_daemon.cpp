/**
 * @file test_daemon.cpp
 * @brief Unit tests for SupervisorDaemon and DaemonClient IPC.
 */

extern "C" {
#include "unity/unity.h"
}

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "virmux/daemon.hpp"

void setUp(void) {}
void tearDown(void) {}

namespace {

std::string get_unique_socket_path() {
  static std::atomic<int> seq{0};
  return "/tmp/virmux_daemon_test_" + std::to_string(::getpid()) + "_" +
         std::to_string(seq.fetch_add(1)) + ".sock";
}

}  // namespace

/**
 * @brief Test 1: Stale socket cleanup and active daemon conflict detection.
 */
void test_daemon_init_and_stale_socket_cleanup(void) {
  std::string sock_path = get_unique_socket_path();

  // Create a dead socket file (bound but closed/unlistened)
  int dead_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  TEST_ASSERT_GREATER_OR_EQUAL(0, dead_fd);

  struct sockaddr_un addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);

  TEST_ASSERT_EQUAL(
      0,
      ::bind(dead_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)));
  ::close(dead_fd);
  TEST_ASSERT_TRUE(std::filesystem::exists(sock_path));

  // Initialize daemon on the same socket path: should detect dead socket,
  // unlink it, and bind
  virmux::DaemonConfig config;
  config.socket_path = sock_path;
  virmux::SupervisorDaemon daemon(config);
  TEST_ASSERT_TRUE(daemon.init());
  TEST_ASSERT_TRUE(std::filesystem::exists(sock_path));

  // Verify socket permissions are 0700
  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, ::stat(sock_path.c_str(), &st));
  TEST_ASSERT_EQUAL_HEX(0700, st.st_mode & 0777);

  // Verify that new daemon is listening (connect succeeds)
  int test_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  TEST_ASSERT_GREATER_OR_EQUAL(0, test_fd);
  TEST_ASSERT_EQUAL(
      0, ::connect(test_fd, reinterpret_cast<struct sockaddr*>(&addr),
                   sizeof(addr)));
  ::close(test_fd);

  // Active daemon detection: a second daemon on the same socket path must fail
  // to init
  virmux::SupervisorDaemon daemon_conflict(config);
  TEST_ASSERT_FALSE(daemon_conflict.init());

  // Stop daemon and verify socket removed
  daemon.stop();
  TEST_ASSERT_FALSE(std::filesystem::exists(sock_path));
}

/**
 * @brief Test 2: DaemonClient connecting and pinging running SupervisorDaemon.
 */
void test_daemon_client_ping(void) {
  std::string sock_path = get_unique_socket_path();
  virmux::DaemonConfig config;
  config.socket_path = sock_path;

  virmux::SupervisorDaemon daemon(config);
  TEST_ASSERT_TRUE(daemon.init());

  std::thread th([&] { daemon.run(); });

  for (int i = 0; i < 100 && !daemon.is_running(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  TEST_ASSERT_TRUE(daemon.is_running());

  virmux::DaemonClient client(sock_path);
  TEST_ASSERT_TRUE(client.is_alive(1000));

  daemon.stop();
  th.join();
  TEST_ASSERT_FALSE(daemon.is_running());
  TEST_ASSERT_FALSE(std::filesystem::exists(sock_path));
}

/**
 * @brief Test 3: Session registration, listing, freeze, thaw, stats, and
 * unregistration.
 */
void test_daemon_client_session_lifecycle(void) {
  std::string sock_path = get_unique_socket_path();
  virmux::DaemonConfig config;
  config.socket_path = sock_path;

  virmux::SupervisorDaemon daemon(config);
  TEST_ASSERT_TRUE(daemon.init());

  std::thread th([&] { daemon.run(); });

  for (int i = 0; i < 100 && !daemon.is_running(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  TEST_ASSERT_TRUE(daemon.is_running());

  // Fork a child process to act as the managed target
  pid_t child_pid = ::fork();
  if (child_pid == 0) {
    while (true) {
      ::pause();
    }
    ::_exit(0);
  }
  TEST_ASSERT_GREATER_THAN(0, child_pid);

  virmux::DaemonClient client(sock_path);

  // 1. Register session
  virmux::SessionRegistration req;
  req.name = "test-worker";
  req.pid = child_pid;
  req.virtual_cores = 3;
  req.memory_limit_bytes = 1024ULL * 1024 * 256;
  req.cgroup_path = "/sys/fs/cgroup/virmux.slice/test-worker";
  req.command = "/bin/sleep 1000";

  uint64_t session_id = client.register_session(req, 1000);
  TEST_ASSERT_GREATER_THAN_UINT64(0, session_id);

  // 2. List sessions and verify fields
  auto sessions = client.list_sessions(1000);
  TEST_ASSERT_EQUAL_UINT32(1, sessions.size());
  TEST_ASSERT_EQUAL_UINT64(session_id, sessions[0].id);
  TEST_ASSERT_EQUAL_STRING("test-worker", sessions[0].name.c_str());
  TEST_ASSERT_EQUAL_INT(child_pid, sessions[0].pid);
  TEST_ASSERT_EQUAL_INT(3, sessions[0].virtual_cores);
  TEST_ASSERT_EQUAL_UINT64(1024 * 1024 * 256, sessions[0].memory_limit_bytes);
  TEST_ASSERT_EQUAL_STRING("/bin/sleep 1000", sessions[0].command.c_str());

  // 3. Freeze session
  TEST_ASSERT_TRUE(client.freeze_session(session_id, 1000));

  // 4. Thaw session
  TEST_ASSERT_TRUE(client.thaw_session(session_id, 1000));

  // 5. Query stats
  std::string stats_json = client.get_stats(1000);
  TEST_ASSERT_TRUE(stats_json.find("\"host_memory\"") != std::string::npos);

  // 6. Kill session
  TEST_ASSERT_TRUE(client.kill_session(session_id, SIGTERM, 1000));
  int wait_status = 0;
  ::waitpid(child_pid, &wait_status, 0);

  // 7. Unregister session
  TEST_ASSERT_TRUE(client.unregister_session(session_id, 1000));
  auto remaining = client.list_sessions(1000);
  TEST_ASSERT_EQUAL_UINT32(0, remaining.size());

  daemon.stop();
  th.join();
  TEST_ASSERT_FALSE(daemon.is_running());
}

/**
 * @brief Test 4: DaemonClient graceful fallback when daemon is offline.
 */
void test_daemon_client_offline_fallback(void) {
  std::string sock_path = "/tmp/virmux_offline_socket_test_999999.sock";
  virmux::DaemonClient client(sock_path);

  TEST_ASSERT_FALSE(client.is_alive(100));

  virmux::SessionRegistration req;
  req.name = "offline-worker";
  req.pid = 1234;

  TEST_ASSERT_EQUAL_UINT64(0, client.register_session(req, 100));
  TEST_ASSERT_FALSE(client.unregister_session(1, 100));

  auto sessions = client.list_sessions(100);
  TEST_ASSERT_EQUAL_UINT32(0, sessions.size());

  TEST_ASSERT_FALSE(client.freeze_session(1, 100));
  TEST_ASSERT_FALSE(client.thaw_session(1, 100));
  TEST_ASSERT_FALSE(client.kill_session(1, SIGTERM, 100));
  TEST_ASSERT_EQUAL_STRING("", client.get_stats(100).c_str());
}

/**
 * @brief Test 5: Graceful daemon stop and socket unlinking.
 */
void test_daemon_graceful_stop_and_socket_removal(void) {
  std::string sock_path = get_unique_socket_path();
  virmux::DaemonConfig config;
  config.socket_path = sock_path;

  virmux::SupervisorDaemon daemon(config);
  TEST_ASSERT_TRUE(daemon.init());
  TEST_ASSERT_TRUE(std::filesystem::exists(sock_path));

  std::thread th([&] { daemon.run(); });

  for (int i = 0; i < 100 && !daemon.is_running(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  TEST_ASSERT_TRUE(daemon.is_running());

  daemon.stop();
  th.join();

  TEST_ASSERT_FALSE(daemon.is_running());
  TEST_ASSERT_FALSE(std::filesystem::exists(sock_path));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_daemon_init_and_stale_socket_cleanup);
  RUN_TEST(test_daemon_client_ping);
  RUN_TEST(test_daemon_client_session_lifecycle);
  RUN_TEST(test_daemon_client_offline_fallback);
  RUN_TEST(test_daemon_graceful_stop_and_socket_removal);
  return UNITY_END();
}
