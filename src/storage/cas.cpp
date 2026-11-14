/**
 * @file cas.cpp
 * @brief Implementation of Content-Addressable Storage (CAS) build cache.
 */

#include "virmux/cas.hpp"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

namespace virmux {

namespace fs = std::filesystem;

namespace {

std::atomic<uint64_t> g_cas_seq{0};

std::string resolve_cache_root(std::string path) {
  if (path.empty()) {
    const char* env_cas = std::getenv("VIRMUX_CAS_DIR");
    if (env_cas != nullptr && *env_cas != '\0') {
      return std::string(env_cas);
    }
    const char* home = std::getenv("HOME");
    if (home != nullptr && *home != '\0') {
      return std::string(home) + "/.cache/virmux/cas";
    }
    return "/tmp/virmux-cas";
  }

  if (path.rfind("~/", 0) == 0) {
    const char* home = std::getenv("HOME");
    if (home != nullptr && *home != '\0') {
      return std::string(home) + path.substr(1);
    }
  }

  return path;
}

}  // namespace

CasStore::CasStore(std::string cache_root)
    : cache_root_(resolve_cache_root(std::move(cache_root))) {
  std::error_code ec;
  fs::create_directories(cache_root_, ec);
  fs::permissions(cache_root_, fs::perms::owner_all, fs::perm_options::replace,
                  ec);

  fs::path objects_dir = fs::path(cache_root_) / "objects";
  fs::create_directories(objects_dir, ec);
  fs::permissions(objects_dir, fs::perms::owner_all, fs::perm_options::replace,
                  ec);

  fs::path tmp_dir = fs::path(cache_root_) / "tmp";
  fs::create_directories(tmp_dir, ec);
  fs::permissions(tmp_dir, fs::perms::owner_all, fs::perm_options::replace, ec);
}

std::string CasStore::get_path(const std::string& key) const {
  if (key.empty() || key.find('/') != std::string::npos ||
      key.find('\\') != std::string::npos ||
      key.find("..") != std::string::npos) {
    return {};
  }
  if (key.size() >= 2) {
    return (fs::path(cache_root_) / "objects" / key.substr(0, 2) / key)
        .string();
  }
  return (fs::path(cache_root_) / "objects" / key).string();
}

bool CasStore::contains(const std::string& key) const {
  std::string p = get_path(key);
  if (p.empty()) {
    return false;
  }
  std::error_code ec;
  return fs::is_regular_file(p, ec);
}

bool CasStore::put(const std::string& key, const std::string& source_file) {
  std::string target_path = get_path(key);
  if (target_path.empty()) {
    return false;
  }

  std::error_code ec;
  if (!fs::is_regular_file(source_file, ec)) {
    return false;
  }

  pid_t pid = ::getpid();
  uint64_t seq = g_cas_seq.fetch_add(1, std::memory_order_relaxed);
  std::string temp_file_name =
      key + "_" + std::to_string(pid) + "_" + std::to_string(seq) + ".tmp";
  fs::path temp_path = fs::path(cache_root_) / "tmp" / temp_file_name;

  fs::copy_file(source_file, temp_path, fs::copy_options::overwrite_existing,
                ec);
  if (ec) {
    fs::remove(temp_path, ec);
    return false;
  }

  fs::path parent_dir = fs::path(target_path).parent_path();
  fs::create_directories(parent_dir, ec);
  if (ec) {
    fs::remove(temp_path, ec);
    return false;
  }

  fs::rename(temp_path, target_path, ec);
  if (ec) {
    fs::remove(temp_path, ec);
    return false;
  }

  return true;
}

bool CasStore::put_bytes(const std::string& key,
                         std::span<const uint8_t> data) {
  std::string target_path = get_path(key);
  if (target_path.empty()) {
    return false;
  }

  pid_t pid = ::getpid();
  uint64_t seq = g_cas_seq.fetch_add(1, std::memory_order_relaxed);
  std::string temp_file_name =
      key + "_" + std::to_string(pid) + "_" + std::to_string(seq) + ".tmp";
  fs::path temp_path = fs::path(cache_root_) / "tmp" / temp_file_name;

  std::error_code ec;
  {
    std::ofstream ofs(temp_path, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
      return false;
    }
    if (!data.empty()) {
      ofs.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size()));
    }
    if (!ofs.good()) {
      ofs.close();
      fs::remove(temp_path, ec);
      return false;
    }
  }

  fs::path parent_dir = fs::path(target_path).parent_path();
  fs::create_directories(parent_dir, ec);
  if (ec) {
    fs::remove(temp_path, ec);
    return false;
  }

  fs::rename(temp_path, target_path, ec);
  if (ec) {
    fs::remove(temp_path, ec);
    return false;
  }

  return true;
}

bool CasStore::get(const std::string& key, const std::string& dest_path) {
  if (key.empty()) {
    misses_++;
    return false;
  }

  std::string src = get_path(key);
  std::error_code ec;
  if (!fs::is_regular_file(src, ec)) {
    misses_++;
    return false;
  }

  hits_++;

  fs::path dest(dest_path);
  if (dest.has_parent_path()) {
    fs::create_directories(dest.parent_path(), ec);
    if (ec) {
      return false;
    }
  }

  fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec);
  return !ec;
}

size_t CasStore::prune(uint64_t max_bytes) {
  struct CacheEntry {
    fs::path path;
    uint64_t size{0};
    fs::file_time_type mtime{};
  };

  std::vector<CacheEntry> entries;
  uint64_t total = 0;
  std::error_code ec;

  fs::path objects_dir = fs::path(cache_root_) / "objects";
  if (fs::exists(objects_dir, ec)) {
    for (const auto& item : fs::recursive_directory_iterator(
             objects_dir, fs::directory_options::skip_permission_denied, ec)) {
      if (item.is_regular_file(ec)) {
        uint64_t sz = item.file_size(ec);
        if (!ec) {
          auto mt = item.last_write_time(ec);
          if (!ec) {
            entries.push_back({item.path(), sz, mt});
            total += sz;
          }
        }
      }
    }
  }

  if (total <= max_bytes) {
    return 0;
  }

  std::sort(entries.begin(), entries.end(),
            [](const CacheEntry& a, const CacheEntry& b) {
              return a.mtime < b.mtime;
            });

  size_t pruned_count = 0;
  for (const auto& entry : entries) {
    if (total <= max_bytes) {
      break;
    }
    if (fs::remove(entry.path, ec)) {
      total = (total >= entry.size) ? (total - entry.size) : 0;
      pruned_count++;
    }
  }

  return pruned_count;
}

CasStats CasStore::get_stats() const {
  CasStats stats{};
  stats.hits = hits_;
  stats.misses = misses_;

  std::error_code ec;
  fs::path objects_dir = fs::path(cache_root_) / "objects";
  if (fs::exists(objects_dir, ec)) {
    for (const auto& item : fs::recursive_directory_iterator(
             objects_dir, fs::directory_options::skip_permission_denied, ec)) {
      if (item.is_regular_file(ec)) {
        uint64_t sz = item.file_size(ec);
        if (!ec) {
          stats.object_count++;
          stats.total_bytes += sz;
        }
      }
    }
  }

  return stats;
}

void CasStore::reset_stats() noexcept {
  hits_ = 0;
  misses_ = 0;
}

}  // namespace virmux
