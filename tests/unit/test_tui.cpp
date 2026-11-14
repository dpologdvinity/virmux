/**
 * @file test_tui.cpp
 * @brief Unit tests for VirmuxTui dashboard snapshot rendering, offline mode,
 * and navigation bounds.
 */

extern "C" {
#include "unity/unity.h"
}

#include <curses.h>

#include <cstring>
#include <string>
#include <vector>

#include "virmux/session.hpp"
#include "virmux/tui.hpp"

void setUp(void) {}
void tearDown(void) {}

void test_tui_render_snapshot_header_and_footer(void) {
  virmux::VirmuxTui tui;
  std::vector<virmux::SessionInfo> sessions;
  std::string snap = tui.render_snapshot(sessions, 0, true, 80, 24);

  // 1. Verify Title and version
  TEST_ASSERT_NOT_NULL(strstr(snap.c_str(), "VIRMUX v0.1.0"));

  // 2. Verify Linux system information
  TEST_ASSERT_NOT_NULL(strstr(snap.c_str(), "Linux"));
  TEST_ASSERT_NOT_NULL(strstr(snap.c_str(), "Cores"));
  TEST_ASSERT_NOT_NULL(strstr(snap.c_str(), "RAM"));
  TEST_ASSERT_NOT_NULL(
      strstr(snap.c_str(), "Dedicated Shield: Cores 0-1 (Fluid)"));
  TEST_ASSERT_NOT_NULL(strstr(snap.c_str(), "Worker Pool:"));

  // 3. Verify Host Load telemetry
  TEST_ASSERT_NOT_NULL(strstr(snap.c_str(), "Host Load:"));

  // 4. Verify Footer hotkeys
  TEST_ASSERT_NOT_NULL(strstr(snap.c_str(), "[j/k] Navigate"));
  TEST_ASSERT_NOT_NULL(strstr(snap.c_str(), "[Space] Freeze/Thaw"));
  TEST_ASSERT_NOT_NULL(strstr(snap.c_str(), "[K] Kill"));
  TEST_ASSERT_NOT_NULL(strstr(snap.c_str(), "[q] Quit"));
}

void test_tui_render_snapshot_sessions(void) {
  virmux::VirmuxTui tui;
  std::vector<virmux::SessionInfo> sessions;

  virmux::SessionInfo s1;
  s1.id = 1;
  s1.name = "task-alpha";
  s1.state = virmux::SessionState::RUNNING_NORMAL;
  s1.virtual_cores = 2;
  s1.memory_rss_bytes = 10485760;  // 10.0 MiB
  s1.ksm_dedup_bytes = 2097152;    // 2.0 MiB
  s1.command = "npm run build";
  sessions.push_back(s1);

  virmux::SessionInfo s2;
  s2.id = 2;
  s2.name = "task-beta";
  s2.state = virmux::SessionState::FROZEN;
  s2.virtual_cores = 4;
  s2.memory_rss_bytes = 20971520;  // 20.0 MiB
  s2.ksm_dedup_bytes = 4194304;    // 4.0 MiB
  s2.command = "python3 worker.py";
  sessions.push_back(s2);

  // Snapshot with row 0 selected
  std::string snap0 = tui.render_snapshot(sessions, 0, true, 80, 24);

  // Both sessions rendered
  TEST_ASSERT_NOT_NULL(strstr(snap0.c_str(), "task-alpha"));
  TEST_ASSERT_NOT_NULL(strstr(snap0.c_str(), "task-beta"));
  TEST_ASSERT_NOT_NULL(strstr(snap0.c_str(), "npm run build"));
  TEST_ASSERT_NOT_NULL(strstr(snap0.c_str(), "python3 worker.py"));
  TEST_ASSERT_NOT_NULL(strstr(snap0.c_str(), "RUNNING"));
  TEST_ASSERT_NOT_NULL(strstr(snap0.c_str(), "FROZEN"));

  // Verify selected row 0 is indicated with '>'
  const char* alpha_pos = strstr(snap0.c_str(), "task-alpha");
  TEST_ASSERT_NOT_NULL(alpha_pos);
  const char* line_alpha_start = alpha_pos;
  while (line_alpha_start > snap0.c_str() && *(line_alpha_start - 1) != '\n') {
    --line_alpha_start;
  }
  const char* gt0 = strchr(line_alpha_start, '>');
  TEST_ASSERT_NOT_NULL(gt0);
  TEST_ASSERT_TRUE(gt0 < alpha_pos);

  // Verify unselected row 1 does not have '>'
  const char* beta_pos = strstr(snap0.c_str(), "task-beta");
  TEST_ASSERT_NOT_NULL(beta_pos);
  const char* line_beta_start = beta_pos;
  while (line_beta_start > snap0.c_str() && *(line_beta_start - 1) != '\n') {
    --line_beta_start;
  }
  const char* beta_line_end = strchr(line_beta_start, '\n');
  std::string beta_line(line_beta_start, beta_line_end
                                             ? beta_line_end - line_beta_start
                                             : strlen(line_beta_start));
  TEST_ASSERT_TRUE(beta_line.find('>') == std::string::npos);

  // Snapshot with row 1 selected
  std::string snap1 = tui.render_snapshot(sessions, 1, true, 80, 24);
  const char* beta_pos1 = strstr(snap1.c_str(), "task-beta");
  TEST_ASSERT_NOT_NULL(beta_pos1);
  const char* line_beta1_start = beta_pos1;
  while (line_beta1_start > snap1.c_str() && *(line_beta1_start - 1) != '\n') {
    --line_beta1_start;
  }
  const char* gt1 = strchr(line_beta1_start, '>');
  TEST_ASSERT_NOT_NULL(gt1);
  TEST_ASSERT_TRUE(gt1 < beta_pos1);
}

void test_tui_render_snapshot_offline_daemon(void) {
  virmux::VirmuxTui tui;
  std::vector<virmux::SessionInfo> sessions;

  std::string snap_offline = tui.render_snapshot(sessions, 0, false, 80, 24);
  TEST_ASSERT_NOT_NULL(strstr(snap_offline.c_str(),
                              "[Supervisor Daemon Offline (Standalone Mode)]"));

  std::string snap_online = tui.render_snapshot(sessions, 0, true, 80, 24);
  TEST_ASSERT_NULL(strstr(snap_online.c_str(),
                          "[Supervisor Daemon Offline (Standalone Mode)]"));
}

void test_tui_navigation_bounds(void) {
  virmux::VirmuxTui tui;

  std::vector<virmux::SessionInfo> sessions;
  for (int i = 0; i < 3; ++i) {
    virmux::SessionInfo s;
    s.id = static_cast<uint64_t>(i + 1);
    s.name = "sess-" + std::to_string(i + 1);
    s.state = virmux::SessionState::RUNNING_NORMAL;
    sessions.push_back(s);
  }
  tui.update_sessions(sessions);

  // Initial index should be 0
  TEST_ASSERT_EQUAL_UINT(0, tui.selected_index());

  // Clamping at index 0 when moving up
  tui.move_up();
  TEST_ASSERT_EQUAL_UINT(0, tui.selected_index());
  tui.handle_key(KEY_UP);
  TEST_ASSERT_EQUAL_UINT(0, tui.selected_index());
  tui.handle_key('k');
  TEST_ASSERT_EQUAL_UINT(0, tui.selected_index());

  // Moving down step-by-step
  tui.move_down();
  TEST_ASSERT_EQUAL_UINT(1, tui.selected_index());

  tui.handle_key('j');
  TEST_ASSERT_EQUAL_UINT(2, tui.selected_index());

  // Clamping at max index (index 2 for 3 sessions)
  tui.move_down();
  TEST_ASSERT_EQUAL_UINT(2, tui.selected_index());
  tui.handle_key(KEY_DOWN);
  TEST_ASSERT_EQUAL_UINT(2, tui.selected_index());
  tui.handle_key('j');
  TEST_ASSERT_EQUAL_UINT(2, tui.selected_index());

  // Clamping when session count shrinks
  std::vector<virmux::SessionInfo> single_session;
  single_session.push_back(sessions[0]);
  tui.update_sessions(single_session);
  TEST_ASSERT_EQUAL_UINT(0, tui.selected_index());

  // Clamping when session list is empty
  std::vector<virmux::SessionInfo> empty_sessions;
  tui.update_sessions(empty_sessions);
  TEST_ASSERT_EQUAL_UINT(0, tui.selected_index());
  tui.move_up();
  TEST_ASSERT_EQUAL_UINT(0, tui.selected_index());
  tui.move_down();
  TEST_ASSERT_EQUAL_UINT(0, tui.selected_index());

  // Direct setter clamping
  tui.update_sessions(sessions);
  tui.set_selected_index(999);
  TEST_ASSERT_EQUAL_UINT(2, tui.selected_index());
}

void test_tui_empty_sessions(void) {
  virmux::VirmuxTui tui;
  std::vector<virmux::SessionInfo> empty_sessions;

  std::string snap_empty = tui.render_snapshot(empty_sessions, 0, true, 80, 24);
  TEST_ASSERT_NOT_NULL(strstr(snap_empty.c_str(), "No active sessions"));

  std::vector<virmux::SessionInfo> populated;
  virmux::SessionInfo s;
  s.id = 1;
  s.name = "active-task";
  populated.push_back(s);

  std::string snap_pop = tui.render_snapshot(populated, 0, true, 80, 24);
  TEST_ASSERT_NULL(strstr(snap_pop.c_str(), "No active sessions"));
}

void test_tui_handle_key_actions(void) {
  virmux::VirmuxTui tui;
  std::vector<virmux::SessionInfo> sessions;
  virmux::SessionInfo s;
  s.id = 10;
  s.name = "test-job";
  s.state = virmux::SessionState::RUNNING_NORMAL;
  sessions.push_back(s);
  tui.update_sessions(sessions);

  // 'q' and 'Q' signal exit
  TEST_ASSERT_FALSE(tui.handle_key('q'));
  TEST_ASSERT_FALSE(tui.handle_key('Q'));

  // Navigation returns true
  TEST_ASSERT_TRUE(tui.handle_key('j'));
  TEST_ASSERT_TRUE(tui.handle_key('k'));

  // Space and K return true without crashing when daemon is offline
  TEST_ASSERT_TRUE(tui.handle_key(' '));
  TEST_ASSERT_TRUE(tui.handle_key('K'));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_tui_render_snapshot_header_and_footer);
  RUN_TEST(test_tui_render_snapshot_sessions);
  RUN_TEST(test_tui_render_snapshot_offline_daemon);
  RUN_TEST(test_tui_navigation_bounds);
  RUN_TEST(test_tui_empty_sessions);
  RUN_TEST(test_tui_handle_key_actions);
  return UNITY_END();
}
