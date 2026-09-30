#pragma once

#include <span>
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"
#include "esphome/components/button/button.h"
#include "esphome/components/light/light_output.h"
#include "esphome/components/select/select.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "bridge_state.h"
#include "command_queue.h"
#include "halo2_protocol.h"
#include "lr1121_radio.h"

namespace esphome::halo2 {

class Halo2Light;

class Halo2 : public PollingComponent {
 public:
  void setup() override;
  void update() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  LR1121Radio &get_radio() { return radio_; }
  void set_frequency_deviation(uint32_t value) { frequency_deviation_ = value; }
  void set_pulse_shape(uint8_t value) { pulse_shape_ = value; }
  void set_light(Halo2Light *value, Section section) { lights_[static_cast<size_t>(section)] = value; }
  void set_ultrasonic_select(select::Select *value) { ultrasonic_select_ = value; }
  void set_radio_status(text_sensor::TextSensor *value) { radio_status_ = value; }
  void set_radio_address_sensor(text_sensor::TextSensor *value) { radio_address_sensor_ = value; }
  void set_auto_discover(bool value) { auto_discover_ = value; }
  void set_radio_address(const halo2_protocol::Address &value) {
    radio_address_ = value;
    address_configured_ = true;
  }
  void set_radio_channel(uint8_t value) { radio_channel_ = value; }
  void set_processing_interval(uint32_t value) { processing_interval_ = value; }
  void set_command_debounce(uint32_t value) { commands_.set_debounce(value); }
  void start_discovery();
  void start_auto_brightness();

  bool accepts_commands() const { return lifecycle_.active(); }
  void control_light(Halo2Light &source);
  void control_ultrasonic(size_t index);
  void resend() {
    if (accepts_commands()) commands_.resend();
  }

 protected:
  // Persisted selection is a bitmask, distinct from LampSelection's wire values.
  enum SavedMode : uint8_t { NONE = 0, FRONT = 1, BACK = 2, BOTH = FRONT | BACK };
  enum class TxOwner { NONE, COMMAND, STATUS_POLL };
  static constexpr uint32_t RECOVERY_INITIAL_DELAY_MS = 1000;

  void request_state_(const LampState &requested);
  bool lights_transitioning_() const;
  void publish_lights_();
  void publish_ultrasonic_();
  void publish_status_(const char *status);
  void save_mode_();
  bool apply_received_(const halo2_protocol::ReceivedPacket &received);
  void publish_address_();
  void scan_channel_();
  void process_discovery_();
  void process_radio_();
  void send_commands_();
  void receive_packet_();
  void poll_status_();
  void start_radio_();
  void radio_ready_();
  void recover_radio_();
  void cancel_requests_();
  bool send_batch_(std::span<const halo2_protocol::AirFrame> frames, TxOwner owner);
  halo2_protocol::AirFrame make_frame_(Command command, bool auto_brightness = false);

  struct SavedLink {
    halo2_protocol::Address address{};
    uint8_t channel{0};
    uint8_t version{0};
    // Same byte and values as the former packet_options field (version 2).
    UltrasonicTimeout ultrasonic_timeout{UltrasonicTimeout::MINUTES_5};
    uint8_t reserved{0};
  };
  static_assert(sizeof(SavedLink) == 8);
  struct Candidate {
    halo2_protocol::Address address{};
    uint8_t channel{0};
    uint8_t count{0};
  };

  std::array<Halo2Light *, 2> lights_{};
  select::Select *ultrasonic_select_{nullptr};
  text_sensor::TextSensor *radio_status_{nullptr};
  text_sensor::TextSensor *radio_address_sensor_{nullptr};
  ESPPreferenceObject mode_preference_;
  ESPPreferenceObject link_preference_;
  halo2_protocol::Address radio_address_{halo2_protocol::RADIO_ADDRESS};
  std::array<Candidate, 4> candidates_{};
  enum class DiscoveryPhase { CHANNEL_PENDING, LISTENING, EXPIRED };
  DiscoveryPhase discovery_phase_{DiscoveryPhase::CHANNEL_PENDING};
  uint8_t scan_step_{0};
  uint8_t radio_channel_{halo2_protocol::RADIO_CHANNEL};
  bool address_configured_{false};
  bool auto_discover_{false};
  BridgeLifecycle lifecycle_;
  LR1121Radio radio_;
  TxOwner tx_owner_{TxOwner::NONE};
  uint8_t app_pid_{0};
  uint8_t last_pcf_{0};
  uint32_t recovery_delay_{RECOVERY_INITIAL_DELAY_MS};
  LampStateModel lamp_state_;
  uint32_t frequency_deviation_{LR1121Radio::DEFAULT_DEVIATION_HZ};
  uint8_t pulse_shape_{LR1121Radio::DEFAULT_PULSE_SHAPE};
  CommandQueue commands_;
  uint32_t processing_interval_{50};
  uint32_t last_process_at_{0};
  StatusPoll status_poll_;
  uint8_t saved_mode_{SavedMode::BOTH};
};

class Halo2Light : public light::LightOutput, public Parented<Halo2> {
 public:
  Halo2Light(Halo2 *parent, Section section) : Parented<Halo2>(parent), section_(section) {}
  light::LightTraits get_traits() override;
  void setup_state(light::LightState *state) override {
    state_ = state;
    parent_->set_light(this, section_);
  }
  void update_state(light::LightState *) override {
    // Capture local commands immediately, before polling can receive a packet.
    // The guard also prevents received state from becoming a new transmission.
    local_write_ = !reflecting_state_ && parent_->accepts_commands();
    if (local_write_) parent_->control_light(*this);
  }
  void write_state(light::LightState *) override {
    // ESPHome installs the final transition values after update_state().
    // Reconcile those here, retaining the origin of the deferred write.
    if (local_write_) parent_->control_light(*this);
    local_write_ = false;
  }

 protected:
  friend class Halo2;
  void restore_into(LampState &state) const;
  void read_into(LampState &state) const;
  void publish(const LampState &state);
  bool needs_temperature_sync(uint16_t temperature) const;
  void sync_temperature(uint16_t temperature);
  bool transitioning() const;

  Section section_;
  light::LightState *state_{nullptr};
  bool local_write_{false};
  bool reflecting_state_{false};
};

class Halo2UltrasonicSelect : public select::Select, public Parented<Halo2> {
 public:
  using Parented<Halo2>::Parented;

 protected:
  void control(size_t index) override { parent_->control_ultrasonic(index); }
};

class Halo2DiscoverButton : public button::Button, public Parented<Halo2> {
 public:
  using Parented<Halo2>::Parented;

 protected:
  void press_action() override { parent_->start_discovery(); }
};

class Halo2AutoButton : public button::Button, public Parented<Halo2> {
 public:
  using Parented<Halo2>::Parented;

 protected:
  void press_action() override { parent_->start_auto_brightness(); }
};

}  // namespace esphome::halo2
