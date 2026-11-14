/**
 * @file cleanse.hpp
 * @brief Post-execution page cache eviction utility for virmux.
 */

#pragma once

#include <filesystem>
#include <string_view>

namespace virmux {

/**
 * @brief Evict page cache pages for a given file or directory tree.
 *
 * For regular files, opens the file with O_RDONLY | O_NOATIME | O_CLOEXEC
 * (with fallback to O_RDONLY | O_CLOEXEC on EPERM/EACCES) and calls
 * posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED) to release cached pages back
 * to the kernel.
 * For directories, recursively walks the directory hierarchy and applies
 * cache eviction to all regular files encountered. Nonexistent paths, empty
 * paths, or permission-denied entries are handled cleanly without throwing.
 *
 * @param path Path to file or directory to cleanse.
 */
void cleanse_path(std::string_view path);

/**
 * @brief Recursively ensure owner read, write, and execute permissions on a path.
 *
 * Designed for sandbox/scratch directory teardown where child processes may have
 * created read-only or restricted subdirectories. Explicitly never follows symlinks
 * (uses fs::perm_options::nofollow and skips symlinks during directory traversal)
 * to prevent host escape vulnerabilities.
 *
 * @param path Path to file or directory whose permissions should be fixed.
 */
void fix_permissions_recursive(const std::filesystem::path& path) noexcept;

}  // namespace virmux
