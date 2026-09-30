#pragma once

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <vector>
#include "check.h"
#include "esphome/components/spi/spi.h"
#include "esphome/core/hal.h"
#include "halo2_protocol.h"

namespace halo2_test {
namespace protocol = halo2_protocol;

// A scripted chip at the SPI boundary, not a replacement for LR1121Radio.
// TX only completes when the test says so. No lamp behavior or ACK freshness
// policy is simulated: tests explicitly supply every received snapshot.
class FakeLR1121 : public esphome::spi::SPIComponent {
 public:
  struct Transmission {
    protocol::AirFrame frame;
    protocol::Address address;
    uint8_t channel;
    uint32_t at;

    protocol::ReceivedPacket packet() const {
      protocol::ReceivedPacket result;
      CHECK(protocol::decode_air_frame(frame.data(), frame.size(), result, address));
      return result;
    }
  };

  void reset() {
    ++reset_count;
    irq_ = 0;
    transmitting = receiving = auto_tx_rx_ = false;
    response_.reset();
    rx_deadline_.reset();
    rx_buffer_.clear();
    tx_buffer_.clear();
  }
  bool irq() const { return irq_ != 0; }
  void fail() { irq_ |= 1U << 23U; }
  void tick() {
    if (rx_deadline_ && time_us >= *rx_deadline_) {
      receiving = false;
      rx_deadline_.reset();
      irq_ |= 1U << 10U;
    }
  }
  void finish_tx() {
    CHECK(transmitting);
    transmitting = false;
    irq_ |= 1U << 2U;
    receiving = auto_tx_rx_;
    if (receiving) rx_deadline_ = time_us + 20000;
  }
  void receive(std::span<const uint8_t> bytes) {
    CHECK(receiving);
    CHECK(bytes.size() == rx_length);
    rx_buffer_.assign(bytes.begin(), bytes.end());
    receiving = false;
    rx_deadline_.reset();
    irq_ |= 1U << 3U;
  }

  void write(const uint8_t *data, size_t size) override {
    CHECK(size >= 2);
    CHECK(!response_);
    const uint16_t opcode = (uint16_t(data[0]) << 8U) | data[1];
    switch (opcode) {
      case 0x0101:  // GetVersion
        response_ = {0, 3, 1, 1};  // LR1121 transceiver firmware 0x0101.
        break;
      case 0x010D:  // GetErrors
        response_ = {0, 0};
        break;
      case 0x0203:  // GetRxBufferStatus
        response_ = {static_cast<uint8_t>(rx_buffer_.size()), 0};
        break;
      case 0x010A:  // ReadBuffer8
        CHECK(size == 4 && data[2] == 0 && data[3] == rx_buffer_.size());
        response_ = rx_buffer_;
        break;
      case 0x0109:  // WriteBuffer8
        tx_buffer_.assign(data + 2, data + size);
        break;
      case 0x010B:  // ClearRxBuffer
        rx_buffer_.clear();
        break;
      case 0x0114: {  // ClearIrq
        CHECK(size == 6);
        const uint32_t mask = (uint32_t(data[2]) << 24U) | (uint32_t(data[3]) << 16U) |
                              (uint32_t(data[4]) << 8U) | data[5];
        irq_ &= ~mask;
        break;
      }
      case 0x011C:  // SetStandby
        CHECK(!transmitting);  // A coordinator must not interrupt an in-flight packet.
        receiving = false;
        rx_deadline_.reset();
        break;
      case 0x020B: {  // SetRfFrequency
        CHECK(size == 6);
        const uint32_t hz = (uint32_t(data[2]) << 24U) | (uint32_t(data[3]) << 16U) |
                            (uint32_t(data[4]) << 8U) | data[5];
        channel = static_cast<uint8_t>(hz / 1000000U - 2400U);
        break;
      }
      case 0x0210:  // SetPacketParams
        CHECK(size == 11);
        rx_length = data[8];
        break;
      case 0x0206:  // SetGfskSyncWord
        CHECK(size == 10);
        sync = data[2];
        address = {data[5], data[4], data[3], data[2]};
        break;
      case 0x020C:  // AutoTxRx
        CHECK(size == 9);
        auto_tx_rx_ = !(data[2] == 0xFF && data[3] == 0xFF && data[4] == 0xFF);
        break;
      case 0x0209:  // SetRx
        CHECK(!transmitting);
        CHECK(!auto_tx_rx_);  // Passive reception must not trigger an automatic TX.
        receiving = true;
        break;
      case 0x020A: {  // SetTx
        CHECK(!transmitting);
        CHECK(tx_buffer_.size() == protocol::AIR_FRAME_SIZE);
        protocol::AirFrame frame;
        std::copy(tx_buffer_.begin(), tx_buffer_.end(), frame.begin());
        transmissions.push_back({frame, address, channel, esphome::millis()});
        transmitting = true;
        receiving = false;
        rx_deadline_.reset();
        break;
      }
      // Initialization commands whose analog effects are outside these tests.
      case 0x010E:  // ClearErrors
      case 0x010F:  // Calibrate
      case 0x0110:  // SetRegulatorMode
      case 0x0112:  // SetRfSwitch
      case 0x0113:  // SetDioIrqParams
      case 0x0117:  // SetTcxoMode
      case 0x020E:  // SetPacketType
      case 0x020F:  // SetModulationParams
      case 0x0211:  // SetTxParams
      case 0x0213:  // SetRxTxFallbackMode
      case 0x0215:  // SetPaConfig
      case 0x0227:  // SetRxBoosted
        break;
      default:
        std::cerr << "Unsupported fake-chip opcode: " << std::hex << opcode << '\n';
        CHECK(false);
    }
  }

  void read(uint8_t *data, size_t size) override {
    std::fill_n(data, size, 0);
    if (response_) {
      CHECK(size == response_->size() + 1);
      data[0] = 3U << 1U;  // CommandStatus::DATA
      std::copy(response_->begin(), response_->end(), data + 1);
      response_.reset();
    } else {
      CHECK(size == 6);  // GetStatus
      data[0] = 2U << 1U;  // CommandStatus::OK
      for (unsigned i = 0; i < 4; ++i) data[2 + i] = static_cast<uint8_t>(irq_ >> (24U - 8U * i));
    }
  }

  bool transmitting{false}, receiving{false};
  uint8_t rx_length{0}, channel{0}, sync{0};
  unsigned reset_count{0};
  protocol::Address address{};
  std::vector<Transmission> transmissions;

 private:
  uint32_t irq_{0};
  bool auto_tx_rx_{false};
  std::optional<int64_t> rx_deadline_;
  std::optional<std::vector<uint8_t>> response_;
  std::vector<uint8_t> tx_buffer_, rx_buffer_;
};
}  // namespace halo2_test
