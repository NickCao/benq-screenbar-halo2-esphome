#pragma once

namespace halo2_test {
// Keep arguments evaluated so native -Werror also checks production log sites.
template<typename... Args> void log(Args &&...) {}
}  // namespace halo2_test

#define ESP_LOGD(...) ::halo2_test::log(__VA_ARGS__)
#define ESP_LOGI(...) ::halo2_test::log(__VA_ARGS__)
#define ESP_LOGV(...) ::halo2_test::log(__VA_ARGS__)
#define ESP_LOGW(...) ::halo2_test::log(__VA_ARGS__)
#define ESP_LOGCONFIG(...) ::halo2_test::log(__VA_ARGS__)
#define LOG_UPDATE_INTERVAL(...) ::halo2_test::log(__VA_ARGS__)
#define LOG_SELECT(...) ::halo2_test::log(__VA_ARGS__)
#define LOG_NUMBER(...) ::halo2_test::log(__VA_ARGS__)
#define LOG_SPI_DEVICE(...) ::halo2_test::log(TAG, __VA_ARGS__)
#define LOG_PIN(...) ::halo2_test::log(__VA_ARGS__)
#define ONOFF(value) ((value) ? "ON" : "OFF")
#define YESNO(value) ((value) ? "YES" : "NO")
