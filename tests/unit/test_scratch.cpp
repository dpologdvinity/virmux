/**
 * @file test_scratch.cpp
 * @brief Unit tests for in-memory scratch space manager and environment
 * redirection.
 */

extern "C" {
#include "unity/unity.h"
}

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "virmux/scratch.hpp"

namespace fs = std::filesystem;

namespace {

std::string create_test_temp_dir() {
  char template_path[] = "/tmp/virmux_scratch_test_XXXXXX";
  char* dir = ::mkdtemp(template_path);
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

void test_scratch_init_creates_dir_and_mode(void) {
  std::string base = create_test_temp_dir();
  {
    virmux::ScratchWorkspace ws(base);
    TEST_ASSERT_FALSE(ws.is_valid());
    TEST_ASSERT_TRUE(ws.path().empty());

    TEST_ASSERT_TRUE(ws.init());
    TEST_ASSERT_TRUE(ws.is_valid());
    TEST_ASSERT_FALSE(ws.path().empty());

    // Path must be inside base directory
    TEST_ASSERT_TRUE(ws.path().rfind(base, 0) == 0);

    // Verify directory exists
    TEST_ASSERT_TRUE(fs::is_directory(ws.path()));

    // Verify directory permissions mode 0700 (owner_all, no group or others
    // permissions)
    struct stat st{};
    TEST_ASSERT_EQUAL_INT(0, ::stat(ws.path().c_str(), &st));
    mode_t mode = st.st_mode & 0777;
    TEST_ASSERT_EQUAL_HEX(0700, mode);

    // Idempotent init
    TEST_ASSERT_TRUE(ws.init());
    TEST_ASSERT_TRUE(ws.is_valid());

    std::string saved_path = ws.path();
    ws.cleanup();
    TEST_ASSERT_FALSE(ws.is_valid());
    TEST_ASSERT_TRUE(ws.path().empty());
    TEST_ASSERT_FALSE(fs::exists(saved_path));
  }

  cleanup_test_dir(base);
}

void test_scratch_default_base_dir(void) {
  virmux::ScratchWorkspace ws;
  TEST_ASSERT_TRUE(ws.init());
  TEST_ASSERT_TRUE(ws.is_valid());
  TEST_ASSERT_FALSE(ws.path().empty());

  // Path should be either in /run/user/<UID>/virmux/scratch or
  // /tmp/virmux/scratch
  std::string run_user_prefix = "/run/user/" + std::to_string(::getuid());
  bool starts_with_run = (ws.path().rfind(run_user_prefix, 0) == 0);
  bool starts_with_tmp = (ws.path().rfind("/tmp/virmux/scratch", 0) == 0);
  TEST_ASSERT_TRUE(starts_with_run || starts_with_tmp);

  TEST_ASSERT_TRUE(fs::is_directory(ws.path()));

  std::string saved_path = ws.path();
  ws.cleanup();
  TEST_ASSERT_FALSE(fs::exists(saved_path));
}

void test_scratch_populate_env(void) {
  std::string base = create_test_temp_dir();
  virmux::ScratchWorkspace ws(base);
  TEST_ASSERT_TRUE(ws.init());

  std::vector<std::string> env;
  ws.populate_env(env);

  // Expect TMPDIR, TEMP, TMP
  TEST_ASSERT_EQUAL_INT(3, env.size());

  std::string expected_tmpdir = "TMPDIR=" + ws.path();
  std::string expected_temp = "TEMP=" + ws.path();
  std::string expected_tmp = "TMP=" + ws.path();

  bool found_tmpdir = false;
  bool found_temp = false;
  bool found_tmp = false;

  for (const auto& var : env) {
    if (var == expected_tmpdir) found_tmpdir = true;
    if (var == expected_temp) found_temp = true;
    if (var == expected_tmp) found_tmp = true;
  }

  TEST_ASSERT_TRUE(found_tmpdir);
  TEST_ASSERT_TRUE(found_temp);
  TEST_ASSERT_TRUE(found_tmp);

  // Overwriting pre-existing environment variables
  std::vector<std::string> existing = {"FOO=BAR", "TMPDIR=/stale/dir",
                                       "OTHER=123"};
  ws.populate_env(existing);

  found_tmpdir = false;
  found_temp = false;
  found_tmp = false;
  bool found_foo = false;
  bool found_other = false;

  for (const auto& var : existing) {
    if (var == expected_tmpdir) found_tmpdir = true;
    if (var == expected_temp) found_temp = true;
    if (var == expected_tmp) found_tmp = true;
    if (var == "FOO=BAR") found_foo = true;
    if (var == "OTHER=123") found_other = true;
  }

  TEST_ASSERT_TRUE(found_tmpdir);
  TEST_ASSERT_TRUE(found_temp);
  TEST_ASSERT_TRUE(found_tmp);
  TEST_ASSERT_TRUE(found_foo);
  TEST_ASSERT_TRUE(found_other);
  // Stale dir should not exist
  TEST_ASSERT_TRUE(std::find(existing.begin(), existing.end(),
                             "TMPDIR=/stale/dir") == existing.end());

  ws.cleanup();
  cleanup_test_dir(base);
}

void test_scratch_write_files_and_cleanup(void) {
  std::string base = create_test_temp_dir();
  virmux::ScratchWorkspace ws(base);
  TEST_ASSERT_TRUE(ws.init());

  std::string file1 = ws.path() + "/temp_build_file.o";
  std::string sub_dir = ws.path() + "/nested/cache";
  std::string file2 = sub_dir + "/temp_data.bin";

  fs::create_directories(sub_dir);
  {
    std::ofstream ofs1(file1);
    ofs1 << "dummy binary content 123456";
    std::ofstream ofs2(file2);
    ofs2 << "nested scratch content 789";
  }

  TEST_ASSERT_TRUE(fs::exists(file1));
  TEST_ASSERT_TRUE(fs::exists(file2));

  std::string saved_path = ws.path();
  ws.cleanup();

  TEST_ASSERT_FALSE(ws.is_valid());
  TEST_ASSERT_TRUE(ws.path().empty());
  TEST_ASSERT_FALSE(fs::exists(saved_path));
  TEST_ASSERT_FALSE(fs::exists(file1));
  TEST_ASSERT_FALSE(fs::exists(file2));

  // Second cleanup call should be safe no-op
  ws.cleanup();

  cleanup_test_dir(base);
}

void test_scratch_move_semantics(void) {
  std::string base = create_test_temp_dir();
  std::string original_path;

  {
    virmux::ScratchWorkspace ws1(base);
    TEST_ASSERT_TRUE(ws1.init());
    original_path = ws1.path();
    TEST_ASSERT_TRUE(fs::exists(original_path));

    // Move construct ws2 from ws1
    virmux::ScratchWorkspace ws2(std::move(ws1));
    // Asserting the documented moved-from state is the point here.
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    TEST_ASSERT_FALSE(ws1.is_valid());
    TEST_ASSERT_TRUE(ws1.path().empty());
    TEST_ASSERT_TRUE(ws2.is_valid());
    TEST_ASSERT_EQUAL_STRING(original_path.c_str(), ws2.path().c_str());
    TEST_ASSERT_TRUE(fs::exists(original_path));

    // Move assign ws3 from ws2
    virmux::ScratchWorkspace ws3(base);
    TEST_ASSERT_TRUE(ws3.init());
    std::string ws3_path = ws3.path();
    TEST_ASSERT_TRUE(fs::exists(ws3_path));

    ws3 = std::move(ws2);
    // ws3's original directory should be cleaned up by move assignment
    TEST_ASSERT_FALSE(fs::exists(ws3_path));

    // ws2 should be moved-from
    // Asserting the documented moved-from state is the point here.
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    TEST_ASSERT_FALSE(ws2.is_valid());
    TEST_ASSERT_TRUE(ws2.path().empty());

    // ws3 has original_path
    TEST_ASSERT_TRUE(ws3.is_valid());
    TEST_ASSERT_EQUAL_STRING(original_path.c_str(), ws3.path().c_str());
    TEST_ASSERT_TRUE(fs::exists(original_path));
  }

  // After exiting block, ws3 destructor must have cleaned up original_path
  TEST_ASSERT_FALSE(fs::exists(original_path));

  cleanup_test_dir(base);
}

void test_scratch_unique_paths(void) {
  std::string base = create_test_temp_dir();
  virmux::ScratchWorkspace ws1(base);
  virmux::ScratchWorkspace ws2(base);

  TEST_ASSERT_TRUE(ws1.init());
  TEST_ASSERT_TRUE(ws2.init());

  TEST_ASSERT_TRUE(ws1.is_valid());
  TEST_ASSERT_TRUE(ws2.is_valid());
  TEST_ASSERT_TRUE(ws1.path() != ws2.path());
  TEST_ASSERT_TRUE(fs::exists(ws1.path()));
  TEST_ASSERT_TRUE(fs::exists(ws2.path()));

  ws1.cleanup();
  ws2.cleanup();
  cleanup_test_dir(base);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_scratch_init_creates_dir_and_mode);
  RUN_TEST(test_scratch_default_base_dir);
  RUN_TEST(test_scratch_populate_env);
  RUN_TEST(test_scratch_write_files_and_cleanup);
  RUN_TEST(test_scratch_move_semantics);
  RUN_TEST(test_scratch_unique_paths);
  return UNITY_END();
}
