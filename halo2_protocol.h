#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// The byte format is shared by the BM5602 direct transmitter and LR1121 FIFO.
namespace halo2_protocol {
constexpr std::array<uint8_t, 4> RADIO_ADDRESS{0x9C, 0xEA, 0xBB, 0x86};
constexpr uint8_t RADIO_CHANNEL = 5;
constexpr size_t PAYLOAD_SIZE = 10;
constexpr size_t FRAME_SIZE = 13;
using Payload = std::array<uint8_t, PAYLOAD_SIZE>;
using Frame = std::array<uint8_t, FRAME_SIZE>;

constexpr std::array<uint8_t, 4> air_address() {
  return {RADIO_ADDRESS[3], RADIO_ADDRESS[2], RADIO_ADDRESS[1], RADIO_ADDRESS[0]};
}

inline uint16_t halo_crc(uint8_t pcf, const uint8_t *payload, size_t length) {
  uint16_t crc = 0xEFDF;
  auto feed = [&](uint8_t byte) {
    crc ^= static_cast<uint16_t>(byte) << 8U;
    for (int i = 0; i < 8; ++i)
      crc = (crc & 0x8000U) ? static_cast<uint16_t>((crc << 1U) ^ 0x1021U)
                            : static_cast<uint16_t>(crc << 1U);
  };
  for (uint8_t byte : air_address()) feed(byte);
  feed(pcf);
  for (size_t i = 0; i < length; ++i) feed(payload[i]);
  return crc;
}

inline Payload make_payload(uint8_t command, bool power, bool pir, bool front, bool back,
                            uint8_t front_brightness, uint8_t back_brightness,
                            uint16_t color_temperature) {
  const uint8_t mode = front && back ? 2 : (back ? 1 : 0);
  const uint8_t control = static_cast<uint8_t>((pir ? 0x20U : 0U) | (mode << 3U) | (power ? 1U : 0U));
  const auto high = static_cast<uint8_t>(color_temperature >> 8U);
  const auto low = static_cast<uint8_t>(color_temperature);
  return {command, control, front_brightness, high, low, back_brightness, high, low, 0x01, 0x02};
}

inline uint8_t request_pcf(uint8_t pid) { return static_cast<uint8_t>(0x50U | ((pid & 3U) << 1U)); }

inline Frame make_frame(uint8_t pcf, const Payload &payload) {
  Frame frame{};
  frame[0] = pcf;
  for (size_t i = 0; i < payload.size(); ++i) frame[i + 1] = payload[i];
  const uint16_t crc = halo_crc(pcf, payload.data(), payload.size());
  frame[11] = static_cast<uint8_t>(crc >> 8U);
  frame[12] = static_cast<uint8_t>(crc);
  return frame;
}

struct HaloRxState {
  bool valid = false, power = false, pir = false, front = false, back = false;
  uint8_t command = 0, front_brightness = 0, back_brightness = 0, pcf = 0;
  uint16_t color_temperature = 0;
};

inline bool decode_frame(const uint8_t *raw, size_t length, HaloRxState &state) {
  state = {};
  if (length != FRAME_SIZE || raw[9] != 0x01 || raw[10] != 0x02) return false;
  // Odd PCF bit 0 identifies lamp replies, whose control byte is not state.
  if ((raw[0] >> 3U) != PAYLOAD_SIZE || (raw[0] & 1U) || raw[1] > 0x05) return false;
  const uint16_t crc = static_cast<uint16_t>((raw[11] << 8U) | raw[12]);
  if (halo_crc(raw[0], raw + 1, PAYLOAD_SIZE) != crc) return false;
  const uint8_t mode = static_cast<uint8_t>((raw[2] & 0x18U) >> 3U);
  const uint16_t temperature = static_cast<uint16_t>((raw[4] << 8U) | raw[5]);
  if (mode > 2 || raw[3] < 1 || raw[3] > 100 || raw[6] < 1 || raw[6] > 100 ||
      temperature < 2700 || temperature > 6500) return false;
  state.pcf = raw[0];
  state.command = raw[1];
  state.power = raw[2] & 1U;
  state.pir = raw[2] & 0x20U;
  state.front = mode == 0 || mode == 2;
  state.back = mode == 1 || mode == 2;
  state.front_brightness = raw[3];
  state.back_brightness = raw[6];
  state.color_temperature = temperature;
  state.valid = true;
  return true;
}
}  // namespace halo2_protocol
