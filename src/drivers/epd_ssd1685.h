#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/spi_master.h"
#include "esp_err.h"
#include "u8g2.h"

/* Panel profiles include glass wiring and OTP waveform assumptions. */
#define EPD_SSD1685_PANEL_GDEY029T71H 1U
#define EPD_SSD1685_WIDTH 168U
#define EPD_SSD1685_HEIGHT 384U
#define EPD_SSD1685_ROW_BYTES (EPD_SSD1685_WIDTH / 8U)
#define EPD_SSD1685_BUFFER_BYTES (EPD_SSD1685_ROW_BYTES * EPD_SSD1685_HEIGHT)

typedef struct {
    const char *spi_bus;
    int cs_pin, dc_pin, reset_pin, busy_pin, power_pin;
    int spi_clock_hz;
    const u8g2_cb_t *rotation;
    unsigned panel;
} epd_ssd1685_config_t;

typedef enum {
    EPD_SSD1685_REFRESH_AUTO,
    EPD_SSD1685_REFRESH_PARTIAL,
    EPD_SSD1685_REFRESH_FULL,
} epd_ssd1685_refresh_mode_t;

typedef struct {
    spi_device_handle_t spi;
    u8g2_t u8g2;
    uint8_t *buffer, *shadow, *line_buffer;
    esp_err_t last_error;
    epd_ssd1685_refresh_mode_t refresh_mode;
    unsigned partial_count;
    int dc_pin, reset_pin, busy_pin, power_pin;
    bool controller_ready, shadow_valid, powered, analog_on, pins_configured;
} epd_ssd1685_t;

esp_err_t epd_ssd1685_init(epd_ssd1685_t *display, const epd_ssd1685_config_t *config);
esp_err_t epd_ssd1685_refresh(epd_ssd1685_t *display);
esp_err_t epd_ssd1685_resume(epd_ssd1685_t *display);
void epd_ssd1685_deinit(epd_ssd1685_t *display);
u8g2_t *epd_ssd1685_get_u8g2(epd_ssd1685_t *display);
const char *epd_ssd1685_controller_mode(const epd_ssd1685_t *display);
const char *epd_ssd1685_controller_mode_values(const epd_ssd1685_t *display);
esp_err_t epd_ssd1685_set_controller_mode(epd_ssd1685_t *display, const char *mode);
