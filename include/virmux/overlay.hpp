/**
 * @file overlay.hpp
 * @brief Ephemeral unprivileged OverlayFS mount namespace controller.
 */

#pragma once

#include <sys/types.h>

#include <string>

namespace virmux {

/**
 * @brief Configuration parameters for OverlayWorkspace.
 */
struct OverlayConfig {
  std::string lower_dir;
  std::string runtime_dir;  ///< Default: /run/user/<UID>/virmux/overlay or
                            ///< /tmp/virmux/overlay
};

/**
 * @brief Ephemeral Copy-on-Write workspace controller backed by Linux user
 * namespaces and OverlayFS.
 */
class OverlayWorkspace {
 public:
  explicit OverlayWorkspace(OverlayConfig config);
  ~OverlayWorkspace();

  OverlayWorkspace(const OverlayWorkspace&) = delete;
  OverlayWorkspace& operator=(const OverlayWorkspace&) = delete;
  OverlayWorkspace(OverlayWorkspace&& other) noexcept;
  OverlayWorkspace& operator=(OverlayWorkspace&& other) noexcept;

  /**
   * @brief Initializes directory layout for OverlayFS (session, upper, work,
   * merged).
   * @return true on success, false on failure.
   */
  bool setup();

  [[nodiscard]] const std::string& merged_dir() const noexcept {
    return merged_dir_;
  }
  [[nodiscard]] const std::string& upper_dir() const noexcept {
    return upper_dir_;
  }
  [[nodiscard]] const std::string& work_dir() const noexcept {
    return work_dir_;
  }
  [[nodiscard]] const std::string& lower_dir() const noexcept {
    return config_.lower_dir;
  }
  [[nodiscard]] const std::string& session_dir() const noexcept {
    return session_dir_;
  }
  [[nodiscard]] bool is_mounted() const noexcept { return mounted_; }

  /**
   * @brief Mounts OverlayFS in the current process's mount namespace.
   *
   * Async-signal-safe (no heap allocation or stdio), so it may run in a
   * child between clone()/fork() and execve().
   *
   * @return true if mount succeeded or was already mounted, false otherwise.
   */
  bool mount_in_current_namespace();

  /**
   * @brief Unmounts, evicts page cache on upper_dir, and deletes session
   * directory.
   */
  void teardown() noexcept;

  /**
   * @brief Moves the calling process into a new unprivileged user + mount
   * namespace via unshare(), then applies configure_user_mount_namespace().
   *
   * unshare(CLONE_NEWUSER) fails with EINVAL unless the caller is
   * single-threaded. A fork() child is not guaranteed to be (ThreadSanitizer
   * and some preloaded runtimes start helper threads in it), so launchers
   * should prefer creating the namespaces atomically with
   * clone(CLONE_NEWUSER | CLONE_NEWNS) and calling
   * configure_user_mount_namespace() in the child instead.
   *
   * Async-signal-safe. Must be called before mount_in_current_namespace().
   *
   * @param orig_uid Pre-unshare user ID to map to root (UID 0) inside
   * namespace.
   * @param orig_gid Pre-unshare group ID to map to root (GID 0) inside
   * namespace.
   * @return true on success, false otherwise.
   */
  static bool enter_user_mount_namespace(uid_t orig_uid, gid_t orig_gid);

  /**
   * @brief Configures a freshly created user + mount namespace that the
   * calling process already occupies (e.g. a clone(CLONE_NEWUSER |
   * CLONE_NEWNS) child): denies setgroups, maps @p orig_uid / @p orig_gid to
   * root, and makes all mounts private so nothing propagates to the host.
   *
   * Async-signal-safe. Must be called before mount_in_current_namespace().
   *
   * @param orig_uid User ID in the parent namespace to map to UID 0.
   * @param orig_gid Group ID in the parent namespace to map to GID 0.
   * @return true on success, false otherwise.
   */
  static bool configure_user_mount_namespace(uid_t orig_uid, gid_t orig_gid);

 private:
  OverlayConfig config_;
  std::string session_dir_;
  std::string upper_dir_;
  std::string work_dir_;
  std::string merged_dir_;
  bool mounted_{false};
};

}  // namespace virmux
