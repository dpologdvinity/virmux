/**
 * @file interceptor.cpp
 * @brief Dynamic LD_PRELOAD interceptor implementation for hardware discovery
 * calls.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#undef _FILE_OFFSET_BITS

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "virmux/browser_trim.hpp"
#include "virmux/intercept.h"

namespace {

using sysconf_fn_t = long (*)(int) noexcept;
using get_nprocs_fn_t = int (*)(void) noexcept;
using open_fn_t = int (*)(const char*, int, ...);
using openat_fn_t = int (*)(int, const char*, int, ...);
using execve_fn_t = int (*)(const char*, char* const[], char* const[]) noexcept;
using mmap_fn_t = void* (*)(void*, size_t, int, int, int, off_t) noexcept;
using mmap64_fn_t = void* (*)(void*, size_t, int, int, int, off64_t) noexcept;

static sysconf_fn_t real_sysconf = nullptr;
static get_nprocs_fn_t real_get_nprocs = nullptr;
static get_nprocs_fn_t real_get_nprocs_conf = nullptr;
static open_fn_t real_open = nullptr;
static openat_fn_t real_openat = nullptr;
static execve_fn_t real_execve = nullptr;
static mmap_fn_t real_mmap = nullptr;
static mmap64_fn_t real_mmap64 = nullptr;

#ifndef MADV_MERGEABLE
#define MADV_MERGEABLE 12
#endif

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS 0x20
#endif

#ifndef MAP_ANON
#define MAP_ANON MAP_ANONYMOUS
#endif

#ifndef SYS_mmap
#if defined(__NR_mmap)
#define SYS_mmap __NR_mmap
#endif
#endif

static thread_local bool in_intercept = false;

/**
 * @brief RAII reentrancy guard ensuring nested library calls bypass
 * interception logic.
 */
struct ReentrancyGuard {
  bool previous_state;
  ReentrancyGuard() noexcept : previous_state(in_intercept) {
    in_intercept = true;
  }
  ~ReentrancyGuard() noexcept { in_intercept = previous_state; }
  ReentrancyGuard(const ReentrancyGuard&) = delete;
  ReentrancyGuard& operator=(const ReentrancyGuard&) = delete;
};

/**
 * @brief Parse the VIRMUX_VIRTUAL_CORES environment variable.
 * @return Positive core count if configured and valid, or 0 otherwise.
 */
inline long get_spoofed_core_count() noexcept {
  const char* env_cores = std::getenv(VIRMUX_ENV_VIRTUAL_CORES);
  if (!env_cores || env_cores[0] == '\0') {
    return 0;
  }
  char* end = nullptr;
  long vcores = std::strtol(env_cores, &end, 10);
  if (end != env_cores && vcores > 0) {
    return vcores;
  }
  return 0;
}

/**
 * @brief Internal open helper shared by explicit open, open64, openat, and
 * openat64 exports.
 * @param dirfd Base directory file descriptor for openat.
 * @param pathname Path to open or redirect.
 * @param flags Access and creation flags.
 * @param mode Creation permissions when O_CREAT or O_TMPFILE is set.
 * @return File descriptor on success, -1 on error.
 */
static int do_openat(int dirfd, const char* pathname, int flags, mode_t mode) {
  if (in_intercept) {
    if (real_openat) {
      return real_openat(dirfd, pathname, flags, mode);
    }
    return static_cast<int>(syscall(SYS_openat, dirfd, pathname, flags, mode));
  }

  ReentrancyGuard guard;

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-warning-option"
#pragma clang diagnostic ignored "-Wtautological-pointer-compare"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnonnull-compare"
#endif
  if (pathname != nullptr && std::strcmp(pathname, "/proc/meminfo") == 0) {
    const char* spoofed_path = std::getenv(VIRMUX_ENV_SPOOFED_MEMINFO);
    if (spoofed_path && spoofed_path[0] != '\0') {
      pathname = spoofed_path;
      dirfd = AT_FDCWD;
    }
  }
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

  if (!real_openat) {
    real_openat = reinterpret_cast<openat_fn_t>(dlsym(RTLD_NEXT, "openat64"));
    if (!real_openat) {
      real_openat = reinterpret_cast<openat_fn_t>(dlsym(RTLD_NEXT, "openat"));
    }
  }
  if (real_openat) {
    return real_openat(dirfd, pathname, flags, mode);
  }
  return static_cast<int>(syscall(SYS_openat, dirfd, pathname, flags, mode));
}

static int do_open(const char* pathname, int flags, mode_t mode) {
  return do_openat(AT_FDCWD, pathname, flags, mode);
}

/**
 * @brief Check whether proactive KSM auto-merge is enabled.
 * @return true if VIRMUX_KSM_AUTO_MERGE is set to "1", false otherwise.
 */
inline bool is_ksm_auto_merge_enabled() noexcept {
  const char* env_ksm = std::getenv(VIRMUX_ENV_KSM_AUTO_MERGE);
  if (!env_ksm || env_ksm[0] == '\0') {
    return false;
  }
  return std::strcmp(env_ksm, "1") == 0;
}

/**
 * @brief Internal mmap helper shared by mmap and mmap64 exports.
 */
template <typename OffT>
static void* do_mmap(void* addr, size_t length, int prot, int flags, int fd,
                     OffT offset, bool is_64) {
  if (in_intercept) {
    if (is_64 && real_mmap64) {
      return real_mmap64(addr, length, prot, flags, fd,
                         static_cast<off64_t>(offset));
    }
    if (!is_64 && real_mmap) {
      return real_mmap(addr, length, prot, flags, fd,
                       static_cast<off_t>(offset));
    }
    return reinterpret_cast<void*>(
        syscall(SYS_mmap, addr, length, prot, flags, fd, offset));
  }

  ReentrancyGuard guard;

  void* res = MAP_FAILED;
  if (is_64) {
    if (!real_mmap64) {
      real_mmap64 = reinterpret_cast<mmap64_fn_t>(dlsym(RTLD_NEXT, "mmap64"));
      if (!real_mmap64) {
        real_mmap64 = reinterpret_cast<mmap64_fn_t>(dlsym(RTLD_NEXT, "mmap"));
      }
    }
    if (real_mmap64) {
      res = real_mmap64(addr, length, prot, flags, fd,
                        static_cast<off64_t>(offset));
    } else {
      res = reinterpret_cast<void*>(
          syscall(SYS_mmap, addr, length, prot, flags, fd, offset));
    }
  } else {
    if (!real_mmap) {
      real_mmap = reinterpret_cast<mmap_fn_t>(dlsym(RTLD_NEXT, "mmap"));
    }
    if (real_mmap) {
      res =
          real_mmap(addr, length, prot, flags, fd, static_cast<off_t>(offset));
    } else {
      res = reinterpret_cast<void*>(
          syscall(SYS_mmap, addr, length, prot, flags, fd, offset));
    }
  }

  if (res != MAP_FAILED && ((flags & MAP_ANONYMOUS) || (flags & MAP_ANON)) &&
      is_ksm_auto_merge_enabled()) {
    int saved_errno = errno;
    (void)::madvise(res, length, MADV_MERGEABLE);
    errno = saved_errno;
  }

  return res;
}

/**
 * @brief Eagerly resolve real glibc symbols using RTLD_NEXT upon library
 * initialization.
 */
__attribute__((constructor)) void init_interceptor() noexcept {
  ReentrancyGuard guard;
  if (!real_sysconf) {
    real_sysconf = reinterpret_cast<sysconf_fn_t>(dlsym(RTLD_NEXT, "sysconf"));
  }
  if (!real_get_nprocs) {
    real_get_nprocs =
        reinterpret_cast<get_nprocs_fn_t>(dlsym(RTLD_NEXT, "get_nprocs"));
  }
  if (!real_get_nprocs_conf) {
    real_get_nprocs_conf =
        reinterpret_cast<get_nprocs_fn_t>(dlsym(RTLD_NEXT, "get_nprocs_conf"));
  }
  if (!real_open) {
    real_open = reinterpret_cast<open_fn_t>(dlsym(RTLD_NEXT, "open64"));
    if (!real_open) {
      real_open = reinterpret_cast<open_fn_t>(dlsym(RTLD_NEXT, "open"));
    }
  }
  if (!real_openat) {
    real_openat = reinterpret_cast<openat_fn_t>(dlsym(RTLD_NEXT, "openat64"));
    if (!real_openat) {
      real_openat = reinterpret_cast<openat_fn_t>(dlsym(RTLD_NEXT, "openat"));
    }
  }
  if (!real_execve) {
    real_execve = reinterpret_cast<execve_fn_t>(dlsym(RTLD_NEXT, "execve"));
  }
  if (!real_mmap) {
    real_mmap = reinterpret_cast<mmap_fn_t>(dlsym(RTLD_NEXT, "mmap"));
  }
  if (!real_mmap64) {
    real_mmap64 = reinterpret_cast<mmap64_fn_t>(dlsym(RTLD_NEXT, "mmap64"));
    if (!real_mmap64) {
      real_mmap64 = reinterpret_cast<mmap64_fn_t>(dlsym(RTLD_NEXT, "mmap"));
    }
  }
}

}  // anonymous namespace

extern "C" __attribute__((visibility("default"))) long sysconf(
    int name) noexcept {
  if (in_intercept) {
    if (real_sysconf) {
      return real_sysconf(name);
    }
    return -1;
  }

  ReentrancyGuard guard;

  if (name == _SC_NPROCESSORS_ONLN || name == _SC_NPROCESSORS_CONF) {
    long vcores = get_spoofed_core_count();
    if (vcores > 0) {
      return vcores;
    }
  }

  if (!real_sysconf) {
    real_sysconf = reinterpret_cast<sysconf_fn_t>(dlsym(RTLD_NEXT, "sysconf"));
  }
  if (real_sysconf) {
    return real_sysconf(name);
  }
  return -1;
}

extern "C" __attribute__((visibility("default"))) int get_nprocs(
    void) noexcept {
  if (in_intercept) {
    if (real_get_nprocs) {
      return real_get_nprocs();
    }
    return 1;
  }

  ReentrancyGuard guard;

  long vcores = get_spoofed_core_count();
  if (vcores > 0) {
    return static_cast<int>(vcores);
  }

  if (!real_get_nprocs) {
    real_get_nprocs =
        reinterpret_cast<get_nprocs_fn_t>(dlsym(RTLD_NEXT, "get_nprocs"));
  }
  if (real_get_nprocs) {
    return real_get_nprocs();
  }
  return 1;
}

extern "C" __attribute__((visibility("default"))) int get_nprocs_conf(
    void) noexcept {
  if (in_intercept) {
    if (real_get_nprocs_conf) {
      return real_get_nprocs_conf();
    }
    return 1;
  }

  ReentrancyGuard guard;

  long vcores = get_spoofed_core_count();
  if (vcores > 0) {
    return static_cast<int>(vcores);
  }

  if (!real_get_nprocs_conf) {
    real_get_nprocs_conf =
        reinterpret_cast<get_nprocs_fn_t>(dlsym(RTLD_NEXT, "get_nprocs_conf"));
  }
  if (real_get_nprocs_conf) {
    return real_get_nprocs_conf();
  }
  return 1;
}

extern "C" __attribute__((visibility("default"))) int open(const char* pathname,
                                                           int flags, ...) {
  mode_t mode = 0;
  if ((flags & O_CREAT)
#ifdef O_TMPFILE
      || ((flags & O_TMPFILE) == O_TMPFILE)
#endif
  ) {
    va_list args;
    va_start(args, flags);
    mode = va_arg(args, mode_t);
    va_end(args);
  }
  return do_open(pathname, flags, mode);
}

extern "C" __attribute__((visibility("default"))) int open64(
    const char* pathname, int flags, ...) {
  mode_t mode = 0;
  if ((flags & O_CREAT)
#ifdef O_TMPFILE
      || ((flags & O_TMPFILE) == O_TMPFILE)
#endif
  ) {
    va_list args;
    va_start(args, flags);
    mode = va_arg(args, mode_t);
    va_end(args);
  }
  return do_open(pathname, flags, mode);
}

extern "C" __attribute__((visibility("default"))) int openat(
    int dirfd, const char* pathname, int flags, ...) {
  mode_t mode = 0;
  if ((flags & O_CREAT)
#ifdef O_TMPFILE
      || ((flags & O_TMPFILE) == O_TMPFILE)
#endif
  ) {
    va_list args;
    va_start(args, flags);
    mode = va_arg(args, mode_t);
    va_end(args);
  }
  return do_openat(dirfd, pathname, flags, mode);
}

extern "C" __attribute__((visibility("default"))) int openat64(
    int dirfd, const char* pathname, int flags, ...) {
  mode_t mode = 0;
  if ((flags & O_CREAT)
#ifdef O_TMPFILE
      || ((flags & O_TMPFILE) == O_TMPFILE)
#endif
  ) {
    va_list args;
    va_start(args, flags);
    mode = va_arg(args, mode_t);
    va_end(args);
  }
  return do_openat(dirfd, pathname, flags, mode);
}

extern "C" __attribute__((visibility("default"))) int execve(
    const char* pathname, char* const argv[], char* const envp[]) noexcept {
  if (in_intercept) {
    if (real_execve) {
      return real_execve(pathname, argv, envp);
    }
    return static_cast<int>(syscall(SYS_execve, pathname, argv, envp));
  }

  ReentrancyGuard guard;

  if (!real_execve) {
    real_execve = reinterpret_cast<execve_fn_t>(dlsym(RTLD_NEXT, "execve"));
  }

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-warning-option"
#pragma clang diagnostic ignored "-Wtautological-pointer-compare"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnonnull-compare"
#endif
  if (pathname != nullptr && argv != nullptr &&
      (virmux::is_browser_binary(pathname) ||
       (argv[0] != nullptr && virmux::is_browser_binary(argv[0])))) {
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
    try {
      std::vector<std::string> augmented = virmux::inject_slim_flags(argv);
      if (!augmented.empty()) {
        std::vector<char*> new_argv;
        new_argv.reserve(augmented.size() + 1);
        for (auto& arg : augmented) {
          new_argv.push_back(const_cast<char*>(arg.c_str()));
        }
        new_argv.push_back(nullptr);

        if (real_execve) {
          return real_execve(pathname, new_argv.data(), envp);
        }
        return static_cast<int>(
            syscall(SYS_execve, pathname, new_argv.data(), envp));
      }
    } catch (...) {
      // In case of allocation failure, fall through to unaugmented execve
    }
  }

  if (real_execve) {
    return real_execve(pathname, argv, envp);
  }
  return static_cast<int>(syscall(SYS_execve, pathname, argv, envp));
}

extern "C" __attribute__((visibility("default"))) void* mmap(
    void* addr, size_t length, int prot, int flags, int fd,
    off_t offset) noexcept {
  return do_mmap(addr, length, prot, flags, fd, offset, false);
}

extern "C" __attribute__((visibility("default"))) void* mmap64(
    void* addr, size_t length, int prot, int flags, int fd,
    off64_t offset) noexcept {
  return do_mmap(addr, length, prot, flags, fd, offset, true);
}
