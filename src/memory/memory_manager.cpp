/**
 * @file memory_manager.cpp
 * @brief Implementation of unified MemoryManager and Linux VMA scanner.
 */

#include "virmux/memory_manager.hpp"

#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace virmux {

namespace {

void trim_trailing_whitespace(std::string& s) {
  while (!s.empty() &&
         (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' ||
          s.back() == '\n')) {
    s.pop_back();
  }
}

}  // namespace

MemoryManager::MemoryManager(std::string procfs_root,
                             std::string ksm_root,
                             std::string sysfs_block_root)
    : procfs_root_(std::move(procfs_root)),
      ksm_(std::move(ksm_root)),
      compactor_(),
      telemetry_(procfs_root_, std::move(sysfs_block_root)) {}

std::vector<VmaRange> MemoryManager::scan_process_vmas(
    pid_t pid, bool anonymous_only) const {
  if (pid <= 0) {
    return {};
  }

  std::filesystem::path maps_path =
      std::filesystem::path(procfs_root_) / std::to_string(pid) / "maps";
  std::ifstream maps_file(maps_path);
  if (!maps_file.is_open()) {
    return {};
  }

  std::vector<VmaRange> vmas;
  std::string line;

  while (std::getline(maps_file, line)) {
    if (line.empty()) {
      continue;
    }

    std::istringstream iss(line);
    std::string addr_range;
    std::string perms;
    std::string offset;
    std::string dev;
    std::string inode;

    if (!(iss >> addr_range >> perms >> offset >> dev >> inode)) {
      continue;
    }

    std::string pathname;
    if (iss >> pathname) {
      std::string rest;
      if (std::getline(iss, rest)) {
        pathname += rest;
      }
      trim_trailing_whitespace(pathname);
    }

    auto dash = addr_range.find('-');
    if (dash == std::string::npos || dash == 0 ||
        dash == addr_range.size() - 1) {
      continue;
    }

    std::string_view start_str(addr_range.data(), dash);
    std::string_view end_str(addr_range.data() + dash + 1,
                             addr_range.size() - dash - 1);

    uintptr_t start = 0;
    auto [p1, ec1] = std::from_chars(
        start_str.data(), start_str.data() + start_str.size(), start, 16);
    if (ec1 != std::errc{} || p1 != start_str.data() + start_str.size()) {
      continue;
    }

    uintptr_t end = 0;
    auto [p2, ec2] = std::from_chars(
        end_str.data(), end_str.data() + end_str.size(), end, 16);
    if (ec2 != std::errc{} || p2 != end_str.data() + end_str.size()) {
      continue;
    }

    if (start >= end) {
      continue;
    }

    // Page alignment check: ensures start % 4096 == 0 and end % 4096 == 0
    if ((start % 4096 != 0) || (end % 4096 != 0)) {
      continue;
    }

    bool is_writable = (perms.size() >= 2 && perms[1] == 'w');

    // Anonymous classification
    bool is_anon = false;
    uint64_t inode_num = std::strtoull(inode.c_str(), nullptr, 10);
    bool is_special_kernel =
        (pathname == "[vdso]" || pathname == "[vvar]" ||
         pathname == "[vvar_vclock]" || pathname == "[vsyscall]");

    if (!is_special_kernel) {
      if ((dev == "00:00" || dev == "0:0") && inode_num == 0) {
        is_anon = true;
      } else if (pathname.empty() || pathname == "[heap]" ||
                 pathname == "[stack]" || pathname.starts_with("[anon")) {
        is_anon = true;
      }
    }

    if (anonymous_only && !is_anon) {
      continue;
    }

    VmaRange range;
    range.start = start;
    range.end = end;
    range.is_anonymous = is_anon;
    range.is_writable = is_writable;
    vmas.push_back(range);
  }

  return vmas;
}

ReclaimResult MemoryManager::compact_process(pid_t pid, ReclaimAdvice advice) {
  if (pid <= 0) {
    return ReclaimResult{false, 0, 0, ESRCH};
  }

  std::vector<VmaRange> vmas = scan_process_vmas(pid, true);
  std::vector<std::pair<uintptr_t, size_t>> ranges;
  ranges.reserve(vmas.size());

  for (const auto& vma : vmas) {
    if (vma.end > vma.start) {
      ranges.emplace_back(vma.start, vma.end - vma.start);
    }
  }

  return compactor_.reclaim_ranges(pid, ranges, advice);
}

size_t MemoryManager::compact_cgroup_procs(
    const std::string& cgroup_procs_path,
    ReclaimAdvice advice) {
  std::ifstream f(cgroup_procs_path);
  if (!f.is_open()) {
    return 0;
  }

  size_t compacted_count = 0;
  std::string line;

  while (std::getline(f, line)) {
    if (line.empty()) {
      continue;
    }

    std::istringstream iss(line);
    pid_t pid = 0;
    if (!(iss >> pid) || pid <= 0) {
      continue;
    }

    auto result = compact_process(pid, advice);
    if (result.success) {
      ++compacted_count;
    }
  }

  return compacted_count;
}

KsmStats MemoryManager::get_ksm_stats() const {
  return ksm_.read_stats();
}

HostMemoryStats MemoryManager::get_host_stats() const {
  return telemetry_.read_host_stats();
}

}  // namespace virmux
