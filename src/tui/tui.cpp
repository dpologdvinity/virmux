/**
 * @file tui.cpp
 * @brief Implementation of VirmuxTui interactive ncurses dashboard and snapshot
 * renderer.
 */

#include "virmux/tui.hpp"

#include <curses.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "virmux/cli_stats.hpp"
#include "virmux/daemon.hpp"
#include "virmux/ksm.hpp"
#include "virmux/session.hpp"
#include "virmux/telemetry.hpp"
#include "virmux/version.hpp"

namespace virmux {

namespace {

std::atomic<bool> g_tui_stop_requested{false};

void tui_signal_handler(int sig) {
  (void)sig;
  g_tui_stop_requested.store(true);
}

std::string format_state_label(SessionState state) {
  switch (state) {
    case SessionState::INITIALIZING:
      return "INIT";
    case SessionState::RUNNING_BURST:
      return "BURST";
    case SessionState::RUNNING_NORMAL:
      return "RUNNING";
    case SessionState::WAITING_IO:
      return "WAITING";
    case SessionState::FROZEN:
      return "FROZEN";
    case SessionState::DEAD:
      return "DEAD";
  }
  return "UNKNOWN";
}

std::string format_box_line(const std::string& inner, int width) {
  if (width < 2) {
    return "|";
  }
  int inner_width = width - 2;
  if (static_cast<int>(inner.size()) >= inner_width) {
    return "|" + inner.substr(0, static_cast<size_t>(inner_width)) + "|";
  }
  return "|" + inner +
         std::string(static_cast<size_t>(inner_width) - inner.size(), ' ') +
         "|";
}

std::string format_border_line(int width) {
  if (width < 2) {
    return "+";
  }
  return "+" + std::string(static_cast<size_t>(width - 2), '-') + "+";
}

}  // namespace

VirmuxTui::VirmuxTui(TuiConfig config) : config_(std::move(config)) {}

VirmuxTui::~VirmuxTui() {
  if (curses_initialized_) {
    endwin();
    curses_initialized_ = false;
  }
}

const TuiConfig& VirmuxTui::config() const { return config_; }

bool VirmuxTui::is_curses_initialized() const { return curses_initialized_; }

size_t VirmuxTui::selected_index() const { return selected_idx_; }

void VirmuxTui::set_selected_index(size_t idx) {
  if (session_count_ == 0) {
    selected_idx_ = 0;
  } else if (idx >= session_count_) {
    selected_idx_ = session_count_ - 1;
  } else {
    selected_idx_ = idx;
  }
}

void VirmuxTui::update_sessions(const std::vector<SessionInfo>& sessions) {
  cached_sessions_ = sessions;
  session_count_ = sessions.size();
  if (session_count_ == 0) {
    selected_idx_ = 0;
  } else if (selected_idx_ >= session_count_) {
    selected_idx_ = session_count_ - 1;
  }
}

void VirmuxTui::move_up() {
  if (selected_idx_ > 0) {
    --selected_idx_;
  }
}

void VirmuxTui::move_down() {
  if (session_count_ > 0 && selected_idx_ + 1 < session_count_) {
    ++selected_idx_;
  }
}

bool VirmuxTui::handle_key(int ch, DaemonClient* client) {
  if (ch == 'q' || ch == 'Q') {
    return false;
  }
  if (ch == KEY_UP || ch == 'k') {
    move_up();
    return true;
  }
  if (ch == KEY_DOWN || ch == 'j') {
    move_down();
    return true;
  }
  if (ch == ' ') {
    if (client != nullptr && !cached_sessions_.empty() &&
        selected_idx_ < cached_sessions_.size()) {
      uint64_t target_id = cached_sessions_[selected_idx_].id;
      if (cached_sessions_[selected_idx_].state == SessionState::FROZEN) {
        client->thaw_session(target_id);
      } else {
        client->freeze_session(target_id);
      }
    }
    return true;
  }
  if (ch == 'K') {
    if (client != nullptr && !cached_sessions_.empty() &&
        selected_idx_ < cached_sessions_.size()) {
      uint64_t target_id = cached_sessions_[selected_idx_].id;
      client->kill_session(target_id, SIGTERM);
    }
    return true;
  }
  return true;
}

std::string VirmuxTui::render_snapshot(const std::vector<SessionInfo>& sessions,
                                       size_t selected_idx, bool daemon_online,
                                       int width, int height) const {
  (void)height;

  int total_cores = static_cast<int>(std::thread::hardware_concurrency());
  if (total_cores <= 0) {
    total_cores = 1;
  }

  MemoryTelemetry mem_telemetry;
  auto host_mem = mem_telemetry.read_host_stats();
  double ram_gb = static_cast<double>(host_mem.mem_total_bytes) /
                  (1024.0 * 1024.0 * 1024.0);
  if (ram_gb <= 0.0) {
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && page_size > 0) {
      ram_gb = (static_cast<double>(pages) * static_cast<double>(page_size)) /
               (1024.0 * 1024.0 * 1024.0);
    } else {
      ram_gb = 16.0;
    }
  }

  char ram_buf[32];
  std::snprintf(ram_buf, sizeof(ram_buf), "%.1fG RAM", ram_gb);

  double loads[3] = {0.0, 0.0, 0.0};
  getloadavg(loads, 3);
  char load_buf[32];
  std::snprintf(load_buf, sizeof(load_buf), "%.2f", loads[0]);

  std::string title_part = " VIRMUX v" + std::string(version_string()) +
                           "  [Linux / " + std::to_string(total_cores) +
                           " Cores / " + ram_buf + "]";
  std::string load_part = "Host Load: " + std::string(load_buf) + " ";

  int inner_width = width - 2;
  int spaces1 = inner_width - static_cast<int>(title_part.size()) -
                static_cast<int>(load_part.size());
  if (spaces1 < 1) {
    spaces1 = 1;
  }
  std::string line1 =
      title_part + std::string(static_cast<size_t>(spaces1), ' ') + load_part;

  std::string shield_worker;
  if (total_cores > 2) {
    shield_worker =
        " Dedicated Shield: Cores 0-1 (Fluid)   | Worker Pool: Cores 2-" +
        std::to_string(total_cores - 1) + " (Managed)";
  } else {
    shield_worker =
        " Dedicated Shield: Cores 0-1 (Fluid)   | Worker Pool: Cores 0-1 "
        "(Shared)";
  }

  uint64_t total_ksm_saved = 0;
  for (const auto& s : sessions) {
    total_ksm_saved += s.ksm_dedup_bytes;
  }
  KsmController ksm_ctrl;
  if (ksm_ctrl.is_available()) {
    auto kstats = ksm_ctrl.read_stats();
    if (kstats.bytes_saved > total_ksm_saved) {
      total_ksm_saved = kstats.bytes_saved;
    }
  }
  std::string ksm_str = format_bytes(total_ksm_saved) + " Saved";

  auto zram_list = mem_telemetry.read_zram_stats();
  uint64_t zorig = 0;
  uint64_t zcompr = 0;
  for (const auto& z : zram_list) {
    zorig += z.orig_data_size;
    zcompr += z.compr_data_size;
  }
  std::string zram_str;
  if (zorig > 0 && zcompr > 0) {
    double ratio = static_cast<double>(zorig) / static_cast<double>(zcompr);
    char r_buf[32];
    std::snprintf(r_buf, sizeof(r_buf), "%.1fx", ratio);
    zram_str = format_bytes(zcompr) + " (" + r_buf + ")";
  } else {
    zram_str = "0 B (1.0x)";
  }

  std::string telemetry_line =
      " KSM Dedup: " + ksm_str + "  | zram: " + zram_str +
      " | Sessions: " + std::to_string(sessions.size());

  std::ostringstream out;
  out << format_border_line(width) << "\n";
  out << format_box_line(line1, width) << "\n";
  if (!daemon_online) {
    out << format_box_line(" [Supervisor Daemon Offline (Standalone Mode)]",
                           width)
        << "\n";
  }
  out << format_box_line(shield_worker, width) << "\n";
  out << format_box_line(telemetry_line, width) << "\n";
  out << format_border_line(width) << "\n";

  std::string table_hdr_inner =
      " S# | NAME        | STATE   | V-CORES | RAM (RSS) | DEDUP | COMMAND";
  out << format_box_line(table_hdr_inner, width) << "\n";
  out << format_border_line(width) << "\n";

  if (sessions.empty()) {
    out << format_box_line("  No active sessions registered", width) << "\n";
  } else {
    for (size_t i = 0; i < sessions.size(); ++i) {
      const auto& s = sessions[i];
      char prefix = (i == selected_idx) ? '>' : ' ';
      std::string state_str = format_state_label(s.state);
      std::string rss_str = format_bytes(s.memory_rss_bytes);
      std::string dedup_str = format_bytes(s.ksm_dedup_bytes);

      char row_buf[512];
      std::snprintf(row_buf, sizeof(row_buf),
                    "%c%-3llu| %-12.12s| %-8.8s| %-8d| %-10.10s| %-6.6s| %s",
                    prefix,
                    static_cast<unsigned long long>(s.id <= 999 ? s.id : 999),
                    s.name.c_str(), state_str.c_str(), s.virtual_cores,
                    rss_str.c_str(), dedup_str.c_str(), s.command.c_str());
      out << format_box_line(row_buf, width) << "\n";
    }
  }
  out << format_border_line(width) << "\n";

  std::string footer_inner =
      " [j/k] Navigate  [Space] Freeze/Thaw  [K] Kill  [q] Quit";
  out << format_box_line(footer_inner, width) << "\n";
  out << format_border_line(width) << "\n";

  return out.str();
}

void VirmuxTui::render_curses(const std::vector<SessionInfo>& sessions,
                              size_t selected_idx, bool daemon_online,
                              int width, int height) {
  int y = 0;
  if (y >= height) return;

  mvprintw(y++, 0, "%s", format_border_line(width).c_str());
  if (y >= height) return;

  int total_cores = static_cast<int>(std::thread::hardware_concurrency());
  if (total_cores <= 0) total_cores = 1;
  MemoryTelemetry mem_telemetry;
  auto host_mem = mem_telemetry.read_host_stats();
  double ram_gb = static_cast<double>(host_mem.mem_total_bytes) /
                  (1024.0 * 1024.0 * 1024.0);
  if (ram_gb <= 0.0) {
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && page_size > 0) {
      ram_gb = (static_cast<double>(pages) * static_cast<double>(page_size)) /
               (1024.0 * 1024.0 * 1024.0);
    } else {
      ram_gb = 16.0;
    }
  }
  char ram_buf[32];
  std::snprintf(ram_buf, sizeof(ram_buf), "%.1fG RAM", ram_gb);
  double loads[3] = {0.0, 0.0, 0.0};
  getloadavg(loads, 3);
  char load_buf[32];
  std::snprintf(load_buf, sizeof(load_buf), "%.2f", loads[0]);

  std::string title_part = " VIRMUX v" + std::string(version_string()) +
                           "  [Linux / " + std::to_string(total_cores) +
                           " Cores / " + ram_buf + "]";
  std::string load_part = "Host Load: " + std::string(load_buf) + " ";
  int inner_w = width - 2;
  int spaces1 = inner_w - static_cast<int>(title_part.size()) -
                static_cast<int>(load_part.size());
  if (spaces1 < 1) spaces1 = 1;
  std::string line1 =
      title_part + std::string(static_cast<size_t>(spaces1), ' ') + load_part;

  if (has_colors()) attron(COLOR_PAIR(1) | A_BOLD);
  mvprintw(y++, 0, "%s", format_box_line(line1, width).c_str());
  if (has_colors()) attroff(COLOR_PAIR(1) | A_BOLD);
  if (y >= height) return;

  if (!daemon_online) {
    if (has_colors()) attron(COLOR_PAIR(3) | A_BOLD);
    mvprintw(
        y++, 0, "%s",
        format_box_line(" [Supervisor Daemon Offline (Standalone Mode)]", width)
            .c_str());
    if (has_colors()) attroff(COLOR_PAIR(3) | A_BOLD);
    if (y >= height) return;
  }

  std::string shield_worker;
  if (total_cores > 2) {
    shield_worker =
        " Dedicated Shield: Cores 0-1 (Fluid)   | Worker Pool: Cores 2-" +
        std::to_string(total_cores - 1) + " (Managed)";
  } else {
    shield_worker =
        " Dedicated Shield: Cores 0-1 (Fluid)   | Worker Pool: Cores 0-1 "
        "(Shared)";
  }
  if (has_colors()) attron(COLOR_PAIR(1));
  mvprintw(y++, 0, "%s", format_box_line(shield_worker, width).c_str());
  if (has_colors()) attroff(COLOR_PAIR(1));
  if (y >= height) return;

  uint64_t total_ksm_saved = 0;
  for (const auto& s : sessions) total_ksm_saved += s.ksm_dedup_bytes;
  KsmController ksm_ctrl;
  if (ksm_ctrl.is_available()) {
    auto ks = ksm_ctrl.read_stats();
    if (ks.bytes_saved > total_ksm_saved) total_ksm_saved = ks.bytes_saved;
  }
  std::string ksm_str = format_bytes(total_ksm_saved) + " Saved";
  auto zram_list = mem_telemetry.read_zram_stats();
  uint64_t zorig = 0, zcompr = 0;
  for (const auto& z : zram_list) {
    zorig += z.orig_data_size;
    zcompr += z.compr_data_size;
  }
  std::string zram_str = "0 B (1.0x)";
  if (zorig > 0 && zcompr > 0) {
    double ratio = static_cast<double>(zorig) / static_cast<double>(zcompr);
    char r_buf[32];
    std::snprintf(r_buf, sizeof(r_buf), "%.1fx", ratio);
    zram_str = format_bytes(zcompr) + " (" + r_buf + ")";
  }
  std::string line3 = " KSM Dedup: " + ksm_str + "  | zram: " + zram_str +
                      " | Sessions: " + std::to_string(sessions.size());
  if (has_colors()) attron(COLOR_PAIR(1));
  mvprintw(y++, 0, "%s", format_box_line(line3, width).c_str());
  if (has_colors()) attroff(COLOR_PAIR(1));
  if (y >= height) return;

  mvprintw(y++, 0, "%s", format_border_line(width).c_str());
  if (y >= height) return;

  if (has_colors()) attron(A_BOLD);
  mvprintw(
      y++, 0, "%s",
      format_box_line(
          " S# | NAME        | STATE   | V-CORES | RAM (RSS) | DEDUP | COMMAND",
          width)
          .c_str());
  if (has_colors()) attroff(A_BOLD);
  if (y >= height) return;

  mvprintw(y++, 0, "%s", format_border_line(width).c_str());
  if (y >= height) return;

  if (sessions.empty()) {
    mvprintw(y++, 0, "%s",
             format_box_line("  No active sessions registered", width).c_str());
  } else {
    int remaining_lines = height - y - 3;
    if (remaining_lines <= 0) remaining_lines = 1;
    size_t visible_count = static_cast<size_t>(remaining_lines);
    size_t start_idx = 0;
    if (selected_idx >= visible_count) {
      start_idx = selected_idx - visible_count + 1;
    }
    size_t end_idx = std::min(sessions.size(), start_idx + visible_count);

    for (size_t i = start_idx; i < end_idx && y < height - 3; ++i) {
      const auto& s = sessions[i];
      char prefix = (i == selected_idx) ? '>' : ' ';
      std::string state_str = format_state_label(s.state);
      std::string rss_str = format_bytes(s.memory_rss_bytes);
      std::string dedup_str = format_bytes(s.ksm_dedup_bytes);

      char row_buf[512];
      std::snprintf(row_buf, sizeof(row_buf),
                    "%c%-3llu| %-12.12s| %-8.8s| %-8d| %-10.10s| %-6.6s| %s",
                    prefix,
                    static_cast<unsigned long long>(s.id <= 999 ? s.id : 999),
                    s.name.c_str(), state_str.c_str(), s.virtual_cores,
                    rss_str.c_str(), dedup_str.c_str(), s.command.c_str());
      std::string formatted_row = format_box_line(row_buf, width);

      if (i == selected_idx) {
        if (has_colors()) attron(COLOR_PAIR(2) | A_BOLD);
        mvprintw(y++, 0, "%s", formatted_row.c_str());
        if (has_colors()) attroff(COLOR_PAIR(2) | A_BOLD);
      } else if (s.state == SessionState::FROZEN) {
        if (has_colors()) attron(COLOR_PAIR(3));
        mvprintw(y++, 0, "%s", formatted_row.c_str());
        if (has_colors()) attroff(COLOR_PAIR(3));
      } else if (s.state == SessionState::DEAD) {
        if (has_colors()) attron(COLOR_PAIR(5));
        mvprintw(y++, 0, "%s", formatted_row.c_str());
        if (has_colors()) attroff(COLOR_PAIR(5));
      } else {
        mvprintw(y++, 0, "%s", formatted_row.c_str());
      }
    }
  }

  if (y < height - 2) {
    mvprintw(y++, 0, "%s", format_border_line(width).c_str());
  }
  if (y < height - 1) {
    if (has_colors()) attron(COLOR_PAIR(1) | A_BOLD);
    mvprintw(
        y++, 0, "%s",
        format_box_line(
            " [j/k] Navigate  [Space] Freeze/Thaw  [K] Kill  [q] Quit", width)
            .c_str());
    if (has_colors()) attroff(COLOR_PAIR(1) | A_BOLD);
  }
  if (y < height) {
    mvprintw(y++, 0, "%s", format_border_line(width).c_str());
  }
}

int VirmuxTui::run() {
  if (config_.headless || !isatty(STDIN_FILENO)) {
    DaemonClient client(config_.socket_path);
    bool online = client.is_alive(200);
    std::vector<SessionInfo> sessions;
    if (online) {
      sessions = client.list_sessions(300);
    }
    std::cout << render_snapshot(sessions, 0, online, 80, 24);
    return 0;
  }

  g_tui_stop_requested.store(false);
  struct sigaction sa{}, old_int{}, old_term{}, old_hup{};
  sa.sa_handler = tui_signal_handler;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, &old_int);
  sigaction(SIGTERM, &sa, &old_term);
  sigaction(SIGHUP, &sa, &old_hup);

  initscr();
  curses_initialized_ = true;
  cbreak();
  noecho();
  keypad(stdscr, TRUE);
  curs_set(0);

  int refresh_ms = std::max(50, config_.refresh_interval_ms);
  timeout(refresh_ms);

  if (has_colors()) {
    start_color();
    use_default_colors();
    init_pair(1, COLOR_CYAN, -1);
    init_pair(2, COLOR_BLACK, COLOR_CYAN);
    init_pair(3, COLOR_YELLOW, -1);
    init_pair(4, COLOR_GREEN, -1);
    init_pair(5, COLOR_RED, -1);
  }

  DaemonClient client(config_.socket_path);
  bool running = true;

  while (running && !g_tui_stop_requested.load()) {
    bool online = client.is_alive(100);
    std::vector<SessionInfo> sessions;
    if (online) {
      sessions = client.list_sessions(300);
    }
    update_sessions(sessions);

    int rows = 24;
    int cols = 80;
    getmaxyx(stdscr, rows, cols);

    erase();
    render_curses(sessions, selected_idx_, online, cols, rows);
    refresh();

    int ch = getch();
    if (ch == ERR) {
      continue;
    }

    if (ch == KEY_RESIZE) {
      erase();
      refresh();
      continue;
    }

    if (!handle_key(ch, online ? &client : nullptr)) {
      running = false;
      break;
    }
  }

  endwin();
  curses_initialized_ = false;

  sigaction(SIGINT, &old_int, nullptr);
  sigaction(SIGTERM, &old_term, nullptr);
  sigaction(SIGHUP, &old_hup, nullptr);

  return 0;
}

}  // namespace virmux
