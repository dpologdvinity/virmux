#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace virmux {

class AdmissionGate;

/**
 * @brief RAII guard for holding an admission token from an AdmissionGate.
 *
 * Acquires a token on construction and automatically releases it on
 * destruction. Implements move-only semantics (Rule of Five).
 */
class ScopedToken {
 public:
  /**
   * @brief Construct an empty/unbound ScopedToken holding no token.
   */
  ScopedToken() noexcept : gate_(nullptr), acquired_(false) {}

  /**
   * @brief Acquire a token from the specified gate using its default timeout.
   * @param gate Reference to the AdmissionGate.
   */
  explicit ScopedToken(AdmissionGate& gate);

  /**
   * @brief Acquire a token from the specified gate with a custom timeout.
   * @param gate Reference to the AdmissionGate.
   * @param timeout Maximum duration to wait for token acquisition.
   */
  ScopedToken(AdmissionGate& gate, std::chrono::milliseconds timeout);

  /**
   * @brief Destructor; automatically releases the held token back to the gate.
   */
  ~ScopedToken();

  // Non-copyable (Rule of Five)
  ScopedToken(const ScopedToken&) = delete;
  ScopedToken& operator=(const ScopedToken&) = delete;

  // Movable (Rule of Five)
  ScopedToken(ScopedToken&& other) noexcept;
  ScopedToken& operator=(ScopedToken&& other) noexcept;

  /**
   * @brief Check if this guard holds an acquired token.
   * @return true if token was successfully acquired and not yet released.
   */
  [[nodiscard]] bool acquired() const noexcept { return acquired_; }

  /**
   * @brief Alias for acquired().
   * @return true if token is currently held.
   */
  [[nodiscard]] bool owns_token() const noexcept { return acquired_; }

  /**
   * @brief Boolean conversion operator equivalent to acquired().
   */
  [[nodiscard]] explicit operator bool() const noexcept { return acquired_; }

  /**
   * @brief Release the held token back to the gate early.
   */
  void release() noexcept;

  /**
   * @brief Alias for release().
   */
  void reset() noexcept { release(); }

 private:
  AdmissionGate* gate_{nullptr};
  bool acquired_{false};
};

/**
 * @brief Admission control token bucket gate for heavy compute tasks.
 *
 * Enforces a concurrency ceiling (`max_concurrent_heavy`) across tasks
 * to eliminate CPU cache thrashing and memory exhaustion.
 * Supports blocking wait-with-timeout and non-blocking try-acquire.
 */
class AdmissionGate {
 public:
  /**
   * @brief Construct an AdmissionGate with concurrency limit and default
   * timeout.
   * @param max_concurrent_heavy Maximum number of concurrent heavy tasks
   * allowed.
   * @param timeout Default wait timeout for acquiring tokens.
   */
  explicit AdmissionGate(
      int max_concurrent_heavy = 2,
      std::chrono::milliseconds timeout = std::chrono::seconds(10));

  /**
   * @brief Destructor.
   */
  ~AdmissionGate() = default;

  // Synchronization primitive: non-copyable, non-movable
  AdmissionGate(const AdmissionGate&) = delete;
  AdmissionGate& operator=(const AdmissionGate&) = delete;
  AdmissionGate(AdmissionGate&&) = delete;
  AdmissionGate& operator=(AdmissionGate&&) = delete;

  /**
   * @brief Acquire a token, blocking up to the default timeout.
   * @return true if token acquired, false on timeout.
   */
  bool acquire_token();

  /**
   * @brief Acquire a token, blocking up to the specified timeout duration.
   * @param timeout Duration to wait before timing out.
   * @return true if token acquired, false on timeout.
   */
  bool acquire_token(std::chrono::milliseconds timeout);

  /**
   * @brief Non-blocking attempt to acquire a token immediately.
   * @return true if token acquired immediately, false if limit reached.
   */
  bool try_acquire_immediate();

  /**
   * @brief Return a token to the gate and notify one waiting task.
   */
  void release_token();

  /**
   * @brief Number of tokens currently active (acquired).
   * @return Active token count.
   */
  [[nodiscard]] int active_count() const;

  /**
   * @brief Number of tasks currently blocked waiting for a token.
   * @return Queued waiter count.
   */
  [[nodiscard]] int queued_count() const;

  /**
   * @brief Concurrency limit configured for this gate.
   * @return Maximum concurrent tokens allowed.
   */
  [[nodiscard]] int max_concurrent() const noexcept;

  /**
   * @brief Default timeout configured for this gate.
   * @return Default timeout duration.
   */
  [[nodiscard]] std::chrono::milliseconds default_timeout() const noexcept;

 private:
  int max_concurrent_{2};
  std::chrono::milliseconds timeout_{std::chrono::seconds(10)};
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  int active_heavy_{0};
  int queued_tasks_{0};
};

}  // namespace virmux
