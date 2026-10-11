// SPDX-License-Identifier: MIT
// Optional receive-only LR1121 reference. No SetTx, AutoTx or PA commands.
#include "driver/spi_master.h"
#include "esp_rom_sys.h"

static spi_device_handle_t halo_reference;
static bool halo_reference_active;
static unsigned halo_reference_op, halo_reference_status;

static bool halo_reference_wait(void) {
    int64_t until = esp_timer_get_time() + 500000;
    while (gpio_get_level(GPIO_NUM_41)) {
        if (esp_timer_get_time() >= until) { halo_reference_status = 0x100000; return false; }
        esp_rom_delay_us(10);
    }
    return true;
}

static bool halo_reference_transfer(const uint8_t *tx, uint8_t *rx, size_t bytes) {
    if (!halo_reference_wait()) return false;
    spi_transaction_t transfer = {.length = bytes * 8, .tx_buffer = tx, .rx_buffer = rx};
    if (spi_device_polling_transmit(halo_reference, &transfer) != ESP_OK) return false;
    esp_rom_delay_us(1);
    return halo_reference_wait();
}

static bool halo_reference_write(uint16_t opcode, const uint8_t *data, size_t size) {
    halo_reference_op = opcode;
    uint8_t message[32] = {(uint8_t)(opcode >> 8), (uint8_t)opcode};
    if (size > sizeof(message) - 2) return false;
    if (size) memcpy(message + 2, data, size);
    return halo_reference_transfer(message, NULL, size + 2);
}

static bool halo_reference_read(uint16_t opcode, const uint8_t *args, size_t nargs,
                                 uint8_t *data, size_t size) {
    uint8_t dummy[34] = {0}, response[34];
    if (size > sizeof(dummy) - 1 || !halo_reference_write(opcode, args, nargs) ||
        !halo_reference_transfer(dummy, response, size + 1)) return false;
    unsigned status = (response[0] >> 1) & 7;
    halo_reference_status = response[0];
    if (status != 2 && status != 3) return false;
    memcpy(data, response + 1, size);
    return true;
}

static void halo_reference_end(void) {
    if (halo_reference) { spi_bus_remove_device(halo_reference); halo_reference = NULL; }
    if (halo_reference_active) spi_bus_free(SPI2_HOST);
    halo_reference_active = false;
    gpio_set_level(GPIO_NUM_39, 0);
    gpio_reset_pin(GPIO_NUM_42);
    gpio_set_level(GPIO_NUM_42, 1);
    gpio_set_direction(GPIO_NUM_42, GPIO_MODE_OUTPUT);
    gpio_hold_en(GPIO_NUM_39);
    gpio_hold_en(GPIO_NUM_42);
}

static bool halo_reference_begin(unsigned bytes, bool inverted) {
    halo_reference_op = halo_reference_status = 0;
    gpio_hold_dis(GPIO_NUM_39);
    gpio_hold_dis(GPIO_NUM_42);
    gpio_reset_pin(GPIO_NUM_39);
    gpio_set_direction(GPIO_NUM_39, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_direction(GPIO_NUM_41, GPIO_MODE_INPUT);
    gpio_set_direction(GPIO_NUM_38, GPIO_MODE_INPUT);
    gpio_set_level(GPIO_NUM_39, 0);
    esp_rom_delay_us(1000);
    spi_bus_config_t bus = {.mosi_io_num = 45, .miso_io_num = 46, .sclk_io_num = 40,
                             .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 34};
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_DISABLED) != ESP_OK) return false;
    halo_reference_active = true;
    spi_device_interface_config_t device = {.clock_speed_hz = 1000000, .mode = 0,
                                             .spics_io_num = 42, .queue_size = 1};
    if (spi_bus_add_device(SPI2_HOST, &device, &halo_reference) != ESP_OK) return false;
    gpio_set_level(GPIO_NUM_39, 1);
    esp_rom_delay_us(10000);
    uint8_t version[4], errors[2];
    if (!halo_reference_read(0x0101, NULL, 0, version, 4)) return false;
    if (version[1] != 3) { halo_reference_status = 0x100u | version[1]; return false; }
    #define REF_WRITE(op, ...) do { const uint8_t data[] = {__VA_ARGS__}; \
        if (!halo_reference_write(op, data, sizeof(data))) return false; } while (0)
    REF_WRITE(0x011C, 0); // Standby RC.
    REF_WRITE(0x0110, 1); // DC-DC regulator.
    REF_WRITE(0x0112, 3, 1, 1, 1, 1, 1, 0, 0); // Sub-GHz switch on receive branch.
    REF_WRITE(0x0117, 6, 0, 1, 0x2C); // 3.0 V TCXO.
    if (!halo_reference_write(0x010E, NULL, 0)) return false;
    REF_WRITE(0x010F, 0x3F);
    if (!halo_reference_read(0x010D, NULL, 0, errors, 2)) return false;
    if (errors[0] || errors[1]) { halo_reference_status = 0x10000u | (errors[0] << 8) | errors[1]; return false; }
    REF_WRITE(0x020E, 1); // GFSK.
    uint32_t hz = frequency_mhz * 1000000u;
    REF_WRITE(0x020B, hz >> 24, hz >> 16, hz >> 8, hz);
    REF_WRITE(0x020F, 0, 1, 0xE8, 0x48, 9, 9, 0, 2, 0x71, 0); // 125 kbps, BT=.5, 467 kHz, 160 kHz deviation.
    REF_WRITE(0x0210, 0, 32, 0, 32, 0, 0, bytes, 1, 0); // Fixed raw length, no CRC/whitening/preamble gate.
    uint32_t sync = halo_config.sync_word;
    if (inverted) sync = ~sync;
    REF_WRITE(0x0206, sync >> 24, sync >> 16, sync >> 8, sync, 0, 0, 0, 0);
    REF_WRITE(0x0213, 1); // RX fallback to standby.
    REF_WRITE(0x0227, 1); // Boosted RX.
    REF_WRITE(0x020C, 0xFF, 0xFF, 0xFF, 1, 0, 0, 0); // Disable automatic transmission.
    REF_WRITE(0x0113, 0, 0, 0, 8, 0, 0, 0, 0); // RX_DONE only.
    REF_WRITE(0x0114, 0xFF, 0xFF, 0xFF, 0xFF);
    if (!halo_reference_write(0x010B, NULL, 0)) return false;
    REF_WRITE(0x0209, 0, 0, 0);
    #undef REF_WRITE
    return true;
}

static unsigned halo_reference_finish(uint8_t *body, unsigned bytes) {
    int64_t until = esp_timer_get_time() + 20000;
    while (!gpio_get_level(GPIO_NUM_38) && esp_timer_get_time() < until) esp_rom_delay_us(10);
    uint8_t dummy[6] = {0}, irq[6], status[2];
    if (!halo_reference_transfer(dummy, irq, 6) || !(irq[5] & 8) ||
        !halo_reference_read(0x0203, NULL, 0, status, 2) || status[0] != bytes) return 0;
    uint8_t args[2] = {status[1], status[0]};
    if (!halo_reference_read(0x010A, args, 2, body, bytes)) return 0;
    return bytes;
}
