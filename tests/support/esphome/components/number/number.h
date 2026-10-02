#pragma once

#include <optional>

namespace esphome::number {
class Number {
 public:
  virtual ~Number() = default;
  void publish_state(float value) { state = value; }
  std::optional<float> state;

 protected:
  virtual void control(float value) = 0;
};
}  // namespace esphome::number
