#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <freertos/semphr.h>

static SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    static StaticSemaphore_t mutexes[8];
    static unsigned count;
    assert(count < 8);
    return xSemaphoreCreateMutexStatic(&mutexes[count++]);
}

#include "../../src/services/solar_os_radio.c"

static const char *settings_dir;
static unsigned configure_calls;
static TickType_t ticks;
static solar_os_radio_status_t fake_status;
TickType_t xTaskGetTickCount(void) { return ticks; }
void vTaskDelay(TickType_t value) { ticks += value; }
size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t length = strlen(src);
    if (size) { size_t n = length < size - 1 ? length : size - 1; memcpy(dst, src, n); dst[n] = 0; }
    return length;
}
void *solar_os_memory_calloc(size_t n, size_t size, solar_os_memory_class_t cls, const char *tag)
{ (void)cls; (void)tag; return calloc(n, size); }
void solar_os_memory_free(void *ptr) { free(ptr); }
esp_err_t solar_os_storage_default_path(const char *relative, char *path, size_t size)
{ (void)relative; return snprintf(path, size, "%s", settings_dir) < (int)size ? ESP_OK : ESP_FAIL; }
esp_err_t solar_os_storage_mkdir(const char *path)
{ return mkdir(path, 0700) == 0 ? ESP_OK : ESP_FAIL; }
esp_err_t solar_os_storage_replace_file(const char *staged, const char *active, const char *backup)
{ (void)backup; return rename(staged, active) == 0 ? ESP_OK : ESP_FAIL; }
esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{ (void)name; (void)mode; (void)handle; return ESP_ERR_NVS_NOT_FOUND; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *data, size_t *size)
{ (void)h; (void)key; (void)data; (void)size; return ESP_ERR_NVS_NOT_FOUND; }
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{ (void)h; (void)key; return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }
static esp_err_t configure(void *ctx, const solar_os_radio_config_t *config)
{ (void)ctx; configure_calls++; fake_status.config = *config; return ESP_OK; }
static esp_err_t status(void *ctx, solar_os_radio_status_t *out)
{ (void)ctx; *out = fake_status; return ESP_OK; }

int main(int argc, char **argv)
{
    assert(argc == 2); settings_dir = argv[1];
    solar_os_radio_config_t config = {.frequency_hz = 868000000,
        .modulation = SOLAR_OS_RADIO_MODULATION_LORA};
    fake_status.config = config;
    const solar_os_radio_ops_t ops = {.configure = configure, .get_status = status};
    solar_os_radio_registration_t registration = {.name = "radio0", .max_packet_len = 255,
        .modulations = SOLAR_OS_RADIO_MODULATION_LORA, .default_config = config,
        .features = SOLAR_OS_RADIO_FEATURE_RX_GAIN,
        .rx_gain_modes = SOLAR_OS_RADIO_RX_GAIN_BOOST_MODES, .ops = &ops};
    assert(solar_os_radio_register(&registration) == ESP_OK);
    solar_os_radio_info_t info;
    assert(solar_os_radio_get_info("radio0", &info) == ESP_OK);
    assert(info.rx_gain_modes == SOLAR_OS_RADIO_RX_GAIN_BOOST_MODES);
    config.rx_gain = SOLAR_OS_RADIO_RX_GAIN_BOOSTED;
    assert(solar_os_radio_configure("radio0", &config) == ESP_OK && configure_calls == 1);
    config.rx_gain = SOLAR_OS_RADIO_RX_GAIN_G1;
    assert(solar_os_radio_configure("radio0", &config) == ESP_ERR_NOT_SUPPORTED && configure_calls == 1);
    config.rx_gain = 255;
    assert(solar_os_radio_configure("radio0", &config) == ESP_ERR_INVALID_ARG && configure_calls == 1);

    solar_os_radio_handle_t handle = SOLAR_OS_RADIO_HANDLE_INIT;
    assert(solar_os_radio_claim("radio0", "test", &handle) == ESP_OK);
    config.rx_gain = SOLAR_OS_RADIO_RX_GAIN_G1;
    assert(solar_os_radio_handle_configure(&handle, &config) == ESP_ERR_NOT_SUPPORTED);
    config.rx_gain = SOLAR_OS_RADIO_RX_GAIN_BOOSTED;
    assert(solar_os_radio_handle_configure(&handle, &config) == ESP_OK);
    assert(solar_os_radio_configure("radio0", &config) == ESP_ERR_INVALID_STATE);
    assert(solar_os_radio_release(&handle) == ESP_OK);

    assert(solar_os_radio_profile_save("radio0", "boost") == ESP_OK);
    solar_os_radio_profile_t profile;
    assert(solar_os_radio_profile_get("boost", &profile) == ESP_OK);
    assert(profile.config.rx_gain == SOLAR_OS_RADIO_RX_GAIN_BOOSTED);
    config.rx_gain = SOLAR_OS_RADIO_RX_GAIN_POWER_SAVING;
    assert(solar_os_radio_configure("radio0", &config) == ESP_OK);
    assert(solar_os_radio_profile_apply("radio0", "boost") == ESP_OK);
    assert(fake_status.config.rx_gain == SOLAR_OS_RADIO_RX_GAIN_BOOSTED);

    radio_profile_store_t legacy;
    radio_profile_store_empty(&legacy);
    legacy.version = 1;
    legacy.profiles[0].active = true;
    strlcpy(legacy.profiles[0].name, "legacy", sizeof(legacy.profiles[0].name));
    legacy.profiles[0].config = config;
    legacy.profiles[0].config.rx_gain = 0xa5; /* Former padding is not a gain setting. */
    assert(radio_profile_store_valid(&legacy, sizeof(legacy)));
    assert(radio_profile_store_save(&legacy) == ESP_OK);
    assert(solar_os_radio_profile_get("legacy", &profile) == ESP_OK);
    assert(profile.config.rx_gain == SOLAR_OS_RADIO_RX_GAIN_DEFAULT);
    legacy.version = RADIO_PROFILE_VERSION;
    assert(!radio_profile_store_valid(&legacy, sizeof(legacy)));
    legacy.profiles[0].config.rx_gain = SOLAR_OS_RADIO_RX_GAIN_G1;
    assert(radio_profile_store_save(&legacy) == ESP_OK);
    assert(solar_os_radio_profile_apply("radio0", "legacy") == ESP_ERR_NOT_SUPPORTED);
    assert(fake_status.config.rx_gain == SOLAR_OS_RADIO_RX_GAIN_BOOSTED);

    /* Older or future drivers without gain support still accept zero-initialized configs. */
    registration.name = "no-gain";
    registration.rx_gain_modes = 0;
    registration.features = 0;
    assert(solar_os_radio_register(&registration) == ESP_OK);
    config.rx_gain = SOLAR_OS_RADIO_RX_GAIN_BOOSTED;
    assert(solar_os_radio_configure("no-gain", &config) == ESP_ERR_NOT_SUPPORTED);
    config.rx_gain = SOLAR_OS_RADIO_RX_GAIN_DEFAULT;
    assert(solar_os_radio_configure("no-gain", &config) == ESP_OK);

    uint8_t gain = 0;
    for (unsigned i = 0; i < SOLAR_OS_RADIO_RX_GAIN_COUNT; i++) {
        assert(solar_os_radio_rx_gain_parse(solar_os_radio_rx_gain_name(i), &gain) && gain == i);
    }
    assert(!solar_os_radio_rx_gain_parse("maximum", &gain));
    char modes[80];
    solar_os_radio_rx_gains_format(info.rx_gain_modes, modes, sizeof(modes));
    assert(strcmp(modes, "power-saving boosted") == 0);
    puts("radio RX gain capabilities, ownership, profiles and v1 migration: ok");
    return 0;
}
