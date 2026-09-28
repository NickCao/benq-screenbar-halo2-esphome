#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// Canonical payload representation and the LR1121's 9-bit PCF on-air format.
namespace halo2_protocol {
using Address = std::array<uint8_t, 4>;
constexpr Address RADIO_ADDRESS{0x9C, 0xEA, 0xBB, 0x86};
constexpr std::array<uint8_t, 3> RADIO_CHANNELS{5, 46, 75};
constexpr uint8_t RADIO_CHANNEL = RADIO_CHANNELS[0];
constexpr unsigned RADIO_BASE_FREQUENCY_MHZ = 2400;

// The controller also sends 0x05 when going to sleep; it saves the timeout.
enum Command : uint8_t { POWER = 0x02, SETTINGS = 0x03, STATUS = 0x04, ULTRASONIC_TIMEOUT = 0x05 };
constexpr uint8_t MAX_COMMAND_CODE = 0x05;
enum LampMode : uint8_t { FRONT_ONLY = 0, BACK_ONLY = 1, BOTH = 2 };
enum UltrasonicTimeout : uint8_t { MINUTES_3 = 0, MINUTES_5 = 1, MINUTES_10 = 2, MINUTES_15 = 3 };
constexpr std::array<uint8_t, 4> ULTRASONIC_TIMEOUT_MINUTES{3, 5, 10, 15};
constexpr uint8_t CONTROL_POWER = 0x01, CONTROL_AUTO_BRIGHTNESS = 0x02, CONTROL_ULTRASONIC = 0x20;
constexpr uint8_t CONTROL_MODE_MASK = 0x18, CONTROL_MODE_SHIFT = 3;
constexpr uint8_t PCF_NO_ACK = 0x01, PCF_PID_MASK = 0x06, PCF_PID_SHIFT = 1, PCF_LENGTH_SHIFT = 3;
constexpr uint8_t PACKET_SUFFIX = 0x02;
constexpr uint8_t MIN_BRIGHTNESS_PERCENT = 1, MAX_BRIGHTNESS_PERCENT = 100;
constexpr uint16_t MIN_TEMPERATURE_K = 2700, MAX_TEMPERATURE_K = 6500, TEMPERATURE_STEP_K = 25;
constexpr uint16_t CRC_INITIAL = 0xFFFF, CRC_POLYNOMIAL = 0x1021, CRC_TOP_BIT = 0x8000;

// Byte offsets in the canonical frame (after removing the leading PCF bit).
namespace frame {
enum Field : size_t {
  PCF,
  COMMAND,
  CONTROL,
  FRONT_BRIGHTNESS,
  FRONT_TEMPERATURE_HIGH,
  FRONT_TEMPERATURE_LOW,
  BACK_BRIGHTNESS,
  BACK_TEMPERATURE_HIGH,
  BACK_TEMPERATURE_LOW,
  ULTRASONIC_TIMEOUT,
  SUFFIX,
  CRC_HIGH,
  CRC_LOW,
  SIZE
};
}  // namespace frame
constexpr size_t PAYLOAD_SIZE = 10;
constexpr size_t FRAME_SIZE = frame::SIZE;
using Payload = std::array<uint8_t, PAYLOAD_SIZE>;
using Frame = std::array<uint8_t, FRAME_SIZE>;
constexpr size_t AIR_FRAME_BITS = FRAME_SIZE * 8 + 1;
constexpr size_t AIR_FRAME_SIZE = (AIR_FRAME_BITS + 7) / 8;
using AirFrame = std::array<uint8_t, AIR_FRAME_SIZE>;

constexpr Address air_address(const Address &address = RADIO_ADDRESS) {
  return {address[3], address[2], address[1], address[0]};
}

inline uint16_t halo_crc(uint8_t pcf, const uint8_t *payload, size_t length, const Address &address = RADIO_ADDRESS) {
  uint16_t crc = CRC_INITIAL;
  auto feed = [&](uint8_t byte) {
    crc ^= static_cast<uint16_t>(byte) << 8U;
    for (int i = 0; i < 8; ++i)
      crc =
          (crc & CRC_TOP_BIT) ? static_cast<uint16_t>((crc << 1U) ^ CRC_POLYNOMIAL) : static_cast<uint16_t>(crc << 1U);
  };
  for (uint8_t byte : air_address(address)) feed(byte);
  // The leading PCF bit is zero for a ten-byte payload and participates in CRC.
  crc = (crc & CRC_TOP_BIT) ? static_cast<uint16_t>((crc << 1U) ^ CRC_POLYNOMIAL) : static_cast<uint16_t>(crc << 1U);
  feed(pcf);
  for (size_t i = 0; i < length; ++i) feed(payload[i]);
  return crc;
}

inline Payload make_payload(uint8_t command, bool power, bool pir, bool front, bool back, uint8_t front_brightness,
                            uint8_t back_brightness, uint16_t color_temperature,
                            UltrasonicTimeout ultrasonic_timeout = UltrasonicTimeout::MINUTES_5,
                            bool auto_brightness = false) {
  const LampMode mode = front && back ? LampMode::BOTH : (back ? LampMode::BACK_ONLY : LampMode::FRONT_ONLY);
  const uint8_t control =
      static_cast<uint8_t>((pir ? CONTROL_ULTRASONIC : 0U) | (mode << CONTROL_MODE_SHIFT) |
                           (auto_brightness ? CONTROL_AUTO_BRIGHTNESS : 0U) | (power ? CONTROL_POWER : 0U));
  const auto high = static_cast<uint8_t>(color_temperature >> 8U);
  const auto low = static_cast<uint8_t>(color_temperature);
  return {command, control, front_brightness, high, low, back_brightness, high, low, ultrasonic_timeout, PACKET_SUFFIX};
}

inline uint8_t request_pcf(uint8_t pid) {
  return static_cast<uint8_t>((PAYLOAD_SIZE << PCF_LENGTH_SHIFT) | ((pid << PCF_PID_SHIFT) & PCF_PID_MASK));
}

inline Frame make_frame(uint8_t pcf, const Payload &payload, const Address &address = RADIO_ADDRESS) {
  Frame frame{};
  frame[frame::PCF] = pcf;
  for (size_t i = 0; i < payload.size(); ++i) frame[i + frame::COMMAND] = payload[i];
  const uint16_t crc = halo_crc(pcf, payload.data(), payload.size(), address);
  frame[frame::CRC_HIGH] = static_cast<uint8_t>(crc >> 8U);
  frame[frame::CRC_LOW] = static_cast<uint8_t>(crc);
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
  UltrasonicTimeout ultrasonic_timeout = UltrasonicTimeout::MINUTES_5;
  uint16_t color_temperature = 0;
};

inline bool decode_frame(const uint8_t *raw, size_t length, HaloRxState &state, const Address &address = RADIO_ADDRESS,
                         bool allow_reply = false) {
  state = {};
  if (length != FRAME_SIZE || raw[frame::ULTRASONIC_TIMEOUT] > UltrasonicTimeout::MINUTES_15 ||
      raw[frame::SUFFIX] != PACKET_SUFFIX)
    return false;
  // Bit 0 is No-ACK: lamp replies set it. Discovery accepts requests only;
  // normal reception may decode replies for a matching status query.
  if ((raw[frame::PCF] >> PCF_LENGTH_SHIFT) != PAYLOAD_SIZE || (!allow_reply && (raw[frame::PCF] & PCF_NO_ACK)) ||
      raw[frame::COMMAND] > MAX_COMMAND_CODE)
    return false;
  const uint16_t crc = static_cast<uint16_t>((raw[frame::CRC_HIGH] << 8U) | raw[frame::CRC_LOW]);
  if (halo_crc(raw[frame::PCF], raw + frame::COMMAND, PAYLOAD_SIZE, address) != crc) return false;
  const uint8_t mode = (raw[frame::CONTROL] & CONTROL_MODE_MASK) >> CONTROL_MODE_SHIFT;
  const uint16_t temperature =
      static_cast<uint16_t>((raw[frame::FRONT_TEMPERATURE_HIGH] << 8U) | raw[frame::FRONT_TEMPERATURE_LOW]);
  if (mode > LampMode::BOTH || raw[frame::FRONT_BRIGHTNESS] < MIN_BRIGHTNESS_PERCENT ||
      raw[frame::FRONT_BRIGHTNESS] > MAX_BRIGHTNESS_PERCENT || raw[frame::BACK_BRIGHTNESS] < MIN_BRIGHTNESS_PERCENT ||
      raw[frame::BACK_BRIGHTNESS] > MAX_BRIGHTNESS_PERCENT || temperature < MIN_TEMPERATURE_K ||
      temperature > MAX_TEMPERATURE_K)
    return false;
  state.pcf = raw[frame::PCF];
  state.reply = raw[frame::PCF] & PCF_NO_ACK;
  state.command = raw[frame::COMMAND];
  state.power = raw[frame::CONTROL] & CONTROL_POWER;
  state.pir = raw[frame::CONTROL] & CONTROL_ULTRASONIC;
  state.front = mode == LampMode::FRONT_ONLY || mode == LampMode::BOTH;
  state.back = mode == LampMode::BACK_ONLY || mode == LampMode::BOTH;
  state.front_brightness = raw[frame::FRONT_BRIGHTNESS];
  state.back_brightness = raw[frame::BACK_BRIGHTNESS];
  state.color_temperature = temperature;
  state.ultrasonic_timeout = static_cast<UltrasonicTimeout>(raw[frame::ULTRASONIC_TIMEOUT]);
  state.valid = true;
  return true;
}

inline bool decode_air_frame(const uint8_t *raw, size_t length, HaloRxState &state, const Address &address,
                             bool allow_reply = false) {
  state = {};
  if (length != AIR_FRAME_SIZE || (raw[0] & 0x80U)) return false;
  Frame frame{};
  for (size_t i = 0; i < frame.size(); ++i) frame[i] = static_cast<uint8_t>((raw[i] << 1U) | (raw[i + 1] >> 7U));
  return decode_frame(frame.data(), frame.size(), state, address, allow_reply);
}

// Discovery uses an alternating preamble byte as the radio sync word, leaving
// the unknown address in the FIFO. Search every bit phase: sync acquisition
// can start partway through the preamble, not necessarily on a byte boundary.
inline bool discover_address(const uint8_t *data, size_t length, Address &address, HaloRxState &state) {
  state = {};
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
    if (!decode_air_frame(packet.data() + candidate.size(), AIR_FRAME_SIZE, state, candidate)) continue;
    address = candidate;
    return true;
  }
  state = {};
  return false;
}
}  // namespace halo2_protocol
