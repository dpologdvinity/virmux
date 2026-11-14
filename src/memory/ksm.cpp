/**
 * @file ksm.cpp
 * @brief Implementation of Linux Kernel Samepage Merging (KSM) controller.
 */

#include "virmux/ksm.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <filesystem>
#include <string_view>
#include <utility>

#include "virmux/raii.hpp"

namespace virmux {

namespace {

/**
 * @brief Safely read an unsigned integer attribute from a sysfs or mock file.
 *
 * Trims leading and trailing whitespace. Defensively returns default_value
 * on missing file, unreadable file, or invalid non-numeric content.
 */
template <typename T>
T read_sysfs_integer(const std::filesystem::path& path, T default_value = 0) {
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return default_value;
  }
  ScopedFd scoped(fd);

  char buf[64];
  ssize_t bytes_read = ::read(scoped.get(), buf, sizeof(buf) - 1);
  if (bytes_read <= 0) {
    return default_value;
  }
  buf[bytes_read] = '\0';

  std::string_view sv(buf, static_cast<size_t>(bytes_read));
  while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' ||
                         sv.front() == '\r' || sv.front() == '\n')) {
    sv.remove_prefix(1);
  }
  while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' ||
                         sv.back() == '\r' || sv.back() == '\n')) {
    sv.remove_suffix(1);
  }

  if (sv.empty()) {
    return default_value;
  }

  T value = default_value;
  auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), value);
  if (ec == std::errc() && ptr == sv.data() + sv.size()) {
    return value;
  }
  return default_value;
}

/**
 * @brief Safely write a string to a sysfs or mock file.
 *
 * Handles interrupted syscalls (EINTR) and returns false on permission
 * denial (EACCES/EROFS) or other write errors.
 */
bool write_sysfs_string(const std::filesystem::path& path,
                        std::string_view value) {
  int fd = ::open(path.c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC);
  if (fd < 0 && (errno == EINVAL || errno == ENOTSUP)) {
    fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
  }
  if (fd < 0) {
    return false;
  }
  ScopedFd scoped(fd);

  const char* data = value.data();
  size_t remaining = value.size();
  while (remaining > 0) {
    ssize_t written = ::write(scoped.get(), data, remaining);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    data += written;
    remaining -= static_cast<size_t>(written);
  }
  return true;
}

}  // namespace

KsmController::KsmController(std::string sysfs_root)
    : sysfs_root_(std::move(sysfs_root)) {}

bool KsmController::is_available() const {
  std::error_code ec;
  std::filesystem::path root(sysfs_root_);
  if (!std::filesystem::is_directory(root, ec) || ec) {
    return false;
  }
  std::filesystem::path run_path = root / "run";
  return ::access(run_path.c_str(), R_OK) == 0;
}

bool KsmController::is_enabled() const { return read_stats().enabled; }

KsmStats KsmController::read_stats() const {
  KsmStats stats;
  if (!is_available()) {
    return stats;
  }

  stats.present = true;
  std::filesystem::path root(sysfs_root_);

  uint32_t run_val = read_sysfs_integer<uint32_t>(root / "run", 0);
  stats.enabled = (run_val == 1);

  stats.pages_shared = read_sysfs_integer<uint64_t>(root / "pages_shared", 0);
  stats.pages_sharing = read_sysfs_integer<uint64_t>(root / "pages_sharing", 0);
  stats.pages_unshared =
      read_sysfs_integer<uint64_t>(root / "pages_unshared", 0);
  stats.pages_volatile =
      read_sysfs_integer<uint64_t>(root / "pages_volatile", 0);
  stats.pages_to_scan = read_sysfs_integer<uint32_t>(root / "pages_to_scan", 0);
  stats.sleep_millisecs =
      read_sysfs_integer<uint32_t>(root / "sleep_millisecs", 0);

  long page_size = ::sysconf(_SC_PAGESIZE);
  if (page_size <= 0) {
    page_size = 4096;
  }
  stats.bytes_saved = stats.pages_sharing * static_cast<uint64_t>(page_size);

  return stats;
}

bool KsmController::enable(bool activate) {
  std::filesystem::path root(sysfs_root_);
  std::string_view val = activate ? "1\n" : "0\n";
  return write_sysfs_string(root / "run", val);
}

bool KsmController::tune(uint32_t pages_to_scan, uint32_t sleep_millisecs) {
  std::filesystem::path root(sysfs_root_);
  std::string scan_str = std::to_string(pages_to_scan) + "\n";
  std::string sleep_str = std::to_string(sleep_millisecs) + "\n";

  if (!write_sysfs_string(root / "pages_to_scan", scan_str)) {
    return false;
  }
  if (!write_sysfs_string(root / "sleep_millisecs", sleep_str)) {
    return false;
  }
  return true;
}

uint32_t KsmController::compute_adaptive_pages(size_t active_sessions) const {
  constexpr uint32_t kMinPages = 100u;
  constexpr uint32_t kMaxPages = 2000u;
  constexpr uint32_t kPagesPerSession = 50u;

  // Prevent integer overflow on huge session counts
  if (active_sessions > 40) {
    return kMaxPages;
  }

  uint32_t scaled =
      kMinPages + static_cast<uint32_t>(active_sessions) * kPagesPerSession;
  return std::clamp(scaled, kMinPages, kMaxPages);
}

}  // namespace virmux
