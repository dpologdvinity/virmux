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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <utility>

#include "virmux/cleanse.hpp"

namespace virmux {

namespace fs = std::filesystem;

namespace {

std::atomic<uint64_t> g_overlay_seq{0};

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

void fix_permissions_recursive(const fs::path& path) noexcept {
  std::error_code ec;
  fs::permissions(path, fs::perms::owner_all, fs::perm_options::add, ec);
  if (!fs::is_directory(path, ec)) {
    return;
  }
  for (const auto& entry : fs::directory_iterator(
           path, fs::directory_options::skip_permission_denied, ec)) {
    fs::permissions(entry.path(), fs::perms::owner_all, fs::perm_options::add,
                    ec);
    if (entry.is_directory(ec)) {
      fix_permissions_recursive(entry.path());
    }
  }
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

  std::string opts = "lowerdir=" + config_.lower_dir +
                     ",upperdir=" + upper_dir_ + ",workdir=" + work_dir_;
  int res = ::mount("overlay", merged_dir_.c_str(), "overlay", 0, opts.c_str());
  if (res == 0) {
    mounted_ = true;
    if (!config_.lower_dir.empty()) {
      if (::mount(merged_dir_.c_str(), config_.lower_dir.c_str(), nullptr,
                  MS_BIND | MS_REC, nullptr) != 0) {
        std::fprintf(stderr,
                     "warning: [overlay] bind mount of merged dir onto lower "
                     "dir failed: %s\n",
                     std::strerror(errno));
      }
    }
    return true;
  }

  return false;
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
    try {
      cleanse_path(upper_dir_);
    } catch (...) {
    }
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
  if (::unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0) {
    return false;
  }

  // 1. write "deny" to /proc/self/setgroups
  int fd_setgroups = ::open("/proc/self/setgroups", O_WRONLY | O_CLOEXEC);
  if (fd_setgroups >= 0) {
    ssize_t w = ::write(fd_setgroups, "deny\n", 5);
    ::close(fd_setgroups);
    if (w < 0) {
      return false;
    }
  }

  // 2. write "0 <orig_uid> 1" to /proc/self/uid_map
  std::string uid_entry = "0 " + std::to_string(orig_uid) + " 1\n";
  int fd_uid = ::open("/proc/self/uid_map", O_WRONLY | O_CLOEXEC);
  if (fd_uid < 0) {
    return false;
  }
  ssize_t w_u = ::write(fd_uid, uid_entry.data(), uid_entry.size());
  ::close(fd_uid);
  if (w_u != static_cast<ssize_t>(uid_entry.size())) {
    return false;
  }

  // 3. write "0 <orig_gid> 1" to /proc/self/gid_map
  std::string gid_entry = "0 " + std::to_string(orig_gid) + " 1\n";
  int fd_gid = ::open("/proc/self/gid_map", O_WRONLY | O_CLOEXEC);
  if (fd_gid < 0) {
    return false;
  }
  ssize_t w_g = ::write(fd_gid, gid_entry.data(), gid_entry.size());
  ::close(fd_gid);
  if (w_g != static_cast<ssize_t>(gid_entry.size())) {
    return false;
  }

  // Remount root filesystem as private to eliminate mount propagation leakage
  ::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr);

  return true;
}

}  // namespace virmux
