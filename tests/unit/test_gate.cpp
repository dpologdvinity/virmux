/**
 * @file test_gate.cpp
 * @brief Unit tests for AdmissionGate and ScopedToken.
 */

extern "C" {
#include "unity/unity.h"
}

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "virmux/gate.hpp"

void setUp(void) {}
void tearDown(void) {}

void test_gate_allows_max_concurrent_tokens(void) {
  virmux::AdmissionGate gate(2, std::chrono::milliseconds(50));
  TEST_ASSERT_EQUAL_INT(2, gate.max_concurrent());
  TEST_ASSERT_EQUAL_INT(0, gate.active_count());
  TEST_ASSERT_EQUAL_INT(0, gate.queued_count());

  TEST_ASSERT_TRUE(gate.acquire_token());
  TEST_ASSERT_TRUE(gate.acquire_token());
  TEST_ASSERT_EQUAL_INT(2, gate.active_count());

  // Third acquisition must timeout or queue
  TEST_ASSERT_FALSE(gate.try_acquire_immediate());
  gate.release_token();
  TEST_ASSERT_EQUAL_INT(1, gate.active_count());
  TEST_ASSERT_TRUE(gate.acquire_token());
}

void test_gate_acquire_blocks_and_times_out(void) {
  virmux::AdmissionGate gate(1, std::chrono::milliseconds(40));
  TEST_ASSERT_TRUE(gate.acquire_token());
  TEST_ASSERT_EQUAL_INT(1, gate.active_count());

  auto start = std::chrono::steady_clock::now();
  bool acquired = gate.acquire_token(std::chrono::milliseconds(50));
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);

  TEST_ASSERT_FALSE(acquired);
  TEST_ASSERT_TRUE(elapsed.count() >= 40);
  TEST_ASSERT_EQUAL_INT(1, gate.active_count());
  TEST_ASSERT_EQUAL_INT(0, gate.queued_count());
}

void test_gate_queued_waiters_woken_on_release(void) {
  virmux::AdmissionGate gate(1, std::chrono::milliseconds(500));
  TEST_ASSERT_TRUE(gate.acquire_token());

  std::atomic<bool> thread_started{false};
  std::atomic<bool> acquired_in_thread{false};

  std::thread waiter([&]() {
    thread_started.store(true);
    bool res = gate.acquire_token();
    acquired_in_thread.store(res);
    if (res) {
      gate.release_token();
    }
  });

  while (!thread_started.load()) {
    std::this_thread::yield();
  }
  // Bounded wait loop polling for waiter to block in queued state
  auto poll_start = std::chrono::steady_clock::now();
  while (gate.queued_count() == 0 &&
         (std::chrono::steady_clock::now() - poll_start) <
             std::chrono::milliseconds(500)) {
    std::this_thread::yield();
  }
  TEST_ASSERT_EQUAL_INT(1, gate.queued_count());

  gate.release_token();
  waiter.join();

  TEST_ASSERT_TRUE(acquired_in_thread.load());
  TEST_ASSERT_EQUAL_INT(0, gate.active_count());
  TEST_ASSERT_EQUAL_INT(0, gate.queued_count());
}

void test_gate_try_acquire_immediate(void) {
  virmux::AdmissionGate gate(1, std::chrono::milliseconds(100));
  TEST_ASSERT_TRUE(gate.try_acquire_immediate());
  TEST_ASSERT_EQUAL_INT(1, gate.active_count());
  TEST_ASSERT_FALSE(gate.try_acquire_immediate());

  gate.release_token();
  TEST_ASSERT_EQUAL_INT(0, gate.active_count());
  TEST_ASSERT_TRUE(gate.try_acquire_immediate());
}

void test_gate_release_when_zero_active(void) {
  virmux::AdmissionGate gate(2, std::chrono::milliseconds(50));
  TEST_ASSERT_EQUAL_INT(0, gate.active_count());
  // Calling release when none active should be a safe no-op
  gate.release_token();
  TEST_ASSERT_EQUAL_INT(0, gate.active_count());
}

void test_scoped_token_raii(void) {
  virmux::AdmissionGate gate(1, std::chrono::milliseconds(50));
  {
    virmux::ScopedToken token(gate);
    TEST_ASSERT_TRUE(token.acquired());
    TEST_ASSERT_TRUE(token.owns_token());
    TEST_ASSERT_TRUE(static_cast<bool>(token));
    TEST_ASSERT_EQUAL_INT(1, gate.active_count());
  }
  // Token must be released on scope exit
  TEST_ASSERT_EQUAL_INT(0, gate.active_count());
}

void test_scoped_token_move(void) {
  virmux::AdmissionGate gate(1, std::chrono::milliseconds(50));
  {
    virmux::ScopedToken token1(gate);
    TEST_ASSERT_TRUE(token1.acquired());
    TEST_ASSERT_EQUAL_INT(1, gate.active_count());

    // Move construction
    virmux::ScopedToken token2(std::move(token1));
    // Asserting the documented moved-from state is the point here.
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    TEST_ASSERT_FALSE(token1.acquired());
    TEST_ASSERT_TRUE(token2.acquired());
    TEST_ASSERT_EQUAL_INT(1, gate.active_count());

    // Move assignment
    virmux::ScopedToken token3;
    token3 = std::move(token2);
    // Asserting the documented moved-from state is the point here.
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    TEST_ASSERT_FALSE(token2.acquired());
    TEST_ASSERT_TRUE(token3.acquired());
    TEST_ASSERT_EQUAL_INT(1, gate.active_count());
  }
  // All destroyed, gate must be empty
  TEST_ASSERT_EQUAL_INT(0, gate.active_count());
}

void test_scoped_token_timeout(void) {
  virmux::AdmissionGate gate(1, std::chrono::milliseconds(20));
  TEST_ASSERT_TRUE(gate.acquire_token());
  TEST_ASSERT_EQUAL_INT(1, gate.active_count());

  {
    virmux::ScopedToken token(gate, std::chrono::milliseconds(20));
    TEST_ASSERT_FALSE(token.acquired());
    TEST_ASSERT_FALSE(static_cast<bool>(token));
  }
  // Destructor of failed token must not release a token
  TEST_ASSERT_EQUAL_INT(1, gate.active_count());
  gate.release_token();
  TEST_ASSERT_EQUAL_INT(0, gate.active_count());
}

void test_admission_gate_concurrency_stress(void) {
  constexpr int kMaxConcurrent = 3;
  constexpr int kNumThreads = 8;
  constexpr int kIterations = 20;

  virmux::AdmissionGate gate(kMaxConcurrent, std::chrono::seconds(2));
  std::atomic<int> max_observed_active{0};
  std::atomic<bool> all_passed{true};

  std::vector<std::thread> workers;
  workers.reserve(kNumThreads);

  for (int i = 0; i < kNumThreads; ++i) {
    workers.emplace_back([&]() {
      for (int iter = 0; iter < kIterations; ++iter) {
        virmux::ScopedToken token(gate);
        if (!token.acquired()) {
          all_passed.store(false);
          return;
        }
        int cur = gate.active_count();
        int prev = max_observed_active.load();
        while (cur > prev &&
               !max_observed_active.compare_exchange_weak(prev, cur)) {
        }
        if (cur > kMaxConcurrent) {
          all_passed.store(false);
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
      }
    });
  }

  for (auto& t : workers) {
    t.join();
  }

  TEST_ASSERT_TRUE(all_passed.load());
  TEST_ASSERT_TRUE(max_observed_active.load() <= kMaxConcurrent);
  TEST_ASSERT_EQUAL_INT(0, gate.active_count());
  TEST_ASSERT_EQUAL_INT(0, gate.queued_count());
}

void test_gate_clamps_non_positive_limit(void) {
  virmux::AdmissionGate gate_zero(0);
  TEST_ASSERT_EQUAL_INT(1, gate_zero.max_concurrent());
  virmux::AdmissionGate gate_neg(-4);
  TEST_ASSERT_EQUAL_INT(1, gate_neg.max_concurrent());
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_gate_allows_max_concurrent_tokens);
  RUN_TEST(test_gate_clamps_non_positive_limit);
  RUN_TEST(test_gate_acquire_blocks_and_times_out);
  RUN_TEST(test_gate_queued_waiters_woken_on_release);
  RUN_TEST(test_gate_try_acquire_immediate);
  RUN_TEST(test_gate_release_when_zero_active);
  RUN_TEST(test_scoped_token_raii);
  RUN_TEST(test_scoped_token_move);
  RUN_TEST(test_scoped_token_timeout);
  RUN_TEST(test_admission_gate_concurrency_stress);
  return UNITY_END();
}
