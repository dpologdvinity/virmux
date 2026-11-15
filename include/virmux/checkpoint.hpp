/**
 * @file checkpoint.hpp
 * @brief Session checkpoint and restore subsystem (CRIU and deep freeze pageout
 * fallback).
 */

#pragma once

#include <cstdint>
#include <string>

#include "virmux/session.hpp"

namespace virmux {

/**
 * @brief Manages saving and restoring session images and hibernation state.
 */
class CheckpointManager {
 public:
  /**
   * @brief Construct CheckpointManager with base directory.
   * Resolves $VIRMUX_CHECKPOINT_DIR, else /run/user/<UID>/virmux/checkpoints,
   * fallback /tmp/virmux-<UID>/checkpoints. Enforces directory permissions
   * 0700.
   */
  explicit CheckpointManager(std::string base_dir = "");
  ~CheckpointManager() = default;

  CheckpointManager(const CheckpointManager&) = delete;
  CheckpointManager& operator=(const CheckpointManager&) = delete;
  CheckpointManager(CheckpointManager&&) noexcept = default;
  CheckpointManager& operator=(CheckpointManager&&) noexcept = default;

  /**
   * @brief Get resolved base directory path.
   */
  [[nodiscard]] const std::string& base_dir() const;

  /**
   * @brief Probe whether CRIU binary and necessary capabilities exist.
   */
  [[nodiscard]] bool is_criu_available() const;

  /**
   * @brief Checkpoint session to disk.
   * If CRIU is available with necessary capabilities, executes criu dump into
   * <base_dir>/<session_id>/. If unprivileged / CRIU unavailable: executes deep
   * freeze fallback (freezes cgroup / SIGSTOP, invokes
   * process_madvise(MADV_PAGEOUT) to compress memory to swap/zram, writes
   * checkpoint.json metadata).
   *
   * @param session Target session info snapshot.
   * @param status_msg Optional output status message.
   * @return true on success, false on failure.
   */
  bool checkpoint_session(const SessionInfo& session,
                          std::string* status_msg = nullptr);

  /**
   * @brief Restore session from checkpoint.
   * Restores CRIU dump or unfreezes (cgroup.freeze = 0 / SIGCONT),
   * removes checkpoint image, restores session to RUNNING_NORMAL.
   *
   * @param session Target session info snapshot.
   * @param status_msg Optional output status message.
   * @return true on success, false on failure.
   */
  bool restore_session(const SessionInfo& session,
                       std::string* status_msg = nullptr);

  /**
   * @brief Check if checkpoint exists on disk for given session ID.
   */
  [[nodiscard]] bool has_checkpoint(uint64_t id) const;

  /**
   * @brief Remove checkpoint image directory for given session ID.
   */
  bool remove_checkpoint(uint64_t id);

 private:
  std::string base_dir_;
};

}  // namespace virmux
