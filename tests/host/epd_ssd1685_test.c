#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../src/drivers/epd_ssd1685.c"

typedef struct {
    uint8_t command;
    size_t length;
    uint8_t data[8064];
} capture_t;
static capture_t captures[80];
static size_t count;
static int dc_level, resets, powers, bus_locks;
static bool stuck_busy, held_cs;
static int fail_after = -1;
static esp_err_t bus_error;
static int64_t time_us;
static uint8_t frame[8064], shadow[8064], line[21];

esp_err_t gpio_config(const gpio_config_t *config) { assert(config); return ESP_OK; }
esp_err_t gpio_reset_pin(gpio_num_t pin) { (void)pin; return ESP_OK; }
esp_err_t spi_bus_remove_device(spi_device_handle_t device) { assert(device); return ESP_OK; }
esp_err_t solar_os_bus_spi_add_device(const char *name,
                                     const spi_device_interface_config_t *config,
                                     spi_device_handle_t *device)
{
    assert(strcmp(name, "spi0") == 0 && config->spics_io_num == 10);
    if (bus_error) return bus_error;
    *device = (void *)1; return ESP_OK;
}

void vTaskDelay(TickType_t ticks)
{
    assert(ticks > 0 && !bus_locks); /* never block SD during panel BUSY */
    time_us += ticks * SOLAR_OS_EPD_TEST_TICK_MS * 1000LL;
}
int64_t esp_timer_get_time(void) { return time_us; }
int gpio_get_level(gpio_num_t pin) { (void)pin; return stuck_busy ? 1 : 0; }
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level)
{
    if (pin == 9) dc_level = level;
    if (pin == 46 && !level) resets++;
    if (pin == 7 && level) powers++;
    return ESP_OK;
}
esp_err_t spi_device_acquire_bus(spi_device_handle_t device, unsigned wait)
{
    assert(device && wait == portMAX_DELAY && !bus_locks);
    bus_locks++;
    return ESP_OK;
}
void spi_device_release_bus(spi_device_handle_t device)
{
    assert(device && bus_locks == 1 && !held_cs);
    bus_locks--;
}
esp_err_t spi_device_polling_transmit(spi_device_handle_t device, spi_transaction_t *t)
{
    assert(device);
    if (fail_after == 0) { fail_after = -1; return ESP_FAIL; }
    if (fail_after > 0) fail_after--;
    if (!t->length) { held_cs = false; return ESP_OK; }
    const uint8_t *data = t->flags & SPI_TRANS_USE_TXDATA ? t->tx_data : t->tx_buffer;
    const size_t length = t->length / 8;
    if (!dc_level) {
        assert(length == 1 && count < 80 && !held_cs);
        captures[count++] = (capture_t){.command = data[0]};
    } else {
        assert(count);
        capture_t *c = &captures[count - 1];
        assert(c->length + length <= sizeof(c->data));
        memcpy(c->data + c->length, data, length);
        c->length += length;
    }
    held_cs = (t->flags & SPI_TRANS_CS_KEEP_ACTIVE) != 0;
    if (held_cs) assert(bus_locks == 1);
    return ESP_OK;
}

static epd_ssd1685_t make_display(void)
{
    memset(frame, 0xff, sizeof(frame)); /* SolarOS 1 = white. */
    memset(shadow, 0xff, sizeof(shadow));
    count = 0;
    return (epd_ssd1685_t){.spi = (void *)1, .buffer = frame, .shadow = shadow,
        .line_buffer = line, .dc_pin = 9, .reset_pin = 46,
        .busy_pin = 3, .power_pin = 7};
}
static capture_t *find(uint8_t cmd, unsigned occurrence)
{
    for (unsigned i = 0; i < count; ++i)
        if (captures[i].command == cmd && occurrence-- == 0) return &captures[i];
    assert(!"missing command"); return NULL;
}
static bool has(uint8_t cmd)
{
    for (unsigned i = 0; i < count; ++i) if (captures[i].command == cmd) return true;
    return false;
}
static void expect(uint8_t cmd, const uint8_t *data, size_t len)
{
    capture_t *c = find(cmd, 0);
    assert(c->length == len && memcmp(c->data, data, len) == 0);
}

static void test_full_and_partial(void)
{
    epd_ssd1685_t d = make_display();
    frame[0] &= (uint8_t)~1U;
    frame[167] &= (uint8_t)~1U;
    frame[8063] &= (uint8_t)~0x80U;
    assert(epd_ssd1685_refresh(&d) == ESP_OK);
    assert(d.shadow_valid && resets == 1 && powers == 1 && !d.analog_on);
    expect(0x01, (uint8_t[]){0x7f, 1, 0}, 3);
    expect(0x44, (uint8_t[]){1, 21}, 2); /* S8..S175, not S0..S167 */
    expect(0x45, (uint8_t[]){0, 0, 0x7f, 1}, 4);
    expect(0x4e, (uint8_t[]){1}, 1);
    expect(0x18, (uint8_t[]){0x80}, 1);
    expect(0x22, (uint8_t[]){0xf7}, 1);
    assert(find(0x21, 0)->data[1] == 0); /* 200-source address mode */
    capture_t *new = find(0x24, 0);
    assert(new->length == 8064 && new->data[0] == 0x7f && new->data[20] == 0xfe);
    assert(new->data[8063] == 0xfe);
    assert(memcmp(find(0x26, 1)->data, new->data, 8064) == 0);
    assert(memcmp(frame, shadow, 8064) == 0);
    count = 0;
    assert(epd_ssd1685_refresh(&d) == ESP_OK && count == 0);

    frame[0] = 0xff; /* erase the top-left ink */
    assert(epd_ssd1685_refresh(&d) == ESP_OK);
    expect(0x21, (uint8_t[]){0, 0}, 2);
    expect(0x22, (uint8_t[]){0xdc}, 1);
    assert(find(0x26, 0)->data[0] == 0x7f && find(0x24, 0)->data[0] == 0xff);
    assert(find(0x26, 1)->data[0] == 0xff && d.partial_count == 1 && d.analog_on);
    for (unsigned i = 2; i <= 6; ++i) {
        count = 0; frame[0] ^= 1;
        assert(epd_ssd1685_refresh(&d) == ESP_OK);
        expect(0x22, (uint8_t[]){i == 6 ? 0xf7 : 0xdc}, 1);
        assert(d.partial_count == (i == 6 ? 0 : i));
    }
    count = 0;
    assert(epd_ssd1685_set_controller_mode(&d, "refresh=full") == ESP_OK);
    assert(epd_ssd1685_refresh(&d) == ESP_OK && has(0x20));
    assert(epd_ssd1685_set_controller_mode(&d, "invalid") == ESP_ERR_INVALID_ARG);
    assert(epd_ssd1685_set_controller_mode(&d, "refresh=partial") == ESP_OK);
    count = 0; frame[0] ^= 1; d.partial_count = 9;
    assert(epd_ssd1685_refresh(&d) == ESP_OK);
    expect(0x22, (uint8_t[]){0xdc}, 1);
}

static void test_failures_and_sleep(void)
{
    epd_ssd1685_t d = make_display();
    d.controller_ready = true; d.shadow_valid = true; d.powered = true;
    frame[0] &= (uint8_t)~1U; stuck_busy = true;
    assert(epd_ssd1685_refresh(&d) == ESP_ERR_TIMEOUT);
    assert(!d.controller_ready && !d.shadow_valid && shadow[0] == 0xff);
    assert(d.last_error == ESP_ERR_TIMEOUT);
    stuck_busy = false; count = 0;
    assert(epd_ssd1685_refresh(&d) == ESP_OK);
    expect(0x22, (uint8_t[]){0xf7}, 1);
    count = 0; frame[0] ^= 1; fail_after = 30; /* mid-plane burst */
    assert(epd_ssd1685_refresh(&d) == ESP_FAIL);
    assert(!d.shadow_valid && !d.controller_ready && !bus_locks && !held_cs);
    count = 0;
    assert(epd_ssd1685_refresh(&d) == ESP_OK);
    expect(0x22, (uint8_t[]){0xf7}, 1);
    count = 0; frame[0] ^= 1;
    assert(epd_ssd1685_refresh(&d) == ESP_OK && d.analog_on);
    count = 0;
    assert(sleep_panel(&d) == ESP_OK);
    expect(0x22, (uint8_t[]){0x83}, 1);
    expect(0x10, (uint8_t[]){1}, 1);
    assert(!d.controller_ready && !d.shadow_valid && !d.powered);
    count = 0;
    assert(epd_ssd1685_refresh(&d) == ESP_OK && has(0x12));
    expect(0x22, (uint8_t[]){0xf7}, 1);
    d.shadow = NULL; count = 0; /* full refresh fallback without shadow memory */
    assert(epd_ssd1685_refresh(&d) == ESP_OK);
    expect(0x22, (uint8_t[]){0xf7}, 1);
}

static void test_init_rotation_and_cleanup(void)
{
    const u8g2_cb_t *rotations[] = {U8G2_R0, U8G2_R1, U8G2_R2, U8G2_R3};
    epd_ssd1685_config_t c = {.spi_bus = "spi0", .cs_pin = 10, .dc_pin = 9,
        .reset_pin = 46, .busy_pin = 3, .power_pin = 7, .spi_clock_hz = 10000000,
        .rotation = U8G2_R0, .panel = EPD_SSD1685_PANEL_GDEY029T71H};
    epd_ssd1685_t d = {0};
    for (unsigned r = 0; r < 4; ++r) {
        count = 0; c.rotation = rotations[r];
        assert(epd_ssd1685_init(&d, &c) == ESP_OK);
        assert(u8g2_GetDisplayWidth(&d.u8g2) == (r % 2 ? 384 : 168));
        assert(u8g2_GetDisplayHeight(&d.u8g2) == (r % 2 ? 168 : 384));
        u8g2_SetDrawColor(&d.u8g2, 0);
        u8g2_DrawPixel(&d.u8g2, 0, 0);
        u8g2_DrawPixel(&d.u8g2, u8g2_GetDisplayWidth(&d.u8g2) - 1,
                       u8g2_GetDisplayHeight(&d.u8g2) - 1);
        unsigned ink = 0;
        for (unsigned i = 0; i < 8064; ++i) ink += __builtin_popcount((unsigned)(uint8_t)~d.buffer[i]);
        assert(ink == 2);
        assert(display_cb(&d.u8g2.u8x8, U8X8_MSG_DISPLAY_SET_POWER_SAVE, 1, NULL));
        assert(!d.controller_ready && !d.shadow_valid && !d.powered);
        assert(epd_ssd1685_resume(&d) == ESP_OK && d.controller_ready && !d.shadow_valid);
        epd_ssd1685_deinit(&d);
        assert(!d.spi && !d.buffer && !d.shadow && !d.line_buffer);
    }
    c.panel = 0;
    assert(epd_ssd1685_init(&d, &c) == ESP_ERR_INVALID_ARG);
    c.panel = 1; c.dc_pin = c.cs_pin;
    assert(epd_ssd1685_init(&d, &c) == ESP_ERR_INVALID_ARG);
    c.dc_pin = 9; c.spi_clock_hz = 10000001;
    assert(epd_ssd1685_init(&d, &c) == ESP_ERR_INVALID_ARG);
    c.spi_clock_hz = 10000000; bus_error = ESP_FAIL;
    assert(epd_ssd1685_init(&d, &c) == ESP_FAIL);
    assert(!d.spi && !d.buffer && !d.powered);
    bus_error = ESP_OK; count = 0; stuck_busy = true;
    assert(epd_ssd1685_init(&d, &c) == ESP_ERR_TIMEOUT);
    assert(!d.spi && !d.buffer && !d.powered);
    stuck_busy = false;
}

int main(void)
{
    test_full_and_partial(); test_failures_and_sleep(); test_init_rotation_and_cleanup();
    puts("epd_ssd1685_test: PASS"); return 0;
}
