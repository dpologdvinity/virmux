/**
 * @file intercept.h
 * @brief Public interface and environment definitions for the virmux preload interceptor.
 */

#ifndef VIRMUX_INTERCEPT_H
#define VIRMUX_INTERCEPT_H

#undef _FILE_OFFSET_BITS

#include <fcntl.h>
#include <sys/sysinfo.h>
#include <unistd.h>

#ifndef __THROW
#define __THROW
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Environment variable specifying virtual core count spoofing.
 *
 * When set to a positive integer string (e.g. "2"), sysconf(_SC_NPROCESSORS_ONLN),
 * sysconf(_SC_NPROCESSORS_CONF), get_nprocs(), and get_nprocs_conf() return this value.
 */
#define VIRMUX_ENV_VIRTUAL_CORES "VIRMUX_VIRTUAL_CORES"

/**
 * @brief Environment variable specifying synthetic /proc/meminfo redirect path.
 *
 * When set to an existing file path, calls to open() or open64() for "/proc/meminfo"
 * are redirected to this path.
 */
#define VIRMUX_ENV_SPOOFED_MEMINFO "VIRMUX_SPOOFED_MEMINFO"

/* Exported interceptor functions matching glibc C ABI */
long sysconf(int name) __THROW;
int get_nprocs(void) __THROW;
int get_nprocs_conf(void) __THROW;
int open(const char *pathname, int flags, ...);
int open64(const char *pathname, int flags, ...);
int execve(const char *pathname, char *const argv[], char *const envp[]) __THROW;

#ifdef __cplusplus
}
#endif

#endif /* VIRMUX_INTERCEPT_H */
