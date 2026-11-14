/**
 * @file test_sandbox_overlay.cpp
 * @brief Unit tests for sandbox ephemeral OverlayFS execution and scratch redirection.
 */

extern "C" {
#include "unity/unity.h"
}

#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "virmux/sandbox.hpp"

namespace fs = std::filesystem;

namespace {

std::string create_test_temp_dir(const char* prefix) {
  std::string pattern = std::string("/tmp/") + prefix + "_XXXXXX";
  std::vector<char> buf(pattern.begin(), pattern.end());
  buf.push_back('\0');
  char* dir = ::mkdtemp(buf.data());
  TEST_ASSERT_NOT_NULL(dir);
  return std::string(dir);
}

void cleanup_test_dir(const std::string& path) {
  std::error_code ec;
  fs::remove_all(path, ec);
}

}  // namespace

void setUp(void) {}
void tearDown(void) {}

void test_sandbox_ephemeral_cow_isolation(void) {
  std::string temp_dir = create_test_temp_dir("virmux_sb_cow");
  std::string base_file = temp_dir + "/base_code.c";
  const std::string original_content = "int main() { return 0; }\n";

  {
    std::ofstream ofs(base_file);
    ofs << original_content;
  }

  virmux::SandboxConfig cfg{
      .ephemeral = true,
      .workspace_dir = temp_dir,
  };

  char arg0[] = "/bin/sh";
  char arg1[] = "-c";
  char arg2[] = "echo 'modified' >> base_code.c && echo 'output' > build.o";
  char* argv[] = {arg0, arg1, arg2, nullptr};

  int exit_code = virmux::spawn_command(cfg, argv);
  TEST_ASSERT_EQUAL_INT(0, exit_code);

  // Verify build.o DOES NOT exist in base directory
  std::string build_out = temp_dir + "/build.o";
  TEST_ASSERT_FALSE(fs::exists(build_out));

  // Verify base_code.c in base directory is 100% UNTOUCHED
  std::ifstream ifs(base_file);
  TEST_ASSERT_TRUE(ifs.is_open());
  std::string current_content(
      (std::istreambuf_iterator<char>(ifs)),
      std::istreambuf_iterator<char>());
  ifs.close();
  TEST_ASSERT_EQUAL_STRING(original_content.c_str(), current_content.c_str());

  cleanup_test_dir(temp_dir);
}

void test_sandbox_scratch_redirection(void) {
  virmux::SandboxConfig cfg{
      .ephemeral = true,
  };

  char arg0[] = "/bin/sh";
  char arg1[] = "-c";
  char arg2[] = "test -d \"$TMPDIR\" && echo 'temp' > \"$TMPDIR/file.tmp\"";
  char* argv[] = {arg0, arg1, arg2, nullptr};

  int exit_code = virmux::spawn_command(cfg, argv);
  TEST_ASSERT_EQUAL_INT(0, exit_code);
}

void test_sandbox_persistent_workspace(void) {
  std::string temp_dir = create_test_temp_dir("virmux_sb_persist");

  virmux::SandboxConfig cfg{
      .ephemeral = false,
      .workspace_dir = temp_dir,
  };

  char arg0[] = "/bin/sh";
  char arg1[] = "-c";
  char arg2[] = "echo 'committed' > persistent.txt";
  char* argv[] = {arg0, arg1, arg2, nullptr};

  int exit_code = virmux::spawn_command(cfg, argv);
  TEST_ASSERT_EQUAL_INT(0, exit_code);

  // Verify persistent.txt EXISTS in base directory
  std::string persistent_file = temp_dir + "/persistent.txt";
  TEST_ASSERT_TRUE(fs::exists(persistent_file));

  std::ifstream ifs(persistent_file);
  TEST_ASSERT_TRUE(ifs.is_open());
  std::string content;
  std::getline(ifs, content);
  ifs.close();
  TEST_ASSERT_EQUAL_STRING("committed", content.c_str());

  cleanup_test_dir(temp_dir);
}

void test_sandbox_scratch_isolated_per_invocation(void) {
  std::string shared_file_marker = "session_marker.tmp";

  // Run 1: write marker into $TMPDIR
  {
    virmux::SandboxConfig cfg{
        .ephemeral = true,
    };
    char arg0[] = "/bin/sh";
    char arg1[] = "-c";
    char arg2[] = "echo 'marker' > \"$TMPDIR/session_marker.tmp\"";
    char* argv[] = {arg0, arg1, arg2, nullptr};

    int exit_code = virmux::spawn_command(cfg, argv);
    TEST_ASSERT_EQUAL_INT(0, exit_code);
  }

  // Run 2: ensure $TMPDIR is a fresh directory without marker
  {
    virmux::SandboxConfig cfg{
        .ephemeral = true,
    };
    char arg0[] = "/bin/sh";
    char arg1[] = "-c";
    char arg2[] = "test ! -f \"$TMPDIR/session_marker.tmp\"";
    char* argv[] = {arg0, arg1, arg2, nullptr};

    int exit_code = virmux::spawn_command(cfg, argv);
    TEST_ASSERT_EQUAL_INT(0, exit_code);
  }
}

void test_sandbox_custom_scratch_dir(void) {
  std::string custom_scratch = create_test_temp_dir("virmux_sb_scratch");
  std::string out_file = "/tmp/virmux_scratch_out.txt";

  virmux::SandboxConfig cfg{
      .ephemeral = true,
      .scratch_dir = custom_scratch,
  };

  std::string script = "echo \"$TMPDIR\" > " + out_file;
  char arg0[] = "/bin/sh";
  char arg1[] = "-c";
  std::vector<char> script_buf(script.begin(), script.end());
  script_buf.push_back('\0');
  char* argv[] = {arg0, arg1, script_buf.data(), nullptr};

  int exit_code = virmux::spawn_command(cfg, argv);
  TEST_ASSERT_EQUAL_INT(0, exit_code);

  std::ifstream ifs(out_file);
  TEST_ASSERT_TRUE(ifs.is_open());
  std::string recorded_tmpdir;
  std::getline(ifs, recorded_tmpdir);
  ifs.close();
  ::unlink(out_file.c_str());

  TEST_ASSERT_TRUE(recorded_tmpdir.rfind(custom_scratch, 0) == 0);

  cleanup_test_dir(custom_scratch);
}

void test_sandbox_ephemeral_default_current_path(void) {
  std::string temp_dir = create_test_temp_dir("virmux_sb_curdir");
  std::string file_path = temp_dir + "/sample.txt";
  {
    std::ofstream ofs(file_path);
    ofs << "original";
  }

  char saved_cwd[PATH_MAX];
  TEST_ASSERT_NOT_NULL(::getcwd(saved_cwd, sizeof(saved_cwd)));
  TEST_ASSERT_EQUAL_INT(0, ::chdir(temp_dir.c_str()));

  virmux::SandboxConfig cfg{
      .ephemeral = true,
  };

  char arg0[] = "/bin/sh";
  char arg1[] = "-c";
  char arg2[] = "echo 'mutated' >> sample.txt && echo 'new' > created.txt";
  char* argv[] = {arg0, arg1, arg2, nullptr};

  int exit_code = virmux::spawn_command(cfg, argv);
  TEST_ASSERT_EQUAL_INT(0, exit_code);

  // Restore working directory
  TEST_ASSERT_EQUAL_INT(0, ::chdir(saved_cwd));

  // Verify created.txt does NOT exist in temp_dir
  TEST_ASSERT_FALSE(fs::exists(temp_dir + "/created.txt"));

  // Verify sample.txt is untouched
  std::ifstream ifs(file_path);
  TEST_ASSERT_TRUE(ifs.is_open());
  std::string content;
  std::getline(ifs, content);
  ifs.close();
  TEST_ASSERT_EQUAL_STRING("original", content.c_str());

  cleanup_test_dir(temp_dir);
}

void test_sandbox_ephemeral_fallback_on_invalid_dir(void) {
  virmux::SandboxConfig cfg{
      .ephemeral = true,
      .workspace_dir = "/tmp/virmux_nonexistent_workspace_99999",
  };

  char arg0[] = "/bin/sh";
  char arg1[] = "-c";
  char arg2[] = "exit 0";
  char* argv[] = {arg0, arg1, arg2, nullptr};

  // Overlay setup should fail, log warning, fallback to non-ephemeral,
  // then child tries to chdir to nonexistent dir and exits 127
  int exit_code = virmux::spawn_command(cfg, argv);
  TEST_ASSERT_EQUAL_INT(127, exit_code);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_sandbox_ephemeral_cow_isolation);
  RUN_TEST(test_sandbox_scratch_redirection);
  RUN_TEST(test_sandbox_persistent_workspace);
  RUN_TEST(test_sandbox_scratch_isolated_per_invocation);
  RUN_TEST(test_sandbox_custom_scratch_dir);
  RUN_TEST(test_sandbox_ephemeral_default_current_path);
  RUN_TEST(test_sandbox_ephemeral_fallback_on_invalid_dir);
  return UNITY_END();
}
