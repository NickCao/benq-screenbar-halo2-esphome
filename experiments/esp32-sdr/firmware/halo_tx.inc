// SPDX-License-Identifier: MIT
// Polar TX sequencing adapted from esp32-sdr-trx firmware/src/radio.c (0BSD),
// pinned in upstream.json. The Halo waveform needs 125 kHz updates and both
// SDM bytes: upstream's 40 kHz, low-byte-only voice stream cannot carry it.
#include "tx.h"
#include "esp_attr.h"
#include "esp_rom_regi2c.h"
#include "tx_reference.inc"

extern void txcal_debuge_mode(void);
extern void txcal_work_mode(void);
extern void start_tx_tone_step(int, int, int, int, int, int);
extern unsigned rom_get_i2c_hostid(unsigned);

static unsigned halo_tx_gain = 96;
static bool halo_tx_inverted;

typedef struct {
    unsigned updates, late_updates, max_late_cycles, max_write_cycles, elapsed_cycles;
    bool complete;
} halo_tx_result_t;

// The SDM-only transaction used by the S3 ROM's rom_chip_i2c_writeReg:
// controller 0x6000e000 + 4*host, command 0x05000000 | block | reg<<8 | data<<16,
// busy bit 25. Avoid per-write PHY locks and host setup in the masked hot loop.
// This is NOT the external GPIO I2C peripheral. Gates are saved/restored.
static inline __attribute__((always_inline)) bool halo_sdm_write(uint32_t controller, unsigned reg, unsigned value) {
    unsigned started = esp_cpu_get_cycle_count();
    while (REG_READ(controller) & (1u << 25))
        if (esp_cpu_get_cycle_count() - started > 2400) return false;
    REG_WRITE(controller, 0x05000063u | (reg << 8) | ((value & 255) << 16));
    while (REG_READ(controller) & (1u << 25))
        if (esp_cpu_get_cycle_count() - started > 2400) return false;
    return true;
}

static halo_tx_result_t IRAM_ATTR __attribute__((noinline)) halo_tx_play(const uint32_t *words, unsigned count) {
    halo_tx_result_t result = {0};
    unsigned previous = words[0];
    esp_rom_regi2c_write(0x63, 1, 4, (previous >> 8) & 255);
    esp_rom_regi2c_write(0x63, 1, 5, previous & 255);
    start_tx_tone_step(1, 0, halo_tx_gain, 0, 0, 0);
    unsigned analog_config = REG_READ(0x6000E044u), analog_config2 = REG_READ(0x6000E048u);
    unsigned host = rom_get_i2c_hostid(0x63);
    uint32_t controller = 0x6000E000u + 4u * host;
    REG_CLR_BIT(0x6000E044u, 0x3FF00u);
    // Short settling lead; the following 256 us alternating preamble acquires
    // the receiver. Timing applies to the waveform, excluding this lead.
    uint32_t lead = esp_cpu_get_cycle_count();
    while (esp_cpu_get_cycle_count() - lead < 20u * 240u) {}
    unsigned irq = portSET_INTERRUPT_MASK_FROM_ISR();
    const unsigned period = 240000000u / SDR_TX_RATE;
    // Let the first pass through the loop settle before the waveform epoch.
    uint32_t next = esp_cpu_get_cycle_count() + 10u * 240u, started = next;
    for (unsigned n = 0; n < count; ++n, next += period) {
        while ((int32_t)(esp_cpu_get_cycle_count() - next) < 0) {}
        uint32_t now = esp_cpu_get_cycle_count();
        unsigned late = now - next;
        if (late > result.max_late_cycles) result.max_late_cycles = late;
        if (late > 120) ++result.late_updates; // More than 0.5 us late.
        if (late >= period) break; // Abort instead of stretching the bit clock.
        uint32_t word = words[n];
        if ((word >> 8) != (previous >> 8) && !halo_sdm_write(controller, 4, word >> 8)) break;
        if ((word & 255) != (previous & 255) && !halo_sdm_write(controller, 5, word)) break;
        unsigned cycles = esp_cpu_get_cycle_count() - now;
        if (cycles > result.max_write_cycles) result.max_write_cycles = cycles;
        previous = word;
        ++result.updates;
    }
    if (result.updates == count) {
        while ((int32_t)(esp_cpu_get_cycle_count() - next) < 0) {}
        result.complete = true;
    }
    start_tx_tone_step(0, 0, 0, 0, 0, 0); // Every exit unkeys.
    result.elapsed_cycles = esp_cpu_get_cycle_count() - started;
    REG_WRITE(0x6000E044u, analog_config);
    REG_WRITE(0x6000E048u, analog_config2);
    portCLEAR_INTERRUPT_MASK_FROM_ISR(irq);
    return result;
}

static bool halo_tx_command(const char *line) {
    unsigned gain, inverted, bits; char hex[SDR_TX_MAX_BITS / 4 + 1], extra;
    if (sscanf(line, "HALOTXGAIN %u %c", &gain, &extra) == 1 && gain >= 64 && gain <= 127) {
        halo_tx_gain = gain; reply("OK\n"); return true;
    }
    if (sscanf(line, "HALOTXPOLARITY %u %c", &inverted, &extra) == 1 && inverted <= 1) {
        halo_tx_inverted = inverted; reply("OK\n"); return true;
    }
    bool reference = !strncmp(line, "HALOTXCHECK ", 12);
    if (!reference && strncmp(line, "HALOTX ", 7)) return false;
    const char *args = line + (reference ? 12 : 7);
    uint8_t body[SDR_TX_MAX_BITS / 8];
    if (sscanf(args, "%u %64s %c", &bits, hex, &extra) != 2 || !sdr_tx_parse(hex, bits, body) ||
        frequency_mhz < 2400 || frequency_mhz > 2483) {
        reply("ERR tx_arguments\n"); return true;
    }
    // ADC capture and TX are serialized. Keep the schedule in internal DRAM,
    // independent of RF bank ownership and of PSRAM cache misses.
    if (REG_READ(0x60033D5Cu) & (1u << 31)) { reply("ERR rx_active\n"); return true; }
    unsigned owner = REG_READ(SRAM_OWNER_REG);
    REG_WRITE(SRAM_OWNER_REG, 0);
    uint32_t *words = heap_caps_malloc(SDR_TX_MAX_UPDATES * sizeof(*words), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!words) { REG_WRITE(SRAM_OWNER_REG, owner); reply("ERR tx_memory\n"); return true; }
    unsigned count = sdr_tx_waveform(frequency_mhz * 1000000u, halo_config.sync_word, body,
                                     bits, halo_tx_inverted, words, SDR_TX_MAX_UPDATES);
    if (!count) { free(words); REG_WRITE(SRAM_OWNER_REG, owner); reply("ERR tx_waveform\n"); return true; }
    if (reference && !halo_reference_begin((bits + 7) / 8, halo_tx_inverted)) {
        unsigned reset_level = gpio_get_level(GPIO_NUM_39), busy_level = gpio_get_level(GPIO_NUM_41);
        halo_reference_end(); free(words); REG_WRITE(SRAM_OWNER_REG, owner);
        char error[100];
        snprintf(error, sizeof(error), "ERR reference_init op=%04X status=%X busy=%u reset=%u\n",
                 halo_reference_op, halo_reference_status, busy_level, reset_level);
        reply(error); return true;
    }
    // Hand the PHY back its RX forces before entering its calibrated TX test
    // mode, as release_receiver()/radio_tx_begin() in esp32-sdr-trx do.
    REG_CLR_BIT(0x6001C080u, 0xC1u);
    REG_CLR_BIT(0x60006110u, 0xCF00u);
    REG_CLR_BIT(0x6002600Cu, 2u);
    phy_pbus_workmode();
    txcal_debuge_mode();
    s3_tune(frequency_mhz); // Test-mode entry may move the PLL.
    uint32_t center = (esp_rom_regi2c_read(0x63, 1, 3) << 16) |
                      (esp_rom_regi2c_read(0x63, 1, 4) << 8) | esp_rom_regi2c_read(0x63, 1, 5);
    // Do not cross the third SDM byte in the hot loop.
    for (unsigned n = 0; n < count; ++n) {
        if ((words[n] >> 16) != (center >> 16)) {
            txcal_work_mode(); rx_ready = false; prepare_rx();
            if (reference) halo_reference_end();
            free(words); REG_WRITE(SRAM_OWNER_REG, owner); reply("ERR tx_pll_range\n"); return true;
        }
    }
    halo_tx_result_t result = halo_tx_play(words, count);
    free(words);
    uint8_t received[SDR_TX_MAX_BITS / 8] = {0};
    unsigned received_bytes = reference ? halo_reference_finish(received, (bits + 7) / 8) : 0;
    if (reference) halo_reference_end();
    txcal_work_mode(); rx_ready = false; prepare_rx();
    REG_WRITE(SRAM_OWNER_REG, owner);
    unsigned byte_count = (bits + 7) / 8;
    bool match = received_bytes == byte_count;
    char wire_hex[SDR_TX_MAX_BITS / 4 + 1] = {0};
    for (unsigned b = 0; b < received_bytes; ++b) {
        snprintf(wire_hex + b * 2, 3, "%02X", received[b]);
        if (halo_tx_inverted) received[b] = ~received[b];
    }
    if (bits % 8) received[byte_count - 1] &= (uint8_t)(0xFFu << (8 - bits % 8));
    if (match) match = !memcmp(body, received, byte_count);
    char received_hex[SDR_TX_MAX_BITS / 4 + 1] = {0};
    for (unsigned b = 0; b < received_bytes; ++b) snprintf(received_hex + b * 2, 3, "%02X", received[b]);
    char out[640];
    snprintf(out, sizeof(out),
        "HALOTXEND {\"backend\":\"esp32s3-pll\",\"bits\":%u,\"updates\":%u,"
        "\"expected_updates\":%u,\"complete\":%s,\"elapsed_us\":%.2f,\"gain_code\":%u,"
        "\"inverted\":%s,\"late_updates\":%u,\"max_late_cycles\":%u,\"max_write_cycles\":%u,"
        "\"rx_restored\":%s,\"lr1121_reset\":true,\"reference_enabled\":%s,"
        "\"reference_received\":%s,\"reference_match\":%s,\"reference_raw\":\"%s\","
        "\"reference_raw_on_air\":\"%s\"}\n",
        bits, result.updates, count, result.complete ? "true" : "false", result.elapsed_cycles / 240.0,
        halo_tx_gain, halo_tx_inverted ? "true" : "false", result.late_updates, result.max_late_cycles,
        result.max_write_cycles, rx_ready ? "true" : "false", reference ? "true" : "false",
        received_bytes ? "true" : "false", match ? "true" : "false", received_hex, wire_hex);
    reply(out);
    return true;
}
