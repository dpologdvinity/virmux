#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace virmux {

struct CasStats {
  size_t object_count{0};
  uint64_t total_bytes{0};
  uint64_t hits{0};
  uint64_t misses{0};
};

class CasStore {
 public:
  explicit CasStore(std::string cache_root = "");

  [[nodiscard]] const std::string& cache_root() const noexcept {
    return cache_root_;
  }

  [[nodiscard]] bool contains(const std::string& key) const;
  bool put(const std::string& key, const std::string& source_file);
  bool put_bytes(const std::string& key, std::span<const uint8_t> data);
  bool get(const std::string& key, const std::string& dest_path);
  [[nodiscard]] std::string get_path(const std::string& key) const;
  size_t prune(uint64_t max_bytes);
  [[nodiscard]] CasStats get_stats() const;
  void reset_stats() noexcept;

 private:
  std::string cache_root_;
  mutable uint64_t hits_{0};
  mutable uint64_t misses_{0};
};

}  // namespace virmux
