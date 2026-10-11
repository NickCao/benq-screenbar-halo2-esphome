// SPDX-License-Identifier: MIT
#include "tx.h"
#include "vendor/esp32-sdr-trx/lo_plan.h"
#include <string.h>

static int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool sdr_tx_parse(const char *hex, unsigned bits, uint8_t *body) {
  if (!hex || !body || !bits || bits > SDR_TX_MAX_BITS) return false;
  const unsigned bytes = (bits + 7) / 8;
  if (strlen(hex) != bytes * 2) return false;
  uint8_t parsed[SDR_TX_MAX_BITS / 8] = {0};
  for (unsigned b = 0; b < bytes; ++b) {
    const int hi = hex_digit(hex[2 * b]), lo = hex_digit(hex[2 * b + 1]);
    if (hi < 0 || lo < 0) return false;
    parsed[b] = (uint8_t)((hi << 4) | lo);
  }
  if (bits % 8 && (parsed[bytes - 1] & ((1u << (8 - bits % 8)) - 1u))) return false;
  memcpy(body, parsed, bytes);
  return true;
}

static int bit_at(uint32_t sync, const uint8_t *body, unsigned bits, int sample) {
  int bit = sample < 0 ? 0 : sample / (int)SDR_TX_SAMPLES_PER_BIT;
  const int total = (int)(SDR_TX_PREAMBLE_BITS + 32 + bits);
  if (bit >= total) bit = total - 1;
  bool value;
  if (bit < (int)SDR_TX_PREAMBLE_BITS) value = bit % 2 == 0;
  else if (bit < (int)SDR_TX_PREAMBLE_BITS + 32)
    value = ((sync >> (31 - (bit - SDR_TX_PREAMBLE_BITS))) & 1) != 0;
  else {
    bit -= SDR_TX_PREAMBLE_BITS + 32;
    value = ((body[bit / 8] >> (7 - bit % 8)) & 1) != 0;
  }
  return value ? 1 : -1;
}

size_t sdr_tx_waveform(uint32_t frequency_hz, uint32_t sync, const uint8_t *body,
                       unsigned bits, bool inverted, uint32_t *words, size_t capacity) {
  if (!body || !words || !bits || bits > SDR_TX_MAX_BITS || !sync || sync == UINT32_MAX ||
      frequency_hz < 2400000000u || frequency_hz > 2483000000u) return 0;
  const unsigned padded_bits = (bits + 7) / 8 * 8;
  const size_t count = (SDR_TX_PREAMBLE_BITS + 32 + padded_bits) * SDR_TX_SAMPLES_PER_BIT;
  if (capacity < count) return 0;
  for (size_t n = 0; n < count; ++n) {
    const int32_t offset = bit_at(sync, body, padded_bits, (int)n) * (int32_t)SDR_TX_DEVIATION * (inverted ? -1 : 1);
    struct esp32s3_lo_plan plan;
    if (!esp32s3_plan_lo((uint32_t)((int64_t)frequency_hz + offset), ESP32S3_LO_NORMAL, &plan)) return 0;
    words[n] = plan.sdm_word;
  }
  return count;
}
