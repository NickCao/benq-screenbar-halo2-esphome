#pragma once

#include <array>
#include "halo2_protocol.h"

namespace halo2_test {
// Original-controller packets from the two 2026-09-30 save/recall cycles.
// See docs/PROTOCOL.md for the action sequence and independent CRC checks.
// Preserve the captured trailing packing bits, including nonzero values.
inline constexpr halo2_protocol::Address FAVORITE_ADDRESS{0x4F, 0x23, 0x8C, 0xCE};

struct FavoriteCapture {
  halo2_protocol::AirFrame frame;
  uint8_t command, pcf, brightness;
};

inline constexpr std::array<FavoriteCapture, 4> FAVORITE_CAPTURES{{
    {{0x2A, 0x04, 0x1A, 0x8C, 0x85, 0x46, 0x0C, 0x85, 0x46, 0x00, 0x81, 0x08, 0x57, 0x75}, 0x08, 0x54, 25},
    {{0x29, 0x03, 0x9A, 0x8C, 0x85, 0x46, 0x0C, 0x85, 0x46, 0x00, 0x81, 0x5B, 0xF5, 0xF7}, 0x07, 0x52, 25},
    {{0x2B, 0x04, 0x1A, 0x98, 0x05, 0x46, 0x18, 0x05, 0x46, 0x00, 0x81, 0x19, 0x5A, 0x50}, 0x08, 0x56, 48},
    {{0x2A, 0x03, 0x9A, 0x98, 0x05, 0x46, 0x18, 0x05, 0x46, 0x00, 0x81, 0x62, 0x5B, 0x7C}, 0x07, 0x54, 48},
}};

inline esphome::halo2::LampState favorite_state(const FavoriteCapture &capture) {
  esphome::halo2::LampState state;
  state.power = true;
  state.selection = esphome::halo2::LampSelection::BOTH;
  state.front_brightness = state.back_brightness = capture.brightness;
  state.color_temperature = 2700;
  state.ultrasonic_enabled = true;
  state.ultrasonic_timeout = esphome::halo2::UltrasonicTimeout::MINUTES_5;
  return state;
}
}  // namespace halo2_test
