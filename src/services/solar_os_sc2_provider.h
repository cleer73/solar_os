#pragma once

/*
 * The "sc2" Messaging provider class. Each instance is one SC2 relay; the
 * device identity and sessions in /.sc2 are shared by all relays.
 *
 * This file registers the class, validates and stores per-relay settings in
 * the instance profile, and keeps the joined room list and conversation
 * security flags. It does not open network connections: queued sends stay
 * queued until a relay transport (the sc2-sync job) is added.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_messaging.h"

#define SOLAR_OS_SC2_RELAY_CONFIG_VERSION 1U
#define SOLAR_OS_SC2_RELAY_URL_MAX 128U
#define SOLAR_OS_SC2_RELAY_PIN_MAX 72U      /* "sha256/" + base64 of 32 bytes */
#define SOLAR_OS_SC2_RELAY_ADMIT_MAX 64U
#define SOLAR_OS_SC2_RELAY_ROOM_CAPACITY 16U
#define SOLAR_OS_SC2_PUBLIC_ROOM_PREFIX "pub:"
#define SOLAR_OS_SC2_PUBLIC_ROOM_NAME_MAX 59U /* after "pub:", fits a provider key */

typedef struct {
    uint32_t version;
    bool enabled;
    bool tls;                                   /* chats:// (true) or chat:// */
    uint16_t port;
    char url[SOLAR_OS_SC2_RELAY_URL_MAX];       /* without the #pin fragment */
    char pin[SOLAR_OS_SC2_RELAY_PIN_MAX];       /* empty: CA bundle */
    char admit[SOLAR_OS_SC2_RELAY_ADMIT_MAX];   /* optional admission code */
    char rooms[SOLAR_OS_SC2_RELAY_ROOM_CAPACITY][SOLAR_OS_MESSAGING_PROVIDER_KEY_MAX];
} solar_os_sc2_relay_config_t;

typedef enum {
    SOLAR_OS_SC2_TARGET_INVALID = 0,
    SOLAR_OS_SC2_TARGET_DEVICE,       /* 32 hex characters */
    SOLAR_OS_SC2_TARGET_PRIVATE_ROOM, /* 32 hex characters */
    SOLAR_OS_SC2_TARGET_PUBLIC_ROOM,  /* pub:name */
} solar_os_sc2_target_kind_t;

/* Register the class and attach saved instances. Safe to call repeatedly. */
esp_err_t solar_os_sc2_provider_init(void);

/* Parse "chats://host[:port][#pin=sha256/B64]" or "chat://host[:port]" into
 * config (url, tls, port, pin). Leaves the other fields untouched. */
esp_err_t solar_os_sc2_relay_parse_url(const char *text, solar_os_sc2_relay_config_t *config);
bool solar_os_sc2_relay_config_valid(const solar_os_sc2_relay_config_t *config);

/* Normalise a room or device target into a provider key (lower case).
 * want_room selects whether a 32-hex value is a room or a device. */
solar_os_sc2_target_kind_t solar_os_sc2_target_normalize(const char *target, bool want_room,
                                                         char *out, size_t size);

/* Settings snapshot for one sc2 instance (copy; contains the admission code). */
esp_err_t solar_os_sc2_relay_get_config(solar_os_messaging_provider_id_t id,
                                        solar_os_sc2_relay_config_t *config);

/* Conversation security flags the provider assigns to each conversation kind. */
uint32_t solar_os_sc2_conversation_security(const solar_os_sc2_relay_config_t *config,
                                            solar_os_sc2_target_kind_t kind, bool peer_key_known);

/* Relay transport hook (sc2-sync): called when an instance is enabled,
 * reconfigured or disabled. Install before solar_os_sc2_provider_init(). */
typedef void (*solar_os_sc2_transport_fn)(solar_os_messaging_provider_id_t id, bool enabled);
void solar_os_sc2_provider_set_transport(solar_os_sc2_transport_fn fn);
