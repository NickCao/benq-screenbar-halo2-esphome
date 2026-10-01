#pragma once

#include <algorithm>
#include <cmath>
#include <source_location>
#include <string_view>
#include "fake_lr1121.h"
#include "halo2.h"

namespace halo2_test {
using namespace esphome::halo2;

inline LampState baseline(bool power = true) {
  LampState state;
  state.power = power;
  state.front_brightness = 37;
  state.back_brightness = 62;
  state.color_temperature = 4500;
  return state;
}

class BridgeFixture {
 public:
  static constexpr protocol::Address ADDRESS{0x12, 0x34, 0x56, 0x78};

  explicit BridgeFixture(bool discover = false, const protocol::Address &link = ADDRESS) {
    time_us = 0;
    esphome::test_preferences.values.clear();
    reset.write = [this](bool high) {
      if (!high) chip.reset();
    };
    irq.read = [this]() { return chip.irq(); };
    auto &radio = bridge.get_radio();
    radio.set_spi_parent(&chip);
    radio.set_reset_pin(&reset);
    radio.set_busy_pin(&busy);
    radio.set_irq_pin(&irq);
    bridge.set_ultrasonic_select(&ultrasonic);
    bridge.set_radio_status(&status);
    bridge.set_radio_address_sensor(&address);
    bridge.set_auto_discover(discover);
    if (!discover) bridge.set_radio_address(link);
    front.setup();
    back.setup();
    bridge.setup();
    wait_until([&]() { return chip.receiving && radio.ready(); });
  }

  void step() {
    time_us += 1000;
    chip.tick();
    bridge.run_timeouts();
    front.loop();
    back.loop();
    bridge.loop();
  }
  void advance(uint32_t ms) {
    const int64_t until = time_us + int64_t(ms) * 1000;
    while (time_us < until) step();
  }
  template<typename Predicate>
  void wait_until(Predicate condition, uint32_t limit_ms = 1500,
                  std::source_location where = std::source_location::current()) {
    const int64_t deadline = time_us + int64_t(limit_ms) * 1000;
    while (!condition() && time_us < deadline) step();
    check(condition(), "wait_until timed out", where.file_name(), where.line());
  }

  FakeLR1121::Transmission next_tx(Command command) {
    const size_t count = chip.transmissions.size();
    wait_until([&]() { return chip.transmissions.size() > count; });
    CHECK(chip.transmissions.size() == count + 1);
    const auto tx = chip.transmissions.back();
    CHECK(tx.packet().command == command);
    return tx;
  }
  void reply(const LampState &state, uint8_t command = Command::STATUS, uint8_t pid_xor = 0, bool favorite = false) {
    const auto &tx = chip.transmissions.back();
    const uint8_t pcf = tx.packet().pcf ^ pid_xor;
    auto payload = protocol::make_payload(command, state);
    payload.control.favorite = favorite;
    chip.finish_tx();
    chip.receive(protocol::make_air_frame(pcf | protocol::PCF_NO_ACK, payload, tx.address));
  }
  void finish_command() {
    chip.finish_tx();
    wait_until([&]() { return bridge.get_radio().idle(); });
    CHECK(status.state == "Command sent");
  }
  void receive(const LampState &state, bool reply = false) {
    const uint8_t pcf = protocol::request_pcf(0) | (reply ? protocol::PCF_NO_ACK : 0);
    receive(protocol::make_air_frame(pcf, protocol::make_payload(Command::STATUS, state), chip.address));
  }
  void receive(const protocol::AirFrame &frame) {
    wait_until([&]() { return chip.receiving && chip.rx_length == protocol::AIR_FRAME_SIZE; });
    const uint32_t count = bridge.get_radio().rx_count();
    chip.receive(frame);
    wait_until([&]() { return bridge.get_radio().rx_count() > count; });
  }
  void discover(const protocol::Address &link, const LampState &state) {
    wait_until([&]() { return chip.receiving && chip.rx_length == LR1121Radio::DISCOVERY_RX_BYTES; });
    std::array<uint8_t, LR1121Radio::DISCOVERY_RX_BYTES> bytes{};
    const auto air_address = protocol::air_address(link);
    const auto frame = protocol::make_air_frame(protocol::request_pcf(1), protocol::make_payload(Command::SETTINGS, state),
                                               link);
    std::copy(air_address.begin(), air_address.end(), bytes.begin());
    std::copy(frame.begin(), frame.end(), bytes.begin() + air_address.size());
    const uint32_t count = bridge.get_radio().capture_count();
    chip.receive(bytes);
    wait_until([&]() { return bridge.get_radio().capture_count() > count; });
  }
  void establish_baseline(const LampState &state) {
    CHECK(!bridge.accepts_commands());
    next_tx(Command::STATUS);
    reply(baseline(!state.power));
    next_tx(Command::STATUS);
    CHECK(!bridge.accepts_commands());
    reply(state);
    wait_until([&]() { return bridge.accepts_commands(); });
    expect_lights(state);
  }
  void readback(const LampState &state) {
    next_tx(Command::STATUS);
    reply(baseline(!state.power));
    next_tx(Command::STATUS);
    const uint32_t count = bridge.get_radio().rx_count();
    reply(state);
    wait_until([&]() { return bridge.get_radio().rx_count() > count; });
    CHECK(status.state == "Lamp status received");
  }
  void expect_lights(const LampState &state) const {
    const auto check_light = [&](const esphome::light::LightState &light, Section section) {
      CHECK(light.remote_values.is_on() == state.is_on(section));
      CHECK(std::abs(light.remote_values.get_brightness() - state.brightness(section) / 100.0f) < 0.0001f);
      CHECK(std::abs(light.remote_values.get_color_temperature() - 1000000.0f / state.color_temperature) < 0.001f);
    };
    check_light(front, Section::FRONT);
    check_light(back, Section::BACK);
  }
  size_t status_count(std::string_view message) const {
    return std::count_if(status.history.begin(), status.history.end(),
                         [&](const auto &entry) { return entry.second == message; });
  }

  FakeLR1121 chip;
  esphome::InternalGPIOPin reset, busy, irq;
  Halo2 bridge;
  Halo2Light front_output{&bridge, Section::FRONT}, back_output{&bridge, Section::BACK};
  esphome::light::LightState front{&front_output}, back{&back_output};
  Halo2UltrasonicSelect ultrasonic{&bridge};
  esphome::text_sensor::TextSensor status, address;
};
}  // namespace halo2_test
