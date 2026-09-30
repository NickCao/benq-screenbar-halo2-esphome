#pragma once

#include <cstdint>

namespace halo2_test {
inline int64_t time_us = 0;
}

namespace esphome {
inline uint32_t millis() { return static_cast<uint32_t>(halo2_test::time_us / 1000); }
inline void delayMicroseconds(uint32_t us) { halo2_test::time_us += us; }
}  // namespace esphome
