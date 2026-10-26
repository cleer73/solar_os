#include "epd_ssd1685.h"

#include <stddef.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_buses.h"

#define BUSY_TIMEOUT_US 30000000LL
#define AUTO_FULL_INTERVAL 6U
#define TRY(call) do { esp_err_t result_ = (call); if (result_ != ESP_OK) return result_; } while (0)

static void delay_ms(unsigned ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    vTaskDelay(ticks ? ticks : 1);
}

static esp_err_t epd_ssd1685_command(epd_ssd1685_t *d, uint8_t command,
                               const uint8_t *data, size_t length)
{
    if (!d || !d->spi || length > EPD_SSD1685_ROW_BYTES || (length && !data))
        return ESP_ERR_INVALID_ARG;
    TRY(gpio_set_level(d->dc_pin, 0));
    spi_transaction_t transaction = {
        .flags = SPI_TRANS_USE_TXDATA, .length = 8, .tx_data = {command},
    };
    TRY(spi_device_polling_transmit(d->spi, &transaction));
    if (!length) return ESP_OK;
    TRY(gpio_set_level(d->dc_pin, 1));
    memcpy(d->line_buffer, data, length);
    transaction = (spi_transaction_t){.length = length * 8U, .tx_buffer = d->line_buffer};
    return spi_device_polling_transmit(d->spi, &transaction);
}

static esp_err_t epd_ssd1685_wait_ready(epd_ssd1685_t *d)
{
    const int64_t deadline = esp_timer_get_time() + BUSY_TIMEOUT_US;
    /* BUSY is active high; allow delayed assertion after master activation. */
    delay_ms(10);
    while (gpio_get_level(d->busy_pin) != 0) {
        if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
        delay_ms(20);
    }
    return ESP_OK;
}

static esp_err_t external_power(epd_ssd1685_t *d, bool on)
{
    if (d->power_pin >= 0) TRY(gpio_set_level(d->power_pin, on));
    d->powered = on;
    if (on) delay_ms(20);
    return ESP_OK;
}

/* GDEY029T71H connects sources S8..S175. Override the OTP's 168-source
 * setting with 200-source RAM addressing before selecting bytes 1..21.
 * Register definitions: Solomon Systech SSD1685 rev 0.11, commands 21/44/45.
 * Panel: https://www.good-display.com/product/540.html */
static esp_err_t set_window(epd_ssd1685_t *d)
{
    TRY(epd_ssd1685_command(d, 0x11, (uint8_t[]){3}, 1));
    TRY(epd_ssd1685_command(d, 0x44, (uint8_t[]){1, 21}, 2));
    TRY(epd_ssd1685_command(d, 0x45, (uint8_t[]){0, 0, 0x7f, 1}, 4));
    TRY(epd_ssd1685_command(d, 0x4e, (uint8_t[]){1}, 1));
    return epd_ssd1685_command(d, 0x4f, (uint8_t[]){0, 0}, 2);
}

static esp_err_t controller_init(epd_ssd1685_t *d)
{
    d->controller_ready = d->shadow_valid = d->analog_on = false;
    d->partial_count = 0;
    TRY(external_power(d, true));
    TRY(gpio_set_level(d->reset_pin, 1)); delay_ms(20);
    TRY(gpio_set_level(d->reset_pin, 0)); delay_ms(10);
    TRY(gpio_set_level(d->reset_pin, 1)); delay_ms(20);
    TRY(epd_ssd1685_wait_ready(d));
    TRY(epd_ssd1685_command(d, 0x12, NULL, 0));
    TRY(epd_ssd1685_wait_ready(d));
    TRY(epd_ssd1685_command(d, 0x01, (uint8_t[]){0x7f, 1, 0}, 3));
    TRY(epd_ssd1685_command(d, 0x3c, (uint8_t[]){5}, 1));
    TRY(epd_ssd1685_command(d, 0x18, (uint8_t[]){0x80}, 1));
    TRY(epd_ssd1685_command(d, 0x21, (uint8_t[]){0, 0}, 2));
    TRY(set_window(d));
    d->controller_ready = true;
    return ESP_OK;
}

/* Convert SolarOS vertical LSB-first tiles to horizontal MSB-first panel RAM.
 * Both formats use 1=white. Release the shared SPI bus before waiting on BUSY. */
static esp_err_t write_plane(epd_ssd1685_t *d, uint8_t command, const uint8_t *frame)
{
    TRY(set_window(d));
    TRY(epd_ssd1685_command(d, command, NULL, 0));
    TRY(gpio_set_level(d->dc_pin, 1));
    TRY(spi_device_acquire_bus(d->spi, portMAX_DELAY));
    esp_err_t err = ESP_OK;
    for (unsigned y = 0; y < EPD_SSD1685_HEIGHT && err == ESP_OK; ++y) {
        memset(d->line_buffer, 0, EPD_SSD1685_ROW_BYTES);
        const uint8_t *tile = frame + (y / 8U) * EPD_SSD1685_WIDTH;
        for (unsigned x = 0; x < EPD_SSD1685_WIDTH; ++x) {
            if (tile[x] & (1U << (y % 8U)))
                d->line_buffer[x / 8U] |= (uint8_t)(0x80U >> (x % 8U));
        }
        spi_transaction_t transaction = {
            .flags = y + 1U < EPD_SSD1685_HEIGHT ? SPI_TRANS_CS_KEEP_ACTIVE : 0,
            .length = EPD_SSD1685_ROW_BYTES * 8U, .tx_buffer = d->line_buffer,
        };
        err = spi_device_polling_transmit(d->spi, &transaction);
    }
    if (err != ESP_OK) {
        spi_transaction_t end = {.length = 0};
        (void)spi_device_polling_transmit(d->spi, &end);
    }
    spi_device_release_bus(d->spi);
    return err;
}

static esp_err_t refresh_frame(epd_ssd1685_t *d)
{
    if (!d->controller_ready) TRY(controller_init(d));
    const bool baseline = d->shadow && d->shadow_valid;
    if (baseline && d->refresh_mode != EPD_SSD1685_REFRESH_FULL &&
        memcmp(d->shadow, d->buffer, EPD_SSD1685_BUFFER_BYTES) == 0) return ESP_OK;
    const bool partial = baseline && d->refresh_mode != EPD_SSD1685_REFRESH_FULL &&
        (d->refresh_mode == EPD_SSD1685_REFRESH_PARTIAL ||
         d->partial_count + 1U < AUTO_FULL_INTERVAL);
    /* A partial waveform compares the previous frame in RAM26 with RAM24.
     * Full refresh bypasses RAM26 and loads the temperature-dependent OTP LUT. */
    TRY(epd_ssd1685_command(d, 0x21, (uint8_t[]){partial ? 0 : 0x40, 0}, 2));
    TRY(write_plane(d, 0x26, partial ? d->shadow : d->buffer));
    TRY(write_plane(d, 0x24, d->buffer));
    TRY(epd_ssd1685_command(d, 0x22, (uint8_t[]){partial ? 0xdc : 0xf7}, 1));
    TRY(epd_ssd1685_command(d, 0x20, NULL, 0));
    TRY(epd_ssd1685_wait_ready(d));
    d->analog_on = partial;
    /* Synchronize the differential baseline only after a successful refresh. */
    TRY(write_plane(d, 0x26, d->buffer));
    if (d->shadow) {
        memcpy(d->shadow, d->buffer, EPD_SSD1685_BUFFER_BYTES);
        d->shadow_valid = true;
    }
    d->partial_count = partial ? d->partial_count + 1U : 0;
    return ESP_OK;
}

esp_err_t epd_ssd1685_refresh(epd_ssd1685_t *d)
{
    if (!d || !d->spi || !d->buffer || !d->line_buffer)
        return ESP_ERR_INVALID_STATE;
    esp_err_t err = refresh_frame(d);
    if (err != ESP_OK) d->controller_ready = d->shadow_valid = false;
    d->last_error = err;
    return err;
}

static esp_err_t sleep_panel(epd_ssd1685_t *d)
{
    esp_err_t err = ESP_OK;
    if (d->powered) {
        if (d->analog_on) {
            err = epd_ssd1685_command(d, 0x22, (uint8_t[]){0x83}, 1);
            if (err == ESP_OK) err = epd_ssd1685_command(d, 0x20, NULL, 0);
            if (err == ESP_OK) err = epd_ssd1685_wait_ready(d);
        }
        if (err == ESP_OK) err = epd_ssd1685_command(d, 0x10, (uint8_t[]){1}, 1);
    }
    const esp_err_t power_err = external_power(d, false);
    d->controller_ready = d->shadow_valid = d->analog_on = false;
    return err != ESP_OK ? err : power_err;
}

static const u8x8_display_info_t display_info = {
    .chip_enable_level = 0, .chip_disable_level = 1,
    .reset_pulse_width_ms = 10, .post_reset_wait_ms = 50,
    .sck_clock_hz = 10000000, .spi_mode = 0,
    .tile_width = EPD_SSD1685_WIDTH / 8U,
    .tile_height = EPD_SSD1685_HEIGHT / 8U,
    .pixel_width = EPD_SSD1685_WIDTH, .pixel_height = EPD_SSD1685_HEIGHT,
};

static uint8_t display_cb(u8x8_t *u8x8, uint8_t message, uint8_t arg, void *ptr)
{
    (void)ptr;
    if (message == U8X8_MSG_DISPLAY_SETUP_MEMORY) {
        u8x8_d_helper_display_setup_memory(u8x8, &display_info);
        return 1;
    }
    epd_ssd1685_t *d = (epd_ssd1685_t *)((uint8_t *)u8x8 -
        offsetof(epd_ssd1685_t, u8g2) - offsetof(u8g2_t, u8x8));
    esp_err_t err;
    switch (message) {
    case U8X8_MSG_DISPLAY_INIT: err = controller_init(d); break;
    case U8X8_MSG_DISPLAY_SET_POWER_SAVE:
        err = arg ? sleep_panel(d) : (d->controller_ready ? ESP_OK : controller_init(d));
        break;
    case U8X8_MSG_DISPLAY_REFRESH: err = epd_ssd1685_refresh(d); break;
    case U8X8_MSG_DISPLAY_DRAW_TILE: return 1;
    default: return 0;
    }
    d->last_error = err;
    return err == ESP_OK;
}

static bool config_valid(const epd_ssd1685_config_t *c)
{
    if (!c || !c->spi_bus || !c->spi_bus[0] || !c->rotation ||
        c->spi_clock_hz < 100000 || c->spi_clock_hz > 10000000 || c->power_pin < -1 ||
        c->panel != EPD_SSD1685_PANEL_GDEY029T71H)
        return false;
    const int pins[] = {c->cs_pin, c->dc_pin, c->reset_pin, c->busy_pin, c->power_pin};
    for (unsigned i = 0; i < 5; ++i) {
        if (i == 4 && pins[i] == -1) continue;
        if (pins[i] < 0 || pins[i] >= GPIO_NUM_MAX || pins[i] >= 64) return false;
        for (unsigned j = i + 1; j < 5; ++j) if (pins[i] == pins[j]) return false;
    }
    return true;
}

static esp_err_t configure_pins(epd_ssd1685_t *d)
{
    const gpio_config_t output = {
        .pin_bit_mask = (1ULL << d->dc_pin) | (1ULL << d->reset_pin) |
            (d->power_pin >= 0 ? 1ULL << d->power_pin : 0),
        .mode = GPIO_MODE_OUTPUT, .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE,
    };
    d->pins_configured = true;
    TRY(gpio_config(&output));
    const gpio_config_t input = {
        .pin_bit_mask = 1ULL << d->busy_pin, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    TRY(gpio_config(&input));
    TRY(gpio_set_level(d->dc_pin, 1));
    return gpio_set_level(d->reset_pin, 1);
}

esp_err_t epd_ssd1685_init(epd_ssd1685_t *d, const epd_ssd1685_config_t *c)
{
    if (!d || !config_valid(c))
        return ESP_ERR_INVALID_ARG;
    *d = (epd_ssd1685_t){.dc_pin = c->dc_pin,
        .reset_pin = c->reset_pin, .busy_pin = c->busy_pin, .power_pin = c->power_pin};
    esp_err_t err = configure_pins(d);
    if (err == ESP_OK) err = external_power(d, false);
    if (err == ESP_OK) {
        const spi_device_interface_config_t device_config = {
            .clock_speed_hz = c->spi_clock_hz, .mode = 0,
            .spics_io_num = c->cs_pin, .queue_size = 1,
        };
        err = solar_os_bus_spi_add_device(c->spi_bus, &device_config, &d->spi);
    }
    if (err == ESP_OK) {
        d->line_buffer = heap_caps_malloc(EPD_SSD1685_ROW_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        d->buffer = heap_caps_calloc(1, EPD_SSD1685_BUFFER_BYTES, MALLOC_CAP_8BIT);
        d->shadow = heap_caps_malloc(EPD_SSD1685_BUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!d->shadow) d->shadow = heap_caps_malloc(EPD_SSD1685_BUFFER_BYTES, MALLOC_CAP_8BIT);
        if (!d->line_buffer || !d->buffer) err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        memset(d->buffer, 0xff, EPD_SSD1685_BUFFER_BYTES);
        u8g2_SetupDisplay(&d->u8g2, display_cb, u8x8_dummy_cb, u8x8_dummy_cb, u8x8_dummy_cb);
        u8g2_SetupBuffer(&d->u8g2, d->buffer, EPD_SSD1685_HEIGHT / 8U,
                         u8g2_ll_hvline_vertical_top_lsb, c->rotation);
        u8g2_InitDisplay(&d->u8g2);
        err = d->last_error;
    }
    if (err != ESP_OK) epd_ssd1685_deinit(d);
    return err;
}

esp_err_t epd_ssd1685_resume(epd_ssd1685_t *d)
{
    if (!d || !d->spi || !d->buffer) return ESP_ERR_INVALID_STATE;
    esp_err_t err = configure_pins(d);
    if (err == ESP_OK) err = controller_init(d);
    d->last_error = err;
    return err;
}

void epd_ssd1685_deinit(epd_ssd1685_t *d)
{
    if (!d) return;
    if (d->spi && d->powered) (void)sleep_panel(d);
    if (d->spi) (void)spi_bus_remove_device(d->spi);
    heap_caps_free(d->line_buffer); heap_caps_free(d->buffer); heap_caps_free(d->shadow);
    if (d->pins_configured) {
        (void)gpio_reset_pin(d->dc_pin); (void)gpio_reset_pin(d->reset_pin);
        (void)gpio_reset_pin(d->busy_pin);
        if (d->power_pin >= 0) (void)gpio_reset_pin(d->power_pin);
    }
    memset(d, 0, sizeof(*d));
}

u8g2_t *epd_ssd1685_get_u8g2(epd_ssd1685_t *d) { return d ? &d->u8g2 : NULL; }

const char *epd_ssd1685_controller_mode(const epd_ssd1685_t *d)
{
    if (!d) return NULL;
    if (d->refresh_mode == EPD_SSD1685_REFRESH_PARTIAL) return "refresh=partial";
    if (d->refresh_mode == EPD_SSD1685_REFRESH_FULL) return "refresh=full";
    return "refresh=auto";
}

const char *epd_ssd1685_controller_mode_values(const epd_ssd1685_t *d)
{
    (void)d;
    return "refresh=<auto,partial,full>";
}

esp_err_t epd_ssd1685_set_controller_mode(epd_ssd1685_t *d, const char *mode)
{
    if (!d || !mode) return ESP_ERR_INVALID_ARG;
    if (strcmp(mode, "refresh=auto") == 0) d->refresh_mode = EPD_SSD1685_REFRESH_AUTO;
    else if (strcmp(mode, "refresh=partial") == 0) d->refresh_mode = EPD_SSD1685_REFRESH_PARTIAL;
    else if (strcmp(mode, "refresh=full") == 0) d->refresh_mode = EPD_SSD1685_REFRESH_FULL;
    else return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}
