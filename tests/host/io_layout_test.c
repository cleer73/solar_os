#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../src/apps/solar_os_io.c"

#define TEST_ROWS 20U
#define TEST_COLS 160U
static uint32_t screen[TEST_ROWS][TEST_COLS];
static size_t screen_cols, cursor_col, cursor_row;
static bool cursor_visible;
static io_state_t state;
static solar_os_bus_info_t bus;

size_t strlcpy(char *dst, const char *src, size_t size)
{
    const size_t len = strlen(src);
    if (size > 0U) {
        const size_t copy = len < size - 1U ? len : size - 1U;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return len;
}

size_t solar_os_tui_cols(const solar_os_tui_t *tui)
{
    (void)tui;
    return screen_cols;
}

size_t solar_os_tui_rows(const solar_os_tui_t *tui)
{
    (void)tui;
    return TEST_ROWS;
}

void solar_os_tui_clear(solar_os_tui_t *tui)
{
    (void)tui;
    for (size_t row = 0; row < TEST_ROWS; row++)
        for (size_t col = 0; col < TEST_COLS; col++) screen[row][col] = ' ';
}

void solar_os_tui_refresh(solar_os_tui_t *tui) { (void)tui; }

esp_err_t solar_os_tui_fill(solar_os_tui_t *tui, size_t row, size_t col,
                            size_t height, size_t width, uint32_t ch, uint8_t attr)
{
    (void)tui;
    (void)attr;
    assert(row + height <= TEST_ROWS && col + width <= screen_cols);
    for (size_t y = row; y < row + height; y++)
        for (size_t x = col; x < col + width; x++) screen[y][x] = ch;
    return ESP_OK;
}

esp_err_t solar_os_tui_putch(solar_os_tui_t *tui, size_t row, size_t col,
                             uint32_t ch, uint8_t attr)
{
    (void)tui;
    (void)attr;
    assert(row < TEST_ROWS && col < screen_cols);
    screen[row][col] = ch;
    return ESP_OK;
}

esp_err_t solar_os_tui_set_cursor_visible(solar_os_tui_t *tui, bool visible)
{
    (void)tui;
    cursor_visible = visible;
    return ESP_OK;
}

esp_err_t solar_os_tui_move(solar_os_tui_t *tui, size_t row, size_t col)
{
    (void)tui;
    assert(row < TEST_ROWS && col < screen_cols);
    cursor_row = row;
    cursor_col = col;
    return ESP_OK;
}

size_t solar_os_bus_count(void) { return 1U; }
bool solar_os_bus_get(size_t index, solar_os_bus_info_t *info)
{
    if (index != 0U) return false;
    *info = bus;
    return true;
}
const char *solar_os_bus_protocol_name(solar_os_bus_protocol_t protocol)
{
    return protocol == SOLAR_OS_BUS_PROTOCOL_UART ? "uart" : "spi";
}

size_t solar_os_pin_count(void) { return 1U; }
bool solar_os_pin_get_info(size_t index, solar_os_pin_info_t *info)
{
    if (index != 0U) return false;
    *info = (solar_os_pin_info_t){.pin = 43, .expansion = true,
        .policy = SOLAR_OS_PIN_POLICY_RELEASABLE, .role = "long pin role"};
    return true;
}
bool solar_os_pin_get_info_by_pin(int pin, solar_os_pin_info_t *info)
{
    return pin == 43 && solar_os_pin_get_info(0U, info);
}
const char *solar_os_pin_policy_name(solar_os_pin_policy_t policy)
{
    (void)policy;
    return "releasable";
}
bool solar_os_gpio_get_pin_info_by_pin(int pin, solar_os_gpio_pin_info_t *info)
{
    (void)pin;
    (void)info;
    return false;
}
const char *solar_os_gpio_mode_name(solar_os_gpio_mode_t mode)
{
    (void)mode;
    return "input";
}
bool solar_os_resource_find_claim(solar_os_resource_kind_t kind, int primary,
                                  int secondary, solar_os_resource_claim_t *claim)
{
    (void)kind;
    (void)primary;
    (void)secondary;
    (void)claim;
    return false;
}
size_t solar_os_resource_claim_count(void) { return 1U; }
bool solar_os_resource_get_claim(size_t index, solar_os_resource_claim_t *claim)
{
    if (index != 0U) return false;
    *claim = (solar_os_resource_claim_t){.kind = SOLAR_OS_RESOURCE_GPIO_PIN,
        .primary = 43, .secondary = -1};
    strlcpy(claim->owner, "bus:receiver_long_name", sizeof(claim->owner));
    strlcpy(claim->label, "external receiver", sizeof(claim->label));
    return true;
}
const char *solar_os_resource_kind_name(solar_os_resource_kind_t kind)
{
    (void)kind;
    return "gpio";
}
bool solar_os_connector_layout_get_info(solar_os_connector_layout_info_t *info)
{
    *info = (solar_os_connector_layout_info_t){.title = "Connectors", .view = "Top",
        .rows = 2U, .columns = 2U};
    return true;
}
size_t solar_os_connector_pin_count(void) { return 4U; }
bool solar_os_connector_pin_find(size_t row, size_t col,
                                 solar_os_connector_pin_info_t *info, size_t *index)
{
    if (row >= 2U || col >= 2U) return false;
    *info = (solar_os_connector_pin_info_t){.connector = "GROVE", .position = row * 2U + col + 1U,
        .row = row, .column = col, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO,
        .pin = 43, .label = "GPIO43"};
    if (index != NULL) *index = row * 2U + col;
    return true;
}
bool solar_os_connector_pin_get_info(size_t index, solar_os_connector_pin_info_t *info)
{
    return solar_os_connector_pin_find(index / 2U, index % 2U, info, NULL);
}
const char *solar_os_connector_pin_kind_name(solar_os_connector_pin_kind_t kind)
{
    (void)kind;
    return "GPIO";
}

static const char *row_text(size_t row)
{
    static char text[TEST_COLS + 1U];
    for (size_t col = 0U; col < screen_cols; col++) {
        text[col] = screen[row][col] < 128U ? (char)screen[row][col] : '?';
    }
    text[screen_cols] = '\0';
    return text;
}

static void reset(size_t cols)
{
    memset(&state, 0, sizeof(state));
    io_state = &state;
    screen_cols = cols;
    solar_os_tui_clear(&state.tui);
}

static void test_tables(void)
{
    /* Exercise the real renderers and shared UTF-8 cell writer at every width.
     * Strict screen mocks reject any write beyond the available rectangle. */
    for (size_t cols = 1U; cols <= TEST_COLS; cols++) {
        reset(cols);
        io_render_title();
        io_render_pins();
        io_render_buses();
        io_render_claims();
        io.selected[IO_VIEW_LAYOUT] = 3U;
        io_render_layout();
        io.form.protocol = SOLAR_OS_BUS_PROTOCOL_SPI;
        io.form.max_transfer = 65536U;
        strlcpy(io.form.name, "long_runtime_bus_name", sizeof(io.form.name));
        io_render_form();
        io.form.editing_name = true;
        io_render_form();
        io_render_actions();
        io_render_protocols();
        io_render_confirm();
        io.mode = IO_MODE_I2C_SPEED;
        io_render();
    }
    reset(26U);
    io.view = IO_VIEW_CLAIMS;
    io_render_title();
    assert(strstr(row_text(0), "[Claims]") != NULL);
    io_render_buses();
    const char *heading = row_text(1U);
    const char *state_heading = strstr(heading, "STATE");
    assert(state_heading != NULL);
    const size_t state_col = (size_t)(state_heading - heading);
    assert(strstr(heading, "ROUTING") == NULL);
    assert(strncmp(row_text(2U) + state_col, "detached", 8U) == 0);
    io_render_claims();
    heading = row_text(1U);
    const char *owner_heading = strstr(heading, "OWNER");
    assert(owner_heading != NULL);
    const size_t owner_col = (size_t)(owner_heading - heading);
    assert(strncmp(row_text(2U) + owner_col, "bus:", 4U) == 0);
    screen_cols = 20U;
    io_render_buses();
    assert(strstr(row_text(2U), "detached") != NULL);
    io_render_claims();
    assert(strstr(row_text(2U), "bus:") != NULL);
    screen_cols = 26U;
    io.selected[IO_VIEW_LAYOUT] = 3U;
    io_render_layout();
    assert(strstr(row_text(3U), "GROVE.4 GPIO43") != NULL);
    reset(80U);
    io_render_buses();
    assert(strstr(row_text(1U), "ROUTING") != NULL);
    assert(strstr(row_text(1U), "LEASES") != NULL);
    assert(strstr(row_text(2U), "uart1 TX=43 RX=44") != NULL);
}

static void test_form(void)
{
    reset(26U);
    io.form.protocol = SOLAR_OS_BUS_PROTOCOL_SPI;
    io.form.max_transfer = 65536U;
    strlcpy(io.form.name, "runtime_bus_123", sizeof(io.form.name));
    io_render_form();
    assert(strstr(row_text(10U), "65536 bytes") != NULL);
    assert(strchr(row_text(10U), ']') != NULL);
    io.form.editing_name = true;
    io_render_form();
    assert(strstr(row_text(1U), "time_bus_123") != NULL);
    assert(cursor_visible && cursor_row == 1U && cursor_col == 24U);
    /* Re-render the same active form after a terminal resize. */
    screen_cols = 20U;
    io_render_form();
    assert(strstr(row_text(1U), "_bus_123") != NULL);
    assert(cursor_col == 18U);
    io.form.name[0] = '\0';
    io_render_form();
    assert(cursor_col == 10U && strchr(row_text(1U), '[') != NULL);
    assert(strstr(row_text(TEST_ROWS - 1U), "Esc") != NULL);
    io_render_actions();
    assert(strstr(row_text(TEST_ROWS - 1U), "Esc") != NULL);
    io_render_protocols();
    assert(strstr(row_text(TEST_ROWS - 1U), "Esc") != NULL);
    io_render_browse();
    assert(strstr(row_text(TEST_ROWS - 1U), "Q:exit") != NULL);
}

int main(void)
{
    bus.protocol = SOLAR_OS_BUS_PROTOCOL_UART;
    bus.config.uart.port = 1;
    bus.config.uart.tx_pin = 43;
    bus.config.uart.rx_pin = 44;
    bus.config.uart.baud_rate = 115200U;
    strlcpy(bus.name, "receiver_long_name", sizeof(bus.name));
    test_tables();
    test_form();
    puts("I/O layout tests passed");
    return 0;
}
