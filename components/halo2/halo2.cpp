#include "halo2.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

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
  ultrasonic_switch_->publish_state(state_.pir);

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
  discovering_ = auto_discover_ && !address_configured_ && !restored;
  // Setup, reception, transmission and recovery all advance in loop().
  enable_loop();
  start_radio_();
}

void Halo2::start_radio_() {
  ready_ = false;
  recovering_ = false;
  publish_status_("Initializing radio");
  if (!radio_.setup(frequency_deviation_, pulse_shape_, radio_address_, radio_channel_)) recover_radio_();
}

void Halo2::recover_radio_() {
  if (recovering_) return;
  ready_ = false;
  recovering_ = true;
  transmission_ = Transmission::NONE;
  // Never replay an interrupted command batch after reconnecting to the lamp.
  pending_command_ = 0;
  pending_auto_brightness_ = false;
  awaiting_status_ = false;
  status_followup_ = false;
  status_poll_pending_ = false;
  cancel_timeout("discovery_dwell");
  state_.valid = false;
  status_set_warning();
  const char *error = radio_.last_error();
  if (error == nullptr) error = "LR1121 unavailable";
  char status[160];
  snprintf(status, sizeof(status), "%s; retrying in %" PRIu32 " s", error, recovery_delay_ / 1000);
  publish_status_(status);
  ESP_LOGW(TAG, "%s", status);
  set_timeout("radio_recovery", recovery_delay_, [this]() { start_radio_(); });
  recovery_delay_ = std::min(recovery_delay_ * 2, uint32_t{30000});
}

void Halo2::publish_address_() {
  char address[80];
  snprintf(address, sizeof(address), "%02X:%02X:%02X:%02X / %u MHz", radio_address_[0], radio_address_[1],
           radio_address_[2], radio_address_[3], 2400U + radio_channel_);
  if (radio_address_sensor_ != nullptr) radio_address_sensor_->publish_state(address);
  ESP_LOGI(TAG, "Radio address %s (register order)", address);
}

void Halo2::start_discovery() {
  if (!ready_) return;
  pending_command_ = 0;
  pending_auto_brightness_ = false;
  awaiting_status_ = false;
  status_followup_ = false;
  status_poll_pending_ = false;
  cancel_timeout("discovery_dwell");
  candidates_ = {};
  scan_step_ = 0;
  discovering_ = true;
  scan_pending_ = true;
  ESP_LOGI(TAG, "Discovery: adjust the controller brightness near the board");
}

bool Halo2::scan_channel_() {
  static constexpr uint8_t channels[]{5, 46, 75};
  const uint8_t channel = channels[scan_step_ / 2];
  const uint8_t sync = scan_step_ % 2 == 0 ? 0xAA : 0x55;
  if (!radio_.listen_for_address(channel, sync)) {
    recover_radio_();
    return false;
  }
  scan_expired_ = false;
  set_timeout("discovery_dwell", 3000, [this]() { scan_expired_ = true; });
  char status[80];
  snprintf(status, sizeof(status), "Discovering at %u MHz; adjust controller brightness", 2400U + channel);
  publish_status_(status);
  ESP_LOGD(TAG, "Scanning %u MHz, preamble %02X", 2400U + channel, sync);
  return true;
}

void Halo2::loop() {
  if (recovering_) return;
  radio_.loop();
  if (radio_.last_error() != nullptr) {
    recover_radio_();
    return;
  }
  if (!ready_) {
    if (!radio_.ready()) return;
    ready_ = true;
    recovery_delay_ = 1000;
    status_timeouts_ = 0;
    status_clear_warning();
    ESP_LOGI(TAG, "LR1121 firmware %04X, %u MHz GFSK; radio ready", radio_.firmware_version(),
             2400U + radio_channel_);
    if (discovering_) {
      start_discovery();
    } else {
      publish_address_();
      publish_status_("Listening");
      // Read the lamp after recovery instead of imposing the saved HA state.
      schedule_status_poll_(500);
    }
  }
  if (radio_.take_tx_done()) {
    if (transmission_ == Transmission::STATUS) {
      status_request_started_ = millis();
    } else if (transmission_ == Transmission::COMMAND) {
      command_sent_at_ = millis();
      command_sent_ = true;
      status_clear_warning();
      publish_status_("Command sent");
      schedule_status_poll_(500);
    }
    transmission_ = Transmission::NONE;
  }
  if (!discovering_) {
    // Keep fast command/RX processing separate from periodic lamp polling.
    const uint32_t now = App.get_loop_component_start_time();
    if (now - last_process_at_ >= processing_interval_) {
      last_process_at_ = now;
      process_radio_();
    }
    return;
  }
  if (!radio_.idle()) return;
  if (scan_pending_) {
    if (scan_channel_()) scan_pending_ = false;
    return;
  }
  halo2_protocol::Address address{};
  halo2_protocol::HaloRxState received;
  const uint32_t previous_count = radio_.capture_count();
  const bool found = radio_.poll_address(address, received);
  if (radio_.capture_count() != previous_count) {
    const auto &data = radio_.capture_data();
    ESP_LOGV(TAG, "Discovery RX: %s", format_hex_pretty(data.data(), data.size()).c_str());
  }
  if (found) {
    const uint8_t channel = radio_.channel();
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
      if (!radio_.use_address(address, channel)) {
        recover_radio_();
        return;
      }
      radio_address_ = address;
      radio_channel_ = channel;
      discovering_ = false;
      cancel_timeout("discovery_dwell");
      const SavedLink saved{address, channel, 2, received.packet_options, 0};
      const bool persisted = link_preference_.save(&saved) && global_preferences->sync();
      state_ = received;
      schedule_status_poll_(1000);
      save_mode_();
      publish_lights_();
      ultrasonic_switch_->publish_state(state_.pir);
      publish_address_();
      publish_status_(persisted ? "Address discovered and saved" : "Address discovered; save failed");
      if (!persisted) status_set_warning();
      ESP_LOGI(TAG, "Discovery complete. radio_address: [0x%02X, 0x%02X, 0x%02X, 0x%02X], radio_channel: %u",
               address[0], address[1], address[2], address[3], channel);
      return;
    }
  }
  if (scan_expired_) {
    ESP_LOGD(TAG, "Discovery captured %" PRIu32 " buffers", radio_.capture_count());
    scan_step_ = (scan_step_ + 1) % 6;
    scan_pending_ = true;
  }
}

void Halo2::dump_config() {
  ESP_LOGCONFIG(TAG, "ScreenBar HALO 2:");
  LR1121Transport::dump_config();
  ESP_LOGCONFIG(TAG, "  Radio: LR1121, deviation: %" PRIu32 " Hz, pulse shape: 0x%02X",
                frequency_deviation_, pulse_shape_);
  ESP_LOGCONFIG(TAG, "  Processing interval: %" PRIu32 " ms", processing_interval_);
  ESP_LOGCONFIG(TAG, "  Frequency: %u MHz", 2400U + radio_channel_);
  ESP_LOGCONFIG(TAG, "  Command debounce: %" PRIu32 " ms", command_debounce_);
  LOG_UPDATE_INTERVAL(this);
}

void Halo2::queue_command_(uint8_t command) {
  // Mode/brightness commands do not change global power. Preserve a power
  // command through the entire batch, in either direction.
  if (pending_command_ != 0x02) pending_command_ = command;
}

bool Halo2::send_state_(uint8_t command, bool auto_brightness) {
  if (!radio_.ready()) return false;
  const auto payload = halo2_protocol::make_payload(command, state_.power, state_.pir, state_.front, state_.back,
      state_.front_brightness, state_.back_brightness, state_.color_temperature, state_.packet_options,
      auto_brightness);
  last_pcf_ = halo2_protocol::request_pcf(app_pid_++);
  return radio_.send(halo2_protocol::make_air_frame(last_pcf_, payload, radio_.address()));
}

void Halo2::start_auto_brightness() {
  if (!accepts_commands()) return;
  pending_auto_brightness_ = true;
  queue_command_(0x03);
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
  // Remember a useful brightness after fading to off, and retain a valid
  // selected mode in the radio state while both lights are off.
  const uint8_t brightness = brightness_percent(on ? values.get_brightness() : light->remote_values.get_brightness());
  uint8_t &stored_brightness = front ? state_.front_brightness : state_.back_brightness;
  const uint16_t temperature = temperature_kelvin(values.get_color_temperature());
  const bool temperature_changed = temperature != state_.color_temperature;
  if (brightness != stored_brightness || temperature_changed) pending_auto_brightness_ = false;
  const bool settings_changed = brightness != stored_brightness || temperature_changed ||
                                was_front != state_.front || was_back != state_.back;
  stored_brightness = brightness;
  state_.color_temperature = temperature;
  if (settings_changed || was_powered != state_.power) {
    queue_command_(was_powered != state_.power ? 0x02 : 0x03);
    save_mode_();
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

void Halo2::control_ultrasonic(bool value) {
  if (!accepts_commands()) return;
  state_.pir = value;
  ultrasonic_switch_->publish_state(state_.pir);
  queue_command_(0x03);
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

void Halo2::apply_received_(const halo2_protocol::HaloRxState &received) {
  const bool changed = !state_.valid || received.power != state_.power || received.front != state_.front ||
                       received.back != state_.back || received.pir != state_.pir ||
                       received.front_brightness != state_.front_brightness ||
                       received.back_brightness != state_.back_brightness ||
                       received.color_temperature != state_.color_temperature;
  if (received.packet_options != state_.packet_options) {
    const SavedLink saved{radio_address_, radio_channel_, 2, received.packet_options, 0};
    link_preference_.save(&saved);
  }
  state_ = received;
  if (changed) {
    save_mode_();
    publish_lights_();
    ultrasonic_switch_->publish_state(state_.pir);
  }
  status_clear_warning();
  publish_status_(received.reply ? "Lamp status received" : "Controller update received");
}

void Halo2::update() {
  if (ready_ && !discovering_ && pending_command_ == 0 && transmission_ == Transmission::NONE &&
      !status_poll_pending_ && !awaiting_status_ && !status_followup_) schedule_status_poll_(0);
}

void Halo2::schedule_status_poll_(uint32_t delay) {
  status_poll_pending_ = true;
  next_status_poll_ = millis() + delay;
}

void Halo2::process_radio_() {
  if (!ready_ || discovering_) return;
  if (transmission_ != Transmission::NONE) return;
  if (pending_command_ != 0) {
    // Send the first request without a debounce delay. During the cooldown
    // after a batch, retain only the latest requested state and send it once
    // the interval expires; incoming changes do not extend the interval.
    const uint32_t now = millis();
    if (command_sent_ && now - command_sent_at_ < command_debounce_) return;
    if (!radio_.idle()) return;
    const uint8_t command = pending_command_;
    const bool auto_brightness = pending_auto_brightness_;
    pending_command_ = 0;
    pending_auto_brightness_ = false;
    // A reply to an earlier query must not overwrite a newer local command.
    status_poll_pending_ = false;
    awaiting_status_ = false;
    status_followup_ = false;
    const auto send = [&](uint8_t opcode) {
      ESP_LOGD(TAG, "TX command 0x%02X, power %s, mode %u/%u", opcode, ONOFF(state_.power),
               state_.front, state_.back);
      return send_state_(opcode, auto_brightness && opcode == 0x03);
    };
    // Power-on must apply the final combined settings and explicitly switch
    // the lamp on. Settings alone leave a powered-off lamp off.
    bool sent = true;
    if (command == 0x02 && state_.power) sent = send(0x03);
    if (sent) sent = send(command);
    if (sent) transmission_ = Transmission::COMMAND;
    else recover_radio_();
    return;
  }

  halo2_protocol::HaloRxState received;
  const uint32_t previous_rx_count = radio_.rx_count();
  const bool received_state = radio_.poll(received);
  if (awaiting_status_ && radio_.rx_count() != previous_rx_count) {
    const auto &frame = radio_.rx_frame();
    ESP_LOGV(TAG, "Status RX: %s, decoded %s, PCF %02X/%02X, command %02X",
             format_hex_pretty(frame.data(), frame.size()).c_str(), YESNO(received_state),
             received.pcf, status_request_pcf_, received.command);
  }
  if (received_state && received.valid) {
    if (!received.reply) {
      awaiting_status_ = false;
      status_followup_ = false;
      apply_received_(received);
      schedule_status_poll_(500);
    } else if (awaiting_status_ && (received.pcf & 0x06U) == (status_request_pcf_ & 0x06U)) {
      if (!status_followup_ || (received.command != 0x04 && status_read_attempts_ < 3)) {
        // The first ACK confirms delivery, but its payload was queued before
        // the request. Give the lamp time to refresh it before reading again.
        // Drain any additional queued command ACKs with bounded read retries.
        awaiting_status_ = false;
        status_followup_ = true;
        schedule_status_poll_(500);
      } else if (received.command == 0x04) {
        awaiting_status_ = false;
        status_followup_ = false;
        status_timeouts_ = 0;
        if (!front_light_->is_transformer_active() && !back_light_->is_transformer_active()) {
          apply_received_(received);
          ESP_LOGD(TAG, "Lamp status: power %s, mode %u/%u, front %u%%, back %u%%, %u K",
                   ONOFF(state_.power), state_.front, state_.back, state_.front_brightness,
                   state_.back_brightness, state_.color_temperature);
        }
      }
    }
  } else if (const char *error = radio_.last_error()) {
    status_set_warning();
    publish_status_(error);
  }

  const uint32_t now = millis();
  if (awaiting_status_ && now - status_request_started_ >= 200) {
    awaiting_status_ = false;
    status_followup_ = false;
    if (status_timeouts_ < 3) ++status_timeouts_;
    ESP_LOGD(TAG, "Lamp status query timed out (%u consecutive, IRQ %08" PRIX32 ")", status_timeouts_,
             radio_.last_irq());
    if (status_timeouts_ >= 3) {
      status_set_warning();
      publish_status_("Lamp status unavailable; retaining last known state");
    }
  }
  if (!status_poll_pending_ || awaiting_status_ || static_cast<int32_t>(now - next_status_poll_) < 0 ||
      front_light_->is_transformer_active() || back_light_->is_transformer_active() ||
      !radio_.idle()) return;
  const bool followup = status_followup_;
  if (followup) {
    ++status_read_attempts_;
  } else {
    status_read_attempts_ = 0;
  }
  status_poll_pending_ = false;
  ESP_LOGD(TAG, "%s lamp status", followup ? "Reading" : "Refreshing");
  if (send_state_(0x04)) {
    awaiting_status_ = true;
    status_request_pcf_ = last_pcf_;
    transmission_ = Transmission::STATUS;
  } else {
    recover_radio_();
  }
}

}  // namespace esphome::halo2
