#pragma once

#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

namespace esphome {
using PreferenceStore = std::map<uint32_t, std::vector<uint8_t>>;

class ESPPreferenceObject {
 public:
  ESPPreferenceObject() = default;
  ESPPreferenceObject(PreferenceStore *store, uint32_t key) : store_(store), key_(key) {}
  template<typename T> bool load(T *value) const {
    if (store_ == nullptr) return false;
    const auto it = store_->find(key_);
    if (it == store_->end() || it->second.size() != sizeof(T)) return false;
    std::memcpy(value, it->second.data(), sizeof(T));
    return true;
  }
  template<typename T> bool save(const T *value) {
    if (store_ == nullptr) return false;
    const auto *bytes = reinterpret_cast<const uint8_t *>(value);
    (*store_)[key_] = {bytes, bytes + sizeof(T)};
    return true;
  }

 private:
  PreferenceStore *store_{nullptr};
  uint32_t key_{0};
};

class ESPPreferences {
 public:
  template<typename T> ESPPreferenceObject make_preference(uint32_t key) { return {&values, key}; }
  bool sync() { return true; }
  PreferenceStore values;
};
inline ESPPreferences test_preferences;
inline ESPPreferences *global_preferences = &test_preferences;
}  // namespace esphome
