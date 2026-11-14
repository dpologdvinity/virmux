/**
 * @file test_sandbox.cpp
 * @brief Unit tests for command sandbox launcher and exit propagation.
 */

extern "C" {
#include "unity/unity.h"
}

#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "virmux/gate.hpp"
#include "virmux/sandbox.hpp"

void setUp(void) {}
void tearDown(void) {}

void test_sandbox_exit_code_propagation(void) {
  virmux::SandboxConfig cfg{.virtual_cores = 2,
                            .memory_limit_bytes = 1024 * 1024 * 100};
  char arg0[] = "/bin/sh";
  char arg1[] = "-c";
  char arg2[] = "exit 42";
  char* argv[] = {arg0, arg1, arg2, nullptr};

  int exit_code = virmux::spawn_command(cfg, argv);
  TEST_ASSERT_EQUAL_INT(42, exit_code);
}

void test_sandbox_missing_binary_returns_127(void) {
  virmux::SandboxConfig cfg{};
  char arg0[] = "/bin/nonexistent_command_12345";
  char* argv[] = {arg0, nullptr};

  int exit_code = virmux::spawn_command(cfg, argv);
  TEST_ASSERT_EQUAL_INT(127, exit_code);
}

void test_sandbox_success_exit_zero(void) {
  virmux::SandboxConfig cfg{};
  char arg0[] = "/bin/sh";
  char arg1[] = "-c";
  char arg2[] = "exit 0";
  char* argv[] = {arg0, arg1, arg2, nullptr};

  int exit_code = virmux::spawn_command(cfg, argv);
  TEST_ASSERT_EQUAL_INT(0, exit_code);
}

void test_sandbox_signal_propagation(void) {
  virmux::SandboxConfig cfg{};
  char arg0[] = "/bin/sh";
  char arg1[] = "-c";
  char arg2[] = "kill -9 $$";
  char* argv[] = {arg0, arg1, arg2, nullptr};

  int exit_code = virmux::spawn_command(cfg, argv);
  // When killed by SIGKILL (9), status should be 128 + 9 = 137
  TEST_ASSERT_EQUAL_INT(137, exit_code);
}

void test_sandbox_null_argv_returns_error(void) {
  virmux::SandboxConfig cfg{};
  int exit_code = virmux::spawn_command(cfg, nullptr);
  TEST_ASSERT_EQUAL_INT(-1, exit_code);

  char* empty_argv[] = {nullptr};
  exit_code = virmux::spawn_command(cfg, empty_argv);
  TEST_ASSERT_EQUAL_INT(-1, exit_code);
}

void test_sandbox_workspace_path(void) {
  std::string temp_dir = "/tmp/virmux_sandbox_test_dir";
  std::filesystem::create_directories(temp_dir);

  virmux::SandboxConfig cfg{.workspace_path = temp_dir};
  char arg0[] = "/bin/sh";
  char arg1[] = "-c";
  char arg2[] = "pwd > /tmp/virmux_sandbox_pwd.out";
  char* argv[] = {arg0, arg1, arg2, nullptr};

  int exit_code = virmux::spawn_command(cfg, argv);
  TEST_ASSERT_EQUAL_INT(0, exit_code);

  std::ifstream ifs("/tmp/virmux_sandbox_pwd.out");
  std::string pwd_res;
  std::getline(ifs, pwd_res);
  ifs.close();
  unlink("/tmp/virmux_sandbox_pwd.out");
  std::filesystem::remove_all(temp_dir);

  TEST_ASSERT_EQUAL_STRING(temp_dir.c_str(), pwd_res.c_str());
}

void test_sandbox_virtual_env_and_meminfo(void) {
  uint64_t mem_limit = 512ULL * 1024 * 1024;  // 512 MB = 524288 kB
  virmux::SandboxConfig cfg{
      .virtual_cores = 3,
      .memory_limit_bytes = mem_limit,
  };

  char arg0[] = "/bin/sh";
  char arg1[] = "-c";
  char arg2[] =
      "test \"$VIRMUX_VIRTUAL_CORES\" = \"3\" && "
      "test \"$OMP_NUM_THREADS\" = \"3\" && "
      "grep -q \"MemTotal:[[:space:]]*524288 kB\" /proc/meminfo";
  char* argv[] = {arg0, arg1, arg2, nullptr};

  int exit_code = virmux::spawn_command(cfg, argv);
  TEST_ASSERT_EQUAL_INT(0, exit_code);
}

void test_sandbox_heavy_admission_gate(void) {
  virmux::AdmissionGate custom_gate(1, std::chrono::milliseconds(200));

  virmux::SandboxConfig cfg{
      .is_heavy = true,
      .gate = &custom_gate,
  };

  // Pre-acquire the single token so spawn_command must wait or timeout
  TEST_ASSERT_TRUE(custom_gate.try_acquire_immediate());
  TEST_ASSERT_EQUAL_INT(1, custom_gate.active_count());

  // In a separate thread, release token after 40ms
  std::thread releaser([&custom_gate]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    custom_gate.release_token();
  });

  char arg0[] = "/bin/sh";
  char arg1[] = "-c";
  char arg2[] = "exit 7";
  char* argv[] = {arg0, arg1, arg2, nullptr};

  int exit_code = virmux::spawn_command(cfg, argv);
  releaser.join();

  TEST_ASSERT_EQUAL_INT(7, exit_code);
  // After spawn_command returns, token must be released back
  TEST_ASSERT_EQUAL_INT(0, custom_gate.active_count());
}

void test_sandbox_heavy_admission_gate_timeout(void) {
  virmux::AdmissionGate custom_gate(1, std::chrono::milliseconds(30));

  virmux::SandboxConfig cfg{
      .is_heavy = true,
      .gate = &custom_gate,
  };

  // Pre-acquire the single token
  TEST_ASSERT_TRUE(custom_gate.try_acquire_immediate());
  TEST_ASSERT_EQUAL_INT(1, custom_gate.active_count());

  char arg0[] = "/bin/sh";
  char arg1[] = "-c";
  char arg2[] = "exit 0";
  char* argv[] = {arg0, arg1, arg2, nullptr};

  // Since token is never released, spawn_command should time out and return -1
  int exit_code = virmux::spawn_command(cfg, argv);
  TEST_ASSERT_EQUAL_INT(-1, exit_code);

  custom_gate.release_token();
  TEST_ASSERT_EQUAL_INT(0, custom_gate.active_count());
}

void test_sandbox_c_wrapper_and_oom_score(void) {
  virmux_set_worker_oom_score(0);

  virmux::SandboxConfig cfg{};
  char arg0[] = "/bin/sh";
  char arg1[] = "-c";
  char arg2[] = "exit 19";
  char* argv[] = {arg0, arg1, arg2, nullptr};

  int exit_code = virmux_spawn_command(cfg, argv);
  TEST_ASSERT_EQUAL_INT(19, exit_code);
}

void test_sandbox_permission_denied_returns_126(void) {
  std::string non_exec_file = "/tmp/virmux_non_exec_test.bin";
  {
    std::ofstream ofs(non_exec_file);
    ofs << "#!/bin/sh\nexit 0\n";
  }
  std::filesystem::permissions(
      non_exec_file,
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
      std::filesystem::perm_options::replace);

  virmux::SandboxConfig cfg{};
  char arg0[128];
  std::strncpy(arg0, non_exec_file.c_str(), sizeof(arg0) - 1);
  arg0[sizeof(arg0) - 1] = '\0';
  char* argv[] = {arg0, nullptr};

  int exit_code = virmux::spawn_command(cfg, argv);
  std::filesystem::remove(non_exec_file);

  TEST_ASSERT_EQUAL_INT(126, exit_code);
}

void test_sandbox_meminfo_cleaned_up(void) {
  virmux::SandboxConfig cfg{};
  char arg0[] = "/bin/sh";
  char arg1[] = "-c";
  char arg2[] = "exit 0";
  char* argv[] = {arg0, arg1, arg2, nullptr};

  int exit_code = virmux::spawn_command(cfg, argv);
  TEST_ASSERT_EQUAL_INT(0, exit_code);

  // Verify no temporary meminfo file for this PID remains in /tmp
  std::string prefix = "virmux_" + std::to_string(getpid()) + "_";
  bool found_leftover = false;
  for (const auto& entry : std::filesystem::directory_iterator("/tmp")) {
    std::string fname = entry.path().filename().string();
    if (fname.rfind(prefix, 0) == 0 &&
        fname.find("_meminfo") != std::string::npos) {
      found_leftover = true;
      break;
    }
  }
  TEST_ASSERT_FALSE(found_leftover);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_sandbox_exit_code_propagation);
  RUN_TEST(test_sandbox_missing_binary_returns_127);
  RUN_TEST(test_sandbox_permission_denied_returns_126);
  RUN_TEST(test_sandbox_success_exit_zero);
  RUN_TEST(test_sandbox_signal_propagation);
  RUN_TEST(test_sandbox_null_argv_returns_error);
  RUN_TEST(test_sandbox_workspace_path);
  RUN_TEST(test_sandbox_virtual_env_and_meminfo);
  RUN_TEST(test_sandbox_heavy_admission_gate);
  RUN_TEST(test_sandbox_heavy_admission_gate_timeout);
  RUN_TEST(test_sandbox_c_wrapper_and_oom_score);
  RUN_TEST(test_sandbox_meminfo_cleaned_up);
  return UNITY_END();
}
