/**
 * @file test_version.cpp
 * @brief Unit tests for virmux version reporting using the Unity test harness.
 */

extern "C" {
#include "unity/unity.h"
}

#include "virmux/version.hpp"
#include <string_view>

void setUp(void) {}
void tearDown(void) {}

void test_version_returns_valid_semver(void) {
    std::string_view ver = virmux::version_string();
    TEST_ASSERT_EQUAL_STRING("0.1.0", ver.data());
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_version_returns_valid_semver);
    return UNITY_END();
}
