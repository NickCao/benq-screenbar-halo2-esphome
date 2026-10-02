#include "halo2.h"

#include <algorithm>
#include <cinttypes>
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::halo2 {

namespace protocol = halo2_protocol;

static const char *const TAG = "halo2";
static constexpr uint32_t MODE_PREFERENCE_KEY = 0x48414C32, LINK_PREFERENCE_KEY = 0x48414C33;
static constexpr uint8_t SAVED_LINK_VERSION = 2;
static constexpr uint32_t RECOVERY_MAX_DELAY_MS = 30000;
static constexpr uint32_t DISCOVERY_DWELL_MS = 3000, DISCOVERY_STATUS_DELAY_MS = 1000;
static constexpr uint8_t DISCOVERY_REQUIRED_CAPTURES = 3;
static constexpr std::array<uint8_t, 2> DISCOVERY_PREAMBLES{0xAA, 0x55};

void Halo2::setup() {
  disable_loop();
  // LightState restores its preferences before this component is initialized.
  // Restore the bridge's last known state without transmitting on boot.
  mode_preference_ = global_preferences->make_preference<uint8_t>(MODE_PREFERENCE_KEY);
  if (!mode_preference_.load(&saved_mode_) || saved_mode_ < SavedMode::FRONT || saved_mode_ > SavedMode::BOTH)
    saved_mode_ = SavedMode::BOTH;
  LampState initial;
  initial.set_selection(saved_mode_ & SavedMode::FRONT, saved_mode_ & SavedMode::BACK);
  initial.front_brightness = initial_brightness_[0];
  initial.back_brightness = initial_brightness_[1];
  light_->restore_into(initial);
  lamp_state_ = initial;
  save_mode_();
  publish_light_();
  // Read the sensor's enable/timeout settings from the lamp before accepting
  // commands. Restoring a UI default must not disable the sensor on boot.

  link_preference_ = global_preferences->make_preference<SavedLink>(LINK_PREFERENCE_KEY);
  SavedLink saved{};
  const bool restored = !address_configured_ && link_preference_.load(&saved) && saved.version == SAVED_LINK_VERSION &&
                        saved.ultrasonic_timeout <= UltrasonicTimeout::MINUTES_15 &&
                        std::find(protocol::RADIO_CHANNELS.begin(), protocol::RADIO_CHANNELS.end(), saved.channel) !=
                            protocol::RADIO_CHANNELS.end();
  if (restored) {
    radio_address_ = saved.address;
    radio_channel_ = saved.channel;
    lamp_state_.ultrasonic_timeout = saved.ultrasonic_timeout;
  }
  lifecycle_ = BridgeLifecycle(auto_discover_ && !address_configured_ && !restored);
  // Setup, reception, transmission and recovery all advance in loop().
  enable_loop();
  start_radio_();
}

void Halo2::start_radio_() {
  if (!lifecycle_.initializing() && !lifecycle_.retry()) return;
  publish_status_("Initializing radio");
  if (!radio_.setup(frequency_deviation_, pulse_shape_, radio_address_, radio_channel_)) recover_radio_();
}

void Halo2::cancel_requests_() {
  // Clearing the request phases also makes a late batch completion harmless.
  commands_.clear();
  status_poll_.cancel();
}

void Halo2::recover_radio_() {
  if (!lifecycle_.on_radio_failure()) return;
  // Never replay an interrupted command batch after reconnecting to the lamp.
  cancel_requests_();
  cancel_timeout("discovery_dwell");
  status_set_warning();
  const char *error = radio_.last_error();
  if (error == nullptr) error = "LR1121 unavailable";
  char status[160];
  snprintf(status, sizeof(status), "%s; retrying in %" PRIu32 " s", error, recovery_delay_ / 1000);
  publish_status_(status);
  ESP_LOGW(TAG, "%s", status);
  set_timeout("radio_recovery", recovery_delay_, [this]() { start_radio_(); });
  recovery_delay_ = std::min(recovery_delay_ * 2, RECOVERY_MAX_DELAY_MS);
}

void Halo2::publish_address_() {
  char address[80];
  snprintf(address, sizeof(address), "%02X:%02X:%02X:%02X / %u MHz", radio_address_[0], radio_address_[1],
           radio_address_[2], radio_address_[3], protocol::RADIO_BASE_FREQUENCY_MHZ + radio_channel_);
  if (radio_address_sensor_ != nullptr) radio_address_sensor_->publish_state(address);
  ESP_LOGI(TAG, "Radio address %s (register order)", address);
}

void Halo2::start_discovery() {
  if (!lifecycle_.begin_discovery()) return;
  cancel_requests_();
  cancel_timeout("discovery_dwell");
  candidates_ = {};
  scan_step_ = 0;
  discovery_phase_ = DiscoveryPhase::CHANNEL_PENDING;
  ESP_LOGI(TAG, "Discovery: adjust the controller brightness near the board");
}

void Halo2::scan_channel_() {
  const uint8_t channel = protocol::RADIO_CHANNELS[scan_step_ / DISCOVERY_PREAMBLES.size()];
  const uint8_t sync = DISCOVERY_PREAMBLES[scan_step_ % DISCOVERY_PREAMBLES.size()];
  if (!radio_.listen_for_address(channel, sync)) {
    recover_radio_();
    return;
  }
  discovery_phase_ = DiscoveryPhase::LISTENING;
  set_timeout("discovery_dwell", DISCOVERY_DWELL_MS, [this]() { discovery_phase_ = DiscoveryPhase::EXPIRED; });
  char status[80];
  snprintf(status, sizeof(status), "Discovering at %u MHz; adjust controller brightness",
           protocol::RADIO_BASE_FREQUENCY_MHZ + channel);
  publish_status_(status);
  ESP_LOGD(TAG, "Scanning %u MHz, preamble %02X", protocol::RADIO_BASE_FREQUENCY_MHZ + channel, sync);
}

void Halo2::radio_ready_() {
  lifecycle_.on_radio_ready();
  recovery_delay_ = RECOVERY_INITIAL_DELAY_MS;
  status_poll_.reset_failures();
  status_clear_warning();
  ESP_LOGI(TAG, "LR1121 firmware %04X, %u MHz GFSK; radio ready", radio_.firmware_version(),
           protocol::RADIO_BASE_FREQUENCY_MHZ + radio_channel_);
  if (lifecycle_.discovering()) {
    start_discovery();
  } else {
    publish_address_();
    publish_status_("Listening");
    // Read the lamp after recovery instead of imposing the restored UI state.
    status_poll_.schedule(millis(), StatusPoll::SETTLE_MS);
  }
}

void Halo2::loop() {
  if (lifecycle_.recovering()) return;
  radio_.loop();
  if (radio_.last_error() != nullptr) {
    recover_radio_();
    return;
  }
  if (lifecycle_.initializing()) {
    if (!radio_.ready()) return;
    radio_ready_();
  }
  if (radio_.take_tx_done()) {
    const uint32_t now = millis();
    if (commands_.on_tx_done(now)) {
      status_poll_.schedule(now, StatusPoll::SETTLE_MS);
      status_clear_warning();
      publish_status_("Command sent");
    } else {
      status_poll_.on_tx_done(now);
    }
  }
  if (lifecycle_.discovering()) {
    process_discovery_();
    return;
  }
  // Fast command/RX processing is independent of periodic lamp polling.
  const uint32_t now = App.get_loop_component_start_time();
  if (now - last_process_at_ >= processing_interval_) {
    last_process_at_ = now;
    process_radio_();
  }
}

void Halo2::process_discovery_() {
  if (!radio_.idle()) return;
  if (discovery_phase_ == DiscoveryPhase::CHANNEL_PENDING) {
    scan_channel_();
    return;
  }
  protocol::Address address{};
  protocol::ReceivedPacket received;
  const uint32_t previous_count = radio_.capture_count();
  const bool found = radio_.poll_address(address, received);
  if (radio_.capture_count() != previous_count) {
    const auto &data = radio_.capture_data();
    ESP_LOGV(TAG, "Discovery RX: %s", format_hex_pretty(data.data(), data.size()).c_str());
  }
  if (found) {
    const auto &state = received.state;
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
    ESP_LOGI(TAG, "Discovery candidate %02X:%02X:%02X:%02X at %u MHz: %u/%u CRC-valid captures", address[0], address[1],
             address[2], address[3], protocol::RADIO_BASE_FREQUENCY_MHZ + channel, candidate->count,
             DISCOVERY_REQUIRED_CAPTURES);
    ESP_LOGD(TAG, "Captured state: front %u%%, rear %u%%, %u K, mode %u/%u", state.front_brightness,
             state.back_brightness, state.color_temperature, state.selected(Section::FRONT),
             state.selected(Section::BACK));
    if (candidate->count >= DISCOVERY_REQUIRED_CAPTURES) {
      if (!radio_.use_address(address, channel)) {
        recover_radio_();
        return;
      }
      radio_address_ = address;
      radio_channel_ = channel;
      cancel_timeout("discovery_dwell");
      const bool persisted = apply_received_(received) && global_preferences->sync();
      status_poll_.schedule(millis(), DISCOVERY_STATUS_DELAY_MS);
      publish_address_();
      publish_status_(persisted ? "Address discovered and saved" : "Address discovered; save failed");
      if (!persisted) status_set_warning();
      ESP_LOGI(TAG, "Discovery complete. radio_address: [0x%02X, 0x%02X, 0x%02X, 0x%02X], radio_channel: %u",
               address[0], address[1], address[2], address[3], channel);
      return;
    }
  }
  if (discovery_phase_ == DiscoveryPhase::EXPIRED) {
    ESP_LOGD(TAG, "Discovery captured %" PRIu32 " buffers", radio_.capture_count());
    scan_step_ = (scan_step_ + 1) % (protocol::RADIO_CHANNELS.size() * DISCOVERY_PREAMBLES.size());
    discovery_phase_ = DiscoveryPhase::CHANNEL_PENDING;
  }
}

void Halo2::dump_config() {
  ESP_LOGCONFIG(TAG, "ScreenBar HALO 2:");
  radio_.dump_config();
  ESP_LOGCONFIG(TAG, "  Radio: LR1121, deviation: %" PRIu32 " Hz, pulse shape: 0x%02X", frequency_deviation_,
                pulse_shape_);
  ESP_LOGCONFIG(TAG, "  Processing interval: %" PRIu32 " ms", processing_interval_);
  ESP_LOGCONFIG(TAG, "  Frequency: %u MHz", protocol::RADIO_BASE_FREQUENCY_MHZ + radio_channel_);
  ESP_LOGCONFIG(TAG, "  Command debounce: %" PRIu32 " ms", commands_.debounce());
  LOG_UPDATE_INTERVAL(this);
  LOG_NUMBER("  ", "Front brightness", brightness_numbers_[0]);
  LOG_NUMBER("  ", "Back brightness", brightness_numbers_[1]);
  LOG_SELECT("  ", "Lighting mode", section_select_);
  LOG_SELECT("  ", "Ultrasonic sensor", ultrasonic_select_);
}

void Halo2::request_state_(const LampState &requested) {
  commands_.request(lamp_state_, requested);
  lamp_state_ = requested;
  save_mode_();
}

bool Halo2::send_batch_(std::span<const protocol::AirFrame> frames) {
  if (!radio_.send_batch(frames)) {
    recover_radio_();
    return false;
  }
  return true;
}

protocol::AirFrame Halo2::make_frame_(Command command, bool auto_brightness) {
  const auto &requested = lamp_state_;
  ESP_LOGD(TAG, "TX command 0x%02X, power %s, mode %u/%u, front %u%%, back %u%%, %u K", command, ONOFF(requested.power),
           requested.selected(Section::FRONT), requested.selected(Section::BACK), requested.front_brightness,
           requested.back_brightness, requested.color_temperature);
  const auto payload = protocol::make_payload(command, requested, auto_brightness);
  last_pcf_ = protocol::request_pcf(app_pid_++);
  return protocol::make_air_frame(last_pcf_, payload, radio_.address());
}

void Halo2::start_auto_brightness() {
  if (!accepts_commands()) return;
  commands_.auto_brightness();
}

void Halo2::control_ultrasonic(size_t index) {
  if (!accepts_commands() || index > static_cast<size_t>(UltrasonicTimeout::MINUTES_15) + 1U) return;
  auto requested = lamp_state_;
  requested.ultrasonic_enabled = index != 0;
  if (requested.ultrasonic_enabled) requested.ultrasonic_timeout = static_cast<UltrasonicTimeout>(index - 1);
  request_state_(requested);
  publish_ultrasonic_();
}

void Halo2::publish_ultrasonic_() {
  // Disabling presence detection retains the lamp's last timeout duration.
  const auto &requested = lamp_state_;
  ultrasonic_select_->publish_state(requested.ultrasonic_enabled ? static_cast<size_t>(requested.ultrasonic_timeout) + 1
                                                                 : size_t{0});
}

void Halo2::publish_status_(const char *status) {
  if (status == nullptr) status = "Radio error";
  if (radio_status_->state != status) radio_status_->publish_state(status);
}

void Halo2::save_mode_() {
  const auto &requested = lamp_state_;
  const uint8_t mode = (requested.selected(Section::FRONT) ? SavedMode::FRONT : SavedMode::NONE) |
                       (requested.selected(Section::BACK) ? SavedMode::BACK : SavedMode::NONE);
  if (mode == saved_mode_) return;
  saved_mode_ = mode;
  mode_preference_.save(&saved_mode_);
}

bool Halo2::apply_received_(const protocol::ReceivedPacket &received) {
  const auto &state = received.state;
  const bool changed = !lifecycle_.active() || lamp_state_ != state;
  lamp_state_ = state;
  lifecycle_.on_received_state();
  // Persist received settings even when they match an optimistic local change.
  // ESPHome skips flash writes when the stored preference is unchanged.
  const SavedLink saved{radio_address_, radio_channel_, SAVED_LINK_VERSION, state.ultrasonic_timeout, 0};
  const bool persisted = link_preference_.save(&saved);
  if (changed) {
    save_mode_();
    publish_light_();
    publish_ultrasonic_();
  }
  status_clear_warning();
  publish_status_(received.is_reply() ? "Lamp status received" : "Controller update received");
  return persisted;
}

void Halo2::update() {
  if (lifecycle_.linked() && commands_.idle() && status_poll_.idle()) status_poll_.schedule(millis(), 0);
}

void Halo2::process_radio_() {
  if (!lifecycle_.linked() || commands_.transmitting() || status_poll_.transmitting()) return;
  if (commands_.pending()) {
    send_commands_();
    return;
  }
  receive_packet_();
  poll_status_();
}

void Halo2::send_commands_() {
  if (!radio_.idle()) return;
  const auto batch = commands_.take(lamp_state_, millis());
  if (!batch) return;
  status_poll_.cancel();
  std::array<protocol::AirFrame, 2> frames;
  std::transform(batch->begin(), batch->end(), frames.begin(), [this, &batch](Command command) {
    return make_frame_(command, batch->auto_brightness && command == Command::SETTINGS);
  });
  send_batch_(std::span(frames).first(batch->count));
}

void Halo2::receive_packet_() {
  protocol::ReceivedPacket received;
  const uint32_t previous_rx_count = radio_.rx_count();
  const bool received_state = radio_.poll(received);
  if (radio_.rx_count() != previous_rx_count) {
    const auto &frame = radio_.rx_frame();
    // Read header bytes directly so rejected packets still show their actual
    // command and control bits. State validation must not filter diagnostics.
    const uint8_t pcf = static_cast<uint8_t>((frame[0] << 1U) | (frame[1] >> 7U));
    const uint8_t command = static_cast<uint8_t>((frame[1] << 1U) | (frame[2] >> 7U));
    const uint8_t control = static_cast<uint8_t>((frame[2] << 1U) | (frame[3] >> 7U));
    ESP_LOGD(TAG, "RX air frame: %s, PCF %02X, command %02X, control %02X, decoded %s",
             format_hex_pretty(frame.data(), frame.size()).c_str(), pcf, command, control, YESNO(received_state));
  }
  if (!received_state) {
    if (const char *error = radio_.last_error()) {
      status_set_warning();
      publish_status_(error);
    }
    return;
  }
  if (!received.is_reply()) {
    apply_received_(received);
    status_poll_.schedule(millis(), StatusPoll::SETTLE_MS);
    return;
  }
  const auto reply =
      status_poll_.on_reply(received.pcf & protocol::PCF_PID_MASK, received.command == Command::STATUS, millis());
  if (reply != StatusPoll::Reply::STATE || light_->transitioning()) return;
  apply_received_(received);
  const auto &state = received.state;
  ESP_LOGD(TAG, "Lamp status: power %s, mode %u/%u, front %u%%, back %u%%, %u K, ultrasonic %s, timeout %u min",
           ONOFF(state.power), state.selected(Section::FRONT), state.selected(Section::BACK), state.front_brightness,
           state.back_brightness, state.color_temperature, ONOFF(state.ultrasonic_enabled),
           ULTRASONIC_TIMEOUT_MINUTES[static_cast<size_t>(state.ultrasonic_timeout)]);
}

void Halo2::poll_status_() {
  const uint32_t now = millis();
  if (status_poll_.expire(now)) {
    ESP_LOGD(TAG, "Lamp status query timed out (%u consecutive, IRQ %08" PRIX32 ")", status_poll_.failures(),
             radio_.last_irq());
    if (status_poll_.failures() >= StatusPoll::FAILURE_THRESHOLD) {
      status_set_warning();
      publish_status_("Lamp status unavailable; retaining last known state");
    }
  }
  if (!status_poll_.due(now) || light_->transitioning() || !radio_.idle()) return;
  ESP_LOGD(TAG, "%s lamp status", status_poll_.reading() ? "Reading" : "Refreshing");
  const std::array<protocol::AirFrame, 1> frames{make_frame_(Command::STATUS)};
  if (send_batch_(frames)) {
    status_poll_.begin_request(last_pcf_ & protocol::PCF_PID_MASK);
  }
}

}  // namespace esphome::halo2
