/**
 * @file compactor.cpp
 * @brief Implementation of ProcessCompactor using pidfd_open and process_madvise.
 */

#include "virmux/compactor.hpp"
#include "virmux/raii.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>
#include <vector>

#ifndef __NR_pidfd_open
#define __NR_pidfd_open 434
#endif

#ifndef __NR_process_madvise
#define __NR_process_madvise 440
#endif

#ifndef MADV_PAGEOUT
#define MADV_PAGEOUT 21
#endif

#ifndef MADV_COLD
#define MADV_COLD 20
#endif

#ifndef MADV_COLLAPSE
#define MADV_COLLAPSE 25
#endif

#ifndef IOV_MAX
#define IOV_MAX 1024
#endif

namespace virmux {

namespace {

int to_madvise_flag(ReclaimAdvice advice) noexcept {
  switch (advice) {
    case ReclaimAdvice::Pageout:
      return MADV_PAGEOUT;
    case ReclaimAdvice::Cold:
      return MADV_COLD;
    case ReclaimAdvice::Collapse:
      return MADV_COLLAPSE;
  }
  return MADV_PAGEOUT;
}

}  // namespace

bool ProcessCompactor::is_supported() noexcept {
  long pidfd = ::syscall(__NR_pidfd_open, ::getpid(), 0);
  if (pidfd < 0 && errno == ENOSYS) {
    return false;
  }
  if (pidfd >= 0) {
    ::close(static_cast<int>(pidfd));
  }

  long madv_ret = ::syscall(__NR_process_madvise, -1, nullptr, 0, 0, 0);
  if (madv_ret < 0 && errno == ENOSYS) {
    return false;
  }

  return true;
}

ReclaimResult ProcessCompactor::reclaim_ranges(
    pid_t target_pid,
    const std::vector<std::pair<uintptr_t, size_t>>& ranges,
    ReclaimAdvice advice) {
  if (target_pid <= 0) {
    return ReclaimResult{false, 0, 0, ESRCH};
  }

  int raw_fd = static_cast<int>(::syscall(__NR_pidfd_open, target_pid, 0));
  if (raw_fd < 0) {
    int err = errno;
    if (err == EINVAL && target_pid <= 0) {
      err = ESRCH;
    }
    return ReclaimResult{false, 0, 0, err};
  }
  ScopedFd pidfd(raw_fd);

  long page_size_raw = ::sysconf(_SC_PAGESIZE);
  size_t page_size = (page_size_raw > 0) ? static_cast<size_t>(page_size_raw) : 4096;
  uintptr_t page_mask = static_cast<uintptr_t>(page_size) - 1;

  std::vector<struct iovec> aligned_iovs;
  aligned_iovs.reserve(ranges.size());

  for (const auto& [addr, len] : ranges) {
    if (len == 0) {
      continue;
    }

    uintptr_t aligned_addr = addr & ~page_mask;
    size_t offset = static_cast<size_t>(addr - aligned_addr);
    size_t aligned_len = ((offset + len + page_size - 1) / page_size) * page_size;

    struct iovec iov;
    iov.iov_base = reinterpret_cast<void*>(aligned_addr);
    iov.iov_len = aligned_len;
    aligned_iovs.push_back(iov);
  }

  if (aligned_iovs.empty()) {
    return ReclaimResult{true, 0, 0, 0};
  }

  int advice_flag = to_madvise_flag(advice);
  size_t total_bytes = 0;
  size_t total_ranges = 0;
  constexpr size_t kBatchLimit = IOV_MAX;

  for (size_t i = 0; i < aligned_iovs.size(); i += kBatchLimit) {
    size_t batch_size = std::min(kBatchLimit, aligned_iovs.size() - i);
    const struct iovec* batch_ptr = &aligned_iovs[i];

    long ret = ::syscall(__NR_process_madvise, pidfd.get(), batch_ptr,
                         batch_size, advice_flag, 0);

    // Unprivileged fallback: if MADV_PAGEOUT fails with EPERM or EACCES, retry with MADV_COLD
    if (ret < 0 && advice == ReclaimAdvice::Pageout &&
        (errno == EPERM || errno == EACCES)) {
      ret = ::syscall(__NR_process_madvise, pidfd.get(), batch_ptr,
                      batch_size, MADV_COLD, 0);
    }

    if (ret < 0) {
      return ReclaimResult{false, total_bytes, total_ranges, errno};
    }

    total_bytes += static_cast<size_t>(ret);
    total_ranges += batch_size;
  }

  return ReclaimResult{true, total_bytes, total_ranges, 0};
}

ReclaimResult ProcessCompactor::reclaim_memory(pid_t target_pid,
                                               ReclaimAdvice advice) {
  if (target_pid <= 0) {
    return ReclaimResult{false, 0, 0, ESRCH};
  }

  std::string maps_path = "/proc/" + std::to_string(target_pid) + "/maps";
  std::ifstream maps_file(maps_path);
  if (!maps_file.is_open()) {
    int raw_fd = static_cast<int>(::syscall(__NR_pidfd_open, target_pid, 0));
    if (raw_fd < 0) {
      int err = errno;
      if (err == EINVAL && target_pid <= 0) {
        err = ESRCH;
      }
      return ReclaimResult{false, 0, 0, err};
    }
    ::close(raw_fd);
    return ReclaimResult{false, 0, 0, EACCES};
  }

  std::vector<std::pair<uintptr_t, size_t>> ranges;
  std::string line;

  while (std::getline(maps_file, line)) {
    if (line.empty()) {
      continue;
    }

    std::istringstream iss(line);
    std::string addr_range, perms, offset, dev, inode;
    if (!(iss >> addr_range >> perms >> offset >> dev >> inode)) {
      continue;
    }

    std::string path;
    iss >> path;  // Optional pathname

    // Anonymous or writable ranges: lines containing rw-p or r--p where path is empty or [heap] or [anon]
    bool perms_match = (perms == "rw-p" || perms == "r--p");
    bool path_match = path.empty() || path == "[heap]" ||
                      path.find("[anon") != std::string::npos;

    if (!perms_match || !path_match) {
      continue;
    }

    auto dash = addr_range.find('-');
    if (dash == std::string::npos) {
      continue;
    }

    uintptr_t start =
        std::strtoull(addr_range.substr(0, dash).c_str(), nullptr, 16);
    uintptr_t end =
        std::strtoull(addr_range.substr(dash + 1).c_str(), nullptr, 16);

    if (end > start) {
      ranges.emplace_back(start, end - start);
    }
  }

  return reclaim_ranges(target_pid, ranges, advice);
}

}  // namespace virmux
