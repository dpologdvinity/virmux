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
   * @return true if mount succeeded or was already mounted, false otherwise.
   */
  bool mount_in_current_namespace();

  /**
   * @brief Unmounts, evicts page cache on upper_dir, and deletes session
   * directory.
   */
  void teardown() noexcept;

  /**
   * @brief Helper to configure unprivileged user and mount namespace with
   * UID/GID mapping. Must be called in child process post-fork before
   * mount_in_current_namespace().
   *
   * @param orig_uid Pre-unshare user ID to map to root (UID 0) inside
   * namespace.
   * @param orig_gid Pre-unshare group ID to map to root (GID 0) inside
   * namespace.
   * @return true on success, false otherwise.
   */
  static bool enter_user_mount_namespace(uid_t orig_uid, gid_t orig_gid);

 private:
  OverlayConfig config_;
  std::string session_dir_;
  std::string upper_dir_;
  std::string work_dir_;
  std::string merged_dir_;
  bool mounted_{false};
};

}  // namespace virmux
