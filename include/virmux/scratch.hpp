/**
 * @file scratch.hpp
 * @brief In-memory scratch space manager and environment redirection.
 */

#pragma once

#include <string>
#include <vector>

namespace virmux {

class ScratchWorkspace {
 public:
  explicit ScratchWorkspace(std::string base_scratch_dir = "");
  ~ScratchWorkspace();

  ScratchWorkspace(const ScratchWorkspace&) = delete;
  ScratchWorkspace& operator=(const ScratchWorkspace&) = delete;
  ScratchWorkspace(ScratchWorkspace&& other) noexcept;
  ScratchWorkspace& operator=(ScratchWorkspace&& other) noexcept;

  bool init();
  [[nodiscard]] const std::string& path() const noexcept { return path_; }
  [[nodiscard]] bool is_valid() const noexcept { return !path_.empty(); }
  void populate_env(std::vector<std::string>& env_vars) const;
  void cleanup() noexcept;

 private:
  std::string base_dir_;
  std::string path_;
  bool created_{false};
};

}  // namespace virmux
