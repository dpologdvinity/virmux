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
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

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
#include "virmux/gate.hpp"
#include "virmux/intercept.h"

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

static std::atomic<uint64_t> s_sandbox_counter{0};

}  // anonymous namespace

AdmissionGate& get_default_admission_gate() {
  static AdmissionGate default_gate(2, std::chrono::seconds(10));
  return default_gate;
}

void set_worker_oom_score(pid_t child_pid) {
  char path[64];
  if (child_pid <= 0) {
    std::snprintf(path, sizeof(path), "/proc/self/oom_score_adj");
  } else {
    std::snprintf(path, sizeof(path), "/proc/%d/oom_score_adj",
                  static_cast<int>(child_pid));
  }
  int fd = ::open(path, O_WRONLY | O_CLOEXEC);
  if (fd >= 0) {
    const char val[] = "800\n";
    (void)::write(fd, val, sizeof(val) - 1);
    ::close(fd);
  }
}

int spawn_command(const SandboxConfig& cfg, char* const argv[]) {
  if (argv == nullptr || argv[0] == nullptr || argv[0][0] == '\0') {
    return -1;
  }

  // Acquire admission token if configured as heavy compute task
  std::unique_ptr<ScopedToken> token;
  if (cfg.is_heavy) {
    AdmissionGate& gate = cfg.gate ? *cfg.gate : get_default_admission_gate();
    token = std::make_unique<ScopedToken>(gate);
    if (!token->acquired()) {
      return -1;
    }
  }

  // Generate synthetic /proc/meminfo file with unique atomic sequence per call
  uint64_t seq = s_sandbox_counter.fetch_add(1, std::memory_order_relaxed);
  std::string meminfo_path = "/tmp/virmux_" + std::to_string(::getpid()) + "_" +
                             std::to_string(seq) + "_meminfo";
  int mem_fd = ::open(meminfo_path.c_str(),
                      O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
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
    preload_val = intercept_lib;
    const char* existing_preload = std::getenv("LD_PRELOAD");
    if (existing_preload && existing_preload[0] != '\0') {
      preload_val = preload_val + ":" + existing_preload;
    }
  }

  std::string vcores_str = std::to_string(cfg.virtual_cores);

  // Set up cgroups v2 slice (gracefully falls back if unprivileged)
  CgroupSlice slice;
  slice.setup("", cfg.memory_limit_bytes, cfg.cpuset_cores);

  pid_t pid = ::fork();
  if (pid < 0) {
    return -1;
  }

  if (pid == 0) {
    // --- In child process ---
    // 1. Attach process to cgroup
    if (slice.is_valid()) {
      slice.attach_pid(0);
    }

    // 2. Set oom_score_adj to sacrificial priority (+800)
    set_worker_oom_score(0);

    // 3. Inject runtime virtualization and interceptor environment
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

    // 4. Switch to configured workspace directory if provided
    if (!cfg.workspace_path.empty()) {
      if (::chdir(cfg.workspace_path.c_str()) != 0) {
        ::_exit(127);
      }
    }

    // 5. Execute target command
    ::execvp(argv[0], argv);
    int err = errno;
    ::_exit(err == ENOENT ? 127 : 126);
  }

  // --- In parent process ---
  if (slice.is_valid()) {
    slice.attach_pid(pid);
  }

  int status = 0;
  while (::waitpid(pid, &status, 0) < 0) {
    if (errno == EINTR) {
      continue;
    }
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
