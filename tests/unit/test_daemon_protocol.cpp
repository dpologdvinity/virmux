/**
 * @file test_daemon_protocol.cpp
 * @brief Unit tests for pure daemon request payload decoding
 * (virmux/daemon_protocol.hpp).
 */

extern "C" {
#include "unity/unity.h"
}

#include <csignal>
#include <cstdint>
#include <string>
#include <string_view>

#include "virmux/daemon_protocol.hpp"
#include "virmux/ipc.hpp"

void setUp(void) {}
void tearDown(void) {}

/**
 * @brief Registration JSON as emitted by DaemonClient decodes field by field.
 */
void test_parse_registration_roundtrip_fields(void) {
  const std::string json =
      "{\n  \"name\": \"web \\\"one\\\"\",\n  \"pid\": 4242,\n"
      "  \"virtual_cores\": 3,\n  \"memory_limit_bytes\": 1073741824,\n"
      "  \"cgroup_path\": \"/sys/fs/cgroup/x\",\n  \"command\": \"sh\"\n}";
  virmux::DaemonRequest req =
      virmux::parse_daemon_request(virmux::MSG_REGISTER_SESSION, json);
  TEST_ASSERT_EQUAL_HEX16(virmux::MSG_REGISTER_SESSION, req.msg_type);
  TEST_ASSERT_EQUAL_STRING("web \"one\"", req.registration.name.c_str());
  TEST_ASSERT_EQUAL_INT(4242, req.registration.pid);
  TEST_ASSERT_EQUAL_INT(3, req.registration.virtual_cores);
  TEST_ASSERT_EQUAL_UINT64(1073741824ULL, req.registration.memory_limit_bytes);
  TEST_ASSERT_EQUAL_STRING("/sys/fs/cgroup/x",
                           req.registration.cgroup_path.c_str());
  TEST_ASSERT_EQUAL_STRING("sh", req.registration.command.c_str());
}

/**
 * @brief Malformed registration payloads fall back to defaults.
 */
void test_parse_registration_malformed(void) {
  virmux::SessionRegistration reg =
      virmux::parse_session_registration("{\"pid\": -, \"virtual_cores\": x");
  TEST_ASSERT_EQUAL_INT(0, reg.pid);
  TEST_ASSERT_EQUAL_INT(2, reg.virtual_cores);
  reg = virmux::parse_session_registration("");
  TEST_ASSERT_TRUE(reg.name.empty());
  // A dangling escape at end of input is kept literally, never over-read.
  reg = virmux::parse_session_registration("{\"name\": \"unterminated\\");
  TEST_ASSERT_EQUAL_STRING("unterminated\\", reg.name.c_str());
}

/**
 * @brief Session IDs decode from plain and JSON forms for id-addressed types.
 */
void test_parse_session_id_forms(void) {
  TEST_ASSERT_EQUAL_UINT64(17, virmux::parse_session_id("17"));
  TEST_ASSERT_EQUAL_UINT64(17, virmux::parse_session_id("  {\"id\": \"17\"}"));
  TEST_ASSERT_EQUAL_UINT64(0, virmux::parse_session_id("{\"id\"}"));
  TEST_ASSERT_EQUAL_UINT64(0, virmux::parse_session_id("-5"));
  TEST_ASSERT_EQUAL_UINT64(0, virmux::parse_session_id(""));

  const uint16_t id_types[] = {
      virmux::MSG_UNREGISTER_SESSION,  virmux::MSG_FREEZE_SESSION_REQ,
      virmux::MSG_THAW_SESSION_REQ,    virmux::MSG_CHECKPOINT_SESSION_REQ,
      virmux::MSG_RESTORE_SESSION_REQ,
  };
  for (uint16_t type : id_types) {
    TEST_ASSERT_EQUAL_UINT64(
        99, virmux::parse_daemon_request(type, "99").session_id);
  }
  TEST_ASSERT_EQUAL_UINT64(
      0, virmux::parse_daemon_request(virmux::MSG_PING, "99").session_id);
}

/**
 * @brief Kill payloads decode id and optional signal in both forms.
 */
void test_parse_kill_payload_forms(void) {
  virmux::DaemonRequest req =
      virmux::parse_daemon_request(virmux::MSG_KILL_SESSION_REQ, "12 9");
  TEST_ASSERT_EQUAL_UINT64(12, req.session_id);
  TEST_ASSERT_EQUAL_INT(9, req.signal);

  req = virmux::parse_daemon_request(virmux::MSG_KILL_SESSION_REQ, "12");
  TEST_ASSERT_EQUAL_UINT64(12, req.session_id);
  TEST_ASSERT_EQUAL_INT(SIGTERM, req.signal);

  req = virmux::parse_daemon_request(virmux::MSG_KILL_SESSION_REQ,
                                     "{\"id\": 5, \"signal\": 2}");
  TEST_ASSERT_EQUAL_UINT64(5, req.session_id);
  TEST_ASSERT_EQUAL_INT(2, req.signal);

  req = virmux::parse_daemon_request(virmux::MSG_KILL_SESSION_REQ,
                                     "{\"signal\": 99999999999}");
  TEST_ASSERT_EQUAL_UINT64(0, req.session_id);
  TEST_ASSERT_EQUAL_INT(SIGTERM, req.signal);
}

/**
 * @brief Unknown message types keep their type and default fields.
 */
void test_parse_unknown_type(void) {
  virmux::DaemonRequest req =
      virmux::parse_daemon_request(0xBEEF, "{\"id\":1}");
  TEST_ASSERT_EQUAL_HEX16(0xBEEF, req.msg_type);
  TEST_ASSERT_EQUAL_UINT64(0, req.session_id);
  TEST_ASSERT_EQUAL_INT(SIGTERM, req.signal);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_parse_registration_roundtrip_fields);
  RUN_TEST(test_parse_registration_malformed);
  RUN_TEST(test_parse_session_id_forms);
  RUN_TEST(test_parse_kill_payload_forms);
  RUN_TEST(test_parse_unknown_type);
  return UNITY_END();
}
