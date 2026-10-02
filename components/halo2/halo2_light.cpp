#include "halo2.h"

#include <algorithm>
#include <cmath>

namespace esphome::halo2 {

static uint8_t brightness_percent(float brightness) {
  return static_cast<uint8_t>(std::clamp<long>(std::lround(brightness * 100.0f), 0, MAX_BRIGHTNESS_PERCENT));
}

static uint16_t temperature_kelvin(float mireds) {
  if (!std::isfinite(mireds) || mireds <= 0) return DEFAULT_TEMPERATURE_K;
  return static_cast<uint16_t>(
      std::clamp<long>(std::lround(1000000.0f / mireds / TEMPERATURE_STEP_K) * TEMPERATURE_STEP_K, MIN_TEMPERATURE_K,
                       MAX_TEMPERATURE_K));
}

light::LightTraits Halo2Light::get_traits() {
  light::LightTraits traits;
  traits.set_supported_color_modes({light::ColorMode::COLOR_TEMPERATURE});
  traits.set_min_mireds(1000000.0f / MAX_TEMPERATURE_K);
  traits.set_max_mireds(1000000.0f / MIN_TEMPERATURE_K);
  return traits;
}

void Halo2Light::restore_into(LampState &state) const {
  const auto &values = state_->remote_values;
  state.power = values.is_on();
  state.color_temperature = temperature_kelvin(values.get_color_temperature());
}

void Halo2Light::read_into(LampState &state, bool use_target) const {
  const auto &target = state_->remote_values;
  float temperature = 0, brightness = 0;
  state_->current_values_as_ct(&temperature, &brightness);
  const auto traits = state_->get_traits();
  const float mireds = use_target ? target.get_color_temperature()
                                : std::lerp(traits.get_min_mireds(), traits.get_max_mireds(), temperature);
  if (use_target) brightness = target.is_on() ? target.get_brightness() : 0;
  state.power = brightness > 0;
  // Never turn OFF fade samples into stored brightness settings. POWER is
  // independent of the selection and the levels used on the next wake.
  if (target.is_on() && brightness > 0)
    state.set_master_brightness(std::max(brightness_percent(brightness), MIN_BRIGHTNESS_PERCENT), dimming_basis_);
  state.color_temperature = temperature_kelvin(mireds);
}

void Halo2Light::publish(const LampState &state) {
  ScopedPublication guard(reflecting_state_);
  auto call = state_->make_call();
  call.set_state(state.power);
  call.set_brightness(state.master_brightness() / 100.0f);
  call.set_color_temperature(1000000.0f / state.color_temperature);
  call.set_transition_length(0);
  call.set_effect("None");
  call.perform();
  dimming_basis_ = state;
}

bool Halo2Light::transitioning() const {
  // ESPHome sets its active flag on the first loop after starting a fade.
  // Divergent current/remote values cover the interval before that loop.
  return state_->is_transformer_active() || state_->current_values != state_->remote_values;
}

void Halo2::control_light() {
  if (!accepts_commands()) return;
  auto requested = lamp_state_;
  light_->read_into(requested);
  request_state_(requested);
  publish_brightness_();
  // Canonicalize integer brightness/25 K temperature, and repair a zero
  // brightness OFF so plain ON can restore the retained profile.
  if (!light_->transitioning()) light_->publish(requested);
}

void Halo2::control_brightness(Section section, float value) {
  if (!accepts_commands() || !std::isfinite(value) || value < MIN_BRIGHTNESS_PERCENT || value > MAX_BRIGHTNESS_PERCENT)
    return;
  auto requested = lamp_state_;
  // The lamp applies brightness settings only to selected sections. Show
  // the stored level of an inactive section without inventing a new one.
  if (!requested.selected(section)) {
    publish_brightness_();
    return;
  }
  // A setting change ends any active master fade at its final target,
  // before changing the profile used for subsequent proportional dimming.
  if (light_->transitioning()) light_->read_into(requested, true);
  requested.set_brightness(section, static_cast<uint8_t>(std::lround(value)));
  request_state_(requested);
  publish_light_();
}

void Halo2::control_selection(size_t index) {
  if (!accepts_commands() || index > static_cast<size_t>(LampSelection::BOTH)) return;
  auto requested = lamp_state_;
  if (light_->transitioning()) light_->read_into(requested, true);
  requested.selection = static_cast<LampSelection>(index);
  request_state_(requested);
  publish_light_();
}

void Halo2::publish_brightness_() {
  const auto &state = lamp_state_;
  for (auto section : {Section::FRONT, Section::BACK})
    brightness_numbers_[static_cast<size_t>(section)]->publish_state(state.brightness(section));
}

void Halo2::publish_light_() {
  light_->publish(lamp_state_);
  publish_brightness_();
  section_select_->publish_state(static_cast<size_t>(lamp_state_.selection));
}

}  // namespace esphome::halo2
