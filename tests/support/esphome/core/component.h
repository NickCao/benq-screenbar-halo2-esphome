#pragma once

#include <functional>
#include <map>
#include <string>
#include <utility>
#include "hal.h"

namespace esphome {
namespace setup_priority {
constexpr float LATE = -100;
}

class Component {
 public:
  virtual ~Component() = default;
  virtual void setup() {}
  virtual void loop() {}
  virtual void dump_config() {}
  virtual float get_setup_priority() const { return 0; }
  void enable_loop() {}
  void disable_loop() {}
  void status_set_warning() { warning_ = true; }
  void status_clear_warning() { warning_ = false; }
  bool status_has_warning() const { return warning_; }

  void set_timeout(const std::string &name, uint32_t delay_ms, std::function<void()> callback) {
    timers_[name] = {halo2_test::time_us + int64_t(delay_ms) * 1000, std::move(callback)};
  }
  void cancel_timeout(const std::string &name) { timers_.erase(name); }

  // The fixture services named timeouts before loop(), like ESPHome's scheduler.
  void run_timeouts() {
    for (auto it = timers_.begin(); it != timers_.end();) {
      if (it->second.at > halo2_test::time_us) {
        ++it;
        continue;
      }
      auto callback = std::move(it->second.callback);
      timers_.erase(it);
      callback();
      it = timers_.begin();
    }
  }

 private:
  struct Timer {
    int64_t at;
    std::function<void()> callback;
  };
  std::map<std::string, Timer> timers_;
  bool warning_{false};
};

class PollingComponent : public Component {
 public:
  virtual void update() {}
};
}  // namespace esphome
