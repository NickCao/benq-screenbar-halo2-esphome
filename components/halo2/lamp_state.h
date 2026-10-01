#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace esphome::halo2 {

enum class Section : uint8_t { FRONT, BACK };
enum class LampSelection : uint8_t { FRONT_ONLY = 0, BACK_ONLY = 1, BOTH = 2 };
enum class UltrasonicTimeout : uint8_t { MINUTES_3 = 0, MINUTES_5 = 1, MINUTES_10 = 2, MINUTES_15 = 3 };
// Application commands using their wire values. Favorite commands are receive-only.
enum Command : uint8_t {
  POWER = 0x02,
  SETTINGS = 0x03,
  STATUS = 0x04,
  ULTRASONIC_TIMEOUT = 0x05,
  FAVORITE_RECALL = 0x07,
  FAVORITE_SAVE = 0x08,
};
constexpr std::array<uint8_t, 4> ULTRASONIC_TIMEOUT_MINUTES{3, 5, 10, 15};
constexpr uint8_t MIN_BRIGHTNESS_PERCENT = 1, MAX_BRIGHTNESS_PERCENT = 100;
constexpr uint16_t MIN_TEMPERATURE_K = 2700, MAX_TEMPERATURE_K = 6500, TEMPERATURE_STEP_K = 25;
constexpr uint16_t DEFAULT_TEMPERATURE_K = 3925;

// Lamp settings contain no packet metadata or ESPHome state.
struct LampState {
  bool power{false};
  LampSelection selection{LampSelection::BOTH};
  uint8_t front_brightness{MIN_BRIGHTNESS_PERCENT}, back_brightness{MIN_BRIGHTNESS_PERCENT};
  uint16_t color_temperature{DEFAULT_TEMPERATURE_K};
  bool ultrasonic_enabled{false};
  UltrasonicTimeout ultrasonic_timeout{UltrasonicTimeout::MINUTES_5};

  bool operator==(const LampState &) const = default;

  bool selected(Section section) const {
    return selection == LampSelection::BOTH ||
           selection == (section == Section::FRONT ? LampSelection::FRONT_ONLY : LampSelection::BACK_ONLY);
  }
  bool is_on(Section section) const { return power && selected(section); }
  uint8_t brightness(Section section) const { return section == Section::FRONT ? front_brightness : back_brightness; }

  void set_selection(bool front, bool back) {
    // There is no wire selection for neither section. Retain the last mode.
    if (front || back)
      selection = front && back ? LampSelection::BOTH : (back ? LampSelection::BACK_ONLY : LampSelection::FRONT_ONLY);
  }
  void set_brightness(Section section, uint8_t percent) {
    // Zero is an OFF request; keep a useful brightness for the next ON.
    if (percent != 0)
      (section == Section::FRONT ? front_brightness : back_brightness) =
          std::clamp(percent, MIN_BRIGHTNESS_PERCENT, MAX_BRIGHTNESS_PERCENT);
  }
  void set_light(Section section, bool on, uint8_t percent) {
    bool front = is_on(Section::FRONT), back = is_on(Section::BACK);
    (section == Section::FRONT ? front : back) = on && percent != 0;
    power = front || back;
    set_selection(front, back);
    set_brightness(section, percent);
  }
};

// Requested settings drive outgoing commands and the optimistic UI. Received
// snapshots establish a baseline and reconcile those settings with the lamp.
class LampStateModel {
 public:
  const LampState &requested() const { return requested_; }
  bool initialized() const { return initialized_; }

  void restore(const LampState &state) {
    requested_ = state;
    invalidate();
  }
  void request(const LampState &state) { requested_ = state; }
  bool receive_request(const LampState &state) { return apply_received_(state); }
  bool receive_status(const LampState &state) {
    auto reconciled = state;
    // Settings packets only apply brightness to selected sections. Keep an
    // inactive section's requested level for its next ON. A new baseline
    // uses all received values, as do original-controller snapshots.
    if (initialized_) {
      if (!state.selected(Section::FRONT)) reconciled.front_brightness = requested_.front_brightness;
      if (!state.selected(Section::BACK)) reconciled.back_brightness = requested_.back_brightness;
    }
    return apply_received_(reconciled);
  }
  void invalidate() { initialized_ = false; }

 private:
  bool apply_received_(const LampState &state) {
    const bool changed = !initialized_ || requested_ != state;
    requested_ = state;
    initialized_ = true;
    return changed;
  }

  LampState requested_;
  bool initialized_{false};
};

}  // namespace esphome::halo2
