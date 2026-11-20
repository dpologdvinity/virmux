/**
 * @file fuzz_session_list_json.cpp
 * @brief libFuzzer harness for SessionRegistry::from_json().
 *
 * Why this target: DaemonClient::list_sessions() (used by `virmux list` and
 * the `virmux top` TUI) parses the daemon's MSG_LIST_SESSIONS_RESP with this
 * hand-written JSON scanner. The socket path falls back to
 * /tmp/virmux-<uid>.sock when /run/user/<uid> is unavailable, so whatever
 * process owns that path is effectively untrusted from the client's point of
 * view. Session names and commands echoed back in the list are also chosen by
 * whichever client registered them.
 */

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "virmux/session.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  std::string_view json(reinterpret_cast<const char*>(data), size);
  std::vector<virmux::SessionInfo> sessions =
      virmux::SessionRegistry::from_json(json);
  // Touch the string state the CLI renders so ASan sees any corruption.
  size_t total = 0;
  for (const auto& s : sessions) {
    (void)virmux::session_state_to_string(s.state);
    total += s.name.size() + s.command.size() + s.cgroup_path.size();
  }
  if (total > size * 2 + 1) {
    // Unescaping never grows a string beyond its source bytes.
    __builtin_trap();
  }
  return 0;
}
