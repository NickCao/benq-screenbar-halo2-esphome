#pragma once

#include <array>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include "esphome/components/spi/spi.h"
#include "esphome/core/gpio.h"
#include "esphome/core/hal.h"
#include "esp_timer.h"
#include "halo2_protocol.h"

namespace esphome::halo2 {
// LR1121 User Manual rev. 1.2, sections 3, 4, 6, 7 and 8.5.
// Reset, BUSY and TX completion advance across loop calls without sleeping.
class LR1121Radio : public spi::SPIDevice<spi::BIT_ORDER_MSB_FIRST, spi::CLOCK_POLARITY_LOW,
                                         spi::CLOCK_PHASE_LEADING, spi::DATA_RATE_1MHZ> {
 public:
  void set_reset_pin(InternalGPIOPin *pin) { reset_pin_ = pin; }
  void set_busy_pin(InternalGPIOPin *pin) { busy_pin_ = pin; }
  void set_irq_pin(InternalGPIOPin *pin) { irq_pin_ = pin; }

  void dump_config() {
    static const char *const TAG = "halo2";
    LOG_SPI_DEVICE(this);
    LOG_PIN("  Reset Pin: ", reset_pin_);
    LOG_PIN("  Busy Pin: ", busy_pin_);
    LOG_PIN("  IRQ Pin: ", irq_pin_);
  }

  bool ready() const { return ready_; }
  bool idle() const { return operation_ == Operation::IDLE; }
  const char *last_error() const { return error_[0] ? error_ : nullptr; }
  uint16_t firmware_version() const { return firmware_version_; }
  const halo2_protocol::Address &address() const { return address_; }
  uint8_t channel() const { return channel_; }
  bool discovering() const { return discovering_; }
  uint32_t capture_count() const { return capture_count_; }
  uint32_t rx_count() const { return rx_count_; }
  uint32_t last_irq() const { return last_irq_; }
  const halo2_protocol::AirFrame &rx_frame() const { return rx_frame_; }
  const std::array<uint8_t, 25> &capture_data() const { return capture_data_; }

  bool take_tx_done() {
    const bool completed = tx_completed_;
    tx_completed_ = false;
    return completed;
  }

  // True means initialization was started; ready() becomes true later.
  bool setup(uint32_t deviation_hz = 160000, uint8_t pulse_shape = 0x09,
             const halo2_protocol::Address &address = halo2_protocol::RADIO_ADDRESS,
             uint8_t channel = halo2_protocol::RADIO_CHANNEL) {
    ready_ = false;
    discovering_ = false;
    rx_pending_ = false;
    needs_receive_ = false;
    tx_completed_ = false;
    tx_frame_count_ = 0;
    address_ = address;
    channel_ = channel;
    firmware_version_ = 0;
    error_[0] = '\0';
    // 125 kbps + 2*fdev must fit inside the largest 467 kHz RX bandwidth.
    if (deviation_hz == 0 || deviation_hz >= 171000 ||
        (pulse_shape != 0 && (pulse_shape < 0x08 || pulse_shape > 0x0B)))
      return fail_("invalid modulation settings");
    if (parent_->is_failed()) return fail_("SPI bus unavailable");
    // Recovery resets the radio while retaining the registered SPI device.
    if (!registered_) {
      busy_pin_->setup();
      irq_pin_->setup();
      reset_pin_->digital_write(true);
      reset_pin_->setup();
      spi_setup();
      if (!release_device_ && !spi_is_ready()) {
        spi_teardown();
        return fail_("SPI initialization failed");
      }
      registered_ = true;
    }
    reset_pin_->digital_write(false);
    begin_(Operation::RESET_LOW);
    read_(0x0101, {}, 4, Response::VERSION);

    // DIO5/DIO6 keep the unused sub-GHz PA isolated on the antenna switch's
    // receive path. RFIO_HF uses a separate connector. Preserve this on reset.
    write_(0x011C, {0x00});
    write_(0x0110, {0x01});  // DC-DC regulator
    write_(0x0112, {0x03, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00});
    write_(0x0117, {0x06, 0x00, 0x01, 0x2C});  // 3.0 V TCXO, 300 RTC ticks
    write_(0x010E, {});  // clear the expected pre-TCXO startup errors
    write_(0x010F, {0x3F}, 100000);
    read_(0x010D, {}, 2, Response::ERRORS);

    const uint32_t frequency = 2400000000UL + channel_ * 1000000UL;
    write_(0x020E, {0x01});  // GFSK
    write_(0x020B, {byte_(frequency, 24), byte_(frequency, 16), byte_(frequency, 8), byte_(frequency, 0)});
    write_(0x020F, {0x00, 0x01, 0xE8, 0x48, pulse_shape, 0x09,
                    byte_(deviation_hz, 24), byte_(deviation_hz, 16), byte_(deviation_hz, 8), byte_(deviation_hz, 0)});
    // 32-bit TX preamble and address sync; no RX preamble gate so short lamp
    // ACKs survive turnaround. Software handles the 9-bit PCF and CRC.
    // No whitening, hardware CRC, length byte, address filter, or auto-ACK.
    const auto air = halo2_protocol::air_address(address_);
    write_(0x0210, {0x00, 0x20, 0x00, 0x20, 0x00, 0x00, halo2_protocol::AIR_FRAME_SIZE, 0x01, 0x00});
    write_(0x0206, {air[0], air[1], air[2], air[3], 0, 0, 0, 0});
    write_(0x0215, {0x02, 0x00, 0x04, 0x00});  // HF PA, VREG, Waveshare duty cycle
    write_(0x0211, {0x00, 0x02});  // 0 dBm, 48 us ramp
    write_(0x0213, {0x01});  // fall back to STBY_RC
    write_(0x0227, {0x01});  // boosted RX
    // DIO9 -> GPIO38. DIO11 remains available for the board's LF crystal.
    write_(0x0113, {byte_(IRQ_MASK, 24), byte_(IRQ_MASK, 16), byte_(IRQ_MASK, 8), byte_(IRQ_MASK, 0),
                    0, 0, 0, 0});
    receive_commands_();
    return operation_ != Operation::FAILED;
  }

  void loop() {
    // At 1 MHz each transfer is at most 32 bytes. Yield on BUSY immediately,
    // and bound work when several commands finish without asserting it.
    for (unsigned step = 0; step < 4; ++step)
      if (!advance_()) break;
  }

  // Queue one frame, or append the second half of a power-on batch before
  // loop() starts it. Completion is reported only after the whole batch.
  bool send(const halo2_protocol::AirFrame &frame) {
    if (!ready_ || discovering_) return false;
    if (operation_ == Operation::TRANSMIT && job_index_ == 0 && !response_phase_ &&
        tx_frame_index_ == 0 && tx_frame_count_ == 1) {
      tx_frames_[tx_frame_count_++] = frame;
      return true;
    }
    if (!idle()) return false;
    rx_pending_ = false;
    needs_receive_ = false;
    tx_completed_ = false;
    tx_frames_[0] = frame;
    tx_frame_index_ = 0;
    tx_frame_count_ = 1;
    transmit_commands_();
    return operation_ != Operation::FAILED;
  }

  bool poll(halo2_protocol::HaloRxState &state) {
    state = {};
    if (!ready_ || discovering_ || !consume_frame_()) return false;
    ++rx_count_;
    return halo2_protocol::decode_air_frame(rx_frame_.data(), rx_frame_.size(), state, address_, true);
  }

  bool listen_for_address(uint8_t channel, uint8_t sync_byte) {
    if (!ready_ || !idle()) return false;
    discovering_ = true;
    discovery_sync_ = sync_byte;
    capture_count_ = 0;
    begin_(Operation::CONFIGURE);
    frequency_commands_(channel);
    write_(0x0210, {0x00, 0x20, 0x00, 0x08, 0x00, 0x00, 24, 0x01, 0x00});
    write_(0x0206, {sync_byte, 0, 0, 0, 0, 0, 0, 0});
    receive_commands_();
    return operation_ != Operation::FAILED;
  }

  bool poll_address(halo2_protocol::Address &address, halo2_protocol::HaloRxState &state) {
    state = {};
    if (!ready_ || !discovering_ || !consume_frame_()) return false;
    ++capture_count_;
    return halo2_protocol::discover_address(capture_data_.data(), capture_data_.size(), address, state);
  }

  bool use_address(const halo2_protocol::Address &address, uint8_t channel) {
    if (!ready_ || !idle()) return false;
    const auto air = halo2_protocol::air_address(address);
    begin_(Operation::CONFIGURE);
    frequency_commands_(channel);
    write_(0x0210, {0x00, 0x20, 0x00, 0x20, 0x00, 0x00, halo2_protocol::AIR_FRAME_SIZE, 0x01, 0x00});
    write_(0x0206, {air[0], air[1], air[2], air[3], 0, 0, 0, 0});
    receive_commands_();
    address_ = address;
    discovering_ = false;
    return operation_ != Operation::FAILED;
  }

 private:
  static constexpr uint32_t IRQ_TX_DONE = 1UL << 2U, IRQ_RX_DONE = 1UL << 3U;
  static constexpr uint32_t IRQ_TIMEOUT = 1UL << 10U;
  static constexpr uint32_t IRQ_CMD_ERROR = 1UL << 22U, IRQ_ERROR = 1UL << 23U;
  static constexpr uint32_t IRQ_MASK = IRQ_TX_DONE | IRQ_RX_DONE | IRQ_TIMEOUT | IRQ_CMD_ERROR | IRQ_ERROR;
  enum class Operation { IDLE, RESET_LOW, RESET_WAIT, INITIALIZE, CONFIGURE, TRANSMIT, WAIT_TX, RECEIVE, FAILED };
  enum class Response { NONE, VERSION, ERRORS, RX_BUFFER, RX_DATA };
  struct Job {
    std::array<uint8_t, 32> bytes{};
    uint32_t timeout_us{20000};
    uint8_t size{0};
    uint8_t read_size{0};
    Response response{Response::NONE};
  };

  InternalGPIOPin *reset_pin_{nullptr};
  InternalGPIOPin *busy_pin_{nullptr};
  InternalGPIOPin *irq_pin_{nullptr};
  bool registered_{false};
  Operation operation_{Operation::FAILED};
  std::array<Job, 24> jobs_{};
  size_t job_count_{0}, job_index_{0};
  bool response_phase_{false};
  bool irq_waiting_{false};
  int64_t phase_started_{0}, tx_started_{0};
  std::array<halo2_protocol::AirFrame, 2> tx_frames_{};
  uint8_t tx_frame_count_{0}, tx_frame_index_{0};
  bool tx_completed_{false}, rx_pending_{false}, needs_receive_{false};
  bool ready_{false}, discovering_{false};
  halo2_protocol::Address address_{halo2_protocol::RADIO_ADDRESS};
  uint8_t channel_{halo2_protocol::RADIO_CHANNEL}, discovery_sync_{0xAA};
  uint32_t capture_count_{0}, rx_count_{0}, last_irq_{0};
  std::array<uint8_t, 25> capture_data_{};
  halo2_protocol::AirFrame rx_frame_{};
  uint16_t firmware_version_{0};
  char error_[96] = "LR1121 not initialized";

  static uint8_t byte_(uint32_t value, unsigned shift) { return static_cast<uint8_t>(value >> shift); }
  bool fail_(const char *message, uint16_t opcode = 0) {
    ready_ = false;
    operation_ = Operation::FAILED;
    rx_pending_ = false;
    tx_completed_ = false;
    tx_frame_count_ = 0;
    if (opcode) std::snprintf(error_, sizeof(error_), "LR1121 %s (0x%04X)", message, opcode);
    else std::snprintf(error_, sizeof(error_), "LR1121 %s", message);
    return false;
  }
  void begin_(Operation operation) {
    operation_ = operation;
    job_count_ = job_index_ = 0;
    response_phase_ = false;
    irq_waiting_ = false;
    phase_started_ = esp_timer_get_time();
    rx_pending_ = false;
    needs_receive_ = false;
  }
  void append_(uint16_t opcode, const uint8_t *data, size_t size, uint8_t read_size,
               Response response, uint32_t timeout_us = 20000) {
    if (operation_ == Operation::FAILED) return;
    if (job_count_ == jobs_.size() || size > 30 || read_size > 31) {
      fail_("invalid command sequence", opcode);
      return;
    }
    auto &job = jobs_[job_count_++];
    job = {};
    job.bytes[0] = byte_(opcode, 8);
    job.bytes[1] = byte_(opcode, 0);
    if (size) std::memcpy(job.bytes.data() + 2, data, size);
    job.size = size + 2;
    job.read_size = read_size;
    job.response = response;
    job.timeout_us = timeout_us;
  }
  void write_(uint16_t opcode, std::initializer_list<uint8_t> data, uint32_t timeout_us = 20000) {
    append_(opcode, data.begin(), data.size(), 0, Response::NONE, timeout_us);
  }
  void read_(uint16_t opcode, std::initializer_list<uint8_t> data, uint8_t size, Response response) {
    append_(opcode, data.begin(), data.size(), size, response);
  }
  void frequency_commands_(uint8_t channel) {
    channel_ = channel;
    const uint32_t frequency = 2400000000UL + channel * 1000000UL;
    write_(0x011C, {0x00});
    write_(0x020B, {byte_(frequency, 24), byte_(frequency, 16), byte_(frequency, 8), byte_(frequency, 0)});
  }
  void receive_commands_() {
    // AutoTxRx is bidirectional. Disable it BEFORE SetRx: overhearing the
    // original controller must never trigger an unsolicited transmission.
    write_(0x011C, {0x00});
    write_(0x020C, {0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x00, 0x00});
    write_(0x0114, {0xFF, 0xFF, 0xFF, 0xFF});
    write_(0x010B, {});
    write_(0x0209, {0, 0, 0});
  }
  void transmit_commands_() {
    begin_(Operation::TRANSMIT);
    write_(0x011C, {0x00});
    write_(0x0114, {0xFF, 0xFF, 0xFF, 0xFF});
    // Direct hardware turnaround preserves ACKs while the host yields.
    write_(0x020C, {0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x90});
    const auto &frame = tx_frames_[tx_frame_index_];
    append_(0x0109, frame.data(), frame.size(), 0, Response::NONE);
    write_(0x020A, {0x00, 0x02, 0x90});
  }
  bool consume_frame_() {
    if (!idle() || !rx_pending_) return false;
    rx_pending_ = false;
    // Defer rearming so the caller can instead start TX or change the address.
    needs_receive_ = true;
    return true;
  }
  bool bus_ready_(uint32_t timeout_us = 20000) {
    if (!busy_pin_->digital_read()) return true;
    if (esp_timer_get_time() - phase_started_ >= timeout_us) fail_("BUSY timeout");
    return false;
  }
  bool transfer_(const uint8_t *tx, uint8_t *rx, size_t size) {
    enable();
    if (!spi_is_ready()) {
      disable();
      return fail_("SPI device unavailable");
    }
    if (rx != nullptr) {
      // Read buffers contain zero dummy bytes; exchange them in place.
      transfer_array(rx, size);
    } else {
      write_array(tx, size);
    }
    // Release CS and the bus, then allow NSS-to-BUSY propagation.
    disable();
    delayMicroseconds(1);
    // SPI errors are logged by ESPHome; response checks and BUSY/TX deadlines
    // detect radio failures because the SPI API has no transfer result.
    return true;
  }
  bool irq_() {
    uint8_t rx[6]{};
    if (!transfer_(nullptr, rx, sizeof(rx))) return false;
    const uint8_t status = (rx[0] >> 1U) & 7U;
    if (status != 2 && status != 3) return fail_("command rejected");
    last_irq_ = (uint32_t(rx[2]) << 24U) | (uint32_t(rx[3]) << 16U) | (uint32_t(rx[4]) << 8U) | rx[5];
    return true;
  }
  bool response_(Response response, const uint8_t *data) {
    switch (response) {
      case Response::VERSION:
        firmware_version_ = (uint16_t(data[2]) << 8U) | data[3];
        if (data[1] != 0x03 || firmware_version_ == 0 || firmware_version_ == 0xFFFF)
          return fail_("expected LR1121 transceiver firmware");
        break;
      case Response::ERRORS:
        if (data[0] || data[1]) return fail_("calibration failed");
        break;
      case Response::RX_BUFFER:
        if (data[0] == (discovering_ ? 24 : halo2_protocol::AIR_FRAME_SIZE))
          read_(0x010A, {data[1], data[0]}, data[0], Response::RX_DATA);
        else
          needs_receive_ = true;
        break;
      case Response::RX_DATA:
        if (discovering_) {
          capture_data_[0] = discovery_sync_;
          std::memcpy(capture_data_.data() + 1, data, capture_data_.size() - 1);
        } else {
          std::memcpy(rx_frame_.data(), data, rx_frame_.size());
        }
        rx_pending_ = true;
        break;
      case Response::NONE:
        break;
    }
    return operation_ != Operation::FAILED;
  }
  bool advance_() {
    if (operation_ == Operation::FAILED) return false;
    if (operation_ == Operation::RESET_LOW) {
      if (esp_timer_get_time() - phase_started_ < 1000) return false;
      reset_pin_->digital_write(true);
      operation_ = Operation::RESET_WAIT;
      phase_started_ = esp_timer_get_time();
      return false;
    }
    if (operation_ == Operation::RESET_WAIT) {
      if (esp_timer_get_time() - phase_started_ < 10000 || !bus_ready_(500000)) return false;
      operation_ = Operation::INITIALIZE;
      phase_started_ = esp_timer_get_time();
    }
    if (operation_ == Operation::IDLE || operation_ == Operation::WAIT_TX) {
      if (rx_pending_) return false;
      if (needs_receive_) {
        begin_(Operation::CONFIGURE);
        receive_commands_();
        return true;
      }
      if (!irq_pin_->digital_read()) {
        irq_waiting_ = false;
        if (operation_ == Operation::WAIT_TX && esp_timer_get_time() - tx_started_ >= 50000)
          fail_("TX completion timeout");
        return false;
      }
      if (!irq_waiting_) {
        irq_waiting_ = true;
        phase_started_ = esp_timer_get_time();
      }
      if (!bus_ready_() || !irq_()) return false;
      irq_waiting_ = false;
      if (last_irq_ & (IRQ_ERROR | IRQ_CMD_ERROR)) return fail_("radio interrupt error");
      if (operation_ == Operation::WAIT_TX) {
        // RX timeout can already be latched too when ESPHome resumes. It does
        // not invalidate TX_DONE; prioritize the successful transmission.
        if (last_irq_ & IRQ_TX_DONE) {
          if (++tx_frame_index_ < tx_frame_count_) {
            transmit_commands_();
            return true;
          }
          tx_completed_ = true;
          operation_ = Operation::IDLE;
        } else {
          if ((last_irq_ & IRQ_TIMEOUT) || esp_timer_get_time() - tx_started_ >= 50000)
            fail_("TX completion timeout");
          return false;
        }
      }
      if (last_irq_ & IRQ_RX_DONE) {
        begin_(Operation::RECEIVE);
        read_(0x0203, {}, 2, Response::RX_BUFFER);
        return true;
      }
      if (last_irq_ & IRQ_TIMEOUT) {
        begin_(Operation::CONFIGURE);
        receive_commands_();
        return true;
      }
      return false;
    }

    auto &job = jobs_[job_index_];
    if (!bus_ready_(response_phase_ ? job.timeout_us : 20000)) return false;
    if (!response_phase_) {
      if (!transfer_(job.bytes.data(), nullptr, job.size)) return false;
      if (job.bytes[0] == 0x02 && job.bytes[1] == 0x0A) tx_started_ = esp_timer_get_time();
      response_phase_ = true;
      phase_started_ = esp_timer_get_time();
      return true;
    }
    if (job.read_size) {
      uint8_t rx[32]{};
      if (!transfer_(nullptr, rx, job.read_size + 1)) return false;
      if (((rx[0] >> 1U) & 7U) != 3) return fail_("read command rejected");
      if (!response_(job.response, rx + 1)) return false;
    } else {
      if (!irq_()) return false;
      if (last_irq_ & IRQ_CMD_ERROR)
        return fail_("command error", (uint16_t(job.bytes[0]) << 8U) | job.bytes[1]);
    }
    response_phase_ = false;
    phase_started_ = esp_timer_get_time();
    if (++job_index_ == job_count_) {
      if (operation_ == Operation::TRANSMIT) {
        operation_ = Operation::WAIT_TX;
      } else {
        if (operation_ == Operation::INITIALIZE) ready_ = true;
        operation_ = Operation::IDLE;
      }
    }
    return true;
  }
};
}  // namespace esphome::halo2
