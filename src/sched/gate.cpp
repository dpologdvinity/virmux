/**
 * @file gate.cpp
 * @brief Implementation of admission control token bucket gate.
 */

#include "virmux/gate.hpp"

#include <utility>

namespace virmux {

// ScopedToken implementation
ScopedToken::ScopedToken(AdmissionGate& gate)
    : gate_(&gate), acquired_(gate.acquire_token()) {}

ScopedToken::ScopedToken(AdmissionGate& gate, std::chrono::milliseconds timeout)
    : gate_(&gate), acquired_(gate.acquire_token(timeout)) {}

ScopedToken::~ScopedToken() { release(); }

ScopedToken::ScopedToken(ScopedToken&& other) noexcept
    : gate_(other.gate_), acquired_(other.acquired_) {
  other.gate_ = nullptr;
  other.acquired_ = false;
}

ScopedToken& ScopedToken::operator=(ScopedToken&& other) noexcept {
  if (this != &other) {
    release();
    gate_ = other.gate_;
    acquired_ = other.acquired_;
    other.gate_ = nullptr;
    other.acquired_ = false;
  }
  return *this;
}

void ScopedToken::release() noexcept {
  if (acquired_ && gate_ != nullptr) {
    gate_->release_token();
    acquired_ = false;
  }
}

// AdmissionGate implementation
AdmissionGate::AdmissionGate(int max_concurrent_heavy,
                             std::chrono::milliseconds timeout)
    : max_concurrent_(max_concurrent_heavy), timeout_(timeout) {}

bool AdmissionGate::acquire_token() { return acquire_token(timeout_); }

bool AdmissionGate::acquire_token(std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (active_heavy_ < max_concurrent_) {
    ++active_heavy_;
    return true;
  }

  ++queued_tasks_;
  bool acquired = cv_.wait_for(
      lock, timeout, [this]() { return active_heavy_ < max_concurrent_; });
  --queued_tasks_;

  if (acquired) {
    ++active_heavy_;
    return true;
  }

  return false;
}

bool AdmissionGate::try_acquire_immediate() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (active_heavy_ < max_concurrent_) {
    ++active_heavy_;
    return true;
  }
  return false;
}

void AdmissionGate::release_token() {
  std::unique_lock<std::mutex> lock(mutex_);
  if (active_heavy_ > 0) {
    --active_heavy_;
    lock.unlock();
    cv_.notify_one();
  }
}

int AdmissionGate::active_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return active_heavy_;
}

int AdmissionGate::queued_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return queued_tasks_;
}

int AdmissionGate::max_concurrent() const noexcept { return max_concurrent_; }

std::chrono::milliseconds AdmissionGate::default_timeout() const noexcept {
  return timeout_;
}

}  // namespace virmux
