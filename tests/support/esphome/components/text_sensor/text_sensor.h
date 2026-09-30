#pragma once

#include <string>
#include <utility>
#include <vector>
#include "esphome/core/hal.h"

namespace esphome::text_sensor {
class TextSensor {
 public:
  void publish_state(const std::string &value) {
    state = value;
    history.emplace_back(millis(), value);
  }
  std::string state;
  std::vector<std::pair<uint32_t, std::string>> history;
};
}  // namespace esphome::text_sensor
