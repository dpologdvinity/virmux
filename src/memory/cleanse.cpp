/**
 * @file cleanse.cpp
 * @brief Implementation of post-execution page cache eviction.
 */

#include "virmux/cleanse.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <filesystem>
#include <system_error>

#include "virmux/raii.hpp"

namespace virmux {

namespace {

/**
 * @brief Evict cached pages for a single file using posix_fadvise.
 *
 * Opens the file with O_RDONLY | O_NOATIME | O_CLOEXEC. If O_NOATIME fails
 * with EPERM or EACCES (e.g., caller is not file owner and lacks CAP_FOWNER),
 * falls back to O_RDONLY | O_CLOEXEC. Calls posix_fadvise with
 * POSIX_FADV_DONTNEED.
 *
 * @param file_path Filesystem path to the regular file.
 */
void evict_file_cache(const std::filesystem::path& file_path) {
  int open_flags = O_RDONLY | O_CLOEXEC;
#ifdef O_NOATIME
  open_flags |= O_NOATIME;
#endif

  ScopedFd fd(::open(file_path.c_str(), open_flags));

#ifdef O_NOATIME
  if (!fd.valid() && (errno == EPERM || errno == EACCES)) {
    fd.reset(::open(file_path.c_str(), O_RDONLY | O_CLOEXEC));
  }
#endif

  if (fd.valid()) {
    ::posix_fadvise(fd.get(), 0, 0, POSIX_FADV_DONTNEED);
  }
}

}  // namespace

void cleanse_path(std::string_view path) {
  if (path.empty()) {
    return;
  }

  std::error_code ec;
  std::filesystem::path fs_path(path);

  if (!std::filesystem::exists(fs_path, ec) || ec) {
    return;
  }

  if (std::filesystem::is_regular_file(fs_path, ec) && !ec) {
    evict_file_cache(fs_path);
    return;
  }

  if (std::filesystem::is_directory(fs_path, ec) && !ec) {
    for (auto it = std::filesystem::recursive_directory_iterator(
             fs_path,
             std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator();
         it.increment(ec)) {
      if (ec) {
        ec.clear();
        continue;
      }

      std::error_code entry_ec;
      if (it->is_regular_file(entry_ec) && !entry_ec) {
        evict_file_cache(it->path());
      }
    }
  }
}

}  // namespace virmux
