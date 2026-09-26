#include "halo2.h"

#include <algorithm>
#include <cmath>
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#ifdef USE_HALO2_LR1121
#include "lr1121_halo2.h"
namespace halo2_radio = lr1121_halo2;
#else
#include "bm5602_halo2.h"
namespace halo2_radio = bm5602_halo2;
#endif

namespace esphome::halo2 {

static const char *const TAG = "halo2";

static uint8_t brightness_percent(float brightness) {
  return static_cast<uint8_t>(std::clamp(std::lround(brightness * 100.0f), 1L, 100L));
}

static uint16_t temperature_kelvin(float mireds) {
  if (!std::isfinite(mireds) || mireds <= 0) return 3925;
  // Match the controller's 25 K steps.
  return static_cast<uint16_t>(std::clamp(std::lround(1000000.0f / mireds / 25.0f) * 25, 2700L, 6500L));
}

light::LightTraits Halo2Light::get_traits() {
  light::LightTraits traits;
  traits.set_supported_color_modes({light::ColorMode::COLOR_TEMPERATURE});
  traits.set_min_mireds(1000000.0f / 6500.0f);
  traits.set_max_mireds(1000000.0f / 2700.0f);
  return traits;
}

void Halo2::setup() {
  disable_loop();
  // LightState restores its preferences before this component is initialized.
  // Restore the bridge's last known state without transmitting on boot.
  const auto &front = front_light_->remote_values;
  const auto &back = back_light_->remote_values;
  mode_preference_ = global_preferences->make_preference<uint8_t>(0x48414C32);
  if (!mode_preference_.load(&saved_mode_) || saved_mode_ < 1 || saved_mode_ > 3) saved_mode_ = 3;
  state_.power = front.is_on() || back.is_on();
  state_.front = state_.power ? front.is_on() : (saved_mode_ & 1U);
  state_.back = state_.power ? back.is_on() : (saved_mode_ & 2U);
  state_.pir = ultrasonic_switch_->get_initial_state_with_restore_mode().value_or(false);
  state_.front_brightness = brightness_percent(front.get_brightness());
  state_.back_brightness = brightness_percent(back.get_brightness());
  state_.color_temperature = temperature_kelvin(front.get_color_temperature());
  save_mode_();
  publish_lights_();
  publish_switches_();

#ifdef USE_HALO2_LR1121
  link_preference_ = global_preferences->make_preference<SavedLink>(0x48414C33);
  SavedLink saved{};
  const bool restored = !address_configured_ && link_preference_.load(&saved) && saved.version == 2 &&
                        saved.packet_options <= 1 &&
                        (saved.channel == 5 || saved.channel == 46 || saved.channel == 75);
  if (restored) {
    radio_address_ = saved.address;
    radio_channel_ = saved.channel;
    state_.packet_options = saved.packet_options;
  }
  ready_ = halo2_radio::setup(frequency_deviation_, pulse_shape_, radio_address_, radio_channel_);
  if (!ready_) {
    publish_status_(halo2_radio::last_error());
    mark_failed();
    return;
  }
  ESP_LOGI(TAG, "LR1121 firmware %04X, %u MHz GFSK", halo2_radio::radio.firmware_version(),
           2400U + radio_channel_);
  if (auto_discover_ && !address_configured_ && !restored) {
    start_discovery();
    return;
  }
  publish_address_();
#else
  const auto version = halo2_radio::setup_exact_pico(halo2_protocol::RADIO_ADDRESS, halo2_protocol::RADIO_CHANNEL);
  if (version != std::array<uint8_t, 3>{0x56, 0x02, 0x01}) {
    ESP_LOGE(TAG, "Unexpected BM5602 version: %02X %02X %02X", version[0], version[1], version[2]);
    publish_status_("BM5602 version error");
    mark_failed();
    return;
  }
  halo2_radio::prepare_halo_receive();
  ready_ = true;
#endif
  publish_status_("Listening");
}

void Halo2::publish_address_() {
  char address[80];
  snprintf(address, sizeof(address), "%02X:%02X:%02X:%02X / %u MHz", radio_address_[0], radio_address_[1],
           radio_address_[2], radio_address_[3], 2400U + radio_channel_);
  if (radio_address_sensor_ != nullptr) radio_address_sensor_->publish_state(address);
  ESP_LOGI(TAG, "Radio address %s (register order)", address);
}

void Halo2::start_discovery() {
#ifdef USE_HALO2_LR1121
  if (!ready_) return;
  pending_command_ = 0;
  candidates_ = {};
  scan_step_ = 0;
  discovering_ = true;
  ESP_LOGI(TAG, "Discovery: adjust the controller brightness near the board");
  if (scan_channel_()) enable_loop();
#endif
}

bool Halo2::scan_channel_() {
#ifdef USE_HALO2_LR1121
  static constexpr uint8_t channels[]{5, 46, 75};
  const uint8_t channel = channels[scan_step_ / 2];
  const uint8_t sync = scan_step_ % 2 == 0 ? 0xAA : 0x55;
  if (!halo2_radio::radio.listen_for_address(channel, sync)) {
    ready_ = false;
    disable_loop();
    status_set_error();
    publish_status_(halo2_radio::last_error());
    return false;
  }
  scan_started_ = millis();
  char status[80];
  snprintf(status, sizeof(status), "Discovering at %u MHz; adjust controller brightness", 2400U + channel);
  publish_status_(status);
  ESP_LOGD(TAG, "Scanning %u MHz, preamble %02X", 2400U + channel, sync);
  return true;
#else
  return false;
#endif
}

void Halo2::loop() {
#ifdef USE_HALO2_LR1121
  if (!ready_ || !discovering_) return;
  halo2_protocol::Address address{};
  halo2_protocol::HaloRxState received;
  const uint32_t previous_count = halo2_radio::radio.capture_count();
  const bool found = halo2_radio::radio.poll_address(address, received);
  if (halo2_radio::radio.capture_count() != previous_count) {
    const auto &data = halo2_radio::radio.capture_data();
    ESP_LOGV(TAG, "Discovery RX: %s", format_hex_pretty(data.data(), data.size()).c_str());
  }
  if (found) {
    const uint8_t channel = halo2_radio::radio.channel();
    Candidate *candidate = nullptr;
    for (auto &entry : candidates_) {
      if (entry.count != 0 && entry.address == address && entry.channel == channel) {
        candidate = &entry;
        break;
      }
    }
    if (candidate == nullptr) {
      candidate = &*std::min_element(candidates_.begin(), candidates_.end(),
                                    [](const Candidate &a, const Candidate &b) { return a.count < b.count; });
      *candidate = {address, channel, 0};
    }
    ++candidate->count;
    ESP_LOGI(TAG, "Discovery candidate %02X:%02X:%02X:%02X at %u MHz: %u/3 CRC-valid captures",
             address[0], address[1], address[2], address[3], 2400U + channel, candidate->count);
    ESP_LOGD(TAG, "Captured state: front %u%%, rear %u%%, %u K, mode %u/%u", received.front_brightness,
             received.back_brightness, received.color_temperature, received.front, received.back);
    if (candidate->count >= 3) {
      if (!halo2_radio::radio.use_address(address, channel)) {
        ready_ = false;
        disable_loop();
        status_set_error();
        publish_status_(halo2_radio::last_error());
        return;
      }
      radio_address_ = address;
      radio_channel_ = channel;
      discovering_ = false;
      disable_loop();
      const SavedLink saved{address, channel, 2, received.packet_options, 0};
      const bool persisted = link_preference_.save(&saved) && global_preferences->sync();
      state_ = received;
      save_mode_();
      publish_lights_();
      publish_switches_();
      publish_address_();
      publish_status_(persisted ? "Address discovered and saved" : "Address discovered; save failed");
      if (!persisted) status_set_warning();
      ESP_LOGI(TAG, "Discovery complete. radio_address: [0x%02X, 0x%02X, 0x%02X, 0x%02X], radio_channel: %u",
               address[0], address[1], address[2], address[3], channel);
      return;
    }
  }
  if (const char *error = halo2_radio::last_error()) {
    ready_ = false;
    disable_loop();
    status_set_error();
    publish_status_(error);
    return;
  }
  if (millis() - scan_started_ >= 3000) {
    ESP_LOGD(TAG, "Discovery captured %u buffers", halo2_radio::radio.capture_count());
    scan_step_ = (scan_step_ + 1) % 6;
    scan_channel_();
  }
#endif
}

void Halo2::dump_config() {
  ESP_LOGCONFIG(TAG, "ScreenBar HALO 2:");
#ifdef USE_HALO2_LR1121
  ESP_LOGCONFIG(TAG, "  Radio: LR1121, deviation: %u Hz, pulse shape: 0x%02X", frequency_deviation_, pulse_shape_);
#else
  ESP_LOGCONFIG(TAG, "  Radio: BM5602");
#endif
  ESP_LOGCONFIG(TAG, "  Frequency: %u MHz", 2400U + radio_channel_);
  LOG_UPDATE_INTERVAL(this);
}

void Halo2::queue_command_(uint8_t command) {
  // Mode/brightness commands do not change global power. Preserve a power
  // command through the entire batch, in either direction.
  if (pending_command_ != 0x02) pending_command_ = command;
}

void Halo2::control_light(light::LightState *light, bool front) {
  if (!accepts_commands()) return;
  const auto &values = light->current_values;
  const bool was_powered = state_.power;
  const bool was_front = state_.front;
  const bool was_back = state_.back;
  bool front_on = state_.power && state_.front;
  bool back_on = state_.power && state_.back;
  // The final frame of an ESPHome fade-out has zero brightness.
  const bool on = values.is_on() && values.get_brightness() > 0.0f;
  (front ? front_on : back_on) = on;
  state_.power = front_on || back_on;
  if (state_.power) {
    state_.front = front_on;
    state_.back = back_on;
  }
  // Remember a useful brightness after fading to off, and retain the selected
  // mode when both lights are off so the master power switch can restore it.
  const uint8_t brightness = brightness_percent(on ? values.get_brightness() : light->remote_values.get_brightness());
  uint8_t &stored_brightness = front ? state_.front_brightness : state_.back_brightness;
  const uint16_t temperature = temperature_kelvin(values.get_color_temperature());
  const bool temperature_changed = temperature != state_.color_temperature;
  const bool settings_changed = brightness != stored_brightness || temperature_changed ||
                                was_front != state_.front || was_back != state_.back;
  stored_brightness = brightness;
  state_.color_temperature = temperature;
  if (settings_changed || was_powered != state_.power) {
    queue_command_(was_powered != state_.power ? 0x02 : 0x03);
    save_mode_();
    publish_switches_();
  }
  if (temperature_changed || (!light->is_transformer_active() &&
      values.get_color_temperature() != 1000000.0f / state_.color_temperature)) {
    synchronize_temperature_(light);
  }
}

void Halo2::synchronize_temperature_(light::LightState *source) {
  auto *peer = source == front_light_ ? back_light_ : front_light_;
  // Temperature is shared by the lamp. Keep the other entity's next command
  // from accidentally restoring its previous temperature.
  publishing_ = true;
  auto call = peer->make_call();
  call.set_color_temperature(1000000.0f / state_.color_temperature);
  call.set_transition_length(0);
  call.perform();
  if (!source->is_transformer_active()) {
    auto source_call = source->make_call();
    source_call.set_color_temperature(1000000.0f / state_.color_temperature);
    source_call.set_transition_length(0);
    source_call.perform();
  }
  publishing_ = false;
  // A shared temperature change also ends a simultaneous peer transition.
  // Include its final state in the same packet so HA and the lamp agree.
  control_light(peer, peer == front_light_);
}

void Halo2::control_switch(bool power, bool value) {
  if (!accepts_commands()) return;
  if (power) {
    state_.power = value;
    publish_lights_();
  } else {
    state_.pir = value;
  }
  publish_switches_();
  queue_command_(power ? 0x02 : 0x03);
}

void Halo2::publish_light_(light::LightState *light, bool front) {
  auto call = light->make_call();
  call.set_state(state_.power && (front ? state_.front : state_.back));
  call.set_brightness((front ? state_.front_brightness : state_.back_brightness) / 100.0f);
  call.set_color_temperature(1000000.0f / state_.color_temperature);
  call.set_transition_length(0);
  call.set_effect("None");
  call.perform();
}

void Halo2::publish_lights_() {
  publishing_ = true;
  publish_light_(front_light_, true);
  publish_light_(back_light_, false);
  publishing_ = false;
}

void Halo2::publish_switches_() {
  power_switch_->publish_state(state_.power);
  ultrasonic_switch_->publish_state(state_.pir);
}

void Halo2::publish_status_(const char *status) {
  if (status == nullptr) status = "Radio error";
  if (radio_status_->state != status) radio_status_->publish_state(status);
}

void Halo2::save_mode_() {
  const uint8_t mode = (state_.front ? 1U : 0U) | (state_.back ? 2U : 0U);
  if (mode == saved_mode_) return;
  saved_mode_ = mode;
  mode_preference_.save(&saved_mode_);
}

void Halo2::update() {
  if (!ready_ || discovering_) return;
  if (pending_command_ != 0) {
    const uint8_t command = pending_command_;
    pending_command_ = 0;
    const auto send = [&](uint8_t opcode) {
      ESP_LOGD(TAG, "TX command 0x%02X, power %s, mode %u/%u", opcode, ONOFF(state_.power),
               state_.front, state_.back);
      return halo2_radio::send_halo_state(opcode, state_.power, state_.pir, state_.front, state_.back,
          state_.front_brightness, state_.back_brightness, state_.color_temperature, state_.packet_options);
    };
    // Power-on must apply the final combined settings and explicitly switch
    // the lamp on. Settings alone leave a powered-off lamp off.
    bool sent = true;
    if (command == 0x02 && state_.power) sent = send(0x03);
    if (sent) sent = send(command);
    if (!sent) {
      status_set_warning();
      publish_status_(halo2_radio::last_error());
    } else {
      status_clear_warning();
      // TX completion is not an acknowledgement from the lamp.
      publish_status_("Command sent");
    }
    return;
  }

  halo2_protocol::HaloRxState received;
  if (halo2_radio::poll_halo_receive(received) && received.valid) {
#ifdef USE_HALO2_LR1121
    if (received.packet_options != state_.packet_options) {
      const SavedLink saved{radio_address_, radio_channel_, 2, received.packet_options, 0};
      link_preference_.save(&saved);
    }
#endif
    state_ = received;
    save_mode_();
    publish_lights_();
    publish_switches_();
    status_clear_warning();
    publish_status_("Controller update received");
  } else if (const char *error = halo2_radio::last_error()) {
    status_set_warning();
    publish_status_(error);
  }
}

}  // namespace esphome::halo2
