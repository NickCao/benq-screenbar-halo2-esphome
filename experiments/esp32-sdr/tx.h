// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SDR_TX_MAX_BITS 256u
#define SDR_TX_PREAMBLE_BITS 32u
#define SDR_TX_SAMPLES_PER_BIT 1u
#define SDR_TX_RATE 125000u
#define SDR_TX_DEVIATION 160000u
#define SDR_TX_MAX_UPDATES ((SDR_TX_PREAMBLE_BITS + 32u + SDR_TX_MAX_BITS) * SDR_TX_SAMPLES_PER_BIT)

// Following bits are MSB first; unused low bits must be zero. No CRC is added.
bool sdr_tx_parse(const char *hex, unsigned bits, uint8_t *body);
// One FSK tone per bit, expressed as absolute PLL SDM words. No calibrated
// Gaussian shaping is claimed for the PLL's hardware transition response.
// Uses the pinned esp32-sdr-trx LO planner. Caller owns the storage; no heap.
size_t sdr_tx_waveform(uint32_t frequency_hz, uint32_t sync, const uint8_t *body,
                       unsigned bits, bool inverted, uint32_t *words, size_t capacity);

#ifdef __cplusplus
}
#endif
