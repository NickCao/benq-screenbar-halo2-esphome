#pragma once

#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/components/light/light_output.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "halo2_protocol.h"

namespace esphome::halo2 {

class Halo2 : public PollingComponent {
 public:
  void setup() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  void set_frequency_deviation(uint32_t value) { frequency_deviation_ = value; }
  void set_pulse_shape(uint8_t value) { pulse_shape_ = value; }
  void set_light(light::LightState *value, bool front) { (front ? front_light_ : back_light_) = value; }
  void set_power_switch(switch_::Switch *value) { power_switch_ = value; }
  void set_ultrasonic_switch(switch_::Switch *value) { ultrasonic_switch_ = value; }
  void set_radio_status(text_sensor::TextSensor *value) { radio_status_ = value; }

  bool accepts_commands() const { return ready_ && !publishing_; }
  void control_light(light::LightState *light, bool front);
  void control_switch(bool power, bool value);
  void resend() { if (ready_) queue_command_(0x03); }

 protected:
  void queue_command_(uint8_t command);
  void publish_lights_();
  void publish_light_(light::LightState *light, bool front);
  void synchronize_temperature_(light::LightState *source);
  void publish_switches_();
  void publish_status_(const char *status);
  void save_mode_();

  light::LightState *front_light_{nullptr};
  light::LightState *back_light_{nullptr};
  switch_::Switch *power_switch_{nullptr};
  switch_::Switch *ultrasonic_switch_{nullptr};
  text_sensor::TextSensor *radio_status_{nullptr};
  ESPPreferenceObject mode_preference_;
  halo2_protocol::HaloRxState state_;
  uint32_t frequency_deviation_{160000};
  uint8_t pulse_shape_{0x09};
  uint8_t pending_command_{0};
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

class Halo2Switch : public switch_::Switch {
 public:
  Halo2Switch(Halo2 *parent, bool power) : parent_(parent), power_(power) {}
  bool assumed_state() override { return true; }

 protected:
  void write_state(bool value) override { parent_->control_switch(power_, value); }
  Halo2 *parent_;
  bool power_;
};

}  // namespace esphome::halo2
