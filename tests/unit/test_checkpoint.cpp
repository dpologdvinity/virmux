/**
 * @file test_checkpoint.cpp
 * @brief Unit tests for CheckpointManager, SessionRegistry checkpointing, and IPC.
 */

extern "C" {
#include "unity/unity.h"
}

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "virmux/checkpoint.hpp"
#include "virmux/daemon.hpp"
#include "virmux/session.hpp"

void setUp(void) {}
void tearDown(void) {}

namespace {

std::string get_unique_temp_dir() {
  static std::atomic<int> seq{0};
  std::string dir = "/tmp/virmux_chkpt_test_" + std::to_string(::getpid()) +
                    "_" + std::to_string(seq.fetch_add(1));
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  return dir;
}

std::string get_unique_socket_path() {
  static std::atomic<int> seq{0};
  return "/tmp/virmux_chkpt_sock_" + std::to_string(::getpid()) + "_" +
         std::to_string(seq.fetch_add(1)) + ".sock";
}

pid_t spawn_sleeper() {
  pid_t pid = ::fork();
  if (pid == 0) {
    execl("/bin/sleep", "sleep", "60", nullptr);
    _exit(127);
  }
  return pid;
}

void terminate_process(pid_t pid) {
  if (pid > 0) {
    ::kill(pid, SIGKILL);
    int status = 0;
    ::waitpid(pid, &status, 0);
  }
}

}  // namespace

void test_checkpoint_path_resolution_and_permissions(void) {
  std::string temp_dir = get_unique_temp_dir();
  {
    virmux::CheckpointManager mgr(temp_dir);
    TEST_ASSERT_EQUAL_STRING(temp_dir.c_str(), mgr.base_dir().c_str());

    std::error_code ec;
    TEST_ASSERT_TRUE(std::filesystem::is_directory(temp_dir, ec));

    struct stat st;
    TEST_ASSERT_EQUAL_INT(0, ::stat(temp_dir.c_str(), &st));
    TEST_ASSERT_EQUAL_INT(0700, st.st_mode & 0777);
  }

  // Test VIRMUX_CHECKPOINT_DIR env override
  std::string env_dir = get_unique_temp_dir();
  ::setenv("VIRMUX_CHECKPOINT_DIR", env_dir.c_str(), 1);
  {
    virmux::CheckpointManager mgr;
    TEST_ASSERT_EQUAL_STRING(env_dir.c_str(), mgr.base_dir().c_str());
    struct stat st;
    TEST_ASSERT_EQUAL_INT(0, ::stat(env_dir.c_str(), &st));
    TEST_ASSERT_EQUAL_INT(0700, st.st_mode & 0777);
  }
  ::unsetenv("VIRMUX_CHECKPOINT_DIR");

  // Cleanup
  std::error_code ec;
  std::filesystem::remove_all(temp_dir, ec);
  std::filesystem::remove_all(env_dir, ec);
}

void test_checkpoint_criu_availability_probe(void) {
  virmux::CheckpointManager mgr;

  // Test force-enable
  ::setenv("VIRMUX_CRIU_FORCE", "1", 1);
  TEST_ASSERT_TRUE(mgr.is_criu_available());

  // Test force-disable
  ::setenv("VIRMUX_CRIU_FORCE", "0", 1);
  TEST_ASSERT_FALSE(mgr.is_criu_available());

  ::unsetenv("VIRMUX_CRIU_FORCE");
  // Default probe in unprivileged sandbox should return false (or true if privileged)
  bool res = mgr.is_criu_available();
  // Either true or false, but does not crash or hang
  (void)res;
}

void test_checkpoint_unprivileged_freeze_pageout(void) {
  std::string temp_dir = get_unique_temp_dir();
  virmux::CheckpointManager mgr(temp_dir);

  ::setenv("VIRMUX_CRIU_FORCE", "0", 1);

  pid_t child_pid = spawn_sleeper();
  TEST_ASSERT_TRUE(child_pid > 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  virmux::SessionInfo session;
  session.id = 101;
  session.name = "worker-sim";
  session.pid = child_pid;
  session.virtual_cores = 2;
  session.memory_limit_bytes = 1024 * 1024 * 256;
  session.command = "/bin/sleep 60";
  session.state = virmux::SessionState::RUNNING_NORMAL;

  std::string msg;
  bool ok = mgr.checkpoint_session(session, &msg);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_TRUE(mgr.has_checkpoint(101));

  // Check metadata json persistence
  std::filesystem::path meta_path =
      std::filesystem::path(temp_dir) / "101" / "checkpoint.json";
  std::error_code ec;
  TEST_ASSERT_TRUE(std::filesystem::exists(meta_path, ec));

  struct stat st;
  TEST_ASSERT_EQUAL_INT(0, ::stat(meta_path.c_str(), &st));
  TEST_ASSERT_EQUAL_INT(0600, st.st_mode & 0777);

  // Read JSON content
  std::ifstream ifs(meta_path);
  std::string content((std::istreambuf_iterator<char>(ifs)),
                      std::istreambuf_iterator<char>());
  ifs.close();

  TEST_ASSERT_TRUE(content.find("\"id\": 101") != std::string::npos);
  TEST_ASSERT_TRUE(content.find("\"name\": \"worker-sim\"") != std::string::npos);
  TEST_ASSERT_TRUE(content.find("\"criu\": false") != std::string::npos);

  // Restore session
  bool restore_ok = mgr.restore_session(session, &msg);
  TEST_ASSERT_TRUE(restore_ok);
  TEST_ASSERT_FALSE(mgr.has_checkpoint(101));
  TEST_ASSERT_FALSE(std::filesystem::exists(meta_path, ec));

  terminate_process(child_pid);
  ::unsetenv("VIRMUX_CRIU_FORCE");
  std::filesystem::remove_all(temp_dir, ec);
}

void test_session_registry_checkpoint_and_restore(void) {
  std::string temp_dir = get_unique_temp_dir();
  virmux::SessionRegistry reg("/proc", temp_dir);

  ::setenv("VIRMUX_CRIU_FORCE", "0", 1);

  pid_t child_pid = spawn_sleeper();
  TEST_ASSERT_TRUE(child_pid > 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  virmux::SessionRegistration req;
  req.name = "proc-test";
  req.pid = child_pid;
  req.virtual_cores = 4;
  req.command = "/bin/sleep 60";

  uint64_t id = reg.register_session(req);
  TEST_ASSERT_EQUAL_UINT64(1, id);

  auto s_init = reg.get_session(id);
  TEST_ASSERT_TRUE(s_init.has_value());
  TEST_ASSERT_EQUAL(virmux::SessionState::RUNNING_NORMAL, s_init->state);

  // Checkpoint session
  std::string msg;
  bool cp_ok = reg.checkpoint_session(id, &msg);
  TEST_ASSERT_TRUE(cp_ok);

  auto s_cp = reg.get_session(id);
  TEST_ASSERT_TRUE(s_cp.has_value());
  TEST_ASSERT_EQUAL(virmux::SessionState::CHECKPOINTED, s_cp->state);
  TEST_ASSERT_EQUAL_UINT64(0, s_cp->memory_rss_bytes);
  TEST_ASSERT_TRUE(reg.has_checkpoint(id));

  // Telemetry refresh should preserve CHECKPOINTED state and 0 RSS
  reg.refresh_telemetry();
  auto s_after_refresh = reg.get_session(id);
  TEST_ASSERT_TRUE(s_after_refresh.has_value());
  TEST_ASSERT_EQUAL(virmux::SessionState::CHECKPOINTED, s_after_refresh->state);
  TEST_ASSERT_EQUAL_UINT64(0, s_after_refresh->memory_rss_bytes);

  // Restore session
  bool r_ok = reg.restore_session(id, &msg);
  TEST_ASSERT_TRUE(r_ok);

  auto s_restored = reg.get_session(id);
  TEST_ASSERT_TRUE(s_restored.has_value());
  TEST_ASSERT_EQUAL(virmux::SessionState::RUNNING_NORMAL, s_restored->state);
  TEST_ASSERT_FALSE(reg.has_checkpoint(id));

  terminate_process(child_pid);
  ::unsetenv("VIRMUX_CRIU_FORCE");
  std::error_code ec;
  std::filesystem::remove_all(temp_dir, ec);
}

void test_daemon_ipc_checkpoint_and_restore(void) {
  std::string test_sock = get_unique_socket_path();
  std::string temp_dir = get_unique_temp_dir();
  ::setenv("VIRMUX_SOCKET", test_sock.c_str(), 1);
  ::setenv("VIRMUX_CHECKPOINT_DIR", temp_dir.c_str(), 1);
  ::setenv("VIRMUX_CRIU_FORCE", "0", 1);

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

  pid_t child_pid = spawn_sleeper();
  TEST_ASSERT_TRUE(child_pid > 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  virmux::SessionRegistration req;
  req.name = "ipc-chkpt-test";
  req.pid = child_pid;
  req.command = "sleep 60";
  uint64_t id = client.register_session(req, 1000);
  TEST_ASSERT_TRUE(id > 0);

  // Checkpoint via client IPC
  std::string msg;
  bool cp_ok = client.checkpoint_session(id, &msg, 2000);
  TEST_ASSERT_TRUE(cp_ok);

  auto sessions = client.list_sessions(1000);
  TEST_ASSERT_EQUAL_size_t(1, sessions.size());
  TEST_ASSERT_EQUAL(virmux::SessionState::CHECKPOINTED, sessions[0].state);
  TEST_ASSERT_EQUAL_UINT64(0, sessions[0].memory_rss_bytes);

  // Restore via client IPC
  bool r_ok = client.restore_session(id, &msg, 2000);
  TEST_ASSERT_TRUE(r_ok);

  sessions = client.list_sessions(1000);
  TEST_ASSERT_EQUAL_size_t(1, sessions.size());
  TEST_ASSERT_EQUAL(virmux::SessionState::RUNNING_NORMAL, sessions[0].state);

  daemon.stop();
  if (daemon_thread.joinable()) {
    daemon_thread.join();
  }

  terminate_process(child_pid);
  ::unsetenv("VIRMUX_SOCKET");
  ::unsetenv("VIRMUX_CHECKPOINT_DIR");
  ::unsetenv("VIRMUX_CRIU_FORCE");
  std::error_code ec;
  std::filesystem::remove(test_sock, ec);
  std::filesystem::remove_all(temp_dir, ec);
}

void test_checkpoint_error_handling(void) {
  std::string temp_dir = get_unique_temp_dir();
  virmux::SessionRegistry reg("/proc", temp_dir);

  std::string msg;
  // Non-existent session
  TEST_ASSERT_FALSE(reg.checkpoint_session(999, &msg));
  TEST_ASSERT_TRUE(msg.find("not found") != std::string::npos);

  // Restore non-existent
  TEST_ASSERT_FALSE(reg.restore_session(999, &msg));

  // Register session
  virmux::SessionRegistration req;
  req.name = "err-test";
  req.pid = ::getpid();
  req.command = "test";
  uint64_t id = reg.register_session(req);

  // Restoring non-checkpointed session
  TEST_ASSERT_FALSE(reg.restore_session(id, &msg));
  TEST_ASSERT_TRUE(msg.find("not checkpointed") != std::string::npos);

  // Checkpoint dead session
  virmux::SessionRegistration dead_req;
  dead_req.name = "dead";
  dead_req.pid = -1;
  uint64_t dead_id = reg.register_session(dead_req);
  TEST_ASSERT_FALSE(reg.checkpoint_session(dead_id, &msg));

  std::error_code ec;
  std::filesystem::remove_all(temp_dir, ec);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_checkpoint_path_resolution_and_permissions);
  RUN_TEST(test_checkpoint_criu_availability_probe);
  RUN_TEST(test_checkpoint_unprivileged_freeze_pageout);
  RUN_TEST(test_session_registry_checkpoint_and_restore);
  RUN_TEST(test_daemon_ipc_checkpoint_and_restore);
  RUN_TEST(test_checkpoint_error_handling);
  return UNITY_END();
}
