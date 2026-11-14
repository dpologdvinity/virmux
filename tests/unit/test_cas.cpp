/**
 * @file test_cas.cpp
 * @brief Unit tests for SHA-256 hashing and Content-Addressable Storage (CAS).
 */

extern "C" {
#include "unity/unity.h"
}

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "virmux/cas.hpp"
#include "virmux/sha256.hpp"

namespace fs = std::filesystem;

namespace {

std::string create_test_temp_dir() {
  char template_path[] = "/tmp/virmux_cas_test_XXXXXX";
  char* dir = ::mkdtemp(template_path);
  TEST_ASSERT_NOT_NULL(dir);
  return std::string(dir);
}

void cleanup_test_dir(const std::string& path) {
  std::error_code ec;
  fs::remove_all(path, ec);
}

}  // namespace

void setUp(void) {}
void tearDown(void) {}

void test_sha256_nist_vectors(void) {
  // Test Vector 1: Empty string
  std::string hash_empty = virmux::sha256_hash("");
  TEST_ASSERT_EQUAL_STRING(
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
      hash_empty.c_str());

  // Test Vector 2: "abc"
  std::string hash_abc = virmux::sha256_hash("abc");
  TEST_ASSERT_EQUAL_STRING(
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
      hash_abc.c_str());

  // Test Vector 3: 448-bit (56 bytes) / multi-block input
  std::string long_input =
      "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  std::string hash_long = virmux::sha256_hash(long_input);
  TEST_ASSERT_EQUAL_STRING(
      "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
      hash_long.c_str());
}

void test_sha256_byte_span(void) {
  std::vector<uint8_t> data = {'a', 'b', 'c'};
  std::string hash_val = virmux::sha256_hash(std::span<const uint8_t>(data));
  TEST_ASSERT_EQUAL_STRING(
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
      hash_val.c_str());
}

void test_sha256_file(void) {
  std::string temp_dir = create_test_temp_dir();
  std::string test_file = temp_dir + "/sample.txt";

  {
    std::ofstream ofs(test_file);
    ofs << "abc";
  }

  std::string hash_val = virmux::sha256_hash_file(test_file);
  TEST_ASSERT_EQUAL_STRING(
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
      hash_val.c_str());

  // Non-existent file must return empty string
  std::string empty_hash =
      virmux::sha256_hash_file(temp_dir + "/nonexistent.txt");
  TEST_ASSERT_EQUAL_STRING("", empty_hash.c_str());

  cleanup_test_dir(temp_dir);
}

void test_cas_put_get_bytes(void) {
  std::string temp_dir = create_test_temp_dir();
  virmux::CasStore cas(temp_dir);

  std::string content = "hello world content addressable storage";
  std::string key = virmux::sha256_hash(content);

  TEST_ASSERT_FALSE(cas.contains(key));

  std::vector<uint8_t> bytes(content.begin(), content.end());
  bool put_ok = cas.put_bytes(key, bytes);
  TEST_ASSERT_TRUE(put_ok);
  TEST_ASSERT_TRUE(cas.contains(key));

  std::string expected_prefix = key.substr(0, 2);
  std::string expected_path =
      temp_dir + "/objects/" + expected_prefix + "/" + key;
  TEST_ASSERT_EQUAL_STRING(expected_path.c_str(), cas.get_path(key).c_str());
  TEST_ASSERT_TRUE(fs::exists(expected_path));

  // Retrieve to destination
  std::string dest_path = temp_dir + "/dest.txt";
  bool get_ok = cas.get(key, dest_path);
  TEST_ASSERT_TRUE(get_ok);
  TEST_ASSERT_TRUE(fs::exists(dest_path));

  std::ifstream ifs(dest_path);
  std::string read_content((std::istreambuf_iterator<char>(ifs)),
                           std::istreambuf_iterator<char>());
  TEST_ASSERT_EQUAL_STRING(content.c_str(), read_content.c_str());

  cleanup_test_dir(temp_dir);
}

void test_cas_put_file_and_atomic_staging(void) {
  std::string temp_dir = create_test_temp_dir();
  virmux::CasStore cas(temp_dir);

  std::string src_path = temp_dir + "/source.bin";
  std::string payload = "compiler-output-object-data-12345";
  {
    std::ofstream ofs(src_path, std::ios::binary);
    ofs << payload;
  }

  std::string key = virmux::sha256_hash(payload);
  bool put_ok = cas.put(key, src_path);
  TEST_ASSERT_TRUE(put_ok);
  TEST_ASSERT_TRUE(cas.contains(key));

  // Check temp directory has no lingering files
  std::string tmp_dir = temp_dir + "/tmp";
  size_t tmp_file_count = 0;
  for (const auto& entry : fs::directory_iterator(tmp_dir)) {
    if (entry.is_regular_file()) {
      tmp_file_count++;
    }
  }
  TEST_ASSERT_EQUAL_UINT(0, tmp_file_count);

  // Put nonexistent source file must return false
  bool fail_put = cas.put("somekey", temp_dir + "/no_such_file.bin");
  TEST_ASSERT_FALSE(fail_put);

  cleanup_test_dir(temp_dir);
}

void test_cas_stats_and_reset(void) {
  std::string temp_dir = create_test_temp_dir();
  virmux::CasStore cas(temp_dir);

  virmux::CasStats initial_stats = cas.get_stats();
  TEST_ASSERT_EQUAL_UINT(0, initial_stats.object_count);
  TEST_ASSERT_EQUAL_UINT64(0, initial_stats.total_bytes);
  TEST_ASSERT_EQUAL_UINT64(0, initial_stats.hits);
  TEST_ASSERT_EQUAL_UINT64(0, initial_stats.misses);

  // Miss
  std::string dest = temp_dir + "/miss.txt";
  TEST_ASSERT_FALSE(cas.get("nonexistent_key_1234", dest));
  TEST_ASSERT_EQUAL_UINT64(1, cas.get_stats().misses);

  // Store 2 objects
  std::string payload1 = "data1";
  std::string key1 = virmux::sha256_hash(payload1);
  cas.put_bytes(key1, std::span<const uint8_t>(
                          reinterpret_cast<const uint8_t*>(payload1.data()),
                          payload1.size()));

  std::string payload2 = "data2_longer";
  std::string key2 = virmux::sha256_hash(payload2);
  cas.put_bytes(key2, std::span<const uint8_t>(
                          reinterpret_cast<const uint8_t*>(payload2.data()),
                          payload2.size()));

  // Hit
  TEST_ASSERT_TRUE(cas.get(key1, dest));
  virmux::CasStats stats = cas.get_stats();
  TEST_ASSERT_EQUAL_UINT(2, stats.object_count);
  TEST_ASSERT_EQUAL_UINT64(payload1.size() + payload2.size(),
                           stats.total_bytes);
  TEST_ASSERT_EQUAL_UINT64(1, stats.hits);
  TEST_ASSERT_EQUAL_UINT64(1, stats.misses);

  // Reset stats
  cas.reset_stats();
  virmux::CasStats reset_stats = cas.get_stats();
  TEST_ASSERT_EQUAL_UINT64(0, reset_stats.hits);
  TEST_ASSERT_EQUAL_UINT64(0, reset_stats.misses);
  // object count and bytes remain
  TEST_ASSERT_EQUAL_UINT(2, reset_stats.object_count);

  cleanup_test_dir(temp_dir);
}

void test_cas_prune_lru(void) {
  std::string temp_dir = create_test_temp_dir();
  virmux::CasStore cas(temp_dir);

  // Store 3 objects of 100 bytes each
  std::string obj1(100, 'A');
  std::string key1 = virmux::sha256_hash(obj1);
  cas.put_bytes(key1, std::span<const uint8_t>(
                          reinterpret_cast<const uint8_t*>(obj1.data()), 100));

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::string obj2(100, 'B');
  std::string key2 = virmux::sha256_hash(obj2);
  cas.put_bytes(key2, std::span<const uint8_t>(
                          reinterpret_cast<const uint8_t*>(obj2.data()), 100));

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::string obj3(100, 'C');
  std::string key3 = virmux::sha256_hash(obj3);
  cas.put_bytes(key3, std::span<const uint8_t>(
                          reinterpret_cast<const uint8_t*>(obj3.data()), 100));

  virmux::CasStats before = cas.get_stats();
  TEST_ASSERT_EQUAL_UINT(3, before.object_count);
  TEST_ASSERT_EQUAL_UINT64(300, before.total_bytes);

  // Prune to 250 bytes: should remove oldest (obj1), remaining 200 bytes
  size_t pruned = cas.prune(250);
  TEST_ASSERT_EQUAL_UINT(1, pruned);
  TEST_ASSERT_FALSE(cas.contains(key1));
  TEST_ASSERT_TRUE(cas.contains(key2));
  TEST_ASSERT_TRUE(cas.contains(key3));

  virmux::CasStats after = cas.get_stats();
  TEST_ASSERT_EQUAL_UINT(2, after.object_count);
  TEST_ASSERT_EQUAL_UINT64(200, after.total_bytes);

  // Prune to 50 bytes: should remove obj2 and obj3 (both 100 bytes > 50 bytes)
  size_t pruned2 = cas.prune(50);
  TEST_ASSERT_EQUAL_UINT(2, pruned2);
  TEST_ASSERT_FALSE(cas.contains(key2));
  TEST_ASSERT_FALSE(cas.contains(key3));

  virmux::CasStats after2 = cas.get_stats();
  TEST_ASSERT_EQUAL_UINT(0, after2.object_count);
  TEST_ASSERT_EQUAL_UINT64(0, after2.total_bytes);

  cleanup_test_dir(temp_dir);
}

void test_cas_overwrite_atomic(void) {
  std::string temp_dir = create_test_temp_dir();
  virmux::CasStore cas(temp_dir);

  std::string key = "test_key_overwrite_123456";
  std::string payload1 = "original content";
  std::string payload2 = "updated content with different length";

  TEST_ASSERT_TRUE(cas.put_bytes(
      key, std::span<const uint8_t>(
               reinterpret_cast<const uint8_t*>(payload1.data()), payload1.size())));

  std::string dest1 = temp_dir + "/dest1.txt";
  TEST_ASSERT_TRUE(cas.get(key, dest1));
  std::ifstream ifs1(dest1);
  std::string read1((std::istreambuf_iterator<char>(ifs1)),
                    std::istreambuf_iterator<char>());
  TEST_ASSERT_EQUAL_STRING(payload1.c_str(), read1.c_str());

  // Overwrite existing key
  TEST_ASSERT_TRUE(cas.put_bytes(
      key, std::span<const uint8_t>(
               reinterpret_cast<const uint8_t*>(payload2.data()), payload2.size())));

  std::string dest2 = temp_dir + "/dest2.txt";
  TEST_ASSERT_TRUE(cas.get(key, dest2));
  std::ifstream ifs2(dest2);
  std::string read2((std::istreambuf_iterator<char>(ifs2)),
                    std::istreambuf_iterator<char>());
  TEST_ASSERT_EQUAL_STRING(payload2.c_str(), read2.c_str());

  cleanup_test_dir(temp_dir);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_sha256_nist_vectors);
  RUN_TEST(test_sha256_byte_span);
  RUN_TEST(test_sha256_file);
  RUN_TEST(test_cas_put_get_bytes);
  RUN_TEST(test_cas_put_file_and_atomic_staging);
  RUN_TEST(test_cas_stats_and_reset);
  RUN_TEST(test_cas_prune_lru);
  RUN_TEST(test_cas_overwrite_atomic);
  return UNITY_END();
}
