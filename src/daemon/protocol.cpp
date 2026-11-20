/**
 * @file protocol.cpp
 * @brief Pure decoding of untrusted supervisor daemon request payloads.
 *
 * These parsers are intentionally lenient (they accept the hand-rolled JSON
 * and plain-text forms produced by DaemonClient) but must never read out of
 * bounds or invoke undefined behaviour on arbitrary bytes. They are covered by
 * tests/unit/test_daemon_protocol.cpp and the fuzz_daemon_request harness.
 */

#include <cctype>
#include <charconv>
#include <csignal>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "virmux/daemon_protocol.hpp"
#include "virmux/ipc.hpp"

namespace virmux {

/**
 * @brief Parse SessionRegistration fields from JSON string.
 */
SessionRegistration parse_session_registration(std::string_view json) {
  SessionRegistration req;
  size_t idx = 0;

  auto skip_ws = [&]() {
    while (idx < json.size() && (json[idx] == ' ' || json[idx] == '\t' ||
                                 json[idx] == '\r' || json[idx] == '\n')) {
      idx++;
    }
  };

  skip_ws();
  if (idx < json.size() && json[idx] == '{') {
    idx++;
  }

  while (idx < json.size()) {
    skip_ws();
    if (idx < json.size() && json[idx] == '}') {
      break;
    }
    if (idx < json.size() && json[idx] == ',') {
      idx++;
      continue;
    }
    if (idx >= json.size() || json[idx] != '"') {
      idx++;
      continue;
    }
    idx++;  // skip '"'
    size_t key_start = idx;
    while (idx < json.size() && json[idx] != '"') {
      idx++;
    }
    std::string_view key = json.substr(key_start, idx - key_start);
    if (idx < json.size()) {
      idx++;  // skip closing '"'
    }

    skip_ws();
    if (idx < json.size() && json[idx] == ':') {
      idx++;
    }
    skip_ws();

    if (key == "name" || key == "cgroup_path" || key == "command") {
      std::string val;
      if (idx < json.size() && json[idx] == '"') {
        idx++;
        while (idx < json.size() && json[idx] != '"') {
          if (json[idx] == '\\' && idx + 1 < json.size()) {
            char next = json[idx + 1];
            if (next == '"')
              val += '"';
            else if (next == '\\')
              val += '\\';
            else if (next == 'n')
              val += '\n';
            else if (next == 'r')
              val += '\r';
            else if (next == 't')
              val += '\t';
            else
              val += next;
            idx += 2;
          } else {
            val += json[idx++];
          }
        }
        if (idx < json.size()) {
          idx++;
        }
      }
      if (key == "name")
        req.name = std::move(val);
      else if (key == "cgroup_path")
        req.cgroup_path = std::move(val);
      else if (key == "command")
        req.command = std::move(val);
    } else if (key == "pid" || key == "virtual_cores" ||
               key == "memory_limit_bytes") {
      size_t num_start = idx;
      while (idx < json.size() &&
             (std::isdigit(static_cast<unsigned char>(json[idx])) ||
              json[idx] == '-')) {
        idx++;
      }
      std::string_view num_sv = json.substr(num_start, idx - num_start);
      if (key == "pid") {
        int val = 0;
        if (std::from_chars(num_sv.data(), num_sv.data() + num_sv.size(), val)
                .ec == std::errc()) {
          req.pid = val;
        }
      } else if (key == "virtual_cores") {
        int val = 2;
        if (std::from_chars(num_sv.data(), num_sv.data() + num_sv.size(), val)
                .ec == std::errc()) {
          req.virtual_cores = val;
        }
      } else if (key == "memory_limit_bytes") {
        uint64_t val = 0;
        if (std::from_chars(num_sv.data(), num_sv.data() + num_sv.size(), val)
                .ec == std::errc()) {
          req.memory_limit_bytes = val;
        }
      }
    } else {
      if (idx < json.size() && json[idx] == '"') {
        idx++;
        while (idx < json.size() && json[idx] != '"') {
          if (json[idx] == '\\' && idx + 1 < json.size())
            idx += 2;
          else
            idx++;
        }
        if (idx < json.size()) idx++;
      } else {
        while (idx < json.size() && json[idx] != ',' && json[idx] != '}') idx++;
      }
    }
  }
  return req;
}

/**
 * @brief Parse session ID from either plain string or JSON payload.
 */
uint64_t parse_session_id(std::string_view str) {
  while (!str.empty() &&
         std::isspace(static_cast<unsigned char>(str.front()))) {
    str.remove_prefix(1);
  }
  if (str.starts_with('{')) {
    auto pos = str.find("\"id\"");
    if (pos != std::string_view::npos) {
      pos = str.find(':', pos);
      if (pos != std::string_view::npos) {
        str = str.substr(pos + 1);
        while (!str.empty() &&
               (std::isspace(static_cast<unsigned char>(str.front())) ||
                str.front() == '"')) {
          str.remove_prefix(1);
        }
      }
    }
  }
  uint64_t val = 0;
  if (std::from_chars(str.data(), str.data() + str.size(), val).ec ==
      std::errc()) {
    return val;
  }
  return 0;
}

/**
 * @brief Parse session ID and signal from either plain string or JSON payload.
 */
void parse_kill_payload(std::string_view payload, uint64_t& out_id,
                        int& out_sig) {
  out_id = 0;
  out_sig = SIGTERM;

  while (!payload.empty() &&
         std::isspace(static_cast<unsigned char>(payload.front()))) {
    payload.remove_prefix(1);
  }

  if (payload.starts_with('{')) {
    auto id_pos = payload.find("\"id\"");
    if (id_pos != std::string_view::npos) {
      id_pos = payload.find(':', id_pos);
      if (id_pos != std::string_view::npos) {
        auto sub = payload.substr(id_pos + 1);
        while (!sub.empty() &&
               (std::isspace(static_cast<unsigned char>(sub.front())) ||
                sub.front() == '"')) {
          sub.remove_prefix(1);
        }
        uint64_t val = 0;
        if (std::from_chars(sub.data(), sub.data() + sub.size(), val).ec ==
            std::errc()) {
          out_id = val;
        }
      }
    }
    auto sig_pos = payload.find("\"signal\"");
    if (sig_pos != std::string_view::npos) {
      sig_pos = payload.find(':', sig_pos);
      if (sig_pos != std::string_view::npos) {
        auto sub = payload.substr(sig_pos + 1);
        while (!sub.empty() &&
               (std::isspace(static_cast<unsigned char>(sub.front())) ||
                sub.front() == '"')) {
          sub.remove_prefix(1);
        }
        int val = SIGTERM;
        if (std::from_chars(sub.data(), sub.data() + sub.size(), val).ec ==
            std::errc()) {
          out_sig = val;
        }
      }
    }
  } else {
    auto [p1, ec1] = std::from_chars(payload.data(),
                                     payload.data() + payload.size(), out_id);
    if (ec1 == std::errc() && p1 < payload.data() + payload.size()) {
      while (p1 < payload.data() + payload.size() &&
             std::isspace(static_cast<unsigned char>(*p1))) {
        p1++;
      }
      if (p1 < payload.data() + payload.size()) {
        int sig_val = SIGTERM;
        if (std::from_chars(p1, payload.data() + payload.size(), sig_val).ec ==
            std::errc()) {
          out_sig = sig_val;
        }
      }
    }
  }
}

DaemonRequest parse_daemon_request(uint16_t msg_type,
                                   std::string_view payload) {
  DaemonRequest req;
  req.msg_type = msg_type;
  switch (msg_type) {
    case MSG_REGISTER_SESSION:
      req.registration = parse_session_registration(payload);
      break;
    case MSG_UNREGISTER_SESSION:
    case MSG_FREEZE_SESSION_REQ:
    case MSG_THAW_SESSION_REQ:
    case MSG_CHECKPOINT_SESSION_REQ:
    case MSG_RESTORE_SESSION_REQ:
      req.session_id = parse_session_id(payload);
      break;
    case MSG_KILL_SESSION_REQ:
      parse_kill_payload(payload, req.session_id, req.signal);
      break;
    default:
      // PING, LIST, STATS and unknown types carry no payload semantics.
      break;
  }
  return req;
}

}  // namespace virmux
