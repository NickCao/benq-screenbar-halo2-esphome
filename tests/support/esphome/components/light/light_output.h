#pragma once

#include <initializer_list>
#include <optional>
#include "check.h"

// Only the LightOutput callback contract used by Halo2 is modeled here.
// Tests inject transition samples explicitly; this is not a fade interpolator.
namespace esphome::light {
enum class ColorMode { COLOR_TEMPERATURE };

class LightTraits {
 public:
  void set_supported_color_modes(std::initializer_list<ColorMode>) {}
  void set_min_mireds(float value) { min_ = value; }
  void set_max_mireds(float value) { max_ = value; }
  float get_min_mireds() const { return min_; }
  float get_max_mireds() const { return max_; }

 private:
  float min_{0}, max_{0};
};

struct LightColorValues {
  bool is_on() const { return state; }
  float get_brightness() const { return brightness; }
  float get_color_temperature() const { return temperature; }
  bool operator==(const LightColorValues &) const = default;

  bool state{false};
  float brightness{1}, temperature{1000000.0f / 4000};
};

class LightState;
class LightCall;

class LightOutput {
 public:
  virtual ~LightOutput() = default;
  virtual LightTraits get_traits() = 0;
  virtual void setup_state(LightState *) {}
  virtual void update_state(LightState *) {}
  virtual void write_state(LightState *) = 0;
};

class LightState {
 public:
  explicit LightState(LightOutput *output) : output_(output) {}
  void setup() { output_->setup_state(this); }
  LightCall make_call();
  LightTraits get_traits() const { return output_->get_traits(); }
  bool is_transformer_active() const { return active_; }
  void current_values_as_ct(float *temperature, float *brightness) const {
    const auto traits = get_traits();
    *temperature = (current_values.temperature - traits.get_min_mireds()) /
                   (traits.get_max_mireds() - traits.get_min_mireds());
    *brightness = current_values.state ? current_values.brightness : 0;
  }

  void loop() {
    if (!write_pending_) return;
    write_pending_ = false;
    output_->write_state(this);
  }

  void transition_sample(const LightColorValues &sample, bool finished = false) {
    CHECK(transition_pending_);
    active_ = true;
    current_values = sample;
    output_->update_state(this);
    write_pending_ = true;
    if (finished) {
      // ESPHome installs the final target AFTER update_state(), before write_state().
      current_values = remote_values;
      transition_pending_ = active_ = false;
    }
  }

  LightColorValues current_values, remote_values;

 private:
  friend class LightCall;
  LightOutput *output_;
  bool active_{false}, transition_pending_{false}, write_pending_{false};
};

class LightCall {
 public:
  explicit LightCall(LightState *state) : state_(state) {}
  LightCall &set_state(bool value) {
    on_ = value;
    return *this;
  }
  LightCall &set_brightness(float value) {
    brightness_ = value;
    return *this;
  }
  LightCall &set_color_temperature(float value) {
    temperature_ = value;
    return *this;
  }
  LightCall &set_transition_length(unsigned value) {
    transition_ = value;
    return *this;
  }
  LightCall &set_effect(const char *) { return *this; }
  void perform() {
    auto target = state_->remote_values;
    if (on_) target.state = *on_;
    if (brightness_) {
      target.brightness = *brightness_;
      if (*brightness_ == 0 && !on_) target.state = false;
    }
    if (temperature_) target.temperature = *temperature_;
    state_->remote_values = target;
    if (transition_ != 0) {
      // Starting a transition does not call update_state() or set the active flag.
      state_->transition_pending_ = true;
      return;
    }
    state_->transition_pending_ = state_->active_ = false;
    state_->current_values = target;
    state_->output_->update_state(state_);
    state_->write_pending_ = true;
  }

 private:
  LightState *state_;
  std::optional<bool> on_;
  std::optional<float> brightness_, temperature_;
  unsigned transition_{0};
};

inline LightCall LightState::make_call() { return LightCall(this); }
}  // namespace esphome::light
