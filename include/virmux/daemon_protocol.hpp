/**
 * @file daemon_protocol.hpp
 * @brief Pure (I/O-free) decoding of supervisor daemon IPC request payloads.
 *
 * Every byte handed to these functions arrives from an untrusted local client
 * over the daemon's Unix socket. They are deliberately side-effect free so the
 * daemon dispatcher and the libFuzzer harnesses exercise the same code path
 * without touching the session registry or spawning sandboxes.
 */

#pragma once

#include <csignal>
#include <cstdint>
#include <string_view>

#include "virmux/session.hpp"

namespace virmux {

/**
 * @brief A daemon request decoded from (msg_type, payload).
 *
 * Only the fields relevant to @ref msg_type are populated; the others keep
 * their defaults. Unknown message types decode to a request whose msg_type is
 * preserved so the dispatcher can reply with an error.
 */
struct DaemonRequest {
  uint16_t msg_type{0};              ///< IpcMsgType of the request.
  SessionRegistration registration;  ///< MSG_REGISTER_SESSION body.
  uint64_t session_id{0};  ///< Target session for id-addressed requests.
  int signal{SIGTERM};     ///< MSG_KILL_SESSION_REQ signal number.
};

/**
 * @brief Parse a SessionRegistration from a lenient JSON object payload.
 *
 * Unknown keys are skipped; malformed numeric fields keep their defaults.
 */
SessionRegistration parse_session_registration(std::string_view json);

/**
 * @brief Parse a session ID from a plain decimal string or `{"id": N}` JSON.
 * @return Parsed ID, or 0 if no valid unsigned integer is present.
 */
uint64_t parse_session_id(std::string_view payload);

/**
 * @brief Parse a kill request: `"<id> [<signal>]"` or
 * `{"id": N, "signal": S}`.
 *
 * @param payload Raw request payload.
 * @param out_id Receives the session ID (0 when absent/invalid).
 * @param out_sig Receives the signal (SIGTERM when absent/invalid).
 */
void parse_kill_payload(std::string_view payload, uint64_t& out_id,
                        int& out_sig);

/**
 * @brief Decode a daemon request payload according to its message type.
 *
 * @param msg_type Message type from the IPC header.
 * @param payload Untrusted request payload.
 * @return Decoded request; never throws on malformed input.
 */
DaemonRequest parse_daemon_request(uint16_t msg_type, std::string_view payload);

}  // namespace virmux
