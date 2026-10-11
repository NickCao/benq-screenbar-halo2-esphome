// SPDX-License-Identifier: MIT
#include "tx.h"
#include "demod.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #c); std::exit(1); } } while (0)

static void collect(const sdr_packet_t *packet, void *context) {
  static_cast<std::vector<sdr_packet_t> *>(context)->push_back(*packet);
}

int main() {
  constexpr uint32_t sync = 0xCE8C234F, hz = 2405000000u;
  std::array<uint8_t, 32> body{};
  CHECK(sdr_tx_parse("290218850546050546008142C100", 105, body.data()));
  CHECK(!sdr_tx_parse("290218850546050546008142C101", 105, body.data()));
  CHECK(!sdr_tx_parse("XX", 8, body.data()));
  CHECK(!sdr_tx_parse("00", 0, body.data()));
  CHECK(!sdr_tx_parse("0000", 8, body.data()));
  CHECK(!sdr_tx_parse("00", 257, body.data()));
  std::array<uint32_t, SDR_TX_MAX_UPDATES + 1> words{};
  words.back() = 0xABCD;
  CHECK(!sdr_tx_waveform(hz, sync, body.data(), 105, false, words.data(), 1));
  CHECK(!sdr_tx_waveform(2399000000u, sync, body.data(), 105, false, words.data(), words.size()));
  CHECK(!sdr_tx_waveform(hz, 0, body.data(), 105, false, words.data(), words.size()));
  unsigned cases = 1;
  // Independently turn PLL words into phase-continuous RF ADC samples and run
  // the actual RX DSP. Test all eight sample phases and both FSK polarities.
  for (unsigned phase = 0; phase < 8; ++phase) {
    bool inverted = phase >= 4;
    const size_t n = sdr_tx_waveform(hz, sync, body.data(), 105, inverted, words.data(), SDR_TX_MAX_UPDATES);
    CHECK(n == (32 + 32 + 112) * SDR_TX_SAMPLES_PER_BIT);
    constexpr unsigned stride = SDR_INPUT_RATE / SDR_TX_RATE;
    std::vector<uint32_t> samples;
    const unsigned lead = 1600 + phase * 16;
    double angle = .31;
    for (unsigned k = 0; k < lead + n * stride + 1600; ++k) {
      bool active = k >= lead && k < lead + n * stride;
      double frequency = 0, amplitude = 0;
      if (active) {
        uint32_t word = words[(k - lead) / stride];
        frequency = 30000000.0 * (32 + word / 65536.0) - hz + (phase % 2 ? 40000 : -40000);
        CHECK(std::abs(frequency) < 201000);
        amplitude = 180;
      }
      angle += 6.283185307179586 * frequency / 16000000;
      int i = static_cast<int>(std::lround(amplitude * std::cos(angle) + 25));
      int q = static_cast<int>(std::lround(amplitude * std::sin(angle) - 17));
      samples.push_back(((static_cast<uint32_t>(i) & 1023) << 10) | (static_cast<uint32_t>(q) & 1023));
    }
    sdr_demod_t demod;
    sdr_reset(&demod);
    CHECK(sdr_push(&demod, samples.data(), samples.size()));
    const sdr_config_t config{sync, 105, 64};
    std::vector<sdr_packet_t> packets;
    CHECK(sdr_detect(&demod, &config, collect, &packets) == 1);
    CHECK(packets[0].complete);
    CHECK(packets[0].inverted == inverted);
    CHECK(std::equal(body.begin(), body.begin() + 14, packets[0].bits));
    ++cases;
  }
  CHECK(sdr_tx_waveform(2483000000u, sync, body.data(), 256, false, words.data(), SDR_TX_MAX_UPDATES) == SDR_TX_MAX_UPDATES);
  CHECK(words.back() == 0xABCD);
  ++cases;
  std::printf("PASS: %u TX cases (packing, FSK PLL waveform, RX roundtrip, polarity, bounds)\n", cases);
}
