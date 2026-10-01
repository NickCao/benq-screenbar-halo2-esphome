#include <algorithm>
#include <iostream>
#include "check.h"
#include "favorite_captures.h"

using namespace esphome::halo2;
using namespace halo2_test;
namespace protocol = halo2_protocol;

static void captured_favorite_requests_decode_as_settings_snapshots() {
  for (const auto &capture : FAVORITE_CAPTURES) {
    protocol::ReceivedPacket packet;
    CHECK(protocol::decode_air_frame(capture.frame.data(), capture.frame.size(), packet, FAVORITE_ADDRESS));
    CHECK(packet.command == capture.command);
    CHECK(packet.pcf == capture.pcf);
    CHECK(!packet.is_reply());
    CHECK(packet.state == favorite_state(capture));
  }
}

static void favorite_requests_remain_crc_and_address_checked() {
  for (const auto &capture : FAVORITE_CAPTURES) {
    protocol::ReceivedPacket packet;
    auto corrupted = capture.frame;
    corrupted[4] ^= 0x01;
    CHECK(!protocol::decode_air_frame(corrupted.data(), corrupted.size(), packet, FAVORITE_ADDRESS));
    auto wrong_address = FAVORITE_ADDRESS;
    wrong_address[0] ^= 0x01;
    CHECK(!protocol::decode_air_frame(capture.frame.data(), capture.frame.size(), packet, wrong_address));
  }
}

static void favorite_frames_keep_ordinary_payload_and_framing_validation() {
  for (const auto &capture : FAVORITE_CAPTURES) {
    auto payload = protocol::make_payload(capture.command, favorite_state(capture));
    payload.control.favorite = 1;
    const auto rejected = [&](auto change) {
      auto invalid = payload;
      change(invalid);
      // Give the invalid fields a valid CRC to isolate payload validation.
      const auto air = protocol::make_air_frame(capture.pcf, invalid, FAVORITE_ADDRESS);
      protocol::ReceivedPacket packet;
      CHECK(!protocol::decode_air_frame(air.data(), air.size(), packet, FAVORITE_ADDRESS));
    };
    rejected([](auto &p) { p.front_brightness = 0; });
    rejected([](auto &p) { p.back_brightness = 101; });
    rejected([](auto &p) { p.front_temperature_be = esphome::convert_big_endian(uint16_t{6501}); });
    rejected([](auto &p) { p.control.mode = 3; });
    rejected([](auto &p) { p.ultrasonic_timeout = static_cast<UltrasonicTimeout>(4); });
    rejected([](auto &p) { p.suffix = 0x03; });

    protocol::ReceivedPacket packet;
    CHECK(!protocol::decode_air_frame(capture.frame.data(), capture.frame.size() - 1, packet, FAVORITE_ADDRESS));
    auto wrong_pcf = capture.frame;
    wrong_pcf[0] |= 0x80;
    CHECK(!protocol::decode_air_frame(wrong_pcf.data(), wrong_pcf.size(), packet, FAVORITE_ADDRESS));
  }
}

static void legacy_commands_stay_accepted_and_unknown_commands_stay_rejected() {
  const auto state = favorite_state(FAVORITE_CAPTURES[0]);
  for (uint8_t command : {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x09, 0x0A, 0x0B, 0xFF}) {
    const auto air = protocol::make_air_frame(protocol::request_pcf(0), protocol::make_payload(command, state),
                                             FAVORITE_ADDRESS);
    protocol::ReceivedPacket packet;
    CHECK(protocol::decode_air_frame(air.data(), air.size(), packet, FAVORITE_ADDRESS) == (command <= 0x05));
  }
}

static void favorite_replies_are_decoded_only_when_allowed() {
  for (const auto &capture : FAVORITE_CAPTURES) {
    const uint8_t pcf = capture.pcf | protocol::PCF_NO_ACK;
    auto payload = protocol::make_payload(capture.command, favorite_state(capture));
    payload.control.favorite = 1;
    const auto air = protocol::make_air_frame(pcf, payload, FAVORITE_ADDRESS);
    protocol::ReceivedPacket packet;
    CHECK(!protocol::decode_air_frame(air.data(), air.size(), packet, FAVORITE_ADDRESS));
    CHECK(protocol::decode_air_frame(air.data(), air.size(), packet, FAVORITE_ADDRESS, true));
    CHECK(packet.command == capture.command);
    CHECK(packet.pcf == pcf);
    CHECK(packet.is_reply());
    CHECK(packet.state == favorite_state(capture));
  }
}

static void captured_favorite_requests_can_discover_their_address() {
  for (const auto &capture : FAVORITE_CAPTURES) {
    const auto address = protocol::air_address(FAVORITE_ADDRESS);
    std::array<uint8_t, address.size() + protocol::AIR_FRAME_SIZE> bytes{};
    std::copy(address.begin(), address.end(), bytes.begin());
    std::copy(capture.frame.begin(), capture.frame.end(), bytes.begin() + address.size());
    protocol::Address discovered;
    protocol::ReceivedPacket packet;
    CHECK(protocol::discover_address(bytes.data(), bytes.size(), discovered, packet));
    CHECK(discovered == FAVORITE_ADDRESS);
    CHECK(packet.command == capture.command);
    CHECK(packet.state == favorite_state(capture));
  }
}

int main() {
  scenario = "captured favorite requests";
  captured_favorite_requests_decode_as_settings_snapshots();
  scenario = "favorite CRC and address validation";
  favorite_requests_remain_crc_and_address_checked();
  scenario = "favorite payload and framing validation";
  favorite_frames_keep_ordinary_payload_and_framing_validation();
  scenario = "legacy and unknown commands";
  legacy_commands_stay_accepted_and_unknown_commands_stay_rejected();
  scenario = "favorite reply direction";
  favorite_replies_are_decoded_only_when_allowed();
  scenario = "favorite address discovery";
  captured_favorite_requests_can_discover_their_address();
  std::cout << "Halo2 protocol tests passed\n";
}
