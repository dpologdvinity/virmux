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
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>

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

uint64_t count_user_threads() {
  uint64_t count = 0;
  uid_t uid = ::getuid();
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator("/proc", ec)) {
    if (ec) break;
    const auto filename = entry.path().filename().native();
    if (filename.empty() || !std::isdigit(static_cast<unsigned char>(filename[0]))) {
      continue;
    }
    auto status_path = entry.path() / "status";
    int fd = ::open(status_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) continue;
    char buf[1024];
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
  if (mem_fd >= 0) {
    std::string mem_content =
        generate_synthetic_meminfo(cfg.memory_limit_bytes);
    (void)::write(mem_fd, mem_content.data(), mem_content.size());
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
  slice.setup("", cfg.memory_limit_bytes, cfg.cpuset_cores);
  if (slice.is_valid() && cfg.max_pids > 0) {
    slice.set_pids_max(cfg.max_pids);
  }

  uint64_t baseline_threads = (cfg.max_pids > 0) ? count_user_threads() : 0;

  pid_t pid = ::fork();
  if (pid < 0) {
    if (scratch) {
      scratch->cleanup();
    }
    if (overlay) {
      overlay->teardown();
    }
    return -1;
  }

  if (pid == 0) {
    // --- In child process ---
    // 1. If ephemeral, enter unprivileged user and mount namespace, then mount
    // overlay
    if (is_ephemeral && overlay && !overlay->is_mounted()) {
      if (!OverlayWorkspace::enter_user_mount_namespace(orig_uid, orig_gid)) {
        std::fprintf(stderr,
                     "error: [overlay] failed to enter user/mount namespace\n");
        ::_exit(126);
      }
      if (!overlay->mount_in_current_namespace()) {
        std::fprintf(
            stderr,
            "error: [overlay] failed to mount overlayfs in namespace\n");
        ::_exit(126);
      }
      if (::chdir(resolved_workspace.c_str()) != 0 &&
          ::chdir(overlay->merged_dir().c_str()) != 0) {
        std::fprintf(stderr,
                     "error: [overlay] failed to chdir to workspace '%s'\n",
                     resolved_workspace.c_str());
        ::_exit(127);
      }
    } else if (explicit_workspace) {
      if (::chdir(resolved_workspace.c_str()) != 0) {
        ::_exit(127);
      }
    }

    // 2. Attach process to cgroup
    if (slice.is_valid()) {
      slice.attach_pid(0);
    }

    // Fork-bomb fallback: enforce RLIMIT_NPROC ceiling if configured
    if (cfg.max_pids > 0) {
      rlim_t limit = static_cast<rlim_t>(baseline_threads + cfg.max_pids);
      struct rlimit cur_rl;
      if (::getrlimit(RLIMIT_NPROC, &cur_rl) == 0 && limit > cur_rl.rlim_max) {
        limit = cur_rl.rlim_max;
      }
      struct rlimit rl{limit, limit};
      (void)::setrlimit(RLIMIT_NPROC, &rl);
    }

    // 3. Set oom_score_adj to sacrificial priority (+800)
    set_process_oom_score(0, 800);

    // 4. Scratch environment variables (TMPDIR, TEMP, TMP)
    if (scratch && scratch->is_valid()) {
      ::setenv("TMPDIR", scratch->path().c_str(), 1);
      ::setenv("TEMP", scratch->path().c_str(), 1);
      ::setenv("TMP", scratch->path().c_str(), 1);
    }

    // 5. Inject runtime virtualization and interceptor environment
    // (pre-allocated)
    if (!preload_val.empty()) {
      ::setenv("LD_PRELOAD", preload_val.c_str(), 1);
    }

    ::setenv(VIRMUX_ENV_VIRTUAL_CORES, vcores_str.c_str(), 1);
    ::setenv(VIRMUX_ENV_SPOOFED_MEMINFO, meminfo_path.c_str(), 1);
    ::setenv("OMP_NUM_THREADS", vcores_str.c_str(), 1);
    ::setenv("OPENBLAS_NUM_THREADS", vcores_str.c_str(), 1);
    ::setenv("MKL_NUM_THREADS", vcores_str.c_str(), 1);
    ::setenv("NUMEXPR_NUM_THREADS", vcores_str.c_str(), 1);

    // 6. Execute target command
    ::execvp(argv[0], argv);
    int err = errno;
    ::_exit(err == ENOENT ? 127 : 126);
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
