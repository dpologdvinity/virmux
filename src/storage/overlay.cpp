/**
 * @file overlay.cpp
 * @brief Implementation of ephemeral unprivileged OverlayFS workspace
 * controller.
 */

#include "virmux/overlay.hpp"

#include <fcntl.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <climits>
#include <cstdlib>
#include <filesystem>
#include <utility>

#include "virmux/cleanse.hpp"

namespace virmux {

namespace fs = std::filesystem;

namespace {

std::atomic<uint64_t> g_overlay_seq{0};

/**
 * @brief Bounded, allocation-free text builder over a caller-owned buffer.
 *
 * The namespace and mount helpers run in a freshly cloned/forked child before
 * execve(), where only async-signal-safe operations are allowed: no heap
 * allocation, no stdio, no locale-dependent formatting. Overflow is sticky
 * and reported through ok(); the buffer always stays NUL-terminated.
 */
class FixedText {
 public:
  FixedText(char* buf, size_t capacity) noexcept : buf_(buf), cap_(capacity) {
    if (cap_ > 0) {
      buf_[0] = '\0';
    }
  }

  void append(const char* text) noexcept {
    for (; *text != '\0'; ++text) {
      put(*text);
    }
  }

  void append_uint(unsigned long long value) noexcept {
    char digits[24];
    size_t n = 0;
    do {
      digits[n++] = static_cast<char>('0' + (value % 10));
      value /= 10;
    } while (value != 0 && n < sizeof(digits));
    while (n > 0) {
      put(digits[--n]);
    }
  }

  [[nodiscard]] bool ok() const noexcept { return !overflow_; }
  [[nodiscard]] const char* c_str() const noexcept { return buf_; }
  [[nodiscard]] size_t size() const noexcept { return len_; }

 private:
  void put(char c) noexcept {
    if (len_ + 1 < cap_) {
      buf_[len_++] = c;
      buf_[len_] = '\0';
    } else {
      overflow_ = true;
    }
  }

  char* buf_;
  size_t cap_;
  size_t len_{0};
  bool overflow_{false};
};

/**
 * @brief Writes an ID map line ("0 <outer_id> 1\n") to @p path.
 * Async-signal-safe.
 */
bool write_id_map(const char* path, unsigned long long outer_id) noexcept {
  char line[64];
  FixedText text(line, sizeof(line));
  text.append("0 ");
  text.append_uint(outer_id);
  text.append(" 1\n");
  if (!text.ok()) {
    return false;
  }
  int fd = ::open(path, O_WRONLY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  ssize_t written = ::write(fd, text.c_str(), text.size());
  ::close(fd);
  return written == static_cast<ssize_t>(text.size());
}

bool is_accessible_or_creatable(const std::string& path) {
  std::error_code ec;
  if (fs::exists(path, ec)) {
    return ::access(path.c_str(), W_OK | X_OK) == 0;
  }
  fs::path p(path);
  while (p.has_parent_path() && p != p.parent_path()) {
    p = p.parent_path();
    if (fs::exists(p, ec)) {
      return ::access(p.c_str(), W_OK | X_OK) == 0;
    }
  }
  return false;
}

std::string resolve_runtime_dir(std::string runtime_dir) {
  if (!runtime_dir.empty()) {
    if (runtime_dir.starts_with("~/")) {
      const char* home = std::getenv("HOME");
      if (home != nullptr && *home != '\0') {
        return std::string(home) + runtime_dir.substr(1);
      }
    }
    return runtime_dir;
  }

  std::string run_user = "/run/user/" + std::to_string(::getuid());
  std::string run_overlay = run_user + "/virmux/overlay";

  if (is_accessible_or_creatable(run_overlay)) {
    return run_overlay;
  }

  return "/tmp/virmux/overlay";
}

std::string expand_home_path(std::string path) {
  if (path.starts_with("~/")) {
    const char* home = std::getenv("HOME");
    if (home != nullptr && *home != '\0') {
      return std::string(home) + path.substr(1);
    }
  }
  return path;
}

}  // namespace

OverlayWorkspace::OverlayWorkspace(OverlayConfig config)
    : config_(std::move(config)) {}

OverlayWorkspace::~OverlayWorkspace() { teardown(); }

OverlayWorkspace::OverlayWorkspace(OverlayWorkspace&& other) noexcept
    : config_(std::move(other.config_)),
      session_dir_(std::move(other.session_dir_)),
      upper_dir_(std::move(other.upper_dir_)),
      work_dir_(std::move(other.work_dir_)),
      merged_dir_(std::move(other.merged_dir_)),
      mounted_(other.mounted_) {
  other.mounted_ = false;
  other.session_dir_.clear();
  other.upper_dir_.clear();
  other.work_dir_.clear();
  other.merged_dir_.clear();
}

OverlayWorkspace& OverlayWorkspace::operator=(
    OverlayWorkspace&& other) noexcept {
  if (this != &other) {
    teardown();
    config_ = std::move(other.config_);
    session_dir_ = std::move(other.session_dir_);
    upper_dir_ = std::move(other.upper_dir_);
    work_dir_ = std::move(other.work_dir_);
    merged_dir_ = std::move(other.merged_dir_);
    mounted_ = other.mounted_;
    other.mounted_ = false;
    other.session_dir_.clear();
    other.upper_dir_.clear();
    other.work_dir_.clear();
    other.merged_dir_.clear();
  }
  return *this;
}

bool OverlayWorkspace::setup() {
  if (!session_dir_.empty()) {
    return true;
  }

  if (config_.lower_dir.empty()) {
    return false;
  }

  // Reject paths containing commas or colons which corrupt overlay mount
  // options
  if (config_.lower_dir.find(',') != std::string::npos ||
      config_.lower_dir.find(':') != std::string::npos) {
    return false;
  }

  std::string expanded_lower = expand_home_path(config_.lower_dir);
  std::error_code ec;
  if (!fs::exists(expanded_lower, ec) ||
      !fs::is_directory(expanded_lower, ec)) {
    return false;
  }

  fs::path canonical_lower = fs::canonical(expanded_lower, ec);
  if (ec) {
    return false;
  }
  config_.lower_dir = canonical_lower.string();

  std::string base_dir = resolve_runtime_dir(config_.runtime_dir);
  if (base_dir.find(',') != std::string::npos ||
      base_dir.find(':') != std::string::npos) {
    return false;
  }
  fs::path base_path(base_dir);
  if (!fs::create_directories(base_path, ec) && !fs::exists(base_path, ec)) {
    return false;
  }

  uint64_t seq = g_overlay_seq.fetch_add(1, std::memory_order_relaxed);
  pid_t pid = ::getpid();
  std::string folder_name =
      "overlay_" + std::to_string(pid) + "_" + std::to_string(seq);

  fs::path s_path = base_path / folder_name;
  if (!fs::create_directories(s_path, ec) && !fs::exists(s_path, ec)) {
    return false;
  }

  fs::permissions(s_path, fs::perms::owner_all, fs::perm_options::replace, ec);
  if (ec) {
    fs::remove_all(s_path, ec);
    return false;
  }

  fs::path u_path = s_path / "upper";
  fs::path w_path = s_path / "work";
  fs::path m_path = s_path / "merged";

  if (!fs::create_directories(u_path, ec) ||
      !fs::create_directories(w_path, ec) ||
      !fs::create_directories(m_path, ec)) {
    fs::remove_all(s_path, ec);
    return false;
  }

  fs::permissions(u_path, fs::perms::owner_all, fs::perm_options::replace, ec);
  fs::permissions(w_path, fs::perms::owner_all, fs::perm_options::replace, ec);
  fs::permissions(m_path, fs::perms::owner_all, fs::perm_options::replace, ec);

  session_dir_ = s_path.string();
  upper_dir_ = u_path.string();
  work_dir_ = w_path.string();
  merged_dir_ = m_path.string();
  return true;
}

bool OverlayWorkspace::mount_in_current_namespace() {
  if (mounted_) {
    return true;
  }

  if (session_dir_.empty() || merged_dir_.empty() || upper_dir_.empty() ||
      work_dir_.empty() || config_.lower_dir.empty()) {
    return false;
  }

  // Built on the stack: this runs between clone() and execve() in the sandbox
  // child, where heap allocation is not async-signal-safe.
  char opts_buf[3 * PATH_MAX + 64];
  FixedText opts(opts_buf, sizeof(opts_buf));
  opts.append("lowerdir=");
  opts.append(config_.lower_dir.c_str());
  opts.append(",upperdir=");
  opts.append(upper_dir_.c_str());
  opts.append(",workdir=");
  opts.append(work_dir_.c_str());
  if (!opts.ok()) {
    return false;
  }
  int res = ::mount("overlay", merged_dir_.c_str(), "overlay", 0, opts.c_str());
  if (res != 0) {
    return false;
  }

  mounted_ = true;
  if (!config_.lower_dir.empty()) {
    if (::mount(merged_dir_.c_str(), config_.lower_dir.c_str(), nullptr,
                MS_BIND | MS_REC, nullptr) != 0) {
      char msg_buf[128];
      FixedText msg(msg_buf, sizeof(msg_buf));
      msg.append(
          "error: [overlay] bind mount of merged dir onto lower dir failed "
          "(errno ");
      msg.append_uint(static_cast<unsigned long long>(errno));
      msg.append(")\n");
      // Best-effort diagnostic; the failure itself is the return value.
      ssize_t ignored = ::write(STDERR_FILENO, msg.c_str(), msg.size());
      (void)ignored;
      ::umount2(merged_dir_.c_str(), MNT_DETACH);
      mounted_ = false;
      return false;
    }
  }
  return true;
}

void OverlayWorkspace::teardown() noexcept {
  if (mounted_) {
    if (!config_.lower_dir.empty()) {
      ::umount2(config_.lower_dir.c_str(), MNT_DETACH);
    }
    ::umount2(merged_dir_.c_str(), MNT_DETACH);
    mounted_ = false;
  }

  if (!upper_dir_.empty()) {
    cleanse_path(upper_dir_);
  }

  if (!session_dir_.empty()) {
    fix_permissions_recursive(session_dir_);
    std::error_code ec;
    fs::remove_all(session_dir_, ec);
    session_dir_.clear();
  }

  upper_dir_.clear();
  work_dir_.clear();
  merged_dir_.clear();
}

bool OverlayWorkspace::enter_user_mount_namespace(uid_t orig_uid,
                                                  gid_t orig_gid) {
  // Requires a single-threaded caller; see the header for why clone()-time
  // namespace creation is preferred.
  if (::unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0) {
    return false;
  }
  return configure_user_mount_namespace(orig_uid, orig_gid);
}

bool OverlayWorkspace::configure_user_mount_namespace(uid_t orig_uid,
                                                      gid_t orig_gid) {
  // 1. write "deny" to /proc/self/setgroups (required before an unprivileged
  // gid_map write)
  int fd_setgroups = ::open("/proc/self/setgroups", O_WRONLY | O_CLOEXEC);
  if (fd_setgroups >= 0) {
    ssize_t w = ::write(fd_setgroups, "deny\n", 5);
    ::close(fd_setgroups);
    if (w < 0) {
      return false;
    }
  }

  // 2. map "0 <orig_uid> 1" and 3. "0 <orig_gid> 1"
  if (!write_id_map("/proc/self/uid_map", orig_uid) ||
      !write_id_map("/proc/self/gid_map", orig_gid)) {
    return false;
  }

  // Remount root filesystem as private to eliminate mount propagation leakage
  ::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr);

  return true;
}

}  // namespace virmux
