#pragma once

#include <cstddef>
#include <optional>

namespace esphome::select {
class Select {
 public:
  virtual ~Select() = default;
  void publish_state(size_t index) {
    state = index;
    ++publication_count;
  }
  std::optional<size_t> state;
  size_t publication_count{0};

 protected:
  virtual void control(size_t index) = 0;
};
}  // namespace esphome::select
