/**
 * @file fake_cgroupfs.hpp
 * @brief Temporary directory tree that imitates a cgroup v2 hierarchy for
 * unit tests of delegation discovery and run-cgroup creation.
 *
 * Each cgroup is a directory holding empty control files. Write permission is
 * modelled with file modes, which access(W_OK) honours for non-root users;
 * tests that depend on a read-only file must skip when running as root.
 */

#pragma once

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>

namespace virmux::testing {

class FakeCgroupFs {
 public:
  /** @brief Create an empty fake hierarchy; @p v2 adds the root's
   * cgroup.controllers, which marks a unified (v2) mount. */
  explicit FakeCgroupFs(bool v2 = true) {
    char tmpl[] = "/tmp/virmux_fake_cgroupfs_XXXXXX";
    const char* dir = ::mkdtemp(tmpl);
    root_ = dir != nullptr ? dir : "";
    if (v2) {
      add("", false);
    }
  }

  ~FakeCgroupFs() {
    std::error_code ec;
    // Restore write permission so remove_all can unlink read-only files.
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(root_, ec)) {
      ::chmod(entry.path().c_str(), entry.is_directory() ? 0755 : 0644);
    }
    std::filesystem::remove_all(root_, ec);
  }

  FakeCgroupFs(const FakeCgroupFs&) = delete;
  FakeCgroupFs& operator=(const FakeCgroupFs&) = delete;

  /**
   * @brief Add cgroup @p rel ("" is the root) with its core control files.
   * @param writable Whether cgroup.procs and cgroup.subtree_control are
   * writable (models ownership / delegation).
   * @param subtree_control Controllers enabled for children.
   * @param extra_files Additional control files to create (always writable).
   */
  void add(const std::string& rel, bool writable,
           const std::string& subtree_control = "cpu memory pids",
           std::initializer_list<const char*> extra_files = {}) const {
    const std::string dir = path(rel);
    std::filesystem::create_directories(dir);
    write(dir + "/cgroup.controllers", "cpu memory pids");
    write(dir + "/cgroup.subtree_control", subtree_control);
    write(dir + "/cgroup.procs", "");
    for (const char* f : extra_files) {
      write(dir + "/" + f, "");
    }
    const mode_t mode = writable ? 0644 : 0444;
    ::chmod((dir + "/cgroup.procs").c_str(), mode);
    ::chmod((dir + "/cgroup.subtree_control").c_str(), mode);
  }

  /** @brief Absolute path of cgroup @p rel. */
  [[nodiscard]] std::string path(const std::string& rel) const {
    return rel.empty() ? root_ : root_ + "/" + rel;
  }

  /** @brief Root of the fake hierarchy (stands in for /sys/fs/cgroup). */
  [[nodiscard]] const std::string& root() const { return root_; }

  /** @brief Overwrite (or create) a file with @p content. */
  static void write(const std::string& file, const std::string& content) {
    std::ofstream out(file, std::ios::trunc);
    out << content;
  }

 private:
  std::string root_;
};

}  // namespace virmux::testing
