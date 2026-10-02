// SPDX-License-Identifier: MIT
#include "demod.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#define CHECK(condition) do { if (!(condition)) { \
  std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); std::exit(1); } } while (0)

constexpr uint32_t SYNC = 0xCE8C234F;
constexpr std::array<uint8_t, 14> BODY{0x2A, 0x02, 0x18, 0x94, 0x07, 0xD0, 0x0A,
                                      0x07, 0xD0, 0x00, 0x81, 0x6B, 0xF7, 0x80};
constexpr unsigned BODY_BITS = 105;
constexpr double PI = 3.14159265358979323846;

struct Signal {
  double clock = 1, offset = 0, phase = .37, start_us = 100, amplitude = 180, noise = 4;
  bool inverted = false;
  uint32_t sync = SYNC;
  unsigned preamble = 8;
};

static std::vector<uint32_t> synthesize(const Signal &signal) {
  std::vector<int> bits;
  for (unsigned b = 0; b < signal.preamble; ++b) bits.push_back(b % 2 == 0);
  for (unsigned b = 0; b < 32; ++b) bits.push_back((signal.sync >> (31 - b)) & 1);
  for (unsigned b = 0; b < BODY_BITS; ++b) bits.push_back((BODY[b / 8] >> (7 - b % 8)) & 1);
  const double period = 128 * signal.clock;
  const double start = signal.start_us * 16;
  const size_t count = static_cast<size_t>(start + bits.size() * period + 100 * 16);
  // Gaussian filter on the NRZ frequency waveform, BT=0.5. This generator
  // shares neither filtering nor timing code with the receiver.
  const double sigma = std::sqrt(std::log(2.0)) * period / (2 * PI * .5);
  const int radius = static_cast<int>(std::ceil(3 * sigma));
  std::vector<double> kernel;
  double sum = 0;
  for (int n = -radius; n <= radius; ++n) {
    const double value = std::exp(-.5 * n * n / (sigma * sigma));
    kernel.push_back(value);
    sum += value;
  }
  for (double &value : kernel) value /= sum;
  std::vector<double> nrz(count + 2 * radius, 0);
  for (size_t n = 0; n < count; ++n) {
    const int bit = static_cast<int>(std::floor((static_cast<double>(n) - start) / period));
    if (bit >= 0 && bit < static_cast<int>(bits.size())) nrz[n + radius] = bits[bit] ? 1 : -1;
  }
  std::mt19937 random(38471);
  std::normal_distribution<double> noise(0, signal.noise);
  std::vector<uint32_t> result;
  double phase = signal.phase;
  for (size_t n = 0; n < count; ++n) {
    double symbol = 0;
    for (size_t k = 0; k < kernel.size(); ++k) symbol += nrz[n + k] * kernel[k];
    phase += 2 * PI * (signal.offset + 160000 * symbol) / SDR_INPUT_RATE;
    const bool active = n >= start && n < start + bits.size() * period;
    const double amplitude = active ? signal.amplitude : 0;
    const int i = std::clamp(static_cast<int>(std::lround(amplitude * std::cos(phase) + 25 + noise(random))), -512, 511);
    const int q = std::clamp(static_cast<int>(std::lround(amplitude * std::sin(phase) * (signal.inverted ? -1 : 1)
                                                      - 17 + noise(random))), -512, 511);
    // Gain and AGC metadata must have no effect on I/Q unpacking.
    result.push_back(0xABC00000u | ((static_cast<uint32_t>(i) & 1023) << 10) | (static_cast<uint32_t>(q) & 1023));
  }
  return result;
}

static void collect(const sdr_packet_t *packet, void *context) {
  static_cast<std::vector<sdr_packet_t> *>(context)->push_back(*packet);
}

static std::vector<sdr_packet_t> detect(const std::vector<uint32_t> &samples, size_t chunk = 997) {
  sdr_demod_t demod;
  sdr_reset(&demod);
  for (size_t n = 0; n < samples.size(); n += chunk)
    CHECK(sdr_push(&demod, samples.data() + n, std::min(chunk, samples.size() - n)));
  const sdr_config_t config{SYNC, BODY_BITS, 64};
  std::vector<sdr_packet_t> packets;
  CHECK(sdr_detect(&demod, &config, collect, &packets) == packets.size());
  return packets;
}

static void check_packet(const sdr_packet_t &packet, const Signal &signal) {
  CHECK(packet.complete);
  CHECK(packet.bit_count == BODY_BITS);
  for (unsigned b = 0; b < BODY_BITS; ++b)
    CHECK(((packet.bits[b / 8] ^ BODY[b / 8]) & (1 << (7 - b % 8))) == 0);
  CHECK((packet.bits[13] & 0x7f) == 0);
  CHECK(packet.inverted == signal.inverted);
  CHECK(std::abs(packet.samples_per_bit - 8 * signal.clock) < .035);
  CHECK(std::abs(packet.offset_hz - signal.offset * (signal.inverted ? -1 : 1)) < 12000);
  CHECK(packet.sync_error < .2);
  CHECK(packet.min_bit_margin > .5);
  CHECK(std::isfinite(packet.timing_error_rms));
}

int main() {
  unsigned cases = 0;
  for (unsigned phase = 0; phase < 8; ++phase) {
    Signal signal;
    signal.start_us += phase + .31;
    signal.offset = phase % 2 ? 60000 : -60000;
    signal.clock = phase % 2 ? 1.004 : .996;
    signal.inverted = phase >= 4;
    signal.preamble = phase % 2 ? 32 : 8;
    const auto packets = detect(synthesize(signal));
    if (packets.size() != 1) std::fprintf(stderr, "phase %u: %zu packets\n", phase, packets.size());
    CHECK(packets.size() == 1);
    check_packet(packets.front(), signal);
    ++cases;
  }
  Signal no_preamble;
  no_preamble.preamble = 0;
  const auto bare = synthesize(no_preamble);
  const auto packet = detect(bare, 1);
  CHECK(packet.size() == 1);
  check_packet(packet.front(), no_preamble);
  CHECK(detect(bare, bare.size()).front().bits[0] == packet.front().bits[0]);
  ++cases;

  auto truncated = bare;
  truncated.resize(truncated.size() - 700 * 16);
  const auto partial = detect(truncated);
  CHECK(partial.size() == 1);
  CHECK(!partial.front().complete && partial.front().bit_count > 0);
  for (unsigned b = 0; b < partial.front().bit_count; ++b)
    CHECK(((partial.front().bits[b / 8] ^ BODY[b / 8]) & (1 << (7 - b % 8))) == 0);
  ++cases;

  Signal wrong;
  wrong.sync ^= 0x00010000;
  CHECK(detect(synthesize(wrong)).empty());
  Signal silence;
  silence.amplitude = 0;
  silence.noise = 30;
  CHECK(detect(synthesize(silence)).empty());
  ++cases;

  auto double_packet = bare;
  double_packet.insert(double_packet.end(), bare.begin(), bare.end());
  const auto both = detect(double_packet);
  CHECK(both.size() == 2);
  check_packet(both[0], no_preamble);
  check_packet(both[1], no_preamble);
  ++cases;

  // Truncated address halves from different capture windows cannot combine.
  const size_t split = static_cast<size_t>((no_preamble.start_us + 16 * 8) * 16);
  CHECK(detect(std::vector<uint32_t>(bare.begin(), bare.begin() + split)).empty());
  CHECK(detect(std::vector<uint32_t>(bare.begin() + split, bare.end())).empty());
  ++cases;

  sdr_demod_t overflow;
  sdr_reset(&overflow);
  std::vector<uint32_t> oversized((SDR_MAX_SAMPLES + 1) * SDR_DECIMATION);
  CHECK(!sdr_push(&overflow, oversized.data(), oversized.size()));
  const sdr_config_t config{SYNC, BODY_BITS, 64};
  CHECK(sdr_detect(&overflow, &config, nullptr, nullptr) == 0);
  ++cases;
  std::printf("PASS: %u demodulation cases (Gaussian FSK, timing, noise, polarity, gaps, bounds)\n", cases);
}
