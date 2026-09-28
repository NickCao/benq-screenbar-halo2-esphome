#pragma once

#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/components/button/button.h"
#include "esphome/components/light/light_output.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "halo2_protocol.h"
#include "lr1121_radio.h"

namespace esphome::halo2 {

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
  void set_light(light::LightState *value, bool front) { (front ? front_light_ : back_light_) = value; }
  void set_ultrasonic_switch(switch_::Switch *value) { ultrasonic_switch_ = value; }
  void set_radio_status(text_sensor::TextSensor *value) { radio_status_ = value; }
  void set_radio_address_sensor(text_sensor::TextSensor *value) { radio_address_sensor_ = value; }
  void set_auto_discover(bool value) { auto_discover_ = value; }
  void set_radio_address(const halo2_protocol::Address &value) { radio_address_ = value; address_configured_ = true; }
  void set_radio_channel(uint8_t value) { radio_channel_ = value; }
  void set_processing_interval(uint32_t value) { processing_interval_ = value; }
  void set_command_debounce(uint32_t value) { command_debounce_ = value; }
  void start_discovery();
  void start_auto_brightness();

  bool accepts_commands() const { return ready_ && !publishing_ && !discovering_; }
  void control_light(light::LightState *light, bool front);
  void control_ultrasonic(bool value);
  void resend() { if (accepts_commands()) queue_command_(0x02); }

 protected:
  void queue_command_(uint8_t command);
  void publish_lights_();
  void publish_light_(light::LightState *light, bool front);
  void synchronize_temperature_(light::LightState *source);
  void publish_status_(const char *status);
  void save_mode_();
  void apply_received_(const halo2_protocol::HaloRxState &received);
  void publish_address_();
  bool scan_channel_();
  void process_radio_();
  void schedule_status_poll_(uint32_t delay);
  void start_radio_();
  void recover_radio_();
  bool send_state_(uint8_t command, bool auto_brightness = false);

  struct SavedLink {
    halo2_protocol::Address address{};
    uint8_t channel{0};
    uint8_t version{0};
    uint8_t packet_options{0x01};
    uint8_t reserved{0};
  };
  struct Candidate {
    halo2_protocol::Address address{};
    uint8_t channel{0};
    uint8_t count{0};
  };

  light::LightState *front_light_{nullptr};
  light::LightState *back_light_{nullptr};
  switch_::Switch *ultrasonic_switch_{nullptr};
  text_sensor::TextSensor *radio_status_{nullptr};
  text_sensor::TextSensor *radio_address_sensor_{nullptr};
  ESPPreferenceObject mode_preference_;
  ESPPreferenceObject link_preference_;
  halo2_protocol::Address radio_address_{halo2_protocol::RADIO_ADDRESS};
  std::array<Candidate, 4> candidates_{};
  bool scan_expired_{false};
  uint8_t scan_step_{0};
  uint8_t radio_channel_{halo2_protocol::RADIO_CHANNEL};
  bool address_configured_{false};
  bool auto_discover_{false};
  bool discovering_{false};
  enum class Transmission { NONE, COMMAND, STATUS };
  LR1121Radio radio_;
  uint8_t app_pid_{0};
  uint8_t last_pcf_{0};
  Transmission transmission_{Transmission::NONE};
  uint32_t recovery_delay_{1000};
  bool recovering_{false};
  bool scan_pending_{false};
  halo2_protocol::HaloRxState state_;
  uint32_t frequency_deviation_{160000};
  uint8_t pulse_shape_{0x09};
  uint8_t pending_command_{0};
  uint32_t command_debounce_{1000};
  uint32_t command_sent_at_{0};
  bool command_sent_{false};
  bool pending_auto_brightness_{false};
  uint32_t processing_interval_{50};
  uint32_t last_process_at_{0};
  bool status_poll_pending_{false};
  uint32_t next_status_poll_{0};
  uint32_t status_request_started_{0};
  uint8_t status_request_pcf_{0};
  uint8_t status_read_attempts_{0};
  uint8_t status_timeouts_{0};
  bool awaiting_status_{false};
  bool status_followup_{false};
  uint8_t saved_mode_{3};
  bool ready_{false};
  bool publishing_{false};
};

class Halo2Light : public light::LightOutput {
 public:
  Halo2Light(Halo2 *parent, bool front) : parent_(parent), front_(front) {}
  light::LightTraits get_traits() override;
  void setup_state(light::LightState *state) override { parent_->set_light(state, front_); }
  void update_state(light::LightState *state) override {
    // Capture local commands immediately, before polling can receive a packet.
    // The guard also prevents received state from becoming a new transmission.
    forward_update_ = parent_->accepts_commands();
    if (forward_update_) parent_->control_light(state, front_);
  }
  void write_state(light::LightState *state) override {
    // ESPHome installs the final transition values after update_state().
    // Reconcile those here, retaining the origin of the deferred write.
    if (forward_update_) parent_->control_light(state, front_);
    forward_update_ = false;
  }

 protected:
  Halo2 *parent_;
  bool front_;
  bool forward_update_{false};
};

class Halo2UltrasonicSwitch : public switch_::Switch {
 public:
  explicit Halo2UltrasonicSwitch(Halo2 *parent) : parent_(parent) {}

 protected:
  void write_state(bool value) override { parent_->control_ultrasonic(value); }
  Halo2 *parent_;
};

class Halo2DiscoverButton : public button::Button {
 public:
  explicit Halo2DiscoverButton(Halo2 *parent) : parent_(parent) {}

 protected:
  void press_action() override { parent_->start_discovery(); }
  Halo2 *parent_;
};

class Halo2AutoButton : public button::Button {
 public:
  explicit Halo2AutoButton(Halo2 *parent) : parent_(parent) {}

 protected:
  void press_action() override { parent_->start_auto_brightness(); }
  Halo2 *parent_;
};

}  // namespace esphome::halo2
