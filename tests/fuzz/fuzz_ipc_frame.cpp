/**
 * @file fuzz_ipc_frame.cpp
 * @brief libFuzzer harness for IPC wire-frame decoding.
 *
 * Why this target: the supervisor daemon listens on a Unix socket that any
 * local process running as the same user (including code inside a sandbox)
 * can connect to, and recv_ipc_message() is the first code that touches those
 * bytes. A bug here (bad length handling, over-read, huge allocation) is
 * reachable before any authentication or message dispatch.
 *
 * The input is treated as a raw byte stream of back-to-back frames and is
 * exercised two ways:
 *   1. decode_ipc_frame(), the pure decoder, walked frame by frame with
 *      invariant checks on every decoded view.
 *   2. recv_ipc_message(), the real socket path, by writing the same bytes
 *      into a socketpair. Results are cross-checked against (1) so the two
 *      implementations cannot silently diverge.
 */

#include <sys/socket.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "virmux/ipc.hpp"
#include "virmux/raii.hpp"

namespace {

/// Largest input pushed through the socketpair. Well below the default
/// AF_UNIX send buffer so the single write() can never block or short-write.
constexpr size_t kMaxSocketInput = 64 * 1024;

/**
 * @brief Abort (reported by libFuzzer as a crash) when an invariant fails.
 */
void require(bool cond) {
  if (!cond) {
    __builtin_trap();
  }
}

/**
 * @brief Decode the buffer as a stream of frames with the pure decoder.
 */
void fuzz_pure_decoder(std::string_view buf) {
  while (true) {
    virmux::IpcFrameView frame{};
    virmux::IpcDecodeStatus status = virmux::decode_ipc_frame(buf, frame);

    virmux::IpcHeader hdr{};
    virmux::IpcDecodeStatus hdr_status = virmux::decode_ipc_header(buf, hdr);

    if (status != virmux::IpcDecodeStatus::kOk) {
      // Frame-level kInvalid must come from the header; a truncated frame
      // with a valid header must be kIncomplete.
      if (status == virmux::IpcDecodeStatus::kInvalid) {
        require(hdr_status == virmux::IpcDecodeStatus::kInvalid);
      }
      return;
    }

    require(hdr_status == virmux::IpcDecodeStatus::kOk);
    require(hdr.msg_type == frame.msg_type);
    require(frame.payload.size() == hdr.payload_len);
    require(frame.payload.size() <= virmux::IPC_MAX_PAYLOAD);
    require(frame.frame_size ==
            sizeof(virmux::IpcHeader) + frame.payload.size());
    require(frame.frame_size <= buf.size());
    require(frame.payload.data() == buf.data() + sizeof(virmux::IpcHeader));

    buf.remove_prefix(frame.frame_size);
  }
}

/**
 * @brief Feed the buffer through recv_ipc_message() over a socketpair and
 * cross-check every message against the pure decoder.
 */
void fuzz_socket_path(std::string_view buf) {
  if (buf.size() > kMaxSocketInput) {
    return;
  }
  int fds[2];
  if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0) {
    return;
  }
  virmux::ScopedFd writer(fds[0]);
  virmux::ScopedFd reader(fds[1]);

  if (!buf.empty()) {
    ssize_t n = ::write(writer.get(), buf.data(), buf.size());
    require(n == static_cast<ssize_t>(buf.size()));
  }
  // EOF after the fuzz bytes: recv must fail cleanly on truncation instead
  // of waiting for more data.
  ::shutdown(writer.get(), SHUT_WR);

  std::string_view remaining = buf;
  while (true) {
    uint16_t type = 0;
    std::string payload;
    bool ok = virmux::recv_ipc_message(reader.get(), type, payload, 0);

    virmux::IpcFrameView frame{};
    virmux::IpcDecodeStatus status = virmux::decode_ipc_frame(remaining, frame);
    if (!ok) {
      require(status != virmux::IpcDecodeStatus::kOk);
      return;
    }
    require(status == virmux::IpcDecodeStatus::kOk);
    require(type == frame.msg_type);
    require(payload == frame.payload);
    remaining.remove_prefix(frame.frame_size);
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  std::string_view buf(reinterpret_cast<const char*>(data), size);
  fuzz_pure_decoder(buf);
  fuzz_socket_path(buf);
  return 0;
}
