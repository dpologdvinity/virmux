/**
 * @file sha256.cpp
 * @brief Self-contained RFC 6234 / FIPS 180-4 compliant SHA-256 implementation.
 */

#include "virmux/sha256.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

namespace virmux {
namespace {

constexpr uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

[[nodiscard]] constexpr inline uint32_t ch(uint32_t x, uint32_t y,
                                           uint32_t z) noexcept {
  return (x & y) ^ (~x & z);
}

[[nodiscard]] constexpr inline uint32_t maj(uint32_t x, uint32_t y,
                                            uint32_t z) noexcept {
  return (x & y) ^ (x & z) ^ (y & z);
}

[[nodiscard]] constexpr inline uint32_t sigma0(uint32_t x) noexcept {
  return std::rotr(x, 2) ^ std::rotr(x, 13) ^ std::rotr(x, 22);
}

[[nodiscard]] constexpr inline uint32_t sigma1(uint32_t x) noexcept {
  return std::rotr(x, 6) ^ std::rotr(x, 11) ^ std::rotr(x, 25);
}

[[nodiscard]] constexpr inline uint32_t gamma0(uint32_t x) noexcept {
  return std::rotr(x, 7) ^ std::rotr(x, 18) ^ (x >> 3);
}

[[nodiscard]] constexpr inline uint32_t gamma1(uint32_t x) noexcept {
  return std::rotr(x, 17) ^ std::rotr(x, 19) ^ (x >> 10);
}

class Sha256Context {
 public:
  Sha256Context() noexcept { reset(); }

  void reset() noexcept {
    state_[0] = 0x6a09e667;
    state_[1] = 0xbb67ae85;
    state_[2] = 0x3c6ef372;
    state_[3] = 0xa54ff53a;
    state_[4] = 0x510e527f;
    state_[5] = 0x9b05688c;
    state_[6] = 0x1f83d9ab;
    state_[7] = 0x5be0cd19;
    count_ = 0;
    buffer_len_ = 0;
  }

  void update(const uint8_t* data, size_t len) noexcept {
    if (len == 0 || data == nullptr) {
      return;
    }
    count_ += len;
    size_t offset = 0;

    if (buffer_len_ > 0) {
      size_t fill = 64 - buffer_len_;
      if (len >= fill) {
        std::memcpy(buffer_.data() + buffer_len_, data, fill);
        transform(buffer_.data());
        buffer_len_ = 0;
        offset = fill;
      } else {
        std::memcpy(buffer_.data() + buffer_len_, data, len);
        buffer_len_ += len;
        return;
      }
    }

    while (offset + 64 <= len) {
      transform(data + offset);
      offset += 64;
    }

    if (offset < len) {
      buffer_len_ = len - offset;
      std::memcpy(buffer_.data(), data + offset, buffer_len_);
    }
  }

  [[nodiscard]] std::string finalize_hex() noexcept {
    uint64_t total_bits = count_ * 8;

    buffer_[buffer_len_++] = 0x80;
    if (buffer_len_ > 56) {
      std::memset(buffer_.data() + buffer_len_, 0, 64 - buffer_len_);
      transform(buffer_.data());
      buffer_len_ = 0;
    }

    std::memset(buffer_.data() + buffer_len_, 0, 56 - buffer_len_);

    for (int i = 7; i >= 0; --i) {
      buffer_[56 + static_cast<size_t>(i)] =
          static_cast<uint8_t>(total_bits & 0xff);
      total_bits >>= 8;
    }
    transform(buffer_.data());

    static constexpr char hex_digits[] = "0123456789abcdef";
    std::string hex_str;
    hex_str.reserve(64);

    for (size_t i = 0; i < 8; ++i) {
      uint32_t w = state_[i];
      for (int shift = 28; shift >= 0; shift -= 4) {
        hex_str.push_back(
            hex_digits[(w >> static_cast<unsigned>(shift)) & 0xf]);
      }
    }

    return hex_str;
  }

 private:
  void transform(const uint8_t* block) noexcept {
    uint32_t w[64];
    for (size_t i = 0; i < 16; ++i) {
      w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
             (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
             (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
             (static_cast<uint32_t>(block[i * 4 + 3]));
    }
    for (size_t i = 16; i < 64; ++i) {
      w[i] = gamma1(w[i - 2]) + w[i - 7] + gamma0(w[i - 15]) + w[i - 16];
    }

    uint32_t a = state_[0];
    uint32_t b = state_[1];
    uint32_t c = state_[2];
    uint32_t d = state_[3];
    uint32_t e = state_[4];
    uint32_t f = state_[5];
    uint32_t g = state_[6];
    uint32_t h = state_[7];

    for (size_t i = 0; i < 64; ++i) {
      uint32_t t1 = h + sigma1(e) + ch(e, f, g) + K[i] + w[i];
      uint32_t t2 = sigma0(a) + maj(a, b, c);
      h = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }

  std::array<uint32_t, 8> state_{};
  uint64_t count_{0};
  size_t buffer_len_{0};
  std::array<uint8_t, 64> buffer_{};
};

}  // namespace

std::string sha256_hash(std::span<const uint8_t> data) {
  Sha256Context ctx;
  if (!data.empty()) {
    ctx.update(data.data(), data.size());
  }
  return ctx.finalize_hex();
}

std::string sha256_hash(std::string_view data) {
  return sha256_hash(std::span<const uint8_t>(
      reinterpret_cast<const uint8_t*>(data.data()), data.size()));
}

std::string sha256_hash_file(const std::string& file_path) {
  std::ifstream file(file_path, std::ios::binary);
  if (!file.is_open()) {
    return "";
  }

  Sha256Context ctx;
  constexpr size_t kBufSize = 65536;
  std::vector<uint8_t> buffer(kBufSize);

  while (file.read(reinterpret_cast<char*>(buffer.data()),
                   static_cast<std::streamsize>(buffer.size()))) {
    ctx.update(buffer.data(), buffer.size());
  }

  std::streamsize bytes_read = file.gcount();
  if (bytes_read > 0) {
    ctx.update(buffer.data(), static_cast<size_t>(bytes_read));
  }

  if (file.bad()) {
    return "";
  }

  return ctx.finalize_hex();
}

}  // namespace virmux
