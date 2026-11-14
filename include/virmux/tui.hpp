/**
 * @file tui.hpp
 * @brief Interactive Terminal User Interface (TUI) dashboard for session and
 * telemetry monitoring.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "virmux/daemon.hpp"
#include "virmux/session.hpp"

namespace virmux {

/**
 * @brief Configuration parameters for VirmuxTui dashboard.
 */
struct TuiConfig {
  std::string socket_path;
  int refresh_interval_ms{500};
  bool headless{false};
};

/**
 * @brief Interactive ncurses-based dashboard for real-time session lifecycle
 * and telemetry control.
 */
class VirmuxTui {
 public:
  explicit VirmuxTui(TuiConfig config = {});
  ~VirmuxTui();

  VirmuxTui(const VirmuxTui&) = delete;
  VirmuxTui& operator=(const VirmuxTui&) = delete;
  VirmuxTui(VirmuxTui&&) = delete;
  VirmuxTui& operator=(VirmuxTui&&) = delete;

  /**
   * @brief Run the interactive curses event loop.
   * @return Process exit code (0 on normal exit).
   */
  int run();

  /**
   * @brief Pure string snapshot renderer for unit testing without interactive
   * TTY.
   * @param sessions List of session snapshots to render.
   * @param selected_idx Index of currently selected session.
   * @param daemon_online Whether the supervisor daemon is currently connected.
   * @param width Screen width in characters (default: 80).
   * @param height Screen height in characters (default: 24).
   * @return Formatted multi-line ASCII dashboard snapshot.
   */
  [[nodiscard]] std::string render_snapshot(
      const std::vector<SessionInfo>& sessions, size_t selected_idx,
      bool daemon_online, int width = 80, int height = 24) const;

  /**
   * @brief Move row selection index up, clamped to 0.
   */
  void move_up();

  /**
   * @brief Move row selection index down, clamped to max session index.
   */
  void move_down();

  /**
   * @brief Get currently selected session row index.
   */
  [[nodiscard]] size_t selected_index() const;

  /**
   * @brief Manually set the selected session row index (clamped to bounds).
   */
  void set_selected_index(size_t idx);

  /**
   * @brief Update session list cache and re-clamp selection bounds.
   */
  void update_sessions(const std::vector<SessionInfo>& sessions);

  /**
   * @brief Handle a single key event (for testing and curses loop).
   * @param ch Key code (curses KEY_* or ASCII character).
   * @param client Optional DaemonClient pointer for lifecycle RPCs.
   * @return false if quit command received ('q'/'Q'), true otherwise.
   */
  bool handle_key(int ch, DaemonClient* client = nullptr);

  /**
   * @brief Check whether curses mode was initialized.
   */
  [[nodiscard]] bool is_curses_initialized() const;

  /**
   * @brief Access configuration.
   */
  [[nodiscard]] const TuiConfig& config() const;

 private:
  void render_curses(const std::vector<SessionInfo>& sessions,
                     size_t selected_idx, bool daemon_online, int width,
                     int height);

  TuiConfig config_;
  size_t selected_idx_{0};
  size_t session_count_{0};
  std::vector<SessionInfo> cached_sessions_;
  bool curses_initialized_{false};
};

}  // namespace virmux
