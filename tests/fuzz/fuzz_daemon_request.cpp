/**
 * @file fuzz_daemon_request.cpp
 * @brief libFuzzer harness for supervisor daemon request payload decoding.
 *
 * Why this target: after a frame is accepted, SupervisorDaemon hands the
 * client-controlled (msg_type, payload) pair to parse_daemon_request(), which
 * runs hand-written JSON and integer parsers (registration, session id, kill
 * signal). Any local client of the daemon socket controls these bytes. The
 * parsers are pure, so the harness exercises the exact daemon decoding path
 * without a registry, cgroups, or real sandboxes.
 *
 * Input layout: bytes [0, 2) select msg_type (host byte order, like the IPC
 * header); the remainder is the payload. Every individual parser is also run
 * on the payload regardless of type to maximise coverage per input.
 */

#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "virmux/daemon_protocol.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < sizeof(uint16_t)) {
    return 0;
  }
  uint16_t msg_type = 0;
  std::memcpy(&msg_type, data, sizeof(msg_type));
  std::string_view payload(reinterpret_cast<const char*>(data) + 2, size - 2);

  virmux::DaemonRequest req = virmux::parse_daemon_request(msg_type, payload);
  if (req.msg_type != msg_type) {
    __builtin_trap();
  }

  (void)virmux::parse_session_registration(payload);
  (void)virmux::parse_session_id(payload);
  uint64_t id = 0;
  int sig = SIGTERM;
  virmux::parse_kill_payload(payload, id, sig);
  return 0;
}
