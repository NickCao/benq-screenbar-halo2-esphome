#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>

namespace esphome {
using std::bit_cast;

template<typename T> class Parented {
 public:
  explicit Parented(T *parent) : parent_(parent) {}

 protected:
  T *parent_;
};

inline uint16_t convert_big_endian(uint16_t value) {
  if constexpr (std::endian::native == std::endian::little)
    return static_cast<uint16_t>((value << 8U) | (value >> 8U));
  return value;
}

// Bitwise equivalent of ESPHome's non-reflected crc16be helper.
inline uint16_t crc16be(const uint8_t *data, size_t length, uint16_t crc, uint16_t polynomial) {
  for (size_t i = 0; i < length; ++i) {
    crc ^= uint16_t(data[i]) << 8U;
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = static_cast<uint16_t>((crc << 1U) ^ ((crc & 0x8000U) ? polynomial : 0));
  }
  return crc;
}

inline std::string format_hex_pretty(const uint8_t *, size_t) { return {}; }
}  // namespace esphome
