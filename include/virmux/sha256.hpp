#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace virmux {

/**
 * @brief Computes SHA-256 hash of a byte buffer.
 * @return 64 lowercase hexadecimal character string.
 */
[[nodiscard]] std::string sha256_hash(std::span<const uint8_t> data);

/**
 * @brief Computes SHA-256 hash of a string.
 * @return 64 lowercase hexadecimal character string.
 */
[[nodiscard]] std::string sha256_hash(std::string_view data);

/**
 * @brief Computes SHA-256 hash of a file on disk.
 * @return 64 hex characters, or empty string on read error.
 */
[[nodiscard]] std::string sha256_hash_file(const std::string& file_path);

}  // namespace virmux
