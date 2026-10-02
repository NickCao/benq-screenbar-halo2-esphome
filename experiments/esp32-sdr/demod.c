// SPDX-License-Identifier: MIT
#include "demod.h"

#include <math.h>
#include <string.h>

#define BIT_PERIOD 8.0f // 1 MS/s / 125 kbit/s.
#define TWO_PI 6.2831853071795864769f

static float clampf(float value, float low, float high) {
  return fminf(high, fmaxf(low, value));
}

void sdr_reset(sdr_demod_t *d) { memset(d, 0, sizeof(*d)); }

static int32_t signed10(uint32_t value) {
  return (int32_t)(value & 511u) - (int32_t)(value & 512u);
}

bool sdr_push(sdr_demod_t *d, const uint32_t *words, size_t count) {
  if (d->overflow) return false;
  for (size_t n = 0; n < count; ++n) {
    const int32_t iq[2] = {signed10(words[n] >> 10), signed10(words[n])};
    for (unsigned k = 0; k < 2; ++k) {
      d->integrator[k][0] += (uint32_t)iq[k];
      d->integrator[k][1] += d->integrator[k][0];
    }
    if (++d->phase != SDR_DECIMATION) continue;
    d->phase = 0;
    if (d->count == SDR_MAX_SAMPLES) {
      d->overflow = true;
      return false;
    }
    float filtered[2];
    for (unsigned k = 0; k < 2; ++k) {
      const uint32_t first = d->integrator[k][1] - d->comb[k][0];
      const uint32_t second = first - d->comb[k][1];
      d->comb[k][0] = d->integrator[k][1];
      d->comb[k][1] = first;
      // Explicit sign extension avoids implementation-defined unsigned casts.
      const int64_t value = (int64_t)(second & 0x7fffffffu) - (int64_t)(second & 0x80000000u);
      filtered[k] = (float)value / (SDR_DECIMATION * SDR_DECIMATION);
    }
    // Remove receiver DC without a notch near either +/-160 kHz tone.
    d->dc_i += (filtered[0] - d->dc_i) / 128.0f;
    d->dc_q += (filtered[1] - d->dc_q) / 128.0f;
    const float i = filtered[0] - d->dc_i, q = filtered[1] - d->dc_q;
    const float power = i * i + q * q;
    float frequency = 0;
    if (d->previous_valid) {
      const float cross = q * d->previous_i - i * d->previous_q;
      const float dot = i * d->previous_i + q * d->previous_q;
      frequency = atan2f(cross, dot) * (SDR_SAMPLE_RATE / TWO_PI / 16.0f);
    }
    d->frequency[d->count] = (int16_t)lrintf(clampf(frequency, -32767, 32767));
    d->power[d->count++] = (uint16_t)lrintf(clampf(power, 0, 65535));
    d->previous_i = i;
    d->previous_q = q;
    d->previous_valid = power > 1;
  }
  return true;
}

static bool symbol(const sdr_demod_t *d, float edge, float period, float *value, float *power) {
  // Integrate the middle half of the symbol, away from Gaussian transitions.
  const int first = (int)ceilf(edge + period * .25f);
  const int end = (int)ceilf(edge + period * .75f);
  if (first < 0 || end > (int)d->count || end <= first) return false;
  float sum = 0, energy = 0;
  for (int n = first; n < end; ++n) {
    sum += d->frequency[n] * 16.0f;
    energy += d->power[n];
  }
  *value = sum / (end - first);
  *power = energy / (end - first);
  return true;
}

static bool sync_bit(uint32_t word, unsigned bit) { return ((word >> (31u - bit)) & 1u) != 0; }

typedef struct {
  float center, deviation, error, power;
  bool inverted;
} sync_fit_t;

static bool fit_sync(const sdr_demod_t *d, const sdr_config_t *config, float edge, float period,
                     sync_fit_t *fit) {
  float values[32], means[2] = {0}, energy = 0;
  unsigned counts[2] = {0};
  const bool nominal = period == BIT_PERIOD && edge >= 0 && floorf(edge) == edge;
  for (unsigned b = 0; b < 32; ++b) {
    float power;
    if (nominal) {
      // The scan checks integer phases at the nominal rate. Avoid floating
      // rounding and division for every symbol of every rejected candidate.
      const unsigned first = (unsigned)edge + b * 8 + 2;
      if (first + 4 > d->count) return false;
      int32_t sum = 0;
      uint32_t energy_sum = 0;
      for (unsigned n = first; n < first + 4; ++n) {
        sum += d->frequency[n];
        energy_sum += d->power[n];
      }
      values[b] = sum * 4.0f;
      power = energy_sum * .25f;
    } else if (!symbol(d, edge + b * period, period, &values[b], &power)) {
      return false;
    }
    if (power < config->min_power) return false;
    const unsigned bit = sync_bit(config->sync_word, b);
    means[bit] += values[b];
    ++counts[bit];
    energy += power;
  }
  if (!counts[0] || !counts[1]) return false;
  means[0] /= counts[0];
  means[1] /= counts[1];
  const float difference = means[1] - means[0];
  fit->deviation = fabsf(difference) * .5f;
  if (fit->deviation < 80000 || fit->deviation > 240000) return false;
  fit->center = (means[0] + means[1]) * .5f;
  fit->inverted = difference < 0;
  fit->power = energy / 32;
  float error = 0;
  for (unsigned b = 0; b < 32; ++b) {
    const bool bit = sync_bit(config->sync_word, b);
    if (((values[b] > fit->center) != fit->inverted) != bit) return false;
    const float residual = (values[b] - means[bit]) / fit->deviation;
    error += residual * residual;
  }
  fit->error = sqrtf(error / 32);
  return fit->error < .35f;
}

static bool crossing(const sdr_demod_t *d, float expected, float radius, float center, bool rising,
                     float *position) {
  const int first = (int)fmaxf(1, floorf(expected - radius));
  const int last = (int)fminf(d->count - 1, ceilf(expected + radius));
  float distance = radius + 1;
  bool found = false;
  for (int n = first; n <= last; ++n) {
    const float before = d->frequency[n - 1] * 16.0f - center;
    const float after = d->frequency[n] * 16.0f - center;
    if (rising ? (before > 0 || after <= 0) : (before < 0 || after >= 0)) continue;
    const float candidate = n - 1 + before / (before - after);
    const float delta = fabsf(candidate - expected);
    if (delta < distance && delta <= radius) {
      *position = candidate;
      distance = delta;
      found = true;
    }
  }
  return found;
}

static void recover_clock(const sdr_demod_t *d, uint32_t sync, const sync_fit_t *fit,
                          float *edge, float *period) {
  // Fit a line through known sync transitions to recover phase AND rate.
  float sx = 0, sy = 0, sxx = 0, sxy = 0, count = 0;
  for (unsigned b = 1; b < 32; ++b) {
    if (sync_bit(sync, b) == sync_bit(sync, b - 1)) continue;
    float at;
    if (!crossing(d, *edge + b * *period, *period * .45f, fit->center,
                  sync_bit(sync, b) != fit->inverted, &at)) continue;
    sx += b;
    sy += at;
    sxx += b * b;
    sxy += b * at;
    ++count;
  }
  const float denominator = count * sxx - sx * sx;
  if (count < 6 || denominator <= 0) return;
  const float recovered = (count * sxy - sx * sy) / denominator;
  if (fabsf(recovered - BIT_PERIOD) > BIT_PERIOD * .02f) return;
  *period = recovered;
  *edge = (sy - recovered * sx) / count;
}

unsigned sdr_detect(const sdr_demod_t *d, const sdr_config_t *config,
                    sdr_packet_fn on_packet, void *context) {
  if (d->overflow || d->count < 260 || !config || config->following_bits > SDR_MAX_BITS ||
      !config->following_bits || !isfinite(config->min_power) || config->min_power < 0) return 0;
  unsigned packets = 0;
  for (unsigned start = 2; start + 32 * (unsigned)BIT_PERIOD < d->count; ++start) {
    sync_fit_t fit;
    float edge = start, period = BIT_PERIOD;
    if (!fit_sync(d, config, edge, period, &fit)) continue;
    recover_clock(d, config->sync_word, &fit, &edge, &period);
    if (!fit_sync(d, config, edge, period, &fit)) continue;
    sdr_packet_t packet = {.sync_word = config->sync_word, .start_sample = edge,
                           .samples_per_bit = period, .offset_hz = fit.center,
                           .deviation_hz = fit.deviation, .sync_error = fit.error,
                           .mean_power = fit.power, .min_bit_margin = INFINITY, .inverted = fit.inverted};
    float timing_squared = 0;
    unsigned timing_count = 0;
    bool previous = sync_bit(config->sync_word, 31);
    edge += 32 * period;
    for (unsigned b = 0; b < config->following_bits; ++b) {
      float value, power;
      if (!symbol(d, edge, period, &value, &power) || power < config->min_power) break;
      bool bit = (value > fit.center) != fit.inverted;
      if (bit != previous) {
        float at;
        if (crossing(d, edge, period * .3f, fit.center, bit != fit.inverted, &at)) {
          const float error = at - edge;
          timing_squared += error * error;
          ++timing_count;
          edge += .5f * error;
          period = clampf(period + .01f * error, BIT_PERIOD * .98f, BIT_PERIOD * 1.02f);
          if (!symbol(d, edge, period, &value, &power) || power < config->min_power) break;
          bit = (value > fit.center) != fit.inverted;
        }
      }
      if (bit) packet.bits[b / 8] |= 1u << (7u - b % 8);
      packet.min_bit_margin = fminf(packet.min_bit_margin, fabsf(value - fit.center) / fit.deviation);
      ++packet.bit_count;
      previous = bit;
      edge += period;
    }
    packet.complete = packet.bit_count == config->following_bits;
    if (!packet.bit_count) packet.min_bit_margin = 0;
    packet.timing_error_rms = timing_count ? sqrtf(timing_squared / timing_count) : 0;
    if (on_packet) on_packet(&packet, context);
    ++packets;
    // One candidate per observed frame, even if several sample phases match.
    start = (unsigned)fmaxf(start, floorf(edge - 1));
  }
  return packets;
}
