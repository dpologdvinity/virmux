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
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "virmux/cgroup.hpp"
#include "virmux/cpu_select.hpp"
#include "virmux/daemon.hpp"
#include "virmux/gate.hpp"
#include "virmux/hw_view.hpp"
#include "virmux/intercept.h"
#include "virmux/overlay.hpp"
#include "virmux/scratch.hpp"

namespace virmux {

namespace {

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
 * @brief Temporary files describing the child's synthetic hardware view
 * (meminfo, cpuinfo, online CPU list); all are removed when the run ends.
 */
class SpoofFiles {
 public:
  /** @param prefix Path prefix shared by this run's files. */
  explicit SpoofFiles(std::string prefix) : prefix_(std::move(prefix)) {}

  ~SpoofFiles() {
    for (const auto& path : paths_) {
      std::error_code ec;
      std::filesystem::remove(path, ec);
    }
  }

  SpoofFiles(const SpoofFiles&) = delete;
  SpoofFiles& operator=(const SpoofFiles&) = delete;

  /**
   * @brief Create `<prefix><name>` (0600, never an existing file) with
   * @p content.
   * @return The path, or "" unless the content was fully written: a
   * truncated file would corrupt the child's view, so it is not advertised.
   */
  std::string add(const std::string& name, const std::string& content) {
    std::string path = prefix_ + name;
    const int fd =
        ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
      return "";
    }
    paths_.push_back(path);
    const bool ok = write_all(fd, content);
    ::close(fd);
    return ok ? path : "";
  }

 private:
  std::string prefix_;
  std::vector<std::string> paths_;
};

/** @brief Whole contents of @p path ("" if unreadable). */
std::string read_text_file(const char* path) {
  std::ifstream in(path);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
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

#ifdef VIRMUX_INTERCEPT_INSTALL_PATH
  // Install location baked in by meson (honours --prefix and the distro
  // libdir, which the relative candidates above miss on multiarch systems).
  if (std::filesystem::exists(VIRMUX_INTERCEPT_INSTALL_PATH, ec)) {
    return std::filesystem::canonical(VIRMUX_INTERCEPT_INSTALL_PATH, ec)
        .string();
  }
#endif

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
  /// Pin to @ref cpu_mask with sched_setaffinity (built here, not in the
  /// child, so the child stays syscall-only).
  bool apply_affinity{false};
  cpu_set_t cpu_mask{};
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

  // Real pinning: what nproc, Go, Rust and Python's sched_getaffinity see.
  if (plan.apply_affinity) {
    (void)::sched_setaffinity(0, sizeof(plan.cpu_mask), &plan.cpu_mask);
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

/// cpu.max period: quota = cores * period gives exactly `cores` CPUs' worth.
constexpr uint64_t kCpuMaxPeriodUs = 100000;

/**
 * @brief Choose the CPUs the child is pinned to.
 * @param cfg Sandbox configuration (pin_cpus, cpuset_cores, virtual_cores).
 * @param rotation Window rotation so concurrent runs spread out.
 * @return Sorted CPUs, or empty to keep the inherited affinity mask.
 */
CpuList plan_cpus(const SandboxConfig& cfg, uint64_t rotation) {
  if (!cfg.pin_cpus) {
    return {};
  }
  const CpuList avail = available_cpus();
  if (!cfg.cpuset_cores.empty()) {
    // An explicit list is honoured as far as this process may use it.
    if (const auto wanted = parse_cpu_list(cfg.cpuset_cores)) {
      CpuList both;
      std::set_intersection(wanted->begin(), wanted->end(), avail.begin(),
                            avail.end(), std::back_inserter(both));
      if (!both.empty()) {
        return both;
      }
    }
  }
  return select_cpus(avail, cfg.virtual_cores, rotation, {});
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

  // One derivation feeds both the cgroup limits and the synthetic meminfo,
  // so what the child sees always matches what the kernel enforces.
  const MemoryLimits mem_limits = compute_memory_limits(cfg.memory_limit_bytes);

  // Generate synthetic /proc/meminfo file with unique atomic sequence per call
  uint64_t seq = s_sandbox_counter.fetch_add(1, std::memory_order_relaxed);
  SpoofFiles spoof("/tmp/virmux_" + std::to_string(::getpid()) + "_" +
                   std::to_string(seq) + "_");
  const std::string meminfo_path =
      spoof.add("meminfo", generate_synthetic_meminfo(mem_limits));

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

  // Real CPUs for the child (empty = keep the inherited mask). The reported
  // core count never exceeds what the child can actually run on.
  const CpuList pinned =
      plan_cpus(cfg, static_cast<uint64_t>(::getpid()) + seq);
  const int cores = pinned.empty() ? cfg.virtual_cores
                                   : std::min(cfg.virtual_cores,
                                              static_cast<int>(pinned.size()));
  std::string vcores_str = std::to_string(cores);

  // /proc/cpuinfo and the online list describe the CPUs the child runs on
  // (without pinning: the first `cores` available CPUs).
  CpuList shown = pinned;
  if (shown.empty()) {
    shown = available_cpus();
    if (cores > 0 && shown.size() > static_cast<size_t>(cores)) {
      shown.resize(static_cast<size_t>(cores));
    }
  }
  std::string cpuinfo_path;
  std::string online_path;
  if (!shown.empty()) {
    cpuinfo_path = spoof.add(
        "cpuinfo", filter_cpuinfo(read_text_file("/proc/cpuinfo"), shown));
    online_path = spoof.add("cpu_online", format_online(shown));
  }

  // Per-run cgroup in a delegated subtree (systemd user manager, or a
  // writable ancestor when root / in a container). Without one the run still
  // proceeds, with a single warning naming the reason.
  CgroupProbe probe = CgroupProbe::current();
  probe.root = cfg.cgroup_root;
  const CgroupDelegation delegation = find_cgroup_delegation(probe);
  CgroupSlice slice;
  std::string cgroup_warning;
  if (delegation.usable()) {
    (void)sweep_stale_runs(delegation.root + "/" + kVirmuxCgroupDir);
    slice = create_run(delegation, "run-" + std::to_string(::getpid()) + "-" +
                                       std::to_string(seq));
    if (!slice.is_valid()) {
      cgroup_warning =
          "no cgroup limits for this run: cannot create a cgroup "
          "under " +
          delegation.root + "/" + kVirmuxCgroupDir + " (" +
          std::strerror(slice.last_error()) + ")";
    }
  } else {
    cgroup_warning = "no cgroup limits for this run: " + delegation.reason;
  }

  bool cgroup_pids_active = false;
  if (slice.is_valid()) {
    CgroupLimits limits;
    if (mem_limits.max > 0) {
      limits.memory_max = mem_limits.max;
      limits.memory_high = mem_limits.high;
      limits.swap_max = mem_limits.swap_max;
    }
    if (cores > 0) {
      limits.cpu_max =
          std::to_string(static_cast<uint64_t>(cores) * kCpuMaxPeriodUs) + " " +
          std::to_string(kCpuMaxPeriodUs);
    }
    limits.cpuset_cpus = format_cpu_list(pinned);
    if (cfg.max_pids > 0) {
      limits.pids_max = cfg.max_pids;
    }
    const std::vector<LimitResult> results = slice.apply_limits(limits);
    cgroup_pids_active = std::any_of(
        results.begin(), results.end(),
        [](const LimitResult& r) { return r.file == "pids.max" && r.applied; });
    cgroup_warning = describe_limit_failures(results, !pinned.empty());
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
  if (!cpuinfo_path.empty()) {
    env_overrides.emplace_back(VIRMUX_ENV_SPOOFED_CPUINFO, cpuinfo_path);
  }
  if (!online_path.empty()) {
    env_overrides.emplace_back(VIRMUX_ENV_SPOOFED_CPU_ONLINE, online_path);
  }
  if (!meminfo_path.empty()) {
    env_overrides.emplace_back(VIRMUX_ENV_SPOOFED_MEMINFO, meminfo_path);
  }
  // Thread-pool sizing variables, "at most N" (see compute_thread_env).
  auto real_getenv = [](const char* name) -> const char* {
    return std::getenv(name);
  };
  for (auto& kv : compute_thread_env(cores, real_getenv)) {
    env_overrides.push_back(std::move(kv));
  }
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
  if (!pinned.empty()) {
    plan.apply_affinity = true;
    CPU_ZERO(&plan.cpu_mask);
    for (int cpu : pinned) {
      if (cpu < CPU_SETSIZE) {
        CPU_SET(cpu, &plan.cpu_mask);
      }
    }
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
    // The child joins itself before exec; this write is a backstop. Check
    // membership so a failed join never goes unreported (unknown, e.g. the
    // child already exited, is not reported).
    const bool attached = slice.attach_pid(pid);
    const int attach_errno = slice.last_error();
    const auto joined = process_in_cgroup(pid, slice.path(), cfg.cgroup_root);
    if (joined.has_value() && !*joined) {
      cgroup_warning =
          "no cgroup limits for this run: could not join " + slice.path();
      if (!attached) {
        cgroup_warning += std::string(" (") + std::strerror(attach_errno) + ")";
      }
    }
  }
  // At most one warning line per run, naming why limits are missing.
  if (!cgroup_warning.empty()) {
    std::fprintf(stderr, "warning: virmux: %s\n", cgroup_warning.c_str());
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
