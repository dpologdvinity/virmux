/**
 * @file scratch.cpp
 * @brief Implementation of in-memory scratch space manager.
 */

#include "virmux/scratch.hpp"

#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <utility>

#include "virmux/cleanse.hpp"

namespace virmux {

namespace fs = std::filesystem;

namespace {

std::atomic<uint64_t> g_scratch_seq{0};

bool is_accessible_or_creatable(const std::string& path) {
  std::error_code ec;
  if (fs::exists(path, ec)) {
    return ::access(path.c_str(), W_OK | X_OK) == 0;
  }
  fs::path p(path);
  while (p.has_parent_path() && p != p.parent_path()) {
    p = p.parent_path();
    if (fs::exists(p, ec)) {
      return ::access(p.c_str(), W_OK | X_OK) == 0;
    }
  }
  return false;
}

std::string resolve_base_dir(std::string base_scratch_dir) {
  if (!base_scratch_dir.empty()) {
    if (base_scratch_dir.rfind("~/", 0) == 0) {
      const char* home = std::getenv("HOME");
      if (home != nullptr && *home != '\0') {
        return std::string(home) + base_scratch_dir.substr(1);
      }
    }
    return base_scratch_dir;
  }

  std::string run_user = "/run/user/" + std::to_string(::getuid());
  std::string run_scratch = run_user + "/virmux/scratch";

  if (is_accessible_or_creatable(run_scratch)) {
    return run_scratch;
  }

  return "/tmp/virmux/scratch";
}

}  // namespace

ScratchWorkspace::ScratchWorkspace(std::string base_scratch_dir)
    : base_dir_(resolve_base_dir(std::move(base_scratch_dir))) {}

ScratchWorkspace::~ScratchWorkspace() { cleanup(); }

ScratchWorkspace::ScratchWorkspace(ScratchWorkspace&& other) noexcept
    : base_dir_(std::move(other.base_dir_)),
      path_(std::move(other.path_)),
      created_(other.created_) {
  other.created_ = false;
  other.path_.clear();
  other.base_dir_.clear();
}

ScratchWorkspace& ScratchWorkspace::operator=(
    ScratchWorkspace&& other) noexcept {
  if (this != &other) {
    cleanup();
    base_dir_ = std::move(other.base_dir_);
    path_ = std::move(other.path_);
    created_ = other.created_;
    other.created_ = false;
    other.path_.clear();
    other.base_dir_.clear();
  }
  return *this;
}

bool ScratchWorkspace::init() {
  if (created_ && !path_.empty()) {
    return true;
  }

  uint64_t seq = g_scratch_seq.fetch_add(1, std::memory_order_relaxed);
  pid_t pid = ::getpid();
  std::string folder_name =
      "scratch_" + std::to_string(pid) + "_" + std::to_string(seq);

  fs::path scratch_path = fs::path(base_dir_) / folder_name;
  std::error_code ec;
  if (!fs::create_directories(scratch_path, ec) &&
      !fs::exists(scratch_path, ec)) {
    return false;
  }

  fs::permissions(scratch_path, fs::perms::owner_all, fs::perm_options::replace,
                  ec);
  if (ec) {
    fs::remove_all(scratch_path, ec);
    return false;
  }

  path_ = scratch_path.string();
  created_ = true;
  return true;
}

void ScratchWorkspace::populate_env(std::vector<std::string>& env_vars) const {
  if (path_.empty()) {
    return;
  }

  const std::pair<std::string_view, std::string> vars[] = {
      {"TMPDIR=", "TMPDIR=" + path_},
      {"TEMP=", "TEMP=" + path_},
      {"TMP=", "TMP=" + path_},
  };

  for (const auto& [prefix, full] : vars) {
    bool found = false;
    for (auto& var : env_vars) {
      if (var.starts_with(prefix)) {
        var = full;
        found = true;
      }
    }
    if (!found) {
      env_vars.push_back(full);
    }
  }
}

void ScratchWorkspace::cleanup() noexcept {
  if (created_ && !path_.empty()) {
    try {
      cleanse_path(path_);
    } catch (...) {
    }

    std::error_code ec;
    fs::remove_all(path_, ec);

    created_ = false;
    path_.clear();
  }
}

}  // namespace virmux
