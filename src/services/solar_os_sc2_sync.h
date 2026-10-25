#pragma once

/*
 * sc2-sync: the relay transport for the "sc2" Messaging provider. One worker
 * task drives the first enabled sc2 instance over TCP (chat://) or TLS
 * (chats://, CA bundle plus optional SPKI pin) with the sans-IO client in
 * solar_os_sc2_client.c: E2E DMs and signed public rooms (#general), with
 * reconnect backoff. Private rooms are not wired in this build.
 */

#include <stdbool.h>
#include "esp_err.h"

esp_err_t solar_os_sc2_sync_start(void);
void solar_os_sc2_sync_stop(void);
/* Install the provider hook so enabling an instance starts the worker. */
void solar_os_sc2_sync_install(void);

typedef struct {
    bool running;
    bool connected;
    bool ready;
    unsigned instance;
    unsigned outbox;
    unsigned attempt;
    char relay_id[64];
    char host[64];
    unsigned port;
    bool tls;
} solar_os_sc2_sync_status_t;

void solar_os_sc2_sync_get_status(solar_os_sc2_sync_status_t *status);
