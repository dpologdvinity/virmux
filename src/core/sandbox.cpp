/**
 * @file sandbox.cpp
 * @brief Implementation of command sandbox launcher and process isolation
 * controller.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "virmux/sandbox.hpp"

#include <fcntl.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "virmux/cgroup.hpp"
#include "virmux/daemon.hpp"
#include "virmux/gate.hpp"
#include "virmux/intercept.h"
#include "virmux/overlay.hpp"
#include "virmux/scratch.hpp"

namespace virmux {

namespace {

/**
 * @brief Format synthetic /proc/meminfo content based on configured memory
 * limit.
 * @param memory_limit_bytes Soft/hard memory ceiling in bytes.
 * @return Formatted meminfo text string.
 */
std::string generate_synthetic_meminfo(uint64_t memory_limit_bytes) {
  uint64_t total_kb = memory_limit_bytes / 1024;
  uint64_t free_kb = (total_kb * 3) / 4;
  uint64_t avail_kb = (total_kb * 7) / 8;
  uint64_t swap_total_kb = total_kb / 4;
  uint64_t swap_free_kb = swap_total_kb;

  std::ostringstream oss;
  oss << "MemTotal:        " << total_kb << " kB\n"
      << "MemFree:         " << free_kb << " kB\n"
      << "MemAvailable:    " << avail_kb << " kB\n"
      << "SwapTotal:       " << swap_total_kb << " kB\n"
      << "SwapFree:        " << swap_free_kb << " kB\n";
  return oss.str();
}

/**
 * @brief RAII guard ensuring temporary synthetic meminfo file is unlinked.
 */
struct MeminfoGuard {
  std::string path;
  ~MeminfoGuard() {
    if (!path.empty()) {
      std::error_code ec;
      std::filesystem::remove(path, ec);
    }
  }
};

/**
 * @brief Write the entire buffer to fd, retrying on EINTR and short writes.
 * @return true only if every byte was written.
 */
bool write_all(int fd, const std::string& data) {
  const char* p = data.data();
  size_t remaining = data.size();
  while (remaining > 0) {
    ssize_t written = ::write(fd, p, remaining);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    p += written;
    remaining -= static_cast<size_t>(written);
  }
  return true;
}

/**
 * @brief Resolve absolute path to libvirmux_intercept.so.
 * @return Absolute canonical path if found, or empty string.
 */
std::string resolve_interceptor_path() {
  const char* env_path = std::getenv("VIRMUX_INTERCEPT_LIB");
  if (!env_path) {
    env_path = std::getenv("VIRMUX_INTERCEPT_PATH");
  }
  std::error_code ec;
  if (env_path && std::filesystem::exists(env_path, ec)) {
    return std::filesystem::canonical(env_path, ec).string();
  }

  auto self_exe = std::filesystem::canonical("/proc/self/exe", ec);
  if (!ec) {
    auto bin_dir = self_exe.parent_path();
    auto cand1 = bin_dir / "libvirmux_intercept.so";
    if (std::filesystem::exists(cand1, ec)) {
      return std::filesystem::canonical(cand1, ec).string();
    }
    auto cand2 = bin_dir / "src" / "intercept" / "libvirmux_intercept.so";
    if (std::filesystem::exists(cand2, ec)) {
      return std::filesystem::canonical(cand2, ec).string();
    }
    auto cand3 = bin_dir.parent_path() / "lib" / "libvirmux_intercept.so";
    if (std::filesystem::exists(cand3, ec)) {
      return std::filesystem::canonical(cand3, ec).string();
    }
    auto cand4 = bin_dir.parent_path() / "lib64" / "libvirmux_intercept.so";
    if (std::filesystem::exists(cand4, ec)) {
      return std::filesystem::canonical(cand4, ec).string();
    }
  }

  auto cwd = std::filesystem::current_path(ec);
  if (!ec) {
    auto cand_cwd = cwd / "build" / "libvirmux_intercept.so";
    if (std::filesystem::exists(cand_cwd, ec)) {
      return std::filesystem::canonical(cand_cwd, ec).string();
    }
    auto cand_cwd2 = cwd / "libvirmux_intercept.so";
    if (std::filesystem::exists(cand_cwd2, ec)) {
      return std::filesystem::canonical(cand_cwd2, ec).string();
    }
  }

  const std::string sys_paths[] = {
      "/usr/local/lib/libvirmux_intercept.so",
      "/usr/lib/libvirmux_intercept.so",
      "/usr/lib/x86_64-linux-gnu/libvirmux_intercept.so",
  };
  for (const auto& p : sys_paths) {
    if (std::filesystem::exists(p, ec)) {
      return std::filesystem::canonical(p, ec).string();
    }
  }

  return "";
}

std::string expand_home_path(const std::string& path) {
  if (path == "~") {
    const char* home = std::getenv("HOME");
    if (home != nullptr && *home != '\0') {
      return home;
    }
  } else if (path.starts_with("~/")) {
    const char* home = std::getenv("HOME");
    if (home != nullptr && *home != '\0') {
      return std::string(home) + path.substr(1);
    }
  }
  return path;
}

static std::atomic<uint64_t> s_sandbox_counter{0};

/**
 * @brief Everything the sandbox child needs, prepared by the parent before
 * clone().
 *
 * The child runs between clone() and execve(). If the caller is
 * multi-threaded (spawn_command is a public library API), other threads may
 * hold malloc, stdio or environment locks at clone time, and those stay locked
 * forever in the child. The child therefore performs only async-signal-safe
 * work (raw syscalls on pre-built strings); every allocation, path join and
 * environment edit happens here in the parent.
 */
struct ChildLaunchPlan {
  char* const* argv{nullptr};
  char* const* envp{nullptr};
  /// Non-null: the child was cloned into fresh user + mount namespaces and
  /// must configure them and mount this overlay before chdir-ing into it.
  OverlayWorkspace* overlay{nullptr};
  uid_t orig_uid{0};
  gid_t orig_gid{0};
  /// Working directory to enter (nullptr = inherit the parent's).
  const char* workdir{nullptr};
  /// Overlay only: fallback when @ref workdir is not reachable in the mount.
  const char* workdir_fallback{nullptr};
  /// "<cgroup>/cgroup.procs" to join before exec (nullptr = none).
  const char* cgroup_procs_path{nullptr};
  bool apply_nproc_limit{false};
  rlim_t nproc_limit{0};
};

/// oom_score_adj applied to sandboxed children so they are sacrificed first.
constexpr char kSandboxOomScoreAdj[] = "800\n";

/// Stack for the cloned child; it only runs a few syscalls before execve().
constexpr size_t kChildStackSize = size_t{1024} * 1024;

/** @brief Writes NUL-terminated @p text to stderr. Async-signal-safe. */
void child_report(const char* text) noexcept {
  ssize_t ignored = ::write(STDERR_FILENO, text, std::strlen(text));
  (void)ignored;
}

/** @brief Writes @p data to the file at @p path. Async-signal-safe. */
bool child_write_file(const char* path, const char* data) noexcept {
  int fd = ::open(path, O_WRONLY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  size_t len = std::strlen(data);
  ssize_t written = ::write(fd, data, len);
  ::close(fd);
  return written == static_cast<ssize_t>(len);
}

/**
 * @brief Entry point of the cloned sandbox child. Async-signal-safe only.
 * @return Exit status if execve() fails (126 = setup/exec error, 127 = not
 * found or workspace unreachable); does not return on success.
 */
int sandbox_child_main(void* arg) {
  const auto& plan = *static_cast<const ChildLaunchPlan*>(arg);

  // 1. Ephemeral: the kernel already placed us in new user + mount
  // namespaces at clone() time; map IDs and mount the overlay.
  if (plan.overlay != nullptr) {
    if (!OverlayWorkspace::configure_user_mount_namespace(plan.orig_uid,
                                                          plan.orig_gid)) {
      child_report("error: [overlay] failed to enter user/mount namespace\n");
      return 126;
    }
    if (!plan.overlay->mount_in_current_namespace()) {
      child_report("error: [overlay] failed to mount overlayfs in namespace\n");
      return 126;
    }
    if (::chdir(plan.workdir) != 0 && ::chdir(plan.workdir_fallback) != 0) {
      child_report("error: [overlay] failed to chdir to workspace '");
      child_report(plan.workdir);
      child_report("'\n");
      return 127;
    }
  } else if (plan.workdir != nullptr && ::chdir(plan.workdir) != 0) {
    return 127;
  }

  // 2. Attach to the cgroup before exec so grandchildren are always covered.
  if (plan.cgroup_procs_path != nullptr) {
    (void)child_write_file(plan.cgroup_procs_path, "0\n");
  }

  // Fork-bomb fallback: RLIMIT_NPROC ceiling when cgroup pids.max is absent.
  if (plan.apply_nproc_limit) {
    rlim_t limit = plan.nproc_limit;
    struct rlimit cur_rl;
    if (::getrlimit(RLIMIT_NPROC, &cur_rl) == 0 && limit > cur_rl.rlim_max) {
      limit = cur_rl.rlim_max;
    }
    struct rlimit rl{limit, limit};
    (void)::setrlimit(RLIMIT_NPROC, &rl);
  }

  // 3. Sacrificial OOM priority.
  (void)child_write_file("/proc/self/oom_score_adj", kSandboxOomScoreAdj);

  // 4. Execute with the pre-built environment (PATH search uses the
  // inherited, unmodified PATH).
  ::execvpe(plan.argv[0], plan.argv, plan.envp);
  return errno == ENOENT ? 127 : 126;
}

/**
 * @brief Copies the current environment, replacing or adding @p overrides.
 * @return Owned "KEY=VALUE" strings; point an envp array into them.
 */
std::vector<std::string> build_child_environment(
    const std::vector<std::pair<std::string, std::string>>& overrides) {
  std::vector<std::string> env;
  for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
    std::string_view kv(*entry);
    std::string_view key = kv.substr(0, kv.find('='));
    bool overridden = std::any_of(
        overrides.begin(), overrides.end(),
        [key](const auto& ov) { return key == std::string_view(ov.first); });
    if (!overridden) {
      env.emplace_back(kv);
    }
  }
  for (const auto& [key, value] : overrides) {
    std::string entry = key;
    entry += '=';
    entry += value;
    env.push_back(std::move(entry));
  }
  return env;
}

/**
 * @brief Starts sandbox_child_main(@p plan) via clone() and returns its pid.
 *
 * clone() is used instead of fork() so that, for ephemeral runs, the user and
 * mount namespaces are created atomically with the process. The alternative,
 * unshare(CLONE_NEWUSER) after fork(), fails with EINVAL whenever the child is
 * not single-threaded, which happens in practice: ThreadSanitizer starts a
 * background thread in every fork() child, and preloaded libraries may start
 * threads from pthread_atfork() handlers. No CLONE_VM: the child gets a
 * copy-on-write address space, so the stack can be unmapped right away.
 *
 * Sanitizer caveat (accepted): clone() bypasses the sanitizer runtimes'
 * fork-time lock handling, so a sanitizer-internal lock held by another
 * thread at clone time would stay held in the child. The child only issues
 * a few intercepted libc calls before execvpe(), and no hang has been
 * observed across repeated TSan/ASan runs; production builds are unaffected.
 *
 * @return Child pid, or -1 on failure.
 */
pid_t clone_sandbox_child(ChildLaunchPlan& plan, bool new_namespaces) {
  void* stack = ::mmap(nullptr, kChildStackSize, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
  if (stack == MAP_FAILED) {
    return -1;
  }
  int flags = SIGCHLD;
  if (new_namespaces) {
    flags |= CLONE_NEWUSER | CLONE_NEWNS;
  }
  pid_t pid =
      ::clone(sandbox_child_main, static_cast<char*>(stack) + kChildStackSize,
              flags, &plan);
  ::munmap(stack, kChildStackSize);
  return pid;
}

uint64_t count_user_threads() {
  uint64_t count = 0;
  uid_t uid = ::getuid();
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator("/proc", ec)) {
    if (ec) break;
    const auto filename = entry.path().filename().native();
    if (filename.empty() ||
        !std::isdigit(static_cast<unsigned char>(filename[0]))) {
      continue;
    }
    auto status_path = entry.path() / "status";
    int fd = ::open(status_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) continue;
    char buf[4096];
    ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);
    if (n <= 0) continue;
    buf[n] = '\0';
    const char* uid_ptr = std::strstr(buf, "Uid:\t");
    if (!uid_ptr) continue;
    long proc_uid = std::strtol(uid_ptr + 5, nullptr, 10);
    if (static_cast<uid_t>(proc_uid) != uid) continue;
    const char* th_ptr = std::strstr(buf, "Threads:\t");
    if (th_ptr) {
      long th = std::strtol(th_ptr + 9, nullptr, 10);
      if (th > 0) {
        count += static_cast<uint64_t>(th);
      }
    } else {
      count += 1;
    }
  }
  return count;
}

}  // anonymous namespace

AdmissionGate& get_default_admission_gate() {
  static AdmissionGate default_gate(2, std::chrono::seconds(10));
  return default_gate;
}

bool set_process_oom_score(pid_t pid, int score) {
  int clamped = std::clamp(score, -1000, 1000);
  char path[64];
  if (pid == 0) {
    std::snprintf(path, sizeof(path), "/proc/self/oom_score_adj");
  } else {
    std::snprintf(path, sizeof(path), "/proc/%d/oom_score_adj",
                  static_cast<int>(pid));
  }
  int fd = ::open(path, O_WRONLY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  std::string val = std::to_string(clamped) + "\n";
  ssize_t written = ::write(fd, val.data(), val.size());
  ::close(fd);
  if (written < 0 || static_cast<size_t>(written) != val.size()) {
    return false;
  }
  return true;
}

void set_worker_oom_score(pid_t child_pid) {
  (void)set_process_oom_score(child_pid, 800);
}

int spawn_command(const SandboxConfig& cfg, char* const argv[]) {
  if (argv == nullptr || argv[0] == nullptr || argv[0][0] == '\0') {
    return -1;
  }

  // Acquire admission token if configured as heavy compute task
  bool is_heavy_task = cfg.is_heavy || cfg.heavy;
  std::unique_ptr<ScopedToken> token;
  if (is_heavy_task) {
    AdmissionGate& gate = cfg.gate ? *cfg.gate : get_default_admission_gate();
    token = std::make_unique<ScopedToken>(gate);
    if (!token->acquired()) {
      return -1;
    }
  }

  // Pre-fork workspace resolution:
  // If cfg.workspace_dir (or cfg.workspace_path) is non-empty, expand ~ and
  // resolve canonical path. If empty, use current working directory.
  std::string raw_ws =
      !cfg.workspace_dir.empty() ? cfg.workspace_dir : cfg.workspace_path;
  bool explicit_workspace = !raw_ws.empty();
  if (raw_ws.empty()) {
    std::error_code ec;
    raw_ws = std::filesystem::current_path(ec).string();
  }

  std::string expanded_ws = expand_home_path(raw_ws);
  std::error_code ws_ec;
  std::filesystem::path canonical_ws =
      std::filesystem::canonical(expanded_ws, ws_ec);
  std::string resolved_workspace =
      (!ws_ec) ? canonical_ws.string() : expanded_ws;

  // Ephemeral workspace setup (OverlayFS)
  bool is_ephemeral = cfg.ephemeral;
  std::unique_ptr<OverlayWorkspace> overlay;
  if (is_ephemeral) {
    OverlayConfig ovl_cfg{
        .lower_dir = resolved_workspace,
        .runtime_dir = "",
    };
    overlay = std::make_unique<OverlayWorkspace>(ovl_cfg);
    if (!overlay->setup()) {
      std::fprintf(
          stderr,
          "error: [overlay] failed to setup ephemeral workspace layout\n");
      return -1;
    }
  }

  // In-memory scratch space setup
  std::unique_ptr<ScratchWorkspace> scratch;
  if (is_ephemeral || !cfg.scratch_dir.empty()) {
    scratch = std::make_unique<ScratchWorkspace>(cfg.scratch_dir);
    if (!scratch->init()) {
      scratch.reset();
    }
  }

  // Capture user / group IDs before unshare
  uid_t orig_uid = ::getuid();
  gid_t orig_gid = ::getgid();

  // Generate synthetic /proc/meminfo file with unique atomic sequence per call
  uint64_t seq = s_sandbox_counter.fetch_add(1, std::memory_order_relaxed);
  std::string meminfo_path = "/tmp/virmux_" + std::to_string(::getpid()) + "_" +
                             std::to_string(seq) + "_meminfo";
  int mem_fd = ::open(meminfo_path.c_str(),
                      O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  // Only advertise the spoofed file to the interceptor if it was fully
  // written; a truncated file would corrupt the child's /proc/meminfo view.
  bool meminfo_ready = false;
  if (mem_fd >= 0) {
    std::string mem_content =
        generate_synthetic_meminfo(cfg.memory_limit_bytes);
    meminfo_ready = write_all(mem_fd, mem_content);
    ::close(mem_fd);
  }
  MeminfoGuard meminfo_guard{meminfo_path};

  // Pre-fork hygiene: resolve interceptor path and prepare environment strings
  // before fork to avoid dynamic allocations or path lookup failures after
  // chdir
  std::string intercept_lib = resolve_interceptor_path();
  std::string preload_val;
  if (!intercept_lib.empty()) {
    const char* existing_preload = std::getenv("LD_PRELOAD");
    if (existing_preload && existing_preload[0] != '\0') {
      std::string_view existing_view(existing_preload);
      // If running under AddressSanitizer or ThreadSanitizer, ASan runtime must
      // come first
      if (existing_view.find("libasan") != std::string_view::npos ||
          existing_view.find("libtsan") != std::string_view::npos ||
          existing_view.find("libubsan") != std::string_view::npos) {
        preload_val = std::string(existing_preload) + ":" + intercept_lib;
      } else {
        preload_val = intercept_lib + ":" + std::string(existing_preload);
      }
    } else {
      preload_val = intercept_lib;
    }
  }

  std::string vcores_str = std::to_string(cfg.virtual_cores);

  // Set up cgroups v2 slice with unique sub-slice per run (gracefully falls
  // back if unprivileged)
  std::string slice_name = "virmux-run-" + std::to_string(::getpid()) + "-" +
                           std::to_string(seq) + ".slice";
  CgroupSlice slice(slice_name);
  bool cgroup_pids_active = false;
  slice.setup("", cfg.memory_limit_bytes, cfg.cpuset_cores);
  if (slice.is_valid() && cfg.max_pids > 0) {
    cgroup_pids_active = slice.set_pids_max(cfg.max_pids);
  }

  // Only scan procfs and compute fallback RLIMIT_NPROC if cgroups pids.max is
  // unavailable
  uint64_t baseline_threads =
      (cfg.max_pids > 0 && !cgroup_pids_active) ? count_user_threads() : 0;

  // Child environment, built here because setenv() after clone() is not
  // async-signal-safe.
  std::vector<std::pair<std::string, std::string>> env_overrides;
  if (scratch && scratch->is_valid()) {
    env_overrides.emplace_back("TMPDIR", scratch->path());
    env_overrides.emplace_back("TEMP", scratch->path());
    env_overrides.emplace_back("TMP", scratch->path());
  }
  if (!preload_val.empty()) {
    env_overrides.emplace_back("LD_PRELOAD", preload_val);
  }
  env_overrides.emplace_back(VIRMUX_ENV_VIRTUAL_CORES, vcores_str);
  if (meminfo_ready) {
    env_overrides.emplace_back(VIRMUX_ENV_SPOOFED_MEMINFO, meminfo_path);
  }
  env_overrides.emplace_back("OMP_NUM_THREADS", vcores_str);
  env_overrides.emplace_back("OPENBLAS_NUM_THREADS", vcores_str);
  env_overrides.emplace_back("MKL_NUM_THREADS", vcores_str);
  env_overrides.emplace_back("NUMEXPR_NUM_THREADS", vcores_str);
  std::vector<std::string> child_env = build_child_environment(env_overrides);
  std::vector<char*> child_envp;
  child_envp.reserve(child_env.size() + 1);
  for (auto& entry : child_env) {
    child_envp.push_back(entry.data());
  }
  child_envp.push_back(nullptr);

  const std::string cgroup_procs_path =
      slice.is_valid() ? slice.path() + "/cgroup.procs" : std::string();

  ChildLaunchPlan plan;
  plan.argv = argv;
  plan.envp = child_envp.data();
  // An already-mounted overlay needs no namespaces (and no chdir) in the child.
  const bool enter_namespaces =
      is_ephemeral && overlay && !overlay->is_mounted();
  if (enter_namespaces) {
    plan.overlay = overlay.get();
    plan.orig_uid = orig_uid;
    plan.orig_gid = orig_gid;
    plan.workdir = resolved_workspace.c_str();
    plan.workdir_fallback = overlay->merged_dir().c_str();
  } else if (explicit_workspace) {
    plan.workdir = resolved_workspace.c_str();
  }
  if (!cgroup_procs_path.empty()) {
    plan.cgroup_procs_path = cgroup_procs_path.c_str();
  }
  if (cfg.max_pids > 0 && !cgroup_pids_active) {
    plan.apply_nproc_limit = true;
    plan.nproc_limit = static_cast<rlim_t>(baseline_threads + cfg.max_pids);
  }

  pid_t pid = clone_sandbox_child(plan, enter_namespaces);
  if (pid < 0) {
    if (scratch) {
      scratch->cleanup();
    }
    if (overlay) {
      overlay->teardown();
    }
    return -1;
  }

  // --- In parent process ---
  if (slice.is_valid()) {
    slice.attach_pid(pid);
  }

  // Check if supervisor daemon is active and register session
  std::string cmd_str;
  for (int i = 0; argv[i] != nullptr; ++i) {
    if (i > 0) {
      cmd_str += ' ';
    }
    cmd_str += argv[i];
  }

  DaemonClient client;
  uint64_t session_id = 0;
  SessionRegistration reg{
      .name = "",
      .pid = pid,
      .virtual_cores = cfg.virtual_cores,
      .memory_limit_bytes = cfg.memory_limit_bytes,
      .cgroup_path = slice.valid() ? slice.path() : "",
      .command = cmd_str,
  };
  session_id = client.register_session(reg, 200);

  int status = 0;
  while (::waitpid(pid, &status, 0) < 0) {
    if (errno == EINTR) {
      continue;
    }
    status = -1;
    break;
  }

  if (session_id > 0) {
    if (WIFEXITED(status)) {
      client.unregister_session(session_id, 200);
    }
  }

  // Cleanup scratch space and ephemeral overlay workspace
  if (scratch) {
    scratch->cleanup();
  }
  if (overlay) {
    overlay->teardown();
  }

  if (status == -1) {
    return -1;
  }
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    return 128 + WTERMSIG(status);
  }

  return status;
}

}  // namespace virmux
