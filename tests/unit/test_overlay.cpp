/**
 * @file test_overlay.cpp
 * @brief Unit tests for unprivileged OverlayFS workspace controller.
 */

extern "C" {
#include "unity/unity.h"
}

#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "virmux/overlay.hpp"

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

/**
 * @brief Runs @p body in a child created with clone() and returns its pid.
 *
 * unshare(CLONE_NEWUSER) requires a single-threaded caller. A fork() child is
 * not guaranteed to be one: ThreadSanitizer starts a background thread in
 * every child of a single-threaded parent, so the namespace tests would fail
 * under TSan for reasons unrelated to the code under test. clone() children
 * (and namespaces requested via @p extra_flags) carry no such helper thread.
 *
 * The body's return value becomes the child's exit status. The child gets a
 * copy-on-write view of the parent's memory (no CLONE_VM), so the stack
 * mapping can be released in the parent as soon as clone() returns.
 */
template <typename Body>
pid_t clone_child(Body& body, int extra_flags) {
  constexpr size_t kStackSize = 1024 * 1024;
  void* stack = ::mmap(nullptr, kStackSize, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
  if (stack == MAP_FAILED) {
    return -1;
  }
  pid_t pid = ::clone(
      [](void* arg) -> int { return (*static_cast<Body*>(arg))(); },
      static_cast<char*>(stack) + kStackSize, extra_flags | SIGCHLD, &body);
  ::munmap(stack, kStackSize);
  return pid;
}

}  // namespace

void setUp(void) {}
void tearDown(void) {}

void test_overlay_setup_creates_layout(void) {
  std::string lower = create_test_temp_dir("virmux_ovl_lower");
  std::string runtime = create_test_temp_dir("virmux_ovl_rt");

  virmux::OverlayConfig cfg{.lower_dir = lower, .runtime_dir = runtime};
  virmux::OverlayWorkspace ws(cfg);

  TEST_ASSERT_FALSE(ws.is_mounted());
  TEST_ASSERT_TRUE(ws.merged_dir().empty());
  TEST_ASSERT_TRUE(ws.upper_dir().empty());
  TEST_ASSERT_TRUE(ws.work_dir().empty());
  TEST_ASSERT_TRUE(ws.session_dir().empty());

  TEST_ASSERT_TRUE(ws.setup());
  TEST_ASSERT_FALSE(ws.session_dir().empty());
  TEST_ASSERT_FALSE(ws.upper_dir().empty());
  TEST_ASSERT_FALSE(ws.work_dir().empty());
  TEST_ASSERT_FALSE(ws.merged_dir().empty());
  TEST_ASSERT_EQUAL_STRING(lower.c_str(), ws.lower_dir().c_str());
  TEST_ASSERT_FALSE(ws.is_mounted());

  // Verify directories exist on filesystem
  TEST_ASSERT_TRUE(fs::is_directory(ws.session_dir()));
  TEST_ASSERT_TRUE(fs::is_directory(ws.upper_dir()));
  TEST_ASSERT_TRUE(fs::is_directory(ws.work_dir()));
  TEST_ASSERT_TRUE(fs::is_directory(ws.merged_dir()));

  // Verify permissions (0700)
  struct stat st{};
  TEST_ASSERT_EQUAL_INT(0, ::stat(ws.session_dir().c_str(), &st));
  TEST_ASSERT_EQUAL_HEX(0700, st.st_mode & 0777);

  std::string saved_session = ws.session_dir();
  ws.teardown();

  TEST_ASSERT_FALSE(fs::exists(saved_session));
  TEST_ASSERT_TRUE(ws.session_dir().empty());
  TEST_ASSERT_TRUE(ws.upper_dir().empty());
  TEST_ASSERT_TRUE(ws.work_dir().empty());
  TEST_ASSERT_TRUE(ws.merged_dir().empty());

  cleanup_test_dir(lower);
  cleanup_test_dir(runtime);
}

void test_overlay_setup_invalid_lower_dir(void) {
  std::string runtime = create_test_temp_dir("virmux_ovl_rt_inv");

  // Empty lower dir
  {
    virmux::OverlayWorkspace ws({.lower_dir = "", .runtime_dir = runtime});
    TEST_ASSERT_FALSE(ws.setup());
  }

  // Nonexistent lower dir
  {
    virmux::OverlayWorkspace ws(
        {.lower_dir = "/tmp/virmux_nonexistent_dir_99999",
         .runtime_dir = runtime});
    TEST_ASSERT_FALSE(ws.setup());
  }

  // Lower dir is a regular file, not a directory
  {
    std::string file_path = runtime + "/regular_file.txt";
    std::ofstream ofs(file_path);
    ofs << "not a directory";
    ofs.close();

    virmux::OverlayWorkspace ws(
        {.lower_dir = file_path, .runtime_dir = runtime});
    TEST_ASSERT_FALSE(ws.setup());
  }

  cleanup_test_dir(runtime);
}

/**
 * @brief How the child under test obtains its user + mount namespace.
 */
enum class NamespaceEntry {
  /// clone() a plain child that calls enter_user_mount_namespace() (unshare).
  kUnshareInChild,
  /// clone(CLONE_NEWUSER | CLONE_NEWNS) and call
  /// configure_user_mount_namespace() in the already-isolated child.
  kCreatedAtClone,
};

/**
 * @brief Mounts @p ws in a namespaced child, mutates the merged view there,
 * and asserts in the parent that the lower dir stayed untouched while the
 * upper dir captured every change.
 */
void run_cow_isolation_case(NamespaceEntry entry) {
  std::string lower = create_test_temp_dir("virmux_ovl_lower_cow");
  std::string runtime = create_test_temp_dir("virmux_ovl_rt_cow");

  // Create baseline file in lower_dir
  std::string base_file = lower + "/base.txt";
  {
    std::ofstream ofs(base_file);
    ofs << "initial base data\n";
  }

  virmux::OverlayConfig cfg{.lower_dir = lower, .runtime_dir = runtime};
  virmux::OverlayWorkspace ws(cfg);
  TEST_ASSERT_TRUE(ws.setup());

  uid_t orig_uid = ::getuid();
  gid_t orig_gid = ::getgid();

  // Child body: the return value is the child's exit status (0 = success).
  auto child_body = [&]() -> int {
    bool ns_ready =
        entry == NamespaceEntry::kUnshareInChild
            ? virmux::OverlayWorkspace::enter_user_mount_namespace(orig_uid,
                                                                   orig_gid)
            : virmux::OverlayWorkspace::configure_user_mount_namespace(
                  orig_uid, orig_gid);
    if (!ns_ready) {
      return 10;
    }

    if (!ws.mount_in_current_namespace()) {
      return 11;
    }

    if (!ws.is_mounted()) {
      return 12;
    }

    // Verify reading original base file through merged_dir
    std::string merged_base = ws.merged_dir() + "/base.txt";
    std::ifstream ifs(merged_base);
    if (!ifs.is_open()) {
      return 13;
    }
    std::string content;
    std::getline(ifs, content);
    ifs.close();
    if (content != "initial base data") {
      return 14;
    }

    // Create a new file in merged_dir
    std::string merged_new = ws.merged_dir() + "/added.txt";
    {
      std::ofstream ofs_new(merged_new);
      if (!ofs_new.is_open()) {
        return 15;
      }
      ofs_new << "new file in merged\n";
    }

    // Modify the existing file in merged_dir
    {
      std::ofstream ofs_mod(merged_base);
      if (!ofs_mod.is_open()) {
        return 16;
      }
      ofs_mod << "modified in merged\n";
    }

    // Verify inside child that modified file shows updated contents
    std::ifstream ifs_mod(merged_base);
    if (!ifs_mod.is_open()) {
      return 17;
    }
    std::string mod_content;
    std::getline(ifs_mod, mod_content);
    ifs_mod.close();
    if (mod_content != "modified in merged") {
      return 18;
    }

    // Verify inside child that added file exists in upper_dir
    std::string upper_new = ws.upper_dir() + "/added.txt";
    if (!fs::exists(upper_new)) {
      return 19;
    }
    return 0;
  };

  int ns_flags = entry == NamespaceEntry::kCreatedAtClone
                     ? (CLONE_NEWUSER | CLONE_NEWNS)
                     : 0;
  pid_t child = clone_child(child_body, ns_flags);
  TEST_ASSERT_TRUE(child > 0);

  // Parent waits for child
  int status = 0;
  pid_t w = ::waitpid(child, &status, 0);
  TEST_ASSERT_EQUAL_INT(child, w);
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));

  // Invariant 1: lower_dir file is COMPLETELY UNTOUCHED
  {
    std::ifstream ifs(base_file);
    TEST_ASSERT_TRUE(ifs.is_open());
    std::string lower_content;
    std::getline(ifs, lower_content);
    ifs.close();
    TEST_ASSERT_EQUAL_STRING("initial base data", lower_content.c_str());
  }

  // Invariant 2: lower_dir has NO added file (zero lower pollution)
  std::string lower_added = lower + "/added.txt";
  TEST_ASSERT_FALSE(fs::exists(lower_added));

  // Invariant 3: upper_dir captured all modifications and additions
  std::string upper_added = ws.upper_dir() + "/added.txt";
  std::string upper_base = ws.upper_dir() + "/base.txt";
  TEST_ASSERT_TRUE(fs::exists(upper_added));
  TEST_ASSERT_TRUE(fs::exists(upper_base));

  {
    std::ifstream ifs_mod(upper_base);
    TEST_ASSERT_TRUE(ifs_mod.is_open());
    std::string upper_content;
    std::getline(ifs_mod, upper_content);
    ifs_mod.close();
    TEST_ASSERT_EQUAL_STRING("modified in merged", upper_content.c_str());
  }

  // Invariant 4: teardown cleanly cleanses and removes temporary dirs
  std::string saved_session = ws.session_dir();
  ws.teardown();
  TEST_ASSERT_FALSE(fs::exists(saved_session));

  cleanup_test_dir(lower);
  cleanup_test_dir(runtime);
}

void test_overlay_unprivileged_mount_cow_isolation(void) {
  run_cow_isolation_case(NamespaceEntry::kUnshareInChild);
}

void test_overlay_cow_isolation_with_namespace_created_at_clone(void) {
  run_cow_isolation_case(NamespaceEntry::kCreatedAtClone);
}

void test_overlay_mount_without_setup_fails(void) {
  virmux::OverlayWorkspace ws({.lower_dir = "/tmp", .runtime_dir = "/tmp"});
  TEST_ASSERT_FALSE(ws.mount_in_current_namespace());
  TEST_ASSERT_FALSE(ws.is_mounted());
}

void test_overlay_mount_without_user_namespace_fails(void) {
  std::string lower = create_test_temp_dir("virmux_ovl_lower_fail");
  std::string runtime = create_test_temp_dir("virmux_ovl_rt_fail");

  virmux::OverlayConfig cfg{.lower_dir = lower, .runtime_dir = runtime};
  virmux::OverlayWorkspace ws(cfg);
  TEST_ASSERT_TRUE(ws.setup());

  pid_t child = ::fork();
  TEST_ASSERT_TRUE(child >= 0);

  if (child == 0) {
    // Attempting to mount without enter_user_mount_namespace should fail
    // (EPERM)
    bool mounted = ws.mount_in_current_namespace();
    ::_exit(mounted ? 1 : 0);
  }

  int status = 0;
  pid_t w = ::waitpid(child, &status, 0);
  TEST_ASSERT_EQUAL_INT(child, w);
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));

  ws.teardown();
  cleanup_test_dir(lower);
  cleanup_test_dir(runtime);
}

void test_overlay_move_semantics(void) {
  std::string lower = create_test_temp_dir("virmux_ovl_lower_mv");
  std::string runtime = create_test_temp_dir("virmux_ovl_rt_mv");

  std::string original_session;
  {
    virmux::OverlayWorkspace ws1({.lower_dir = lower, .runtime_dir = runtime});
    TEST_ASSERT_TRUE(ws1.setup());
    original_session = ws1.session_dir();
    TEST_ASSERT_TRUE(fs::exists(original_session));

    // Move construct ws2 from ws1
    virmux::OverlayWorkspace ws2(std::move(ws1));
    // Asserting the documented moved-from state is the point here.
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    TEST_ASSERT_TRUE(ws1.session_dir().empty());
    TEST_ASSERT_TRUE(ws1.merged_dir().empty());
    TEST_ASSERT_TRUE(ws1.upper_dir().empty());
    TEST_ASSERT_TRUE(ws1.work_dir().empty());

    TEST_ASSERT_EQUAL_STRING(original_session.c_str(),
                             ws2.session_dir().c_str());
    TEST_ASSERT_TRUE(fs::exists(ws2.session_dir()));

    // Move assign ws3 from ws2
    virmux::OverlayWorkspace ws3({.lower_dir = lower, .runtime_dir = runtime});
    TEST_ASSERT_TRUE(ws3.setup());
    std::string ws3_session = ws3.session_dir();
    TEST_ASSERT_TRUE(fs::exists(ws3_session));

    ws3 = std::move(ws2);
    // ws3's original session directory should be cleaned up by move assignment
    TEST_ASSERT_FALSE(fs::exists(ws3_session));
    // Asserting the documented moved-from state is the point here.
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    TEST_ASSERT_TRUE(ws2.session_dir().empty());
    TEST_ASSERT_EQUAL_STRING(original_session.c_str(),
                             ws3.session_dir().c_str());
    TEST_ASSERT_TRUE(fs::exists(original_session));
  }

  // After destructor of ws3, original_session must be deleted
  TEST_ASSERT_FALSE(fs::exists(original_session));

  cleanup_test_dir(lower);
  cleanup_test_dir(runtime);
}

void test_overlay_default_runtime_dir(void) {
  std::string lower = create_test_temp_dir("virmux_ovl_lower_def");

  virmux::OverlayConfig cfg{.lower_dir = lower, .runtime_dir = ""};
  virmux::OverlayWorkspace ws(cfg);
  TEST_ASSERT_TRUE(ws.setup());
  TEST_ASSERT_FALSE(ws.session_dir().empty());

  std::string run_user_prefix = "/run/user/" + std::to_string(::getuid());
  bool starts_with_run = (ws.session_dir().rfind(run_user_prefix, 0) == 0);
  bool starts_with_tmp =
      (ws.session_dir().rfind("/tmp/virmux/overlay", 0) == 0);
  TEST_ASSERT_TRUE(starts_with_run || starts_with_tmp);

  std::string saved_session = ws.session_dir();
  ws.teardown();
  TEST_ASSERT_FALSE(fs::exists(saved_session));

  cleanup_test_dir(lower);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_overlay_setup_creates_layout);
  RUN_TEST(test_overlay_setup_invalid_lower_dir);
  RUN_TEST(test_overlay_unprivileged_mount_cow_isolation);
  RUN_TEST(test_overlay_cow_isolation_with_namespace_created_at_clone);
  RUN_TEST(test_overlay_mount_without_setup_fails);
  RUN_TEST(test_overlay_mount_without_user_namespace_fails);
  RUN_TEST(test_overlay_move_semantics);
  RUN_TEST(test_overlay_default_runtime_dir);
  return UNITY_END();
}
