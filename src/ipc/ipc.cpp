#include "virmux/ipc.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace virmux {

namespace {

/**
 * @brief Poll a file descriptor for read or write readiness with deadline enforcement.
 */
bool poll_fd(int fd, short events, int timeout_ms, std::chrono::steady_clock::time_point deadline) {
  int wait_ms = 0;
  if (timeout_ms < 0) {
    wait_ms = -1;
  } else if (timeout_ms == 0) {
    wait_ms = 0;
  } else {
    auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      return false;
    }
    auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    wait_ms = (rem <= 0) ? 0 : static_cast<int>(rem);
  }

  struct pollfd pfd{};
  pfd.fd = fd;
  pfd.events = events;

  while (true) {
    int ret = ::poll(&pfd, 1, wait_ms);
    if (ret > 0) {
      if ((events & POLLOUT) && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
        return false;
      }
      if ((events & POLLIN) && (pfd.revents & (POLLERR | POLLNVAL))) {
        return false;
      }
      return true;
    }
    if (ret == 0) {
      return false;  // Timed out
    }
    if (errno == EINTR) {
      if (timeout_ms > 0) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
          return false;
        }
        auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        wait_ms = (rem <= 0) ? 0 : static_cast<int>(rem);
      }
      continue;
    }
    return false;  // Unrecoverable poll error
  }
}

/**
 * @brief Send all bytes in buffer, handling partial sends and timeouts.
 */
bool send_all(int fd, const void* buffer, size_t length, int timeout_ms,
              std::chrono::steady_clock::time_point deadline) {
  const auto* ptr = static_cast<const uint8_t*>(buffer);
  size_t remaining = length;

  while (remaining > 0) {
    if (!poll_fd(fd, POLLOUT, timeout_ms, deadline)) {
      return false;
    }
    ssize_t n = ::send(fd, ptr, remaining, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n > 0) {
      ptr += n;
      remaining -= static_cast<size_t>(n);
    } else if (n < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
      return false;
    } else {
      return false;
    }
  }
  return true;
}

/**
 * @brief Receive exactly length bytes into buffer, handling partial reads and timeouts.
 */
bool recv_all(int fd, void* buffer, size_t length, int timeout_ms,
              std::chrono::steady_clock::time_point deadline) {
  auto* ptr = static_cast<uint8_t*>(buffer);
  size_t remaining = length;

  while (remaining > 0) {
    if (!poll_fd(fd, POLLIN, timeout_ms, deadline)) {
      return false;
    }
    ssize_t n = ::recv(fd, ptr, remaining, MSG_DONTWAIT);
    if (n > 0) {
      ptr += n;
      remaining -= static_cast<size_t>(n);
    } else if (n < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
      return false;
    } else {
      // Peer closed connection prematurely
      return false;
    }
  }
  return true;
}

}  // namespace

std::string resolve_socket_path() {
  const char* env_path = std::getenv("VIRMUX_SOCKET");
  if (env_path != nullptr && env_path[0] != '\0') {
    return std::string(env_path);
  }

  uid_t uid = ::getuid();
  std::string run_user_dir = "/run/user/" + std::to_string(uid);
  std::error_code ec;
  if (std::filesystem::is_directory(run_user_dir, ec) &&
      ::access(run_user_dir.c_str(), W_OK | X_OK) == 0) {
    return run_user_dir + "/virmux.sock";
  }

  return "/tmp/virmux-" + std::to_string(uid) + ".sock";
}

bool send_ipc_message(int fd, uint16_t msg_type, std::string_view payload, int timeout_ms) {
  if (fd < 0 || payload.size() > IPC_MAX_PAYLOAD) {
    return false;
  }

  auto start = std::chrono::steady_clock::now();
  auto deadline = start + std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);

  IpcHeader hdr{};
  hdr.magic = IPC_MAGIC;
  hdr.version = IPC_VERSION;
  hdr.msg_type = msg_type;
  hdr.payload_len = static_cast<uint32_t>(payload.size());

  if (!send_all(fd, &hdr, sizeof(hdr), timeout_ms, deadline)) {
    return false;
  }

  if (!payload.empty()) {
    if (!send_all(fd, payload.data(), payload.size(), timeout_ms, deadline)) {
      return false;
    }
  }

  return true;
}

bool recv_ipc_message(int fd, uint16_t& out_type, std::string& out_payload, int timeout_ms) {
  if (fd < 0) {
    return false;
  }

  auto start = std::chrono::steady_clock::now();
  auto deadline = start + std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);

  IpcHeader hdr{};
  if (!recv_all(fd, &hdr, sizeof(hdr), timeout_ms, deadline)) {
    return false;
  }

  if (hdr.magic != IPC_MAGIC || hdr.version != IPC_VERSION || hdr.payload_len > IPC_MAX_PAYLOAD) {
    return false;
  }

  out_type = hdr.msg_type;

  if (hdr.payload_len == 0) {
    out_payload.clear();
    return true;
  }

  try {
    out_payload.resize(hdr.payload_len);
  } catch (...) {
    return false;
  }

  if (!recv_all(fd, out_payload.data(), hdr.payload_len, timeout_ms, deadline)) {
    out_payload.clear();
    return false;
  }

  return true;
}

}  // namespace virmux
