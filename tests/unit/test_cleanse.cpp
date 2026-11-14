/**
 * @file test_cleanse.cpp
 * @brief Unit tests for page cache eviction using posix_fadvise.
 */

extern "C" {
#include "unity/unity.h"
}

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "virmux/cleanse.hpp"

void setUp(void) {}
void tearDown(void) {}

namespace {

/**
 * @brief Check if any pages of the given file are currently resident in the OS
 * page cache.
 *
 * Uses mmap and mincore to query page cache residency without faulting pages
 * into memory.
 */
bool is_file_in_page_cache(const std::string& path, size_t size) {
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return false;
  }

  void* addr = ::mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
  if (addr == MAP_FAILED) {
    ::close(fd);
    return false;
  }

  long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0) {
    page_size = 4096;
  }

  size_t num_pages = (size + page_size - 1) / page_size;
  std::vector<unsigned char> vec(num_pages, 0);
  int res = ::mincore(addr, size, vec.data());

  ::munmap(addr, size);
  ::close(fd);

  if (res != 0) {
    return false;
  }

  for (size_t i = 0; i < num_pages; ++i) {
    if (vec[i] & 1) {
      return true;
    }
  }
  return false;
}

}  // namespace

void test_cleanse_path_evicts_cache_without_deleting_file(void) {
  char file_template[] = "/tmp/virmux_cleanse_test_XXXXXX";
  int fd = ::mkstemp(file_template);
  TEST_ASSERT_TRUE(fd >= 0);
  ::close(fd);
  std::string test_file = file_template;

  {
    std::ofstream out(test_file);
    out << "temporary buffer test content for fadvise testing\n";
  }

  virmux::cleanse_path(test_file);

  // File must still exist and be intact, but cached pages evicted
  TEST_ASSERT_TRUE(std::filesystem::exists(test_file));
  std::filesystem::remove(test_file);
}

void test_cleanse_path_actually_evicts_page_cache(void) {
  char file_template[] = "/tmp/virmux_cleanse_page_cache_XXXXXX";
  int fd = ::mkstemp(file_template);
  TEST_ASSERT_TRUE(fd >= 0);
  ::close(fd);
  std::string test_file = file_template;

  constexpr size_t test_size = 4096 * 4;  // 4 pages = 16 KB

  // Write file and flush to disk
  {
    int write_fd = ::open(test_file.c_str(), O_WRONLY | O_TRUNC, 0644);
    TEST_ASSERT_TRUE(write_fd >= 0);
    std::vector<char> buf(test_size, 'Z');
    ssize_t written = ::write(write_fd, buf.data(), buf.size());
    TEST_ASSERT_EQUAL_INT(test_size, written);
    ::fdatasync(write_fd);
    ::close(write_fd);
  }

  // Read file via read() to populate the kernel page cache
  {
    int read_fd = ::open(test_file.c_str(), O_RDONLY);
    TEST_ASSERT_TRUE(read_fd >= 0);
    std::vector<char> read_buf(test_size);
    ssize_t bytes_read = ::read(read_fd, read_buf.data(), read_buf.size());
    TEST_ASSERT_EQUAL_INT(test_size, bytes_read);
    ::close(read_fd);
  }

  // Confirm pages are in page cache before eviction
  bool cached_before = is_file_in_page_cache(test_file, test_size);
  TEST_ASSERT_TRUE(cached_before);

  // Cleanse path
  virmux::cleanse_path(test_file);

  // Verify cached pages are evicted
  bool cached_after = is_file_in_page_cache(test_file, test_size);
  TEST_ASSERT_FALSE(cached_after);

  // Verify file content is intact
  TEST_ASSERT_TRUE(std::filesystem::exists(test_file));
  TEST_ASSERT_EQUAL_UINT64(test_size, std::filesystem::file_size(test_file));

  std::filesystem::remove(test_file);
}

void test_cleanse_path_directory_recursive(void) {
  char dir_template[] = "/tmp/virmux_cleanse_dir_XXXXXX";
  char* tmp_dir = ::mkdtemp(dir_template);
  TEST_ASSERT_NOT_NULL(tmp_dir);

  std::string base_dir(tmp_dir);
  std::string sub_dir = base_dir + "/subdir";
  std::string sub_sub_dir = sub_dir + "/nested";

  std::filesystem::create_directories(sub_sub_dir);

  std::string file1 = base_dir + "/file1.bin";
  std::string file2 = sub_dir + "/file2.bin";
  std::string file3 = sub_sub_dir + "/file3.bin";

  constexpr size_t file_size = 4096 * 2;

  auto populate_and_cache = [](const std::string& path) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    TEST_ASSERT_TRUE(fd >= 0);
    std::vector<char> buf(file_size, 'A');
    ssize_t written = ::write(fd, buf.data(), buf.size());
    TEST_ASSERT_EQUAL_INT(file_size, written);
    ::fdatasync(fd);
    ::close(fd);

    fd = ::open(path.c_str(), O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    std::vector<char> read_buf(file_size);
    ssize_t bytes_read = ::read(fd, read_buf.data(), read_buf.size());
    TEST_ASSERT_EQUAL_INT(file_size, bytes_read);
    ::close(fd);
  };

  populate_and_cache(file1);
  populate_and_cache(file2);
  populate_and_cache(file3);

  TEST_ASSERT_TRUE(is_file_in_page_cache(file1, file_size));
  TEST_ASSERT_TRUE(is_file_in_page_cache(file2, file_size));
  TEST_ASSERT_TRUE(is_file_in_page_cache(file3, file_size));

  // Recursively cleanse root directory
  virmux::cleanse_path(base_dir);

  // All files should be evicted from page cache
  TEST_ASSERT_FALSE(is_file_in_page_cache(file1, file_size));
  TEST_ASSERT_FALSE(is_file_in_page_cache(file2, file_size));
  TEST_ASSERT_FALSE(is_file_in_page_cache(file3, file_size));

  // All files must still exist
  TEST_ASSERT_TRUE(std::filesystem::exists(file1));
  TEST_ASSERT_TRUE(std::filesystem::exists(file2));
  TEST_ASSERT_TRUE(std::filesystem::exists(file3));

  std::filesystem::remove_all(base_dir);
}

void test_cleanse_path_directory_continues_past_unreadable(void) {
  char dir_template[] = "/tmp/virmux_cleanse_perm_XXXXXX";
  char* tmp_dir = ::mkdtemp(dir_template);
  TEST_ASSERT_NOT_NULL(tmp_dir);

  std::string base_dir(tmp_dir);
  std::string sub_unreadable = base_dir + "/unreadable_sub";
  std::string sub_deep = sub_unreadable + "/deep";
  std::filesystem::create_directories(sub_deep);

  std::string file_nested = sub_deep + "/nested.bin";
  std::string file_after = base_dir + "/file_after.bin";

  constexpr size_t file_size = 4096 * 2;
  auto populate_and_cache = [](const std::string& path) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    TEST_ASSERT_TRUE(fd >= 0);
    std::vector<char> buf(file_size, 'U');
    ssize_t written = ::write(fd, buf.data(), buf.size());
    TEST_ASSERT_EQUAL_INT(file_size, written);
    ::fdatasync(fd);
    ::close(fd);

    fd = ::open(path.c_str(), O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    std::vector<char> read_buf(file_size);
    ssize_t bytes_read = ::read(fd, read_buf.data(), read_buf.size());
    TEST_ASSERT_EQUAL_INT(file_size, bytes_read);
    ::close(fd);
  };

  populate_and_cache(file_nested);
  populate_and_cache(file_after);

  TEST_ASSERT_TRUE(is_file_in_page_cache(file_after, file_size));

  // Make sub_unreadable unreadable
  ::chmod(sub_unreadable.c_str(), 0000);

  // Cleanse directory - must continue past unreadable entry and cleanse
  // file_after
  virmux::cleanse_path(base_dir);

  // Restore permissions for cleanup
  ::chmod(sub_unreadable.c_str(), 0755);

  TEST_ASSERT_FALSE(is_file_in_page_cache(file_after, file_size));
  TEST_ASSERT_TRUE(std::filesystem::exists(file_after));

  std::filesystem::remove_all(base_dir);
}

void test_cleanse_path_nonexistent_and_empty(void) {
  // Empty path should return cleanly
  virmux::cleanse_path("");

  // Nonexistent path generated via mkdtemp then removed
  char dir_template[] = "/tmp/virmux_cleanse_nonexistent_XXXXXX";
  char* tmp_dir = ::mkdtemp(dir_template);
  TEST_ASSERT_NOT_NULL(tmp_dir);
  std::string non_existent = tmp_dir;
  std::filesystem::remove_all(non_existent);

  virmux::cleanse_path(non_existent);
}

void test_cleanse_path_symlinks_and_broken_symlinks(void) {
  char dir_template[] = "/tmp/virmux_cleanse_symlink_XXXXXX";
  char* tmp_dir = ::mkdtemp(dir_template);
  TEST_ASSERT_NOT_NULL(tmp_dir);

  std::string base_dir(tmp_dir);
  std::string target_file = base_dir + "/target.txt";
  std::string valid_symlink = base_dir + "/link_valid";
  std::string broken_symlink = base_dir + "/link_broken";

  {
    std::ofstream out(target_file);
    out << "valid target file content\n";
  }

  std::error_code ec;
  std::filesystem::create_symlink(target_file, valid_symlink, ec);
  std::filesystem::create_symlink(base_dir + "/nonexistent_target",
                                  broken_symlink, ec);

  // Cleanse must handle symlinks and broken symlinks without crashing or
  // throwing
  virmux::cleanse_path(base_dir);

  TEST_ASSERT_TRUE(std::filesystem::exists(target_file));

  std::filesystem::remove_all(base_dir);
}

void test_fix_permissions_recursive_symlink_safety(void) {
  char dir_template[] = "/tmp/virmux_fixperm_XXXXXX";
  char* tmp_dir = ::mkdtemp(dir_template);
  TEST_ASSERT_NOT_NULL(tmp_dir);

  char out_template[] = "/tmp/virmux_fixperm_out_XXXXXX";
  char* tmp_out = ::mkdtemp(out_template);
  TEST_ASSERT_NOT_NULL(tmp_out);

  std::string base_dir(tmp_dir);
  std::string out_dir(tmp_out);

  // Outside target file with 0444 read-only permissions
  std::string outside_target = out_dir + "/external_file.txt";
  {
    std::ofstream out(outside_target);
    out << "external secret data\n";
  }
  std::error_code ec;
  std::filesystem::permissions(outside_target, std::filesystem::perms::owner_read,
                               std::filesystem::perm_options::replace, ec);

  // Inside base_dir: read-only directory and file, plus a symlink pointing outside
  std::string internal_dir = base_dir + "/readonly_subdir";
  std::filesystem::create_directory(internal_dir, ec);
  std::string internal_file = internal_dir + "/ro_file.txt";
  {
    std::ofstream out(internal_file);
    out << "internal data\n";
  }
  std::filesystem::permissions(internal_file, std::filesystem::perms::owner_read,
                               std::filesystem::perm_options::replace, ec);
  std::filesystem::permissions(internal_dir,
                               std::filesystem::perms::owner_read |
                                   std::filesystem::perms::owner_exec,
                               std::filesystem::perm_options::replace, ec);

  // Symlink pointing to outside target
  std::string symlink_path = base_dir + "/link_to_external";
  std::filesystem::create_symlink(outside_target, symlink_path, ec);

  // Fix permissions on base_dir
  virmux::fix_permissions_recursive(base_dir);

  // Verify internal read-only subdir was fixed so remove_all succeeds
  TEST_ASSERT_TRUE(std::filesystem::remove_all(base_dir, ec) > 0);
  TEST_ASSERT_FALSE(ec);

  // Verify external file was NEVER touched or chown'd/chmod'd
  auto out_perms = std::filesystem::status(outside_target, ec).permissions();
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(std::filesystem::perms::owner_read),
      static_cast<int>(out_perms & std::filesystem::perms::owner_all));

  // Cleanup external dir
  std::filesystem::permissions(outside_target, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace, ec);
  std::filesystem::remove_all(out_dir, ec);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_cleanse_path_evicts_cache_without_deleting_file);
  RUN_TEST(test_cleanse_path_actually_evicts_page_cache);
  RUN_TEST(test_cleanse_path_directory_recursive);
  RUN_TEST(test_cleanse_path_directory_continues_past_unreadable);
  RUN_TEST(test_cleanse_path_nonexistent_and_empty);
  RUN_TEST(test_cleanse_path_symlinks_and_broken_symlinks);
  RUN_TEST(test_fix_permissions_recursive_symlink_safety);
  return UNITY_END();
}
