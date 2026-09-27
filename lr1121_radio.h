#pragma once

#include <array>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include "halo2_protocol.h"

namespace lr1121_halo2 {
// LR1121 User Manual rev. 1.2, sections 3, 4, 6, 7 and 8.5.
// Keep command framing separate from the ESP-IDF SPI and GPIO transport.
template<class Transport> class Radio {
 public:
  explicit Radio(Transport &transport) : transport_(transport) {}

  bool ready() const { return ready_; }
  const char *last_error() const { return error_[0] ? error_ : nullptr; }
  uint16_t firmware_version() const { return firmware_version_; }
  const halo2_protocol::Address &address() const { return address_; }
  uint8_t channel() const { return channel_; }
  bool discovering() const { return discovering_; }
  uint32_t capture_count() const { return capture_count_; }
  const std::array<uint8_t, 25> &capture_data() const { return capture_data_; }

  bool setup(uint32_t deviation_hz = 160000, uint8_t pulse_shape = 0x09,
             const halo2_protocol::Address &address = halo2_protocol::RADIO_ADDRESS,
             uint8_t channel = halo2_protocol::RADIO_CHANNEL) {
    ready_ = false;
    discovering_ = false;
    address_ = address;
    channel_ = channel;
    // 125 kbps + 2*fdev must fit inside the largest 467 kHz RX bandwidth.
    if (deviation_hz == 0 || deviation_hz >= 171000 ||
        (pulse_shape != 0 && (pulse_shape < 0x08 || pulse_shape > 0x0B)))
      return fail("invalid modulation settings");
    if (!transport_.begin()) return fail("SPI/GPIO initialization failed");
    transport_.reset();
    if (!wait_ready(500000)) return false;
    uint8_t version[4]{};
    if (!read(0x0101, {}, version, sizeof(version))) return false;
    firmware_version_ = static_cast<uint16_t>((version[2] << 8U) | version[3]);
    if (version[1] != 0x03 || firmware_version_ == 0 || firmware_version_ == 0xFFFF)
      return fail("expected LR1121 transceiver firmware");

    // Configure the board in STBY_RC before enabling its 3.0 V TCXO.
    // DIO5/DIO6 control only the sub-GHz antenna switch; RFIO_HF uses a
    // separate connector. Hold DIO5 high and DIO6 low in every mode to keep
    // the unused sub-GHz PA isolated on the RTC6603SP's receive path.
    // The switch has no documented both-off state.
    if (!write(0x011C, {0x00}) ||
        !write(0x0110, {0x01}) ||  // DC-DC regulator
        !write(0x0112, {0x03, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00}) ||
        !write(0x0117, {0x06, 0x00, 0x01, 0x2C}) ||  // 3.0 V, 300 RTC ticks
        !write(0x010E, {}) ||  // clear the expected pre-TCXO startup errors
        !write(0x010F, {0x3F}, 100000)) return false;
    uint8_t errors[2]{};
    if (!read(0x010D, {}, errors, sizeof(errors))) return false;
    if (errors[0] || errors[1]) return fail("calibration failed");

    const uint32_t frequency = 2400000000UL + channel_ * 1000000UL;
    if (!write(0x020E, {0x01}) ||  // GFSK
        !write(0x020B, {byte(frequency, 24), byte(frequency, 16), byte(frequency, 8), byte(frequency, 0)}) ||
        !write(0x020F, {0x00, 0x01, 0xE8, 0x48, pulse_shape, 0x09,
                        byte(deviation_hz, 24), byte(deviation_hz, 16), byte(deviation_hz, 8), byte(deviation_hz, 0)}))
      return false;
    // 32-bit TX preamble, 8-bit RX preamble detection, 32-bit sync/address.
    // Fixed 14-byte captures: 9-bit PCF + payload + software CRC + padding.
    // No whitening, hardware CRC, length byte, address filter, or auto-ACK.
    const auto air = halo2_protocol::air_address(address_);
    if (!write(0x0210, {0x00, 0x20, 0x04, 0x20, 0x00, 0x00, halo2_protocol::AIR_FRAME_SIZE, 0x01, 0x00}) ||
        !write(0x0206, {air[0], air[1], air[2], air[3], 0, 0, 0, 0}) ||
        !write(0x0215, {0x02, 0x00, 0x04, 0x00}) ||  // HF PA, VREG, Waveshare duty cycle
        !write(0x0211, {0x00, 0x02}) ||  // 0 dBm, 48 us ramp
        !write(0x0213, {0x01}) ||  // fall back to STBY_RC
        !write(0x0227, {0x01}) ||  // boosted RX
        // DIO9 -> GPIO38. DIO11 is left for the board's LF crystal.
        !write(0x0113, {byte(IRQ_MASK, 24), byte(IRQ_MASK, 16), byte(IRQ_MASK, 8), byte(IRQ_MASK, 0),
                        0, 0, 0, 0}) ||
        !start_receive()) return false;
    ready_ = true;
    error_[0] = '\0';
    return true;
  }

  bool send(const halo2_protocol::AirFrame &frame) {
    if (!ready_ || discovering_) return false;
    bool sent = false;
    if (write(0x011C, {0x00}) && clear_irq() &&
        write(0x0109, frame.data(), frame.size()) &&
        write(0x020A, {0x00, 0x02, 0x90})) {  // ~20 ms hardware TX timeout
      const auto start = transport_.now_us();
      while (transport_.now_us() - start < 50000) {
        if (transport_.irq()) {
          uint32_t irq = 0;
          if (!get_irq(irq)) break;
          if (irq & (IRQ_ERROR | IRQ_CMD_ERROR)) { fail("radio error during TX"); break; }
          if (irq & IRQ_TIMEOUT) { fail("TX timeout"); break; }
          if (irq & IRQ_TX_DONE) { sent = true; break; }
        }
        transport_.delay_us(100);
      }
      if (!sent && ready_) fail("TX completion timeout");
    }
    // Attempt to leave TX even on a failed command or a missing TX_DONE IRQ.
    // Preserve the original failure if reception can be restored.
    const bool receiving = start_receive();
    ready_ = receiving;
    if (sent && receiving) error_[0] = '\0';
    return sent && receiving;
  }

  bool poll(halo2_protocol::HaloRxState &state) {
    state = {};
    if (!ready_ || discovering_ || !transport_.irq()) return false;
    uint32_t irq = 0;
    if (!get_irq(irq)) return false;
    if (irq & (IRQ_ERROR | IRQ_CMD_ERROR)) return fail("radio error during RX");
    halo2_protocol::AirFrame frame{};
    bool received = false;
    if (irq & IRQ_RX_DONE) {
      uint8_t buffer[2]{};
      if (!read(0x0203, {}, buffer, sizeof(buffer))) return false;
      if (buffer[0] == frame.size()) {
        // ReadBuffer8 must use the offset returned by GetRxBufferStatus.
        if (!read(0x010A, {buffer[1], buffer[0]}, frame.data(), frame.size())) return false;
        received = true;
      }
    }
    // Single RX holds the first frame until polling, so the lamp's immediate
    // reply cannot overwrite an authoritative controller request in the FIFO.
    if (!start_receive()) return false;
    return received && halo2_protocol::decode_air_frame(frame.data(), frame.size(), state, address_);
  }

  bool listen_for_address(uint8_t channel, uint8_t sync_byte) {
    discovering_ = true;
    discovery_sync_ = sync_byte;
    capture_count_ = 0;
    if (!set_frequency(channel) ||
        // No preamble gate; capture through the unknown address and full frame.
        !write(0x0210, {0x00, 0x20, 0x00, 0x08, 0x00, 0x00, 24, 0x01, 0x00}) ||
        !write(0x0206, {sync_byte, 0, 0, 0, 0, 0, 0, 0}) || !start_receive()) return false;
    error_[0] = '\0';
    return true;
  }

  bool poll_address(halo2_protocol::Address &address, halo2_protocol::HaloRxState &state) {
    state = {};
    if (!ready_ || !discovering_ || !transport_.irq()) return false;
    uint32_t irq = 0;
    if (!get_irq(irq)) return false;
    if (irq & (IRQ_ERROR | IRQ_CMD_ERROR)) return fail("radio error during discovery");
    bool received = false;
    if (irq & IRQ_RX_DONE) {
      uint8_t buffer[2]{};
      if (!read(0x0203, {}, buffer, sizeof(buffer))) return false;
      if (buffer[0] == 24) {
        // Include the sync byte: it may overlap the first address bits.
        capture_data_[0] = discovery_sync_;
        if (!read(0x010A, {buffer[1], buffer[0]}, capture_data_.data() + 1, 24)) return false;
        ++capture_count_;
        received = true;
      }
    }
    if (!start_receive()) return false;
    return received && halo2_protocol::discover_address(capture_data_.data(), capture_data_.size(), address, state);
  }

  bool use_address(const halo2_protocol::Address &address, uint8_t channel) {
    const auto air = halo2_protocol::air_address(address);
    if (!set_frequency(channel) ||
        !write(0x0210, {0x00, 0x20, 0x04, 0x20, 0x00, 0x00, halo2_protocol::AIR_FRAME_SIZE, 0x01, 0x00}) ||
        !write(0x0206, {air[0], air[1], air[2], air[3], 0, 0, 0, 0}) || !start_receive()) return false;
    address_ = address;
    discovering_ = false;
    error_[0] = '\0';
    return true;
  }

 private:
  static constexpr uint32_t IRQ_TX_DONE = 1UL << 2U, IRQ_RX_DONE = 1UL << 3U;
  static constexpr uint32_t IRQ_TIMEOUT = 1UL << 10U;
  static constexpr uint32_t IRQ_CMD_ERROR = 1UL << 22U, IRQ_ERROR = 1UL << 23U;
  static constexpr uint32_t IRQ_MASK = IRQ_TX_DONE | IRQ_RX_DONE | IRQ_TIMEOUT | IRQ_CMD_ERROR | IRQ_ERROR;
  Transport &transport_;
  bool ready_ = false;
  bool discovering_ = false;
  halo2_protocol::Address address_{halo2_protocol::RADIO_ADDRESS};
  uint8_t channel_{halo2_protocol::RADIO_CHANNEL};
  uint8_t discovery_sync_{0xAA};
  uint32_t capture_count_{0};
  std::array<uint8_t, 25> capture_data_{};
  uint16_t firmware_version_ = 0;
  char error_[96] = "LR1121 not initialized";

  static uint8_t byte(uint32_t value, unsigned shift) { return static_cast<uint8_t>(value >> shift); }
  bool fail(const char *message, uint16_t opcode = 0) {
    ready_ = false;
    if (opcode) std::snprintf(error_, sizeof(error_), "LR1121 %s (0x%04X)", message, opcode);
    else std::snprintf(error_, sizeof(error_), "LR1121 %s", message);
    return false;
  }
  bool wait_ready(uint32_t timeout_us = 20000) {
    const auto start = transport_.now_us();
    while (transport_.busy()) {
      if (transport_.now_us() - start >= timeout_us) return fail("BUSY timeout");
      transport_.delay_ms(1);
    }
    return true;
  }
  bool transfer(const uint8_t *tx, uint8_t *rx, size_t size) {
    if (!transport_.transfer(tx, rx, size)) return fail("SPI transfer failed");
    // Give BUSY time to assert following the rising edge of NSS.
    transport_.delay_us(1);
    return true;
  }
  bool command(uint16_t opcode, const uint8_t *data, size_t size, uint32_t timeout_us = 20000) {
    std::array<uint8_t, 32> tx{};
    if (size > tx.size() - 2) return fail("command too long", opcode);
    tx[0] = byte(opcode, 8);
    tx[1] = byte(opcode, 0);
    if (size) std::memcpy(tx.data() + 2, data, size);
    return wait_ready() && transfer(tx.data(), nullptr, size + 2) && wait_ready(timeout_us);
  }
  bool get_irq(uint32_t &irq) {
    // Status is a single six-byte NOP transaction, not a two-phase read.
    const uint8_t tx[6]{};
    uint8_t rx[6]{};
    if (!wait_ready() || !transfer(tx, rx, sizeof(rx))) return false;
    const uint8_t status = (rx[0] >> 1U) & 7U;
    if (status != 2 && status != 3) return fail("command rejected");
    irq = (uint32_t(rx[2]) << 24U) | (uint32_t(rx[3]) << 16U) | (uint32_t(rx[4]) << 8U) | rx[5];
    return true;
  }
  bool write(uint16_t opcode, const uint8_t *data, size_t size, uint32_t timeout_us = 20000) {
    if (!command(opcode, data, size, timeout_us)) return false;
    uint32_t irq = 0;
    if (!get_irq(irq)) return false;
    if (irq & IRQ_CMD_ERROR) return fail("command error", opcode);
    return true;
  }
  bool write(uint16_t opcode, std::initializer_list<uint8_t> data, uint32_t timeout_us = 20000) {
    return write(opcode, data.begin(), data.size(), timeout_us);
  }
  bool read(uint16_t opcode, std::initializer_list<uint8_t> params, uint8_t *data, size_t size) {
    std::array<uint8_t, 32> tx{}, rx{};
    if (size >= rx.size()) return fail("read too long", opcode);
    // The response uses a second NSS pulse and begins with one status byte.
    if (!command(opcode, params.begin(), params.size()) || !transfer(tx.data(), rx.data(), size + 1)) return false;
    if (((rx[0] >> 1U) & 7U) != 3) return fail("read command rejected", opcode);
    std::memcpy(data, rx.data() + 1, size);
    return true;
  }
  bool clear_irq() { return write(0x0114, {0xFF, 0xFF, 0xFF, 0xFF}); }
  bool set_frequency(uint8_t channel) {
    const uint32_t frequency = 2400000000UL + channel * 1000000UL;
    if (!write(0x011C, {0x00}) ||
        !write(0x020B, {byte(frequency, 24), byte(frequency, 16), byte(frequency, 8), byte(frequency, 0)})) return false;
    channel_ = channel;
    return true;
  }
  bool start_receive() {
    return clear_irq() && write(0x011C, {0x00}) && write(0x010B, {}) && write(0x0209, {0, 0, 0});
  }
};
}  // namespace lr1121_halo2
