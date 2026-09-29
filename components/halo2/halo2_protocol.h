#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "esphome/core/helpers.h"
#include "lamp_state.h"

// Canonical payload representation and the LR1121's 9-bit PCF on-air format.
namespace halo2_protocol {
using esphome::halo2::LampSelection;
using esphome::halo2::LampState;
using esphome::halo2::UltrasonicTimeout;
using Address = std::array<uint8_t, 4>;
constexpr Address RADIO_ADDRESS{0x9C, 0xEA, 0xBB, 0x86};
constexpr std::array<uint8_t, 3> RADIO_CHANNELS{5, 46, 75};
constexpr uint8_t RADIO_CHANNEL = RADIO_CHANNELS[0];
constexpr unsigned RADIO_BASE_FREQUENCY_MHZ = 2400;

// The controller also sends 0x05 when going to sleep; it saves the timeout.
enum Command : uint8_t { POWER = 0x02, SETTINGS = 0x03, STATUS = 0x04, ULTRASONIC_TIMEOUT = 0x05 };
constexpr uint8_t MAX_COMMAND_CODE = 0x05;
constexpr uint8_t PCF_NO_ACK = 0x01, PCF_PID_MASK = 0x06, PCF_PID_SHIFT = 1, PCF_LENGTH_SHIFT = 3;
constexpr uint8_t PACKET_SUFFIX = 0x02;
constexpr uint16_t CRC_INITIAL = 0xFFFF, CRC_POLYNOMIAL = 0x1021, CRC_TOP_BIT = 0x8000;

// Bit-fields follow the ESP32/GCC layout: least significant bit first.
struct Control {
  uint8_t power : 1;
  uint8_t auto_brightness : 1;
  uint8_t favorite : 1;
  uint8_t mode : 2;
  uint8_t ultrasonic : 1;
  uint8_t reserved : 2;
} __attribute__((packed));

struct Payload {
  uint8_t command;
  Control control;
  uint8_t front_brightness;
  uint16_t front_temperature_be;
  uint8_t back_brightness;
  uint16_t back_temperature_be;
  UltrasonicTimeout ultrasonic_timeout;
  uint8_t suffix;
} __attribute__((packed));

// Canonical frame after removing the leading bit of the 9-bit PCF.
struct Frame {
  uint8_t pcf;
  Payload payload;
  uint16_t crc_be;
} __attribute__((packed));

static_assert(sizeof(Control) == 1);
static_assert(sizeof(Payload) == 10);
static_assert(sizeof(Frame) == 13);
constexpr size_t PAYLOAD_SIZE = sizeof(Payload);
constexpr size_t FRAME_SIZE = sizeof(Frame);
using FrameBytes = std::array<uint8_t, FRAME_SIZE>;
constexpr size_t AIR_FRAME_BITS = FRAME_SIZE * 8 + 1;
constexpr size_t AIR_FRAME_SIZE = (AIR_FRAME_BITS + 7) / 8;
using AirFrame = std::array<uint8_t, AIR_FRAME_SIZE>;

enum class PacketDirection : uint8_t { REQUEST, REPLY };

struct ReceivedPacket {
  LampState state;
  uint8_t command{0}, pcf{0};
  PacketDirection direction{PacketDirection::REQUEST};

  bool is_reply() const { return direction == PacketDirection::REPLY; }
};

constexpr Address air_address(const Address &address = RADIO_ADDRESS) {
  return {address[3], address[2], address[1], address[0]};
}

inline uint16_t halo_crc(uint8_t pcf, const Payload &payload, const Address &address = RADIO_ADDRESS) {
  const auto address_bytes = air_address(address);
  uint16_t crc = esphome::crc16be(address_bytes.data(), address_bytes.size(), CRC_INITIAL, CRC_POLYNOMIAL);
  // The leading PCF bit is zero for a ten-byte payload and participates in CRC.
  crc = (crc & CRC_TOP_BIT) ? static_cast<uint16_t>((crc << 1U) ^ CRC_POLYNOMIAL) : static_cast<uint16_t>(crc << 1U);
  crc = esphome::crc16be(&pcf, sizeof(pcf), crc, CRC_POLYNOMIAL);
  const auto payload_bytes = esphome::bit_cast<std::array<uint8_t, PAYLOAD_SIZE>>(payload);
  return esphome::crc16be(payload_bytes.data(), payload_bytes.size(), crc, CRC_POLYNOMIAL);
}

inline Payload make_payload(uint8_t command, const LampState &state, bool auto_brightness = false) {
  Payload payload{};
  payload.command = command;
  payload.control.power = state.power;
  payload.control.auto_brightness = auto_brightness;
  payload.control.mode = static_cast<uint8_t>(state.selection);
  payload.control.ultrasonic = state.ultrasonic_enabled;
  payload.front_brightness = state.front_brightness;
  payload.back_brightness = state.back_brightness;
  payload.front_temperature_be = payload.back_temperature_be = esphome::convert_big_endian(state.color_temperature);
  payload.ultrasonic_timeout = state.ultrasonic_timeout;
  payload.suffix = PACKET_SUFFIX;
  return payload;
}

inline uint8_t request_pcf(uint8_t pid) {
  return static_cast<uint8_t>((PAYLOAD_SIZE << PCF_LENGTH_SHIFT) | ((pid << PCF_PID_SHIFT) & PCF_PID_MASK));
}

inline Frame make_frame(uint8_t pcf, const Payload &payload, const Address &address = RADIO_ADDRESS) {
  return {pcf, payload, esphome::convert_big_endian(halo_crc(pcf, payload, address))};
}

inline AirFrame make_air_frame(uint8_t pcf, const Payload &payload, const Address &address) {
  const auto frame = esphome::bit_cast<FrameBytes>(make_frame(pcf, payload, address));
  AirFrame air{};
  for (size_t i = 0; i < frame.size(); ++i) {
    air[i] |= frame[i] >> 1U;
    air[i + 1] |= static_cast<uint8_t>(frame[i] << 7U);
  }
  return air;
}

inline bool decode_frame(const uint8_t *raw, size_t length, ReceivedPacket &packet,
                         const Address &address = RADIO_ADDRESS, bool allow_reply = false) {
  packet = {};
  if (length != FRAME_SIZE) return false;
  Frame frame;
  std::memcpy(&frame, raw, sizeof(frame));
  const auto &payload = frame.payload;
  if (payload.ultrasonic_timeout > UltrasonicTimeout::MINUTES_15 || payload.suffix != PACKET_SUFFIX) return false;
  // Bit 0 is No-ACK: lamp replies set it. Discovery accepts requests only;
  // normal reception may decode replies for a matching status query.
  if ((frame.pcf >> PCF_LENGTH_SHIFT) != PAYLOAD_SIZE || (!allow_reply && (frame.pcf & PCF_NO_ACK)) ||
      payload.command > MAX_COMMAND_CODE)
    return false;
  if (halo_crc(frame.pcf, payload, address) != esphome::convert_big_endian(frame.crc_be)) return false;
  const auto selection = static_cast<LampSelection>(payload.control.mode);
  const uint16_t temperature = esphome::convert_big_endian(payload.front_temperature_be);
  if (selection > LampSelection::BOTH || payload.front_brightness < esphome::halo2::MIN_BRIGHTNESS_PERCENT ||
      payload.front_brightness > esphome::halo2::MAX_BRIGHTNESS_PERCENT ||
      payload.back_brightness < esphome::halo2::MIN_BRIGHTNESS_PERCENT ||
      payload.back_brightness > esphome::halo2::MAX_BRIGHTNESS_PERCENT ||
      temperature < esphome::halo2::MIN_TEMPERATURE_K || temperature > esphome::halo2::MAX_TEMPERATURE_K)
    return false;
  packet.pcf = frame.pcf;
  packet.direction = frame.pcf & PCF_NO_ACK ? PacketDirection::REPLY : PacketDirection::REQUEST;
  packet.command = payload.command;
  auto &state = packet.state;
  state.power = payload.control.power;
  state.ultrasonic_enabled = payload.control.ultrasonic;
  state.selection = selection;
  state.front_brightness = payload.front_brightness;
  state.back_brightness = payload.back_brightness;
  state.color_temperature = temperature;
  state.ultrasonic_timeout = payload.ultrasonic_timeout;
  return true;
}

inline bool decode_air_frame(const uint8_t *raw, size_t length, ReceivedPacket &packet, const Address &address,
                             bool allow_reply = false) {
  packet = {};
  if (length != AIR_FRAME_SIZE || (raw[0] & 0x80U)) return false;
  FrameBytes frame{};
  for (size_t i = 0; i < frame.size(); ++i) frame[i] = static_cast<uint8_t>((raw[i] << 1U) | (raw[i + 1] >> 7U));
  return decode_frame(frame.data(), frame.size(), packet, address, allow_reply);
}

// Discovery uses an alternating preamble byte as the radio sync word, leaving
// the unknown address in the FIFO. Search every bit phase: sync acquisition
// can start partway through the preamble, not necessarily on a byte boundary.
inline bool discover_address(const uint8_t *data, size_t length, Address &address, ReceivedPacket &received) {
  received = {};
  constexpr size_t packet_bits = Address{}.size() * 8 + AIR_FRAME_BITS;
  if (length * 8 < packet_bits) return false;
  std::array<uint8_t, Address{}.size() + AIR_FRAME_SIZE> packet{};
  for (size_t start = 0; start + packet_bits <= length * 8; ++start) {
    for (size_t i = 0; i < packet.size(); ++i) {
      const size_t bit = start + i * 8;
      const unsigned shift = bit % 8;
      packet[i] = static_cast<uint8_t>(data[bit / 8] << shift);
      if (shift && bit / 8 + 1 < length) packet[i] |= data[bit / 8 + 1] >> (8 - shift);
    }
    const Address candidate{packet[3], packet[2], packet[1], packet[0]};
    if (!decode_air_frame(packet.data() + candidate.size(), AIR_FRAME_SIZE, received, candidate)) continue;
    address = candidate;
    return true;
  }
  received = {};
  return false;
}
}  // namespace halo2_protocol
