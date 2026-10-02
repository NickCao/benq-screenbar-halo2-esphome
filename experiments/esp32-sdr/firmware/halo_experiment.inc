// SPDX-License-Identifier: MIT
// Included in esp-sdr's S3 receiver.c: use its calibrated RX and checked bank
// rotation without duplicating undocumented radio register sequences.
#include "demod.h"
#include "halo_version.h"
#include <math.h>
#include "driver/gpio.h"

static sdr_demod_t *halo_demod;
static sdr_config_t halo_config = {.sync_word = 0xCE8C234F, .following_bits = 105, .min_power = 64};
static unsigned halo_window, halo_candidates, halo_complete;

static void halo_board_init(void) {
    // Waveshare ESP32-S3-LR1121-HF: never initialize or transmit with LR1121.
    // Assert reset and deselect SPI before starting the ESP32 receiver.
    ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_39, 0));
    ESP_ERROR_CHECK(gpio_set_direction(GPIO_NUM_39, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_hold_en(GPIO_NUM_39));
    ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_42, 1));
    ESP_ERROR_CHECK(gpio_set_direction(GPIO_NUM_42, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_hold_en(GPIO_NUM_42));
    frequency_mhz = 2405;
}

static void halo_packet(const sdr_packet_t *packet, void *context) {
    (void)context;
    char hex[SDR_MAX_BITS / 4 + 1];
    unsigned bytes = (packet->bit_count + 7) / 8;
    for (unsigned i = 0; i < bytes; ++i) snprintf(hex + i * 2, 3, "%02X", packet->bits[i]);
    hex[bytes * 2] = '\0';
    char line[640];
    snprintf(line, sizeof(line),
        "CANDIDATE {\"window\":%u,\"sync\":\"%08" PRIX32 "\",\"start_us\":%.2f,"
        "\"samples_per_bit\":%.5f,\"offset_hz\":%.0f,\"deviation_hz\":%.0f,"
        "\"sync_error\":%.4f,\"power\":%.1f,\"inverted\":%s,\"bits\":%u,"
        "\"bit_margin_min\":%.4f,\"timing_error_rms\":%.4f,"
        "\"complete\":%s,\"raw\":\"%s\"}\n",
        halo_window, packet->sync_word, (double)packet->start_sample,
        (double)packet->samples_per_bit, (double)packet->offset_hz,
        (double)packet->deviation_hz, (double)packet->sync_error, (double)packet->mean_power,
        packet->inverted ? "true" : "false", packet->bit_count,
        (double)packet->min_bit_margin, (double)packet->timing_error_rms,
        packet->complete ? "true" : "false", hex);
    reply(line);
    ++halo_candidates;
    if (packet->complete) ++halo_complete;
}

static bool halo_command(const char *line) {
    unsigned count; uint32_t word; float power; char extra;
    if (!strcmp(line, "HALOINFO?")) {
        char out[320];
        snprintf(out, sizeof(out),
            "HALOINFO {\"backend\":\"esp32s3-iq\",\"build\":\"" HALO_BUILD_ID "\",\"frequency_mhz\":%u,"
            "\"sample_rate\":%u,\"bitrate\":125000,\"sync\":\"%08" PRIX32 "\","
            "\"following_bits\":%u,\"min_power\":%.1f,\"lr1121_reset\":true,\"crc_checked\":false}\n",
            frequency_mhz, SDR_INPUT_RATE, halo_config.sync_word,
            halo_config.following_bits, (double)halo_config.min_power);
        reply(out);
        return true;
    }
    if (sscanf(line, "HALOSYNC %" SCNx32 " %c", &word, &extra) == 1 && word != 0 && word != UINT32_MAX) {
        halo_config.sync_word = word; reply("OK\n"); return true;
    }
    if (sscanf(line, "HALOBITS %u %c", &count, &extra) == 1 && count && count <= SDR_MAX_BITS) {
        halo_config.following_bits = count; reply("OK\n"); return true;
    }
    if (sscanf(line, "HALOPOWER %f %c", &power, &extra) == 1 && isfinite(power) && power >= 1 && power <= 65535) {
        halo_config.min_power = power; reply("OK\n"); return true;
    }
    if (sscanf(line, "HALORUN %u %c", &count, &extra) != 1 || !count || count > 1000) return false;
    if (!halo_demod) halo_demod = heap_caps_calloc(1, sizeof(*halo_demod), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!halo_demod) { reply("ERR demod_memory\n"); return true; }
    reply("HALOBEGIN\n");
    halo_candidates = halo_complete = 0;
    uint64_t samples = 0;
    unsigned windows = 0, failure = 0, detail = 0, peak_power = 0;
    int64_t started = esp_timer_get_time();
    for (halo_window = 0; halo_window < count; ++halo_window) {
        const ring_config_t capture = {.mode = RING_MODE_CAPTURE, .rate = 6, .capture_units = 3};
        ring_result_t result;
        prepare_rx();
        rx_filter_apply();
        ring_capture_run(&capture, &result);
        rx_filter_restore();
        if (result.status) { failure = result.status; detail = result.detail; break; }
        sdr_reset(halo_demod);
        for (unsigned unit = 0; unit < result.units; ++unit) {
            const ring_unit_t *part = &result.cap[unit];
            const uint32_t *data = ring_capture_bank(part->bank);
            unsigned head = RING_PAIRS - part->first;
            if (head > part->count) head = part->count;
            sdr_push(halo_demod, data + part->first, head);
            sdr_push(halo_demod, data, part->count - head);
        }
        if (halo_demod->overflow) { failure = 100; break; }
        samples += result.pairs;
        ++windows;
        for (unsigned n = 0; n < halo_demod->count; ++n)
            if (halo_demod->power[n] > peak_power) peak_power = halo_demod->power[n];
        sdr_detect(halo_demod, &halo_config, halo_packet, NULL);
        vTaskDelay(1);
    }
    char out[320];
    snprintf(out, sizeof(out),
        "HALOEND {\"windows\":%u,\"samples\":%" PRIu64 ",\"rf_us\":%" PRIu64 ","
        "\"elapsed_us\":%" PRIi64 ",\"candidates\":%u,\"complete\":%u,"
        "\"peak_power\":%u,\"error\":%u,\"detail\":%u}\n",
        windows, samples, samples / (SDR_INPUT_RATE / 1000000), esp_timer_get_time() - started,
        halo_candidates, halo_complete, peak_power, failure, detail);
    reply(out);
    return true;
}
