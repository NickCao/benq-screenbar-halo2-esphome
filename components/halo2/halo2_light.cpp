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
  state.set_light(section_, values.is_on(), brightness_percent(values.get_brightness()));
  if (section_ == Section::FRONT) state.color_temperature = temperature_kelvin(values.get_color_temperature());
}

void Halo2Light::read_into(LampState &state) const {
  float temperature = 0, brightness = 0;
  state_->current_values_as_ct(&temperature, &brightness);
  const auto traits = state_->get_traits();
  const float mireds = std::lerp(traits.get_min_mireds(), traits.get_max_mireds(), temperature);
  // The final fade-out sample has zero brightness. Retain the remote level
  // for OFF, and let LampState preserve it when the remote level is also zero.
  const bool on = brightness > 0;
  if (!on) brightness = state_->remote_values.get_brightness();
  state.set_light(section_, on, brightness_percent(brightness));
  state.color_temperature = temperature_kelvin(mireds);
}

void Halo2Light::publish(const LampState &state) {
  ScopedPublication guard(reflecting_state_);
  auto call = state_->make_call();
  call.set_state(state.is_on(section_));
  call.set_brightness(state.brightness(section_) / 100.0f);
  call.set_color_temperature(1000000.0f / state.color_temperature);
  call.set_transition_length(0);
  call.set_effect("None");
  call.perform();
}

bool Halo2Light::needs_temperature_sync(uint16_t temperature) const {
  const float mireds = 1000000.0f / temperature;
  return state_->current_values.get_color_temperature() != mireds ||
         state_->remote_values.get_color_temperature() != mireds;
}

void Halo2Light::sync_temperature(uint16_t temperature) {
  if (!needs_temperature_sync(temperature)) return;
  ScopedPublication guard(reflecting_state_);
  auto call = state_->make_call();
  call.set_color_temperature(1000000.0f / temperature);
  call.set_transition_length(0);
  call.perform();
}

bool Halo2Light::transitioning() const {
  // ESPHome sets its active flag on the first loop after starting a fade.
  // Divergent current/remote values cover the interval before that loop.
  return state_->is_transformer_active() || state_->current_values != state_->remote_values;
}

void Halo2::control_light(Halo2Light &source) {
  if (!accepts_commands()) return;
  auto requested = lamp_state_.requested();
  source.read_into(requested);
  const auto temperature = requested.color_temperature;
  Halo2Light *peer = nullptr;
  if (temperature != lamp_state_.requested().color_temperature) {
    peer = lights_[source.section_ == Section::FRONT ? 1 : 0];
    if (peer->needs_temperature_sync(temperature)) {
      // An immediate temperature call finishes a peer fade at its remote
      // target. Include that target before publishing either entity.
      const auto &target = peer->state_->remote_values;
      requested.set_light(peer->section_, target.is_on(), brightness_percent(target.get_brightness()));
    } else {
      peer->read_into(requested);
    }
  }
  request_state_(requested);
  if (peer != nullptr) peer->sync_temperature(temperature);
  if (source.transitioning()) return;
  // Repair zero-brightness OFF immediately so a following plain ON uses the
  // retained level. Otherwise only canonicalize the source's temperature.
  if (source.state_->remote_values.get_brightness() == 0)
    source.publish(requested);
  else
    source.sync_temperature(temperature);
}

bool Halo2::lights_transitioning_() const { return lights_[0]->transitioning() || lights_[1]->transitioning(); }

void Halo2::publish_lights_() {
  for (auto *light : lights_) light->publish(lamp_state_.requested());
}

}  // namespace esphome::halo2
