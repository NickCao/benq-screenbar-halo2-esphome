// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ESP32-S3 ADC dump: 16 MS/s, signed 10-bit I and Q in each 32-bit word.
// Processing is deliberately after capture; no real-time CPU budget assumed.
#define SDR_INPUT_RATE 16000000u
#define SDR_DECIMATION 16u
#define SDR_SAMPLE_RATE (SDR_INPUT_RATE / SDR_DECIMATION)
#define SDR_MAX_SAMPLES 4096u
#define SDR_MAX_BITS 256u

typedef struct {
  uint32_t sync_word;       // On-air order, most significant bit first.
  unsigned following_bits; // Fixed observation length; no payload parsing.
  float min_power;         // Mean squared ADC units after filtering, not dBm.
} sdr_config_t;

typedef struct {
  uint32_t sync_word;
  float start_sample; // Sync start in the decimated capture, includes filter delay.
  float samples_per_bit;
  float offset_hz;
  float deviation_hz;
  float sync_error;   // RMS tone error / estimated deviation.
  float mean_power;
  float min_bit_margin; // Smallest following-bit distance from the slicer, / deviation.
  float timing_error_rms; // Following transition timing error in decimated samples.
  unsigned bit_count;
  bool inverted;     // Conjugated I/Q / opposite FSK polarity was detected.
  bool complete;     // Observation length reached, NOT CRC validation.
  uint8_t bits[SDR_MAX_BITS / 8]; // MSB first, unused low bits zero.
} sdr_packet_t;

typedef void (*sdr_packet_fn)(const sdr_packet_t *packet, void *context);

typedef struct {
  // Two-stage CIC decimator. Unsigned arithmetic provides defined modulo
  // overflow; its signed comb output fits comfortably in 32 bits.
  uint32_t integrator[2][2], comb[2][2];
  unsigned phase, count;
  float dc_i, dc_q, previous_i, previous_q;
  bool previous_valid, overflow;
  int16_t frequency[SDR_MAX_SAMPLES]; // Hz / 16, +/- 524 kHz.
  uint16_t power[SDR_MAX_SAMPLES];
} sdr_demod_t;

void sdr_reset(sdr_demod_t *demod);
// Calls can split one contiguous capture at arbitrary boundaries. Reset
// between captures: samples on opposite sides of a gap must never join.
bool sdr_push(sdr_demod_t *demod, const uint32_t *words, size_t count);
// Searches both FSK polarities. The sync word supplies timing acquisition;
// short or absent preambles are allowed. No command, length or CRC decoding.
unsigned sdr_detect(const sdr_demod_t *demod, const sdr_config_t *config,
                    sdr_packet_fn on_packet, void *context);

#ifdef __cplusplus
}
#endif
