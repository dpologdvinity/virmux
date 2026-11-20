/**
 * @file test_ipc.cpp
 * @brief Unit tests for IPC binary framing, socket path resolution, and Unix
 * socket transport.
 */

extern "C" {
#include "unity/unity.h"
}

#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

#include "virmux/ipc.hpp"
#include "virmux/raii.hpp"

void setUp(void) {}
void tearDown(void) {}

/**
 * @brief Test 1: Verify header packing size is exactly 12 bytes and magic
 * encoding is correct.
 */
void test_ipc_header_size_and_endianness(void) {
  TEST_ASSERT_EQUAL_UINT32(12, sizeof(virmux::IpcHeader));
  TEST_ASSERT_EQUAL_HEX32(0x564D5558, virmux::IPC_MAGIC);
  TEST_ASSERT_EQUAL_UINT16(1, virmux::IPC_VERSION);
  TEST_ASSERT_EQUAL_UINT32(16 * 1024 * 1024, virmux::IPC_MAX_PAYLOAD);

  virmux::IpcHeader hdr{};
  hdr.magic = virmux::IPC_MAGIC;
  hdr.version = virmux::IPC_VERSION;
  hdr.msg_type = virmux::MSG_PING;
  hdr.payload_len = 0;

  TEST_ASSERT_EQUAL_HEX32(0x564D5558, hdr.magic);
  TEST_ASSERT_EQUAL_UINT16(1, hdr.version);
  TEST_ASSERT_EQUAL_UINT16(0x0001, hdr.msg_type);
  TEST_ASSERT_EQUAL_UINT32(0, hdr.payload_len);

  // Verify memory layout for magic ASCII representation: 'V', 'M', 'U', 'X'
  const auto* bytes = reinterpret_cast<const uint8_t*>(&hdr.magic);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  TEST_ASSERT_EQUAL_HEX8('X', bytes[0]);
  TEST_ASSERT_EQUAL_HEX8('U', bytes[1]);
  TEST_ASSERT_EQUAL_HEX8('M', bytes[2]);
  TEST_ASSERT_EQUAL_HEX8('V', bytes[3]);
#else
  TEST_ASSERT_EQUAL_HEX8('V', bytes[0]);
  TEST_ASSERT_EQUAL_HEX8('M', bytes[1]);
  TEST_ASSERT_EQUAL_HEX8('U', bytes[2]);
  TEST_ASSERT_EQUAL_HEX8('X', bytes[3]);
#endif
}

/**
 * @brief Test 2: Verify socket path resolution with env override, user runtime
 * dir, and /tmp fallback.
 */
void test_resolve_socket_path(void) {
  const char* orig_env = std::getenv("VIRMUX_SOCKET");
  std::string saved_env = (orig_env != nullptr) ? orig_env : "";
  bool had_env = (orig_env != nullptr);

  // Test environment variable override
  const std::string custom_socket = "/tmp/virmux_custom_override.sock";
  ::setenv("VIRMUX_SOCKET", custom_socket.c_str(), 1);
  TEST_ASSERT_EQUAL_STRING(custom_socket.c_str(),
                           virmux::resolve_socket_path().c_str());

  // Test resolution without environment variable
  ::unsetenv("VIRMUX_SOCKET");
  uid_t uid = ::getuid();
  std::string expected_user_dir = "/run/user/" + std::to_string(uid);

  std::error_code ec;
  std::string expected;
  if (std::filesystem::is_directory(expected_user_dir, ec) &&
      ::access(expected_user_dir.c_str(), W_OK | X_OK) == 0) {
    expected = expected_user_dir + "/virmux.sock";
  } else {
    expected = "/tmp/virmux-" + std::to_string(uid) + ".sock";
  }

  TEST_ASSERT_EQUAL_STRING(expected.c_str(),
                           virmux::resolve_socket_path().c_str());

  // Restore original environment
  if (had_env) {
    ::setenv("VIRMUX_SOCKET", saved_env.c_str(), 1);
  } else {
    ::unsetenv("VIRMUX_SOCKET");
  }
}

/**
 * @brief Test 3: Send and receive messages of varying sizes using socketpair.
 */
void test_ipc_send_recv_socketpair(void) {
  int fds[2];
  TEST_ASSERT_EQUAL_INT(0, ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
  virmux::ScopedFd fd0(fds[0]);
  virmux::ScopedFd fd1(fds[1]);

  // 1. Empty payload (e.g. MSG_PING)
  {
    bool sent = virmux::send_ipc_message(fd0.get(), virmux::MSG_PING, "", 1000);
    TEST_ASSERT_TRUE(sent);

    uint16_t msg_type = 0;
    std::string payload = "non-empty-init";
    bool recvd = virmux::recv_ipc_message(fd1.get(), msg_type, payload, 1000);
    TEST_ASSERT_TRUE(recvd);
    TEST_ASSERT_EQUAL_UINT16(virmux::MSG_PING, msg_type);
    TEST_ASSERT_EQUAL_UINT32(0, payload.size());
  }

  // 2. Small text payload (e.g. MSG_ACK_RESP)
  {
    std::string text_msg = "virmux session registration acknowledged";
    bool sent = virmux::send_ipc_message(fd1.get(), virmux::MSG_ACK_RESP,
                                         text_msg, 1000);
    TEST_ASSERT_TRUE(sent);

    uint16_t msg_type = 0;
    std::string payload;
    bool recvd = virmux::recv_ipc_message(fd0.get(), msg_type, payload, 1000);
    TEST_ASSERT_TRUE(recvd);
    TEST_ASSERT_EQUAL_UINT16(virmux::MSG_ACK_RESP, msg_type);
    TEST_ASSERT_EQUAL_STRING(text_msg.c_str(), payload.c_str());
  }

  // 3. 100KB binary payload (e.g. MSG_LIST_SESSIONS_RESP)
  {
    std::string binary_data(100UL * 1024, '\0');
    for (size_t i = 0; i < binary_data.size(); ++i) {
      binary_data[i] = static_cast<char>((i * 31 + 7) & 0xFF);
    }

    bool sent = virmux::send_ipc_message(
        fd0.get(), virmux::MSG_LIST_SESSIONS_RESP, binary_data, 1000);
    TEST_ASSERT_TRUE(sent);

    uint16_t msg_type = 0;
    std::string payload;
    bool recvd = virmux::recv_ipc_message(fd1.get(), msg_type, payload, 1000);
    TEST_ASSERT_TRUE(recvd);
    TEST_ASSERT_EQUAL_UINT16(virmux::MSG_LIST_SESSIONS_RESP, msg_type);
    TEST_ASSERT_EQUAL_UINT32(binary_data.size(), payload.size());
    TEST_ASSERT_EQUAL_MEMORY(binary_data.data(), payload.data(),
                             binary_data.size());
  }
}

/**
 * @brief Test 4: Verify timeout expiration when receiving on an idle socket.
 */
void test_ipc_timeout_on_empty_socket(void) {
  int fds[2];
  TEST_ASSERT_EQUAL_INT(0, ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
  virmux::ScopedFd fd0(fds[0]);
  virmux::ScopedFd fd1(fds[1]);

  uint16_t msg_type = 0;
  std::string payload;
  auto t0 = std::chrono::steady_clock::now();
  bool recvd = virmux::recv_ipc_message(fd0.get(), msg_type, payload, 60);
  auto t1 = std::chrono::steady_clock::now();
  auto elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

  TEST_ASSERT_FALSE(recvd);
  TEST_ASSERT_TRUE(elapsed_ms >= 50);
}

/**
 * @brief Test 5: Verify corrupted magic number header is rejected.
 */
void test_ipc_invalid_magic_rejected(void) {
  int fds[2];
  TEST_ASSERT_EQUAL_INT(0, ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
  virmux::ScopedFd fd0(fds[0]);
  virmux::ScopedFd fd1(fds[1]);

  virmux::IpcHeader bad_hdr{};
  bad_hdr.magic = 0xDEADBEEF;
  bad_hdr.version = virmux::IPC_VERSION;
  bad_hdr.msg_type = virmux::MSG_PING;
  bad_hdr.payload_len = 0;

  ssize_t n = ::write(fd0.get(), &bad_hdr, sizeof(bad_hdr));
  TEST_ASSERT_EQUAL_INT(sizeof(bad_hdr), n);

  uint16_t msg_type = 0;
  std::string payload;
  bool recvd = virmux::recv_ipc_message(fd1.get(), msg_type, payload, 500);
  TEST_ASSERT_FALSE(recvd);
}

/**
 * @brief Test 6: Verify oversized payload (> 16 MiB) is rejected.
 */
void test_ipc_oversized_payload_rejected(void) {
  int fds[2];
  TEST_ASSERT_EQUAL_INT(0, ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
  virmux::ScopedFd fd0(fds[0]);
  virmux::ScopedFd fd1(fds[1]);

  // Case A: Receiving a header claiming payload exceeds 16 MiB limit
  virmux::IpcHeader huge_hdr{};
  huge_hdr.magic = virmux::IPC_MAGIC;
  huge_hdr.version = virmux::IPC_VERSION;
  huge_hdr.msg_type = virmux::MSG_PING;
  huge_hdr.payload_len = virmux::IPC_MAX_PAYLOAD + 1024;

  ssize_t n = ::write(fd0.get(), &huge_hdr, sizeof(huge_hdr));
  TEST_ASSERT_EQUAL_INT(sizeof(huge_hdr), n);

  uint16_t msg_type = 0;
  std::string payload;
  bool recvd = virmux::recv_ipc_message(fd1.get(), msg_type, payload, 500);
  TEST_ASSERT_FALSE(recvd);

  // Case B: Sending a payload exceeding 16 MiB
  std::string_view oversized_view("", virmux::IPC_MAX_PAYLOAD + 1);
  bool sent = virmux::send_ipc_message(fd0.get(), virmux::MSG_PING,
                                       oversized_view, 500);
  TEST_ASSERT_FALSE(sent);
}

/**
 * @brief Additional Test: Verify invalid protocol version is rejected.
 */
void test_ipc_invalid_version_rejected(void) {
  int fds[2];
  TEST_ASSERT_EQUAL_INT(0, ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
  virmux::ScopedFd fd0(fds[0]);
  virmux::ScopedFd fd1(fds[1]);

  virmux::IpcHeader bad_hdr{};
  bad_hdr.magic = virmux::IPC_MAGIC;
  bad_hdr.version = 99;
  bad_hdr.msg_type = virmux::MSG_PING;
  bad_hdr.payload_len = 0;

  ssize_t n = ::write(fd0.get(), &bad_hdr, sizeof(bad_hdr));
  TEST_ASSERT_EQUAL_INT(sizeof(bad_hdr), n);

  uint16_t msg_type = 0;
  std::string payload;
  bool recvd = virmux::recv_ipc_message(fd1.get(), msg_type, payload, 500);
  TEST_ASSERT_FALSE(recvd);
}

/**
 * @brief Additional Test: Verify graceful handling of closed sockets and
 * invalid fds.
 */
void test_ipc_socket_disconnect_and_invalid_fd(void) {
  int fds[2];
  TEST_ASSERT_EQUAL_INT(0, ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
  ::close(fds[1]);
  virmux::ScopedFd fd0(fds[0]);

  uint16_t msg_type = 0;
  std::string payload;
  // Recv on peer-closed socket returns false immediately
  TEST_ASSERT_FALSE(
      virmux::recv_ipc_message(fd0.get(), msg_type, payload, 100));
  // Send on peer-closed socket fails cleanly without SIGPIPE crashing process
  TEST_ASSERT_FALSE(
      virmux::send_ipc_message(fd0.get(), virmux::MSG_PING, "ping", 100));

  // Invalid fd checks
  TEST_ASSERT_FALSE(virmux::recv_ipc_message(-1, msg_type, payload, 100));
  TEST_ASSERT_FALSE(virmux::send_ipc_message(-1, virmux::MSG_PING, "", 100));
}

/**
 * @brief Verify fragmented stream reassembly across partial read boundaries.
 */
void test_ipc_fragmented_stream_reassembly(void) {
  int fds[2];
  TEST_ASSERT_EQUAL_INT(0, ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
  virmux::ScopedFd fd0(fds[0]);
  virmux::ScopedFd fd1(fds[1]);

  std::string test_payload =
      "fragmented_stream_test_payload_1234567890_abcdefghij";
  virmux::IpcHeader hdr{};
  hdr.magic = virmux::IPC_MAGIC;
  hdr.version = virmux::IPC_VERSION;
  hdr.msg_type = virmux::MSG_LIST_SESSIONS_RESP;
  hdr.payload_len = static_cast<uint32_t>(test_payload.size());

  // Write header in small 3-byte chunks
  const uint8_t* hdr_bytes = reinterpret_cast<const uint8_t*>(&hdr);
  size_t hdr_sent = 0;
  while (hdr_sent < sizeof(hdr)) {
    size_t chunk = std::min<size_t>(3, sizeof(hdr) - hdr_sent);
    ssize_t n = ::write(fd0.get(), hdr_bytes + hdr_sent, chunk);
    TEST_ASSERT_EQUAL_INT(chunk, n);
    hdr_sent += chunk;
  }

  // Write payload in 7-byte chunks
  const uint8_t* payload_bytes =
      reinterpret_cast<const uint8_t*>(test_payload.data());
  size_t payload_sent = 0;
  while (payload_sent < test_payload.size()) {
    size_t chunk = std::min<size_t>(7, test_payload.size() - payload_sent);
    ssize_t n = ::write(fd0.get(), payload_bytes + payload_sent, chunk);
    TEST_ASSERT_EQUAL_INT(chunk, n);
    payload_sent += chunk;
  }

  // Recv should correctly reassemble entire packet
  uint16_t msg_type = 0;
  std::string out_payload;
  bool recvd = virmux::recv_ipc_message(fd1.get(), msg_type, out_payload, 1000);
  TEST_ASSERT_TRUE(recvd);
  TEST_ASSERT_EQUAL_HEX16(virmux::MSG_LIST_SESSIONS_RESP, msg_type);
  TEST_ASSERT_EQUAL_STRING(test_payload.c_str(), out_payload.c_str());
}

namespace {

/**
 * @brief Build a raw wire frame (header + payload) for pure decoder tests.
 */
std::string make_frame(uint32_t magic, uint16_t version, uint16_t msg_type,
                       uint32_t payload_len, std::string_view payload) {
  virmux::IpcHeader hdr{};
  hdr.magic = magic;
  hdr.version = version;
  hdr.msg_type = msg_type;
  hdr.payload_len = payload_len;
  std::string frame(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
  frame.append(payload);
  return frame;
}

}  // namespace

/**
 * @brief Verify the pure frame decoder accepts a well-formed frame and reports
 * the exact number of bytes it consumed.
 */
void test_ipc_decode_frame_valid(void) {
  std::string wire = make_frame(virmux::IPC_MAGIC, virmux::IPC_VERSION,
                                virmux::MSG_KILL_SESSION_REQ, 4, "7 15");
  wire += "trailing";

  virmux::IpcFrameView frame{};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(virmux::IpcDecodeStatus::kOk),
      static_cast<int>(virmux::decode_ipc_frame(wire, frame)));
  TEST_ASSERT_EQUAL_HEX16(virmux::MSG_KILL_SESSION_REQ, frame.msg_type);
  TEST_ASSERT_EQUAL_STRING("7 15", std::string(frame.payload).c_str());
  TEST_ASSERT_EQUAL_UINT64(sizeof(virmux::IpcHeader) + 4, frame.frame_size);
}

/**
 * @brief Verify truncated headers and payloads report kIncomplete rather than
 * reading past the end of the buffer.
 */
void test_ipc_decode_frame_incomplete(void) {
  std::string wire = make_frame(virmux::IPC_MAGIC, virmux::IPC_VERSION,
                                virmux::MSG_PING, 10, "abc");
  virmux::IpcFrameView frame{};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(virmux::IpcDecodeStatus::kIncomplete),
      static_cast<int>(virmux::decode_ipc_frame(wire, frame)));

  for (size_t len = 0; len < sizeof(virmux::IpcHeader); ++len) {
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(virmux::IpcDecodeStatus::kIncomplete),
        static_cast<int>(virmux::decode_ipc_frame(
            std::string_view(wire.data(), len), frame)));
  }
}

/**
 * @brief Verify bad magic, bad version, and oversized length are rejected by
 * the header decoder before any payload is examined.
 */
void test_ipc_decode_header_invalid(void) {
  virmux::IpcHeader hdr{};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(virmux::IpcDecodeStatus::kInvalid),
      static_cast<int>(virmux::decode_ipc_header(
          make_frame(0xDEADBEEF, virmux::IPC_VERSION, virmux::MSG_PING, 0, ""),
          hdr)));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(virmux::IpcDecodeStatus::kInvalid),
      static_cast<int>(virmux::decode_ipc_header(
          make_frame(virmux::IPC_MAGIC, 99, virmux::MSG_PING, 0, ""), hdr)));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(virmux::IpcDecodeStatus::kInvalid),
      static_cast<int>(virmux::decode_ipc_header(
          make_frame(virmux::IPC_MAGIC, virmux::IPC_VERSION, virmux::MSG_PING,
                     virmux::IPC_MAX_PAYLOAD + 1, ""),
          hdr)));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(virmux::IpcDecodeStatus::kOk),
      static_cast<int>(virmux::decode_ipc_header(
          make_frame(virmux::IPC_MAGIC, virmux::IPC_VERSION, virmux::MSG_PING,
                     virmux::IPC_MAX_PAYLOAD, ""),
          hdr)));
  TEST_ASSERT_EQUAL_UINT32(virmux::IPC_MAX_PAYLOAD, hdr.payload_len);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_ipc_decode_frame_valid);
  RUN_TEST(test_ipc_decode_frame_incomplete);
  RUN_TEST(test_ipc_decode_header_invalid);
  RUN_TEST(test_ipc_header_size_and_endianness);
  RUN_TEST(test_resolve_socket_path);
  RUN_TEST(test_ipc_send_recv_socketpair);
  RUN_TEST(test_ipc_timeout_on_empty_socket);
  RUN_TEST(test_ipc_invalid_magic_rejected);
  RUN_TEST(test_ipc_oversized_payload_rejected);
  RUN_TEST(test_ipc_invalid_version_rejected);
  RUN_TEST(test_ipc_socket_disconnect_and_invalid_fd);
  RUN_TEST(test_ipc_fragmented_stream_reassembly);
  return UNITY_END();
}
