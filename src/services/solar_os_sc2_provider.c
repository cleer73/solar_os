/*
 * "sc2" Messaging provider class: per-relay instances, room list and
 * conversation security flags. See solar_os_sc2_provider.h.
 */

#include "solar_os_sc2_provider.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_memory.h"
#include "solar_os_sc2.h"

#define SC2P_CLASS "sc2"
#define SC2P_DETAIL_DISABLED "disabled"
#define SC2P_DETAIL_NO_TRANSPORT "enabled; SC2 relay transport is not in this build yet"

typedef struct sc2p_relay {
    struct sc2p_relay *next;
    solar_os_messaging_provider_id_t id;
    solar_os_sc2_relay_config_t config; /* protected by sc2p.lock */
} sc2p_relay_t;

static struct {
    bool initialized;
    SemaphoreHandle_t lock;      /* relay list and node->config */
    SemaphoreHandle_t edit_lock; /* serialises read-modify-save of profiles */
    sc2p_relay_t *relays;
} sc2p;

static esp_err_t sc2p_join_open(void *context, const char *target, solar_os_conversation_id_t *conversation);
static esp_err_t sc2p_leave(void *context, const char *target);
static esp_err_t sc2p_direct_open(void *context, const char *target, solar_os_conversation_id_t *conversation);
static esp_err_t sc2p_connect(void *context, bool enabled);
static esp_err_t sc2p_create(const char *name, int argc, const char *const *argv,
                             solar_os_messaging_provider_id_t *id);
static esp_err_t sc2p_configure(void *context, int argc, const char *const *argv);
static esp_err_t sc2p_normalize(void *context, const char *target, char *out, size_t size);
static esp_err_t sc2p_remove_prepare(void *context);
static void sc2p_remove_finish(void *context, bool removed);

static const solar_os_messaging_provider_class_t sc2p_class = {
    .name = SC2P_CLASS,
    .capabilities = SOLAR_OS_ENDPOINT_CAP_DIRECT | SOLAR_OS_ENDPOINT_CAP_ROOM,
    .connection_oriented = true,
    .join_open = sc2p_join_open,
    .leave = sc2p_leave,
    .direct_open = sc2p_direct_open,
    .connect = sc2p_connect,
    .create = sc2p_create,
    .configure = sc2p_configure,
    .normalize_target = sc2p_normalize,
    .remove_prepare = sc2p_remove_prepare,
    .remove_finish = sc2p_remove_finish,
};

static void sc2p_wipe(void *memory, size_t size)
{
    volatile unsigned char *p = memory;
    while (size--) {
        *p++ = 0;
    }
}

/* Commands can run on small task stacks; keep config copies on the heap. */
static solar_os_sc2_relay_config_t *sc2p_config_alloc(void)
{
    return solar_os_memory_calloc(1, sizeof(solar_os_sc2_relay_config_t), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                  "sc2.relay");
}

static void sc2p_config_free(solar_os_sc2_relay_config_t *config)
{
    if (config) {
        sc2p_wipe(config, sizeof(*config));
        solar_os_memory_free(config);
    }
}

static bool sc2p_copy(char *dst, size_t cap, const char *src, size_t len)
{
    if (len >= cap) {
        return false;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
    return true;
}

/* ---------------- validation ---------------- */

static bool sc2p_host_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
}

static bool sc2p_b64_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '+' || c == '/';
}

/* "sha256/" followed by the 44-character standard base64 of 32 bytes. */
static bool sc2p_pin_valid(const char *pin)
{
    static const char prefix[] = "sha256/";
    const size_t plen = sizeof(prefix) - 1u;
    if (strncmp(pin, prefix, plen) != 0 || strlen(pin) != plen + 44u) {
        return false;
    }
    for (size_t i = 0; i < 43u; i++) {
        if (!sc2p_b64_char(pin[plen + i])) {
            return false;
        }
    }
    return pin[plen + 43u] == '=';
}

static bool sc2p_admit_valid(const char *admit, size_t cap)
{
    size_t len = strnlen(admit, cap);
    if (len >= cap) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)admit[i];
        if (c <= ' ' || c >= 127) {
            return false;
        }
    }
    return true;
}

esp_err_t solar_os_sc2_relay_parse_url(const char *text, solar_os_sc2_relay_config_t *config)
{
    if (text == NULL || config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    bool tls;
    const char *p;
    if (strncmp(text, "chats://", 8) == 0) {
        tls = true;
        p = text + 8;
    } else if (strncmp(text, "chat://", 7) == 0) {
        tls = false;
        p = text + 7;
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    const char *host = p;
    while (sc2p_host_char(*p)) {
        p++;
    }
    size_t host_len = (size_t)(p - host);
    if (host_len == 0 || host[0] == '.' || host[0] == '-' || *p != ':') {
        return ESP_ERR_INVALID_ARG;
    }
    p++;
    unsigned long port = 0;
    const char *digits = p;
    while (*p >= '0' && *p <= '9') {
        port = port * 10u + (unsigned long)(*p - '0');
        if (port > 65535u) {
            return ESP_ERR_INVALID_ARG;
        }
        p++;
    }
    if (p == digits || port == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *url_end = p;
    char pin[SOLAR_OS_SC2_RELAY_PIN_MAX] = "";
    if (*p == '#') {
        static const char frag[] = "#pin=";
        if (!tls || strncmp(p, frag, sizeof(frag) - 1u) != 0 ||
            !sc2p_copy(pin, sizeof(pin), p + sizeof(frag) - 1u, strlen(p + sizeof(frag) - 1u)) ||
            !sc2p_pin_valid(pin)) {
            return ESP_ERR_INVALID_ARG;
        }
    } else if (*p != '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!sc2p_copy(config->url, sizeof(config->url), text, (size_t)(url_end - text))) {
        return ESP_ERR_INVALID_SIZE;
    }
    config->tls = tls;
    config->port = (uint16_t)port;
    memcpy(config->pin, pin, sizeof(config->pin));
    return ESP_OK;
}

solar_os_sc2_target_kind_t solar_os_sc2_target_normalize(const char *target, bool want_room, char *out,
                                                         size_t size)
{
    if (target == NULL || out == NULL || size == 0) {
        return SOLAR_OS_SC2_TARGET_INVALID;
    }
    const size_t prefix_len = sizeof(SOLAR_OS_SC2_PUBLIC_ROOM_PREFIX) - 1u;
    size_t len = strlen(target);
    if (len > prefix_len && strncasecmp(target, SOLAR_OS_SC2_PUBLIC_ROOM_PREFIX, prefix_len) == 0) {
        size_t name_len = len - prefix_len;
        if (!want_room || name_len > SOLAR_OS_SC2_PUBLIC_ROOM_NAME_MAX || len >= size) {
            return SOLAR_OS_SC2_TARGET_INVALID;
        }
        memcpy(out, SOLAR_OS_SC2_PUBLIC_ROOM_PREFIX, prefix_len);
        for (size_t i = 0; i < name_len; i++) {
            char c = target[prefix_len + i];
            if (c >= 'A' && c <= 'Z') {
                c = (char)(c - 'A' + 'a');
            }
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) {
                return SOLAR_OS_SC2_TARGET_INVALID;
            }
            out[prefix_len + i] = c;
        }
        out[len] = '\0';
        return SOLAR_OS_SC2_TARGET_PUBLIC_ROOM;
    }
    if (len != SC2_DEVICE_ID_HEX_LEN || len >= size) {
        return SOLAR_OS_SC2_TARGET_INVALID;
    }
    for (size_t i = 0; i < len; i++) {
        char c = target[i];
        if (c >= 'A' && c <= 'F') {
            c = (char)(c - 'A' + 'a');
        }
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return SOLAR_OS_SC2_TARGET_INVALID;
        }
        out[i] = c;
    }
    out[len] = '\0';
    return want_room ? SOLAR_OS_SC2_TARGET_PRIVATE_ROOM : SOLAR_OS_SC2_TARGET_DEVICE;
}

bool solar_os_sc2_relay_config_valid(const solar_os_sc2_relay_config_t *config)
{
    if (config == NULL || config->version != SOLAR_OS_SC2_RELAY_CONFIG_VERSION ||
        strnlen(config->url, sizeof(config->url)) >= sizeof(config->url) ||
        strnlen(config->pin, sizeof(config->pin)) >= sizeof(config->pin) ||
        !sc2p_admit_valid(config->admit, sizeof(config->admit))) {
        return false;
    }
    solar_os_sc2_relay_config_t *parsed = sc2p_config_alloc();
    if (parsed == NULL) {
        return false;
    }
    bool ok = solar_os_sc2_relay_parse_url(config->url, parsed) == ESP_OK && parsed->tls == config->tls &&
              parsed->port == config->port && parsed->pin[0] == '\0' &&
              (config->pin[0] == '\0' || (config->tls && sc2p_pin_valid(config->pin)));
    sc2p_config_free(parsed);
    for (size_t i = 0; ok && i < SOLAR_OS_SC2_RELAY_ROOM_CAPACITY; i++) {
        const char *room = config->rooms[i];
        if (room[0] == '\0') {
            continue;
        }
        char key[SOLAR_OS_MESSAGING_PROVIDER_KEY_MAX];
        if (strnlen(room, sizeof(config->rooms[i])) >= sizeof(config->rooms[i]) ||
            solar_os_sc2_target_normalize(room, true, key, sizeof(key)) == SOLAR_OS_SC2_TARGET_INVALID ||
            strcmp(key, room) != 0) {
            ok = false;
        }
        for (size_t j = 0; ok && j < i; j++) {
            if (strcmp(config->rooms[j], room) == 0) {
                ok = false;
            }
        }
    }
    return ok;
}

uint32_t solar_os_sc2_conversation_security(const solar_os_sc2_relay_config_t *config,
                                            solar_os_sc2_target_kind_t kind, bool peer_key_known)
{
    uint32_t flags = (config != NULL && config->tls) ? SOLAR_OS_SECURITY_TRANSPORT_SECURED : 0u;
    switch (kind) {
    case SOLAR_OS_SC2_TARGET_DEVICE:
        flags |= SOLAR_OS_SECURITY_ENCRYPTED;
        if (peer_key_known) {
            flags |= SOLAR_OS_SECURITY_PEER_KEY_KNOWN;
        }
        break;
    case SOLAR_OS_SC2_TARGET_PRIVATE_ROOM:
        flags |= SOLAR_OS_SECURITY_ENCRYPTED | SOLAR_OS_SECURITY_SHARED_KEY;
        break;
    case SOLAR_OS_SC2_TARGET_PUBLIC_ROOM:
        flags |= SOLAR_OS_SECURITY_PUBLIC_SIGNED;
        break;
    default:
        flags = 0;
        break;
    }
    return flags;
}

/* ---------------- instances ---------------- */

static sc2p_relay_t *sc2p_find(solar_os_messaging_provider_id_t id)
{
    xSemaphoreTake(sc2p.lock, portMAX_DELAY);
    sc2p_relay_t *node = sc2p.relays;
    while (node != NULL && node->id != id) {
        node = node->next;
    }
    xSemaphoreGive(sc2p.lock);
    return node;
}

static void sc2p_snapshot(sc2p_relay_t *node, solar_os_sc2_relay_config_t *config)
{
    xSemaphoreTake(sc2p.lock, portMAX_DELAY);
    *config = node->config;
    xSemaphoreGive(sc2p.lock);
}

static solar_os_sc2_transport_fn sc2p_transport;

void solar_os_sc2_provider_set_transport(solar_os_sc2_transport_fn fn)
{
    sc2p_transport = fn;
}

static void sc2p_status(solar_os_messaging_provider_id_t id, const solar_os_sc2_relay_config_t *config)
{
    if (sc2p_transport != NULL && config->enabled) {
        sc2p_transport(id, true); /* the sc2-sync worker reports the status */
        return;
    }
    (void)solar_os_messaging_provider_set_status(id, false, false, ESP_OK,
                                                 config->enabled ? SC2P_DETAIL_NO_TRANSPORT : SC2P_DETAIL_DISABLED);
    if (sc2p_transport != NULL) {
        sc2p_transport(id, false);
    }
}

static esp_err_t sc2p_upsert_room(solar_os_messaging_provider_id_t id, const solar_os_sc2_relay_config_t *config,
                                  const char *room, solar_os_conversation_id_t *conversation)
{
    char key[SOLAR_OS_MESSAGING_PROVIDER_KEY_MAX];
    solar_os_sc2_target_kind_t kind = solar_os_sc2_target_normalize(room, true, key, sizeof(key));
    if (kind == SOLAR_OS_SC2_TARGET_INVALID) {
        return ESP_ERR_INVALID_ARG;
    }
    const solar_os_messaging_conversation_upsert_t request = {
        .provider = id,
        .provider_key = key,
        .kind = SOLAR_OS_CONVERSATION_ROOM,
        .title = key,
        .security_flags = solar_os_sc2_conversation_security(config, kind, false),
    };
    return solar_os_messaging_conversation_upsert(&request, conversation);
}

static void sc2p_upsert_rooms(solar_os_messaging_provider_id_t id, const solar_os_sc2_relay_config_t *config)
{
    for (size_t i = 0; i < SOLAR_OS_SC2_RELAY_ROOM_CAPACITY; i++) {
        if (config->rooms[i][0] == '\0') {
            continue;
        }
        if (sc2p_upsert_room(id, config, config->rooms[i], NULL) == ESP_OK) {
            /* Not connected: configured rooms wait for the relay transport. */
            (void)solar_os_messaging_join_update(id, config->rooms[i], SOLAR_OS_MESSAGING_JOIN_LEFT, NULL);
        }
    }
}

static esp_err_t sc2p_attach(solar_os_messaging_provider_id_t id, const solar_os_sc2_relay_config_t *config)
{
    if (sc2p_find(id) != NULL) {
        return ESP_OK;
    }
    if (!solar_os_sc2_relay_config_valid(config)) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_messaging_provider_status_t status;
    esp_err_t error = solar_os_messaging_provider_get_status(id, &status);
    if (error != ESP_OK) {
        return error;
    }
    sc2p_relay_t *node =
        solar_os_memory_calloc(1, sizeof(*node), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "sc2.instance");
    if (node == NULL) {
        return ESP_ERR_NO_MEM;
    }
    node->id = id;
    node->config = *config;
    error = solar_os_messaging_instance_register(id, SC2P_CLASS, status.name, node);
    if (error != ESP_OK) {
        sc2p_wipe(node, sizeof(*node));
        solar_os_memory_free(node);
        return error;
    }
    xSemaphoreTake(sc2p.lock, portMAX_DELAY);
    node->next = sc2p.relays;
    sc2p.relays = node;
    xSemaphoreGive(sc2p.lock);
    sc2p_upsert_rooms(id, config);
    sc2p_status(id, config);
    return ESP_OK;
}

/* Caller holds edit_lock. */
static esp_err_t sc2p_save(sc2p_relay_t *node, const solar_os_sc2_relay_config_t *config)
{
    if (!solar_os_sc2_relay_config_valid(config)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = solar_os_messaging_profile_save(node->id, config, sizeof(*config));
    if (error != ESP_OK) {
        return error;
    }
    xSemaphoreTake(sc2p.lock, portMAX_DELAY);
    node->config = *config;
    xSemaphoreGive(sc2p.lock);
    return ESP_OK;
}

esp_err_t solar_os_sc2_provider_init(void)
{
    if (sc2p.initialized) {
        return ESP_OK;
    }
    esp_err_t error = solar_os_messaging_init();
    if (error != ESP_OK) {
        return error;
    }
    sc2p.lock = xSemaphoreCreateMutex();
    sc2p.edit_lock = xSemaphoreCreateMutex();
    solar_os_sc2_relay_config_t *config = sc2p_config_alloc();
    if (sc2p.lock == NULL || sc2p.edit_lock == NULL || config == NULL) {
        if (sc2p.lock != NULL) {
            vSemaphoreDelete(sc2p.lock);
        }
        if (sc2p.edit_lock != NULL) {
            vSemaphoreDelete(sc2p.edit_lock);
        }
        sc2p_config_free(config);
        memset(&sc2p, 0, sizeof(sc2p));
        return ESP_ERR_NO_MEM;
    }
    error = solar_os_messaging_class_register(&sc2p_class);
    if (error != ESP_OK) {
        vSemaphoreDelete(sc2p.lock);
        vSemaphoreDelete(sc2p.edit_lock);
        sc2p_config_free(config);
        memset(&sc2p, 0, sizeof(sc2p));
        return error;
    }
    sc2p.initialized = true;
    solar_os_messaging_provider_id_t cursor = 0;
    solar_os_messaging_provider_status_t status;
    while (solar_os_messaging_provider_next(&cursor, &status)) {
        if (strcmp(status.class_name, SC2P_CLASS) != 0) {
            continue;
        }
        error = solar_os_messaging_profile_load(cursor, config, sizeof(*config));
        if (error == ESP_OK) {
            error = sc2p_attach(cursor, config);
        }
        if (error != ESP_OK) {
            (void)solar_os_messaging_provider_set_status(cursor, false, false, error,
                                                         "SC2 relay profile unavailable or invalid");
        }
        sc2p_wipe(config, sizeof(*config));
    }
    sc2p_config_free(config);
    return ESP_OK;
}

esp_err_t solar_os_sc2_relay_get_config(solar_os_messaging_provider_id_t id, solar_os_sc2_relay_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = solar_os_sc2_provider_init();
    if (error != ESP_OK) {
        return error;
    }
    sc2p_relay_t *node = sc2p_find(id);
    if (node != NULL) {
        sc2p_snapshot(node, config);
        return ESP_OK;
    }
    if (!solar_os_messaging_provider_is_class(id, SC2P_CLASS)) {
        return ESP_ERR_NOT_FOUND;
    }
    return solar_os_messaging_profile_load(id, config, sizeof(*config));
}

/* ---------------- class callbacks ---------------- */

static esp_err_t sc2p_create(const char *name, int argc, const char *const *argv,
                             solar_os_messaging_provider_id_t *id)
{
    if (name == NULL || id == NULL || argc < 1 || argc > 2 || argv == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = solar_os_sc2_provider_init();
    if (error != ESP_OK) {
        return error;
    }
    solar_os_sc2_relay_config_t *config = sc2p_config_alloc();
    if (config == NULL) {
        return ESP_ERR_NO_MEM;
    }
    config->version = SOLAR_OS_SC2_RELAY_CONFIG_VERSION;
    error = solar_os_sc2_relay_parse_url(argv[0], config);
    if (error == ESP_OK && argc == 2 &&
        !sc2p_copy(config->admit, sizeof(config->admit), argv[1], strlen(argv[1]))) {
        error = ESP_ERR_INVALID_ARG;
    }
    if (error == ESP_OK && !solar_os_sc2_relay_config_valid(config)) {
        error = ESP_ERR_INVALID_ARG;
    }
    if (error == ESP_OK) {
        xSemaphoreTake(sc2p.edit_lock, portMAX_DELAY);
        error = solar_os_messaging_instance_create(SC2P_CLASS, name, config, sizeof(*config), id);
        if (error == ESP_OK) {
            error = sc2p_attach(*id, config);
        }
        xSemaphoreGive(sc2p.edit_lock);
    }
    sc2p_config_free(config);
    return error;
}

/* Settings: "URL [ADMIT]", "admit CODE|off" or "pin sha256/B64|off". */
static esp_err_t sc2p_configure(void *context, int argc, const char *const *argv)
{
    sc2p_relay_t *node = context;
    if (node == NULL || argc < 1 || argv == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_sc2_relay_config_t *config = sc2p_config_alloc();
    if (config == NULL) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(sc2p.edit_lock, portMAX_DELAY);
    sc2p_snapshot(node, config);
    esp_err_t error = ESP_ERR_INVALID_ARG;
    bool flags_changed = false;
    if (argc == 2 && strcmp(argv[0], "admit") == 0) {
        const char *value = strcmp(argv[1], "off") == 0 ? "" : argv[1];
        sc2p_wipe(config->admit, sizeof(config->admit));
        if (sc2p_copy(config->admit, sizeof(config->admit), value, strlen(value))) {
            error = ESP_OK;
        }
    } else if (argc == 2 && strcmp(argv[0], "pin") == 0) {
        const char *value = strcmp(argv[1], "off") == 0 ? "" : argv[1];
        if (sc2p_copy(config->pin, sizeof(config->pin), value, strlen(value))) {
            error = ESP_OK;
        }
    } else if ((argc == 1 || argc == 2) && strstr(argv[0], "://") != NULL) {
        bool was_tls = config->tls;
        error = solar_os_sc2_relay_parse_url(argv[0], config);
        if (error == ESP_OK && argc == 2) {
            sc2p_wipe(config->admit, sizeof(config->admit));
            if (!sc2p_copy(config->admit, sizeof(config->admit), argv[1], strlen(argv[1]))) {
                error = ESP_ERR_INVALID_ARG;
            }
        }
        flags_changed = was_tls != config->tls;
    }
    if (error == ESP_OK) {
        error = sc2p_save(node, config);
    }
    xSemaphoreGive(sc2p.edit_lock);
    if (error == ESP_OK) {
        if (flags_changed) {
            sc2p_upsert_rooms(node->id, config);
        }
        sc2p_status(node->id, config);
    }
    sc2p_config_free(config);
    return error;
}

static esp_err_t sc2p_connect(void *context, bool enabled)
{
    sc2p_relay_t *node = context;
    if (node == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    solar_os_sc2_relay_config_t *config = sc2p_config_alloc();
    if (config == NULL) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(sc2p.edit_lock, portMAX_DELAY);
    sc2p_snapshot(node, config);
    config->enabled = enabled;
    esp_err_t error = sc2p_save(node, config);
    xSemaphoreGive(sc2p.edit_lock);
    if (error == ESP_OK) {
        sc2p_status(node->id, config);
    }
    sc2p_config_free(config);
    return error;
}

static esp_err_t sc2p_normalize(void *context, const char *target, char *out, size_t size)
{
    if (context == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* A 32-hex target normalises identically as a room or a device. */
    return solar_os_sc2_target_normalize(target, true, out, size) == SOLAR_OS_SC2_TARGET_INVALID
               ? ESP_ERR_INVALID_ARG
               : ESP_OK;
}

static esp_err_t sc2p_join_open(void *context, const char *target, solar_os_conversation_id_t *conversation)
{
    sc2p_relay_t *node = context;
    char key[SOLAR_OS_MESSAGING_PROVIDER_KEY_MAX];
    if (node == NULL || solar_os_sc2_target_normalize(target, true, key, sizeof(key)) == SOLAR_OS_SC2_TARGET_INVALID) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_sc2_relay_config_t *config = sc2p_config_alloc();
    if (config == NULL) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(sc2p.edit_lock, portMAX_DELAY);
    sc2p_snapshot(node, config);
    int found = -1;
    int empty = -1;
    for (size_t i = 0; i < SOLAR_OS_SC2_RELAY_ROOM_CAPACITY; i++) {
        if (strcmp(config->rooms[i], key) == 0) {
            found = (int)i;
        } else if (config->rooms[i][0] == '\0' && empty < 0) {
            empty = (int)i;
        }
    }
    esp_err_t error = ESP_OK;
    if (found < 0) {
        if (empty < 0) {
            error = ESP_ERR_NO_MEM;
        } else {
            memcpy(config->rooms[empty], key, strlen(key) + 1u);
            error = sc2p_save(node, config);
        }
    }
    xSemaphoreGive(sc2p.edit_lock);
    if (error == ESP_OK) {
        error = sc2p_upsert_room(node->id, config, key, conversation);
    }
    if (error == ESP_OK) {
        /* Requested; the relay transport joins it once connected. */
        (void)solar_os_messaging_join_update(node->id, key, SOLAR_OS_MESSAGING_JOIN_PENDING, NULL);
    }
    sc2p_config_free(config);
    return error;
}

static esp_err_t sc2p_leave(void *context, const char *target)
{
    sc2p_relay_t *node = context;
    char key[SOLAR_OS_MESSAGING_PROVIDER_KEY_MAX];
    if (node == NULL || solar_os_sc2_target_normalize(target, true, key, sizeof(key)) == SOLAR_OS_SC2_TARGET_INVALID) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_sc2_relay_config_t *config = sc2p_config_alloc();
    if (config == NULL) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(sc2p.edit_lock, portMAX_DELAY);
    sc2p_snapshot(node, config);
    bool changed = false;
    for (size_t i = 0; i < SOLAR_OS_SC2_RELAY_ROOM_CAPACITY; i++) {
        if (strcmp(config->rooms[i], key) == 0) {
            memset(config->rooms[i], 0, sizeof(config->rooms[i]));
            changed = true;
        }
    }
    esp_err_t error = changed ? sc2p_save(node, config) : ESP_OK;
    xSemaphoreGive(sc2p.edit_lock);
    if (error == ESP_OK) {
        (void)solar_os_messaging_join_update(node->id, key, SOLAR_OS_MESSAGING_JOIN_LEFT, NULL);
    }
    sc2p_config_free(config);
    return error;
}

static esp_err_t sc2p_direct_open(void *context, const char *target, solar_os_conversation_id_t *conversation)
{
    sc2p_relay_t *node = context;
    char key[SOLAR_OS_MESSAGING_PROVIDER_KEY_MAX];
    if (node == NULL ||
        solar_os_sc2_target_normalize(target, false, key, sizeof(key)) != SOLAR_OS_SC2_TARGET_DEVICE) {
        return ESP_ERR_INVALID_ARG;
    }
    sc2e_peer_info_t peer;
    bool known = solar_os_sc2_peer_info(key, &peer) == ESP_OK && peer.known;
    solar_os_sc2_relay_config_t *config = sc2p_config_alloc();
    if (config == NULL) {
        return ESP_ERR_NO_MEM;
    }
    sc2p_snapshot(node, config);
    const solar_os_messaging_conversation_upsert_t request = {
        .provider = node->id,
        .provider_key = key,
        .kind = SOLAR_OS_CONVERSATION_DIRECT,
        .title = key,
        .security_flags = solar_os_sc2_conversation_security(config, SOLAR_OS_SC2_TARGET_DEVICE, known),
    };
    sc2p_config_free(config);
    return solar_os_messaging_conversation_upsert(&request, conversation);
}

static esp_err_t sc2p_remove_prepare(void *context)
{
    if (context == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(sc2p.edit_lock, portMAX_DELAY);
    return ESP_OK;
}

static void sc2p_remove_finish(void *context, bool removed)
{
    sc2p_relay_t *node = context;
    if (removed) {
        xSemaphoreTake(sc2p.lock, portMAX_DELAY);
        sc2p_relay_t **link = &sc2p.relays;
        while (*link != NULL && *link != node) {
            link = &(*link)->next;
        }
        if (*link != NULL) {
            *link = node->next;
        }
        xSemaphoreGive(sc2p.lock);
        sc2p_wipe(node, sizeof(*node));
        solar_os_memory_free(node);
    }
    xSemaphoreGive(sc2p.edit_lock);
}
