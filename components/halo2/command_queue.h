#pragma once

#include <bitset>
#include <optional>
#include "lamp_state.h"

namespace esphome::halo2 {

class CommandQueue {
  enum class Phase { READY, TRANSMITTING, COOLDOWN };

 public:
  struct Batch {
    std::array<Command, 2> commands{};
    uint8_t count{1};
    bool auto_brightness{false};

    auto begin() const { return commands.begin(); }
    auto end() const { return commands.begin() + count; }
  };

  void set_debounce(uint32_t delay) { debounce_ = delay; }
  uint32_t debounce() const { return debounce_; }
  bool pending() const { return pending_.any(); }
  bool transmitting() const { return phase_ == Phase::TRANSMITTING; }
  bool idle() const { return !pending() && !transmitting(); }

  void request(const LampState &before, const LampState &after) {
    const bool light_settings = before.front_brightness != after.front_brightness ||
                                before.back_brightness != after.back_brightness ||
                                before.color_temperature != after.color_temperature;
    if (light_settings) auto_brightness_ = false;
    if (before.power != after.power) pending_.set(Command::POWER);
    if (light_settings || before.selection != after.selection || before.ultrasonic_enabled != after.ultrasonic_enabled)
      pending_.set(Command::SETTINGS);
    if (before.ultrasonic_timeout != after.ultrasonic_timeout) pending_.set(Command::ULTRASONIC_TIMEOUT);
  }

  void auto_brightness() {
    auto_brightness_ = true;
    pending_.set(Command::SETTINGS);
  }
  void resend() {
    pending_.set(Command::POWER);
    pending_.set(Command::SETTINGS);
    pending_.set(Command::ULTRASONIC_TIMEOUT);
  }
  void clear() {
    pending_.reset();
    auto_brightness_ = false;
    phase_ = Phase::READY;
  }

  // Taking a batch reserves the transmitter. Requests received during TX
  // stay pending; the fixed cooldown starts when the entire batch completes.
  std::optional<Batch> take(const LampState &state, uint32_t now) {
    if (!pending() || transmitting() || (phase_ == Phase::COOLDOWN && now - sent_at_ < debounce_)) return std::nullopt;
    const auto command = pending_.test(Command::POWER)      ? Command::POWER
                         : pending_.test(Command::SETTINGS) ? Command::SETTINGS
                                                            : Command::ULTRASONIC_TIMEOUT;
    Batch batch{{command}, 1, auto_brightness_};
    pending_.reset(command);
    // ON needs settings before power. OFF retains independently queued
    // settings, because its power packet does not apply those fields.
    if (command == Command::POWER && state.power) batch = {{Command::SETTINGS, Command::POWER}, 2, auto_brightness_};
    if (command == Command::SETTINGS || (command == Command::POWER && state.power)) {
      pending_.reset(Command::SETTINGS);
      auto_brightness_ = false;
    }
    phase_ = Phase::TRANSMITTING;
    return batch;
  }
  bool on_tx_done(uint32_t now) {
    if (!transmitting()) return false;
    phase_ = Phase::COOLDOWN;
    sent_at_ = now;
    return true;
  }

 private:
  std::bitset<Command::ULTRASONIC_TIMEOUT + 1> pending_;
  Phase phase_{Phase::READY};
  uint32_t debounce_{1000}, sent_at_{0};
  bool auto_brightness_{false};
};

}  // namespace esphome::halo2
