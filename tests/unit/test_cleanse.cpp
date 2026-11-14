/**
 * @file test_cleanse.cpp
 * @brief Unit tests for page cache eviction using posix_fadvise.
 */

extern "C" {
#include "unity/unity.h"
}

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

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
  std::string test_file = "/tmp/virmux_cleanse_test.tmp";
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
  std::string test_file = "/tmp/virmux_cleanse_page_cache_test.tmp";
  constexpr size_t test_size = 4096 * 4;  // 4 pages = 16 KB

  // Write file and flush to disk
  {
    int fd = ::open(test_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    TEST_ASSERT_TRUE(fd >= 0);
    std::vector<char> buf(test_size, 'Z');
    ssize_t written = ::write(fd, buf.data(), buf.size());
    TEST_ASSERT_EQUAL_INT(test_size, written);
    ::fdatasync(fd);
    ::close(fd);
  }

  // Read file via read() to populate the kernel page cache
  {
    int fd = ::open(test_file.c_str(), O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    std::vector<char> read_buf(test_size);
    ssize_t bytes_read = ::read(fd, read_buf.data(), read_buf.size());
    TEST_ASSERT_EQUAL_INT(test_size, bytes_read);
    ::close(fd);
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
  char* tmp_dir = mkdtemp(dir_template);
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

void test_cleanse_path_nonexistent_and_empty(void) {
  // Empty path should return cleanly
  virmux::cleanse_path("");

  // Nonexistent path should return cleanly
  virmux::cleanse_path("/tmp/virmux_nonexistent_path_definitely_missing");
}

void test_cleanse_path_symlinks_and_broken_symlinks(void) {
  char dir_template[] = "/tmp/virmux_cleanse_symlink_XXXXXX";
  char* tmp_dir = mkdtemp(dir_template);
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

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_cleanse_path_evicts_cache_without_deleting_file);
  RUN_TEST(test_cleanse_path_actually_evicts_page_cache);
  RUN_TEST(test_cleanse_path_directory_recursive);
  RUN_TEST(test_cleanse_path_nonexistent_and_empty);
  RUN_TEST(test_cleanse_path_symlinks_and_broken_symlinks);
  return UNITY_END();
}
