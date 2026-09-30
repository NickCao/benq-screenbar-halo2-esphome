#pragma once

#include <cstddef>
#include <optional>

namespace esphome::select {
class Select {
 public:
  virtual ~Select() = default;
  void publish_state(size_t index) { state = index; }
  std::optional<size_t> state;

 protected:
  virtual void control(size_t index) = 0;
};
}  // namespace esphome::select
