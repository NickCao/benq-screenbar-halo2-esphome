#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// Canonical payload representation and the LR1121's 9-bit PCF on-air format.
namespace halo2_protocol {
using Address = std::array<uint8_t, 4>;
constexpr Address RADIO_ADDRESS{0x9C, 0xEA, 0xBB, 0x86};
constexpr uint8_t RADIO_CHANNEL = 5;
constexpr size_t PAYLOAD_SIZE = 10;
constexpr size_t FRAME_SIZE = 13;
using Payload = std::array<uint8_t, PAYLOAD_SIZE>;
using Frame = std::array<uint8_t, FRAME_SIZE>;
constexpr size_t AIR_FRAME_BITS = FRAME_SIZE * 8 + 1;
constexpr size_t AIR_FRAME_SIZE = (AIR_FRAME_BITS + 7) / 8;
using AirFrame = std::array<uint8_t, AIR_FRAME_SIZE>;

constexpr Address air_address(const Address &address = RADIO_ADDRESS) {
  return {address[3], address[2], address[1], address[0]};
}

inline uint16_t halo_crc(uint8_t pcf, const uint8_t *payload, size_t length,
                         const Address &address = RADIO_ADDRESS) {
  uint16_t crc = 0xFFFF;
  auto feed = [&](uint8_t byte) {
    crc ^= static_cast<uint16_t>(byte) << 8U;
    for (int i = 0; i < 8; ++i)
      crc = (crc & 0x8000U) ? static_cast<uint16_t>((crc << 1U) ^ 0x1021U)
                            : static_cast<uint16_t>(crc << 1U);
  };
  for (uint8_t byte : air_address(address)) feed(byte);
  // The leading PCF bit is zero for a ten-byte payload and participates in CRC.
  crc = (crc & 0x8000U) ? static_cast<uint16_t>((crc << 1U) ^ 0x1021U)
                        : static_cast<uint16_t>(crc << 1U);
  feed(pcf);
  for (size_t i = 0; i < length; ++i) feed(payload[i]);
  return crc;
}

inline Payload make_payload(uint8_t command, bool power, bool pir, bool front, bool back,
                            uint8_t front_brightness, uint8_t back_brightness,
                            uint16_t color_temperature, uint8_t packet_options = 0x01,
                            bool auto_brightness = false) {
  const uint8_t mode = front && back ? 2 : (back ? 1 : 0);
  const uint8_t control = static_cast<uint8_t>((pir ? 0x20U : 0U) | (mode << 3U) |
                                             (auto_brightness ? 0x02U : 0U) | (power ? 1U : 0U));
  const auto high = static_cast<uint8_t>(color_temperature >> 8U);
  const auto low = static_cast<uint8_t>(color_temperature);
  return {command, control, front_brightness, high, low, back_brightness, high, low, packet_options, 0x02};
}

inline uint8_t request_pcf(uint8_t pid) { return static_cast<uint8_t>(0x50U | ((pid & 3U) << 1U)); }

inline Frame make_frame(uint8_t pcf, const Payload &payload, const Address &address = RADIO_ADDRESS) {
  Frame frame{};
  frame[0] = pcf;
  for (size_t i = 0; i < payload.size(); ++i) frame[i + 1] = payload[i];
  const uint16_t crc = halo_crc(pcf, payload.data(), payload.size(), address);
  frame[11] = static_cast<uint8_t>(crc >> 8U);
  frame[12] = static_cast<uint8_t>(crc);
  return frame;
}

inline AirFrame make_air_frame(uint8_t pcf, const Payload &payload, const Address &address) {
  const auto frame = make_frame(pcf, payload, address);
  AirFrame air{};
  for (size_t i = 0; i < frame.size(); ++i) {
    air[i] |= frame[i] >> 1U;
    air[i + 1] |= static_cast<uint8_t>(frame[i] << 7U);
  }
  return air;
}

struct HaloRxState {
  bool valid = false, power = false, pir = false, front = false, back = false;
  bool reply = false;
  uint8_t command = 0, front_brightness = 0, back_brightness = 0, pcf = 0;
  uint8_t packet_options = 0x01;
  uint16_t color_temperature = 0;
};

inline bool decode_frame(const uint8_t *raw, size_t length, HaloRxState &state,
                          const Address &address = RADIO_ADDRESS, bool allow_reply = false) {
  state = {};
  if (length != FRAME_SIZE || raw[9] > 0x01 || raw[10] != 0x02) return false;
  // Bit 0 is No-ACK: lamp replies set it. Discovery accepts requests only;
  // normal reception may decode replies for a matching status query.
  if ((raw[0] >> 3U) != PAYLOAD_SIZE || (!allow_reply && (raw[0] & 1U)) || raw[1] > 0x05) return false;
  const uint16_t crc = static_cast<uint16_t>((raw[11] << 8U) | raw[12]);
  if (halo_crc(raw[0], raw + 1, PAYLOAD_SIZE, address) != crc) return false;
  const uint8_t mode = static_cast<uint8_t>((raw[2] & 0x18U) >> 3U);
  const uint16_t temperature = static_cast<uint16_t>((raw[4] << 8U) | raw[5]);
  if (mode > 2 || raw[3] < 1 || raw[3] > 100 || raw[6] < 1 || raw[6] > 100 ||
      temperature < 2700 || temperature > 6500) return false;
  state.pcf = raw[0];
  state.reply = raw[0] & 1U;
  state.command = raw[1];
  state.power = raw[2] & 1U;
  state.pir = raw[2] & 0x20U;
  state.front = mode == 0 || mode == 2;
  state.back = mode == 1 || mode == 2;
  state.front_brightness = raw[3];
  state.back_brightness = raw[6];
  state.color_temperature = temperature;
  state.packet_options = raw[9];
  state.valid = true;
  return true;
}

inline bool decode_air_frame(const uint8_t *raw, size_t length, HaloRxState &state, const Address &address,
                             bool allow_reply = false) {
  state = {};
  if (length != AIR_FRAME_SIZE || (raw[0] & 0x80U)) return false;
  Frame frame{};
  for (size_t i = 0; i < frame.size(); ++i)
    frame[i] = static_cast<uint8_t>((raw[i] << 1U) | (raw[i + 1] >> 7U));
  return decode_frame(frame.data(), frame.size(), state, address, allow_reply);
}

// Discovery uses an alternating preamble byte as the radio sync word, leaving
// the unknown address in the FIFO. Search every bit phase: sync acquisition
// can start partway through the preamble, not necessarily on a byte boundary.
inline bool discover_address(const uint8_t *data, size_t length, Address &address, HaloRxState &state) {
  state = {};
  constexpr size_t packet_bits = 32 + AIR_FRAME_BITS;
  if (length * 8 < packet_bits) return false;
  std::array<uint8_t, 4 + AIR_FRAME_SIZE> packet{};
  for (size_t start = 0; start + packet_bits <= length * 8; ++start) {
    for (size_t i = 0; i < packet.size(); ++i) {
      const size_t bit = start + i * 8;
      const unsigned shift = bit % 8;
      packet[i] = static_cast<uint8_t>(data[bit / 8] << shift);
      if (shift && bit / 8 + 1 < length) packet[i] |= data[bit / 8 + 1] >> (8 - shift);
    }
    const Address candidate{packet[3], packet[2], packet[1], packet[0]};
    if (!decode_air_frame(packet.data() + 4, AIR_FRAME_SIZE, state, candidate)) continue;
    address = candidate;
    return true;
  }
  state = {};
  return false;
}
}  // namespace halo2_protocol
