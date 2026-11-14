#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace virmux {

/**
 * @brief Protocol magic number ('V', 'M', 'U', 'X')
 */
inline constexpr uint32_t IPC_MAGIC = 0x564D5558;

/**
 * @brief Current IPC protocol version
 */
inline constexpr uint16_t IPC_VERSION = 1;

/**
 * @brief Maximum permitted payload length (16 MiB safety limit)
 */
inline constexpr uint32_t IPC_MAX_PAYLOAD = 16 * 1024 * 1024;

/**
 * @brief Packed binary TLV header for all IPC messages.
 *
 * Exactly 12 bytes: magic (4), version (2), msg_type (2), payload_len (4).
 * Serialized in host native byte order for zero-copy performance over local AF_UNIX sockets.
 */
#pragma pack(push, 1)
struct IpcHeader {
  uint32_t magic;       ///< 0x564D5558 ('V', 'M', 'U', 'X') in host byte order
  uint16_t version;     ///< IPC protocol version (1)
  uint16_t msg_type;    ///< Message type identifier (see IpcMsgType)
  uint32_t payload_len; ///< Byte length of payload immediately following header
};
#pragma pack(pop)

static_assert(sizeof(IpcHeader) == 12, "IpcHeader must be exactly 12 bytes");

/**
 * @brief IPC message type identifiers.
 */
enum IpcMsgType : uint16_t {
  MSG_PING = 0x0001,
  MSG_PONG = 0x0002,
  MSG_REGISTER_SESSION = 0x0010,
  MSG_UNREGISTER_SESSION = 0x0011,
  MSG_LIST_SESSIONS_REQ = 0x0020,
  MSG_LIST_SESSIONS_RESP = 0x0021,
  MSG_FREEZE_SESSION_REQ = 0x0030,
  MSG_FREEZE_SESSION_RESP = 0x0031,
  MSG_THAW_SESSION_REQ = 0x0032,
  MSG_THAW_SESSION_RESP = 0x0033,
  MSG_KILL_SESSION_REQ = 0x0040,
  MSG_KILL_SESSION_RESP = 0x0041,
  MSG_GET_STATS_REQ = 0x0050,
  MSG_GET_STATS_RESP = 0x0051,
  MSG_CHECKPOINT_SESSION_REQ = 0x0060,
  MSG_CHECKPOINT_SESSION_RESP = 0x0061,
  MSG_RESTORE_SESSION_REQ = 0x0062,
  MSG_RESTORE_SESSION_RESP = 0x0063,
  MSG_ERROR_RESP = 0x00E0,
  MSG_ACK_RESP = 0x00E1,
};

/**
 * @brief Resolve the Unix domain socket path for virmux daemon IPC.
 *
 * Checks VIRMUX_SOCKET environment variable first.
 * If unset, checks /run/user/<UID>/virmux.sock.
 * Falls back to /tmp/virmux-<UID>.sock if runtime dir is unavailable or unwritable.
 *
 * @return Resolved socket filesystem path.
 */
std::string resolve_socket_path();

/**
 * @brief Send a binary-framed IPC message over a connected socket.
 *
 * Encodes the 12-byte IpcHeader and payload, handling partial sends and poll timeouts.
 *
 * @param fd Connected socket descriptor.
 * @param msg_type Message type (e.g. MSG_PING).
 * @param payload Payload string or byte view.
 * @param timeout_ms Timeout in milliseconds (-1 for infinite, 0 for non-blocking).
 * @return true on complete transmission, false on timeout, disconnect, or error.
 */
bool send_ipc_message(int fd, uint16_t msg_type, std::string_view payload, int timeout_ms = 1000);

/**
 * @brief Receive a binary-framed IPC message from a connected socket.
 *
 * Reads and validates the 12-byte IpcHeader and full payload with poll timeouts.
 *
 * @param fd Connected socket descriptor.
 * @param out_type Output parameter receiving the message type.
 * @param out_payload Output parameter receiving the payload content.
 * @param timeout_ms Timeout in milliseconds (-1 for infinite, 0 for non-blocking).
 * @return true on successful reception, false on timeout, header error, disconnect, or payload > 16MiB.
 */
bool recv_ipc_message(int fd, uint16_t& out_type, std::string& out_payload, int timeout_ms = 1000);

}  // namespace virmux
