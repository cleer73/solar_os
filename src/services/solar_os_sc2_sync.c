/*
 * sc2-sync worker: relay transport for the "sc2" Messaging provider.
 * See solar_os_sc2_sync.h. RAM: one worker stack (SOLAR_OS_SC2_SYNC_STACK), the
 * client struct and its three buffers (PSRAM preferred), one outbound
 * request copy (PSRAM preferred) and the TLS session when chats:// is used.
 *
 * SPDX-License-Identifier: MIT
 */
#include "solar_os_sc2_sync.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <time.h>

#include "esp_crt_bundle.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"
#include "mbedtls/ssl.h"
#include "mbedtls/pk.h"
#include "solar_os_jobs.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_messaging.h"
#include "solar_os_sc2.h"
#include "solar_os_sc2_client.h"
#include "solar_os_sc2_provider.h"
#include "solar_os_sc2_relay_policy.h"
#include "solar_os_task.h"

static const char *TAG = "sc2-sync";
_Static_assert(sizeof(((solar_os_sc2_sync_status_t *)0)->host) >= SOLAR_OS_SC2_RELAY_URL_MAX,
               "sc2 relay status must hold the whole host");

#define SC2_SYNC_CONNECT_MS 15000
#define SC2_SYNC_WRITE_MS 8000u
#define SC2_SYNC_CHECK_MS 2000u
#define SC2_SYNC_DEFAULT_ROOM "pub:general"
/* Multi-relay: see solar_os_sc2_relay_policy.h. Inbound provider message keys carry the relay index in the top byte (0
 * for the primary, as before), so per-relay sequence numbers never collide
 * and room cursors stay per relay. */
#define SC2_SYNC_TAG_SHIFT 56u
#define SC2_SYNC_TAG_MASK (0xFFull << SC2_SYNC_TAG_SHIFT)
#define SC2_SYNC_TAG(relay) ((uint64_t)(relay) << SC2_SYNC_TAG_SHIFT)

typedef struct {
    TaskHandle_t task;
    volatile bool stop;
    volatile bool done;
    volatile bool running;
    bool connected;
    bool ready;
    bool client_ok;
    bool auto_joined;
    bool rebuild;     /* relay switch pending: rebuild the client */
    const char *key_refused; /* login refused: relay key differs from the pin */
    sc2rp_t rp;       /* relay in use, home relay, visits */
    uint32_t attempt;
    uint64_t retry_at;
    uint64_t checked_at;
    uint64_t again_at;
    uint32_t again_id;
    solar_os_messaging_provider_id_t id;
    solar_os_sc2_relay_config_t *cfg;  /* active */
    solar_os_sc2_relay_config_t *next; /* scratch for change detection */
    solar_os_sc2_relay_endpoint_t *ep; /* relay in use (copy from cfg) */
    solar_os_messaging_outbound_t *ob;
    sc2c_t *c;
    esp_tls_t *tls;
    char host[SOLAR_OS_SC2_RELAY_URL_MAX];
    char tag[SC2C_TAG_MAX + 1];
    char nick[SC2C_NICK_MAX + 1];
} sc2s_t;

static sc2s_t s;

static uint64_t mono_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

/* ---------------- client host ---------------- */

static int h_send(void *ctx, const char *line, size_t len)
{
    (void)ctx;
    if (s.tls == NULL) {
        return -1;
    }
    uint64_t deadline = mono_ms() + SC2_SYNC_WRITE_MS;
    for (int part = 0; part < 2; part++) {
        const char *p = part == 0 ? line : "\n";
        size_t left = part == 0 ? len : 1u;
        while (left > 0) {
            int n = esp_tls_conn_write(s.tls, p, left);
            if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE || n == 0) {
                if (mono_ms() > deadline) {
                    return -1;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            if (n < 0) {
                return -1;
            }
            p += n;
            left -= (size_t)n;
        }
    }
    return 0;
}

static sc2e_t *h_lock(void *ctx)
{
    (void)ctx;
    return solar_os_sc2_engine_lock();
}

static void h_unlock(void *ctx)
{
    (void)ctx;
    solar_os_sc2_engine_unlock();
}

static void *h_alloc(void *ctx, size_t len)
{
    (void)ctx;
    return solar_os_sc2_buffer_alloc(len);
}

static void h_release(void *ctx, void *ptr, size_t len)
{
    (void)ctx;
    solar_os_sc2_buffer_free(ptr, len);
}

static uint64_t h_mono(void *ctx)
{
    (void)ctx;
    return mono_ms();
}

static uint64_t h_wall(void *ctx)
{
    (void)ctx;
    time_t t = time(NULL);
    return t >= (time_t)SOLAR_OS_SC2_MIN_WALL_CLOCK ? (uint64_t)t : 0u;
}

static void status(bool connected, esp_err_t err, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void status(bool connected, esp_err_t err, const char *fmt, ...)
{
    if (s.id == 0) {
        return;
    }
    char detail[SOLAR_OS_MESSAGING_ERROR_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof(detail), fmt, ap);
    va_end(ap);
    (void)solar_os_messaging_provider_set_status(s.id, true, connected, err, detail);
}

static void short_id(char *out, size_t cap, const char *hex)
{
    snprintf(out, cap, "%.8s", hex != NULL ? hex : "");
}

static void on_message(const sc2c_event_t *ev)
{
    bool dm = ev->kind == SC2C_DM;
    if (!dm && ev->kind != SC2C_PUBLIC) {
        return; /* private rooms are not wired in this build */
    }
    char title[SOLAR_OS_MESSAGING_TITLE_MAX];
    char sender[SOLAR_OS_MESSAGING_SENDER_MAX];
    char sid[12];
    short_id(sid, sizeof(sid), ev->from);
    if (dm) {
        snprintf(title, sizeof(title), "sc2 %s", sid);
        snprintf(sender, sizeof(sender), "%s", sid);
    } else {
        snprintf(title, sizeof(title), "#%s", ev->conv + strlen(SOLAR_OS_SC2_PUBLIC_ROOM_PREFIX));
        /* public names are self-chosen: always show the key prefix too */
        if (ev->name != NULL && ev->name[0] != '\0') {
            snprintf(sender, sizeof(sender), "%.20s~%.6s", ev->name, sid);
        } else {
            snprintf(sender, sizeof(sender), "%s", sid);
        }
    }
    bool truncated = ev->body_len >= SOLAR_OS_MESSAGING_BODY_MAX;
    const solar_os_messaging_inbound_t in = {
        .provider = s.id,
        .conversation_key = ev->conv,
        .conversation_kind = dm ? SOLAR_OS_CONVERSATION_DIRECT : SOLAR_OS_CONVERSATION_ROOM,
        .conversation_title = title,
        .provider_message_key = (ev->seq & ~SC2_SYNC_TAG_MASK) | SC2_SYNC_TAG(s.rp.relay),
        .timestamp_ms = ev->ts != 0 ? ev->ts * 1000u : (uint64_t)h_wall(NULL) * 1000u,
        .security_flags = solar_os_sc2_conversation_security(
            s.cfg, dm ? SOLAR_OS_SC2_TARGET_DEVICE : SOLAR_OS_SC2_TARGET_PUBLIC_ROOM, dm),
        .sender = sender,
        .body = ev->body,
        .truncated = truncated,
    };
    esp_err_t err = solar_os_messaging_publish_inbound(&in, NULL, NULL);
    if (err != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "inbound message not stored: %s", esp_err_to_name(err));
    }
}

static void h_event(void *ctx, const sc2c_event_t *ev)
{
    (void)ctx;
    switch (ev->type) {
    case SC2C_EV_MESSAGE:
        on_message(ev);
        break;
    case SC2C_EV_SENT:
        (void)solar_os_messaging_outbox_update(ev->token, SOLAR_OS_DELIVERY_SENT, NULL);
        break;
    case SC2C_EV_FAILED:
        (void)solar_os_messaging_outbox_update(ev->token, SOLAR_OS_DELIVERY_FAILED,
                                               ev->text != NULL && ev->text[0] ? ev->text : "refused by relay");
        break;
    case SC2C_EV_STATE:
        s.ready = ev->code == SC2C_READY;
        if (ev->code == SC2C_READY) {
            s.attempt = 0;
            sc2rp_ready(&s.rp, mono_ms());
            if (s.rp.relay == 0) {
                status(true, ESP_OK, "connected to %s:%u (%s)", s.host, (unsigned)s.ep->port, s.c->relay_id);
            } else {
                status(true, ESP_OK, "connected to %s:%u (%s), fallback %u%s", s.host, (unsigned)s.ep->port,
                       s.c->relay_id, (unsigned)s.rp.relay, s.rp.visiting ? ", visit" : "");
            }
            SOLAR_OS_LOGI(TAG, "ready: relay %u %s as %s", (unsigned)s.rp.relay, s.c->relay_id, s.c->self_hex);
        } else if (ev->code != SC2C_OFFLINE) {
            status(false, ESP_OK, "%s with %s:%u", sc2c_state_name(ev->code), s.host, (unsigned)s.ep->port);
        }
        break;
    case SC2C_EV_ROOM: {
        solar_os_messaging_join_state_t js = SOLAR_OS_MESSAGING_JOIN_PENDING;
        if (ev->code == SC2C_ROOM_JOINED) {
            js = SOLAR_OS_MESSAGING_JOIN_JOINED;
        } else if (ev->code == SC2C_ROOM_FAILED) {
            js = SOLAR_OS_MESSAGING_JOIN_FAILED;
        } else if (ev->code == SC2C_ROOM_LEFT || ev->code == SC2C_ROOM_IDLE) {
            js = SOLAR_OS_MESSAGING_JOIN_LEFT;
        }
        (void)solar_os_messaging_join_update(s.id, ev->conv, js, ev->text);
        break;
    }
    case SC2C_EV_NOTICE:
        if (ev->code == SC2C_NOTICE_RELAY_KEY) {
            s.key_refused = ev->text; /* a string literal in the client */
            SOLAR_OS_LOGE(TAG, "relay %u: %s", (unsigned)s.rp.relay, ev->text);
            break;
        }
        SOLAR_OS_LOGI(TAG, "relay notice %d: %s", ev->code, ev->text != NULL ? ev->text : "");
        break;
    case SC2C_EV_RELAY_KEY: {
        /* signed by the pinned key and proven by the relay: keep the new pin */
        const size_t i = s.rp.relay;
        uint8_t old[SOLAR_OS_SC2_RELAY_KEY_LEN];
        if (solar_os_sc2_relay_key(s.cfg, i, old)) {
            esp_err_t err = solar_os_sc2_relay_set_key(s.id, i, old, s.c->relay_key);
            if (err == ESP_OK) {
                memcpy(s.cfg->keys[i], s.c->relay_key, SOLAR_OS_SC2_RELAY_KEY_LEN); /* no reconnect for it */
            }
            SOLAR_OS_LOGW(TAG, "relay %u: %s%s", (unsigned)i, ev->text, err == ESP_OK ? "; pin updated" : "; saving failed");
        }
        break;
    }
    default:
        break;
    }
}

/* ---------------- transport ---------------- */

static bool pin_matches(void)
{
    if (s.ep->pin[0] == '\0') {
        return true;
    }
    mbedtls_ssl_context *ssl = esp_tls_get_ssl_context(s.tls);
    const mbedtls_x509_crt *crt = ssl != NULL ? mbedtls_ssl_get_peer_cert(ssl) : NULL;
    if (crt == NULL) {
        return false;
    }
    unsigned char der[600];
    int n = mbedtls_pk_write_pubkey_der((mbedtls_pk_context *)&crt->pk, der, sizeof(der));
    if (n <= 0) {
        return false;
    }
    unsigned char hash[32];
    if (mbedtls_sha256(der + sizeof(der) - (size_t)n, (size_t)n, hash, 0) != 0) {
        return false;
    }
    unsigned char b64[48];
    size_t bl = 0;
    if (mbedtls_base64_encode(b64, sizeof(b64), &bl, hash, sizeof(hash)) != 0) {
        return false;
    }
    static const char prefix[] = "sha256/";
    const char *pin = s.ep->pin;
    if (strncmp(pin, prefix, sizeof(prefix) - 1u) == 0) {
        pin += sizeof(prefix) - 1u;
    }
    return strlen(pin) == bl && memcmp(pin, b64, bl) == 0;
}

static void drop(esp_err_t err, const char *why)
{
    if (s.tls != NULL) {
        esp_tls_conn_destroy(s.tls);
        s.tls = NULL;
    }
    bool was = s.connected;
    s.connected = false;
    s.ready = false;
    if (s.client_ok && was) {
        sc2c_on_disconnected(s.c);
    }
    if (why == NULL) {
        return;
    }
    const unsigned from = s.rp.relay;
    sc2rp_action_t act = sc2rp_failed(&s.rp);
    s.rebuild = act != SC2RP_STAY;
    if (act == SC2RP_SWITCH_NOW) {
        /* a visited relay is not needed now: straight back home, no backoff */
        SOLAR_OS_LOGW(TAG, "%s (relay %u %s:%u); back to relay %u", why, from, s.host, (unsigned)s.ep->port,
                      (unsigned)s.rp.relay);
        s.retry_at = mono_ms();
        return;
    }
    /* the existing backoff paces the attempts, also across relays */
    uint32_t delay = sc2c_backoff_ms(&s.attempt, esp_random());
    s.retry_at = mono_ms() + delay;
    const char *next = act == SC2RP_STAY ? "" : s.rp.relay == 0 ? ", next: primary" : ", next: fallback";
    status(false, err, "%s; retry in %u s%s", why, (unsigned)((delay + 999u) / 1000u), next);
    SOLAR_OS_LOGW(TAG, "%s (%s:%u); retry in %u ms%s", why, s.host, (unsigned)s.ep->port, (unsigned)delay, next);
}

/* chats://...#pin=: the pinned SPKI is the trust anchor, so a self-signed
 * relay works. The handshake runs without a CA chain (VERIFY_OPTIONAL) and
 * pin_matches() checks the key before anything is sent. */
static esp_err_t pin_only_attach(void *conf)
{
    mbedtls_ssl_conf_authmode((mbedtls_ssl_config *)conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
    return ESP_OK;
}

static void try_connect(void)
{
    esp_tls_cfg_t tc;
    memset(&tc, 0, sizeof(tc));
    tc.timeout_ms = SC2_SYNC_CONNECT_MS;
    if (s.ep->tls) {
        tc.crt_bundle_attach = s.ep->pin[0] != '\0' ? pin_only_attach : esp_crt_bundle_attach;
    } else {
        tc.is_plain_tcp = true;
    }
    s.tls = esp_tls_init();
    if (s.tls == NULL) {
        drop(ESP_ERR_NO_MEM, "TLS allocation failed");
        return;
    }
    status(false, ESP_OK, "connecting to %s:%u (%s%s)", s.host, (unsigned)s.ep->port,
           s.ep->tls ? (s.ep->pin[0] != '\0' ? "TLS, pinned key" : "TLS") : "plain TCP",
           s.rp.relay == 0 ? "" : s.rp.visiting ? ", visiting fallback" : ", fallback");
    if (esp_tls_conn_new_sync(s.host, (int)strlen(s.host), s.ep->port, &tc, s.tls) != 1) {
        drop(ESP_FAIL, s.ep->tls ? "TLS connection failed" : "TCP connection failed");
        return;
    }
    if (s.ep->tls && !pin_matches()) {
        drop(ESP_ERR_INVALID_RESPONSE, "relay TLS key does not match the pin");
        return;
    }
    s.connected = true;
    if (sc2c_on_connected(s.c) != SC2_OK) {
        drop(ESP_FAIL, "hello failed");
    }
}

/* Wait up to wait_ms for input, then feed what arrived. */
static void pump_input(uint32_t wait_ms)
{
    int fd = -1;
    if (esp_tls_get_conn_sockfd(s.tls, &fd) != ESP_OK || fd < 0) {
        drop(ESP_FAIL, "connection lost");
        return;
    }
    if (esp_tls_get_bytes_avail(s.tls) <= 0) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(fd, &rd);
        struct timeval tv = {.tv_sec = 0, .tv_usec = (suseconds_t)wait_ms * 1000};
        if (select(fd + 1, &rd, NULL, NULL, &tv) <= 0) {
            return;
        }
    }
    uint8_t buf[512];
    for (int pass = 0; pass < 8 && s.tls != NULL; pass++) {
        int n = esp_tls_conn_read(s.tls, buf, sizeof(buf));
        if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) {
            return;
        }
        if (n <= 0) {
            drop(ESP_FAIL, "relay closed the connection");
            return;
        }
        int rc = sc2c_feed(s.c, buf, (size_t)n);
        if (rc != SC2_OK) {
            const char *why = rc == SC2_ERR_AUTH ? "relay refused login" : "protocol error";
            esp_err_t err = ESP_FAIL;
            if (rc == SC2_ERR_AUTH && s.key_refused != NULL) {
                why = s.key_refused; /* hard fail; the pin stays as it is */
                err = ESP_ERR_INVALID_RESPONSE;
            }
            s.key_refused = NULL;
            drop(err, why);
            return;
        }
        if (esp_tls_get_bytes_avail(s.tls) <= 0) {
            return;
        }
    }
}

/* ---------------- Messaging glue ---------------- */

static void push_outbox(void)
{
    /* not while visiting: peers look for us (and our posts) on our home relay */
    if (!s.ready || s.rp.visiting || solar_os_messaging_outbox_peek(s.id, s.ob) != ESP_OK) {
        return;
    }
    uint64_t now = mono_ms();
    if (s.ob->id == s.again_id && now < s.again_at) {
        return;
    }
    solar_os_messaging_conversation_t conv;
    sc2c_kind_t kind;
    if (solar_os_messaging_conversation_get(s.ob->conversation_id, &conv) != ESP_OK) {
        (void)solar_os_messaging_outbox_update(s.ob->id, SOLAR_OS_DELIVERY_FAILED, "conversation not found");
        return;
    }
    if (conv.kind == SOLAR_OS_CONVERSATION_DIRECT) {
        kind = SC2C_DM;
    } else if (strncmp(s.ob->provider_key, SOLAR_OS_SC2_PUBLIC_ROOM_PREFIX,
                       strlen(SOLAR_OS_SC2_PUBLIC_ROOM_PREFIX)) == 0) {
        kind = SC2C_PUBLIC;
    } else {
        (void)solar_os_messaging_outbox_update(s.ob->id, SOLAR_OS_DELIVERY_FAILED,
                                               "private SC2 rooms are not in this build");
        return;
    }
    int rc = sc2c_submit(s.c, kind, s.ob->provider_key, s.ob->body, strlen(s.ob->body), s.ob->id,
                         (uint64_t)s.ob->message_key);
    if (rc == SC2_OK) {
        (void)solar_os_messaging_outbox_update(s.ob->id, SOLAR_OS_DELIVERY_SENDING, NULL);
    } else if (rc == SC2C_AGAIN) {
        s.again_id = s.ob->id;
        s.again_at = now + 500u;
    } else {
        char why[SOLAR_OS_MESSAGING_ERROR_MAX];
        snprintf(why, sizeof(why), "%s", rc == SC2_ERR_NOT_FOUND ? "peer has no keys on this relay" : sc2_strerror(rc));
        (void)solar_os_messaging_outbox_update(s.ob->id, SOLAR_OS_DELIVERY_FAILED, why);
    }
}

static bool room_configured(const char *key)
{
    for (size_t i = 0; i < SOLAR_OS_SC2_RELAY_ROOM_CAPACITY; i++) {
        if (strcmp(s.cfg->rooms[i], key) == 0) {
            return true;
        }
    }
    return false;
}

static void add_rooms(void)
{
    bool have_general = false;
    /* `messages leave` drops a room from the config: leave it on the relay
     * too, or its posts keep arriving for the rest of the session. */
    for (size_t i = 0; i < SC2C_ROOMS_MAX; i++) {
        const sc2c_room_t *r = &s.c->rooms[i];
        if (r->key[0] != '\0' && r->kind == SC2C_PUBLIC && !room_configured(r->key)) {
            (void)sc2c_room_remove(s.c, r->key);
        }
    }
    for (size_t i = 0; i < SOLAR_OS_SC2_RELAY_ROOM_CAPACITY; i++) {
        const char *key = s.cfg->rooms[i];
        if (key[0] == '\0') {
            continue;
        }
        if (strcmp(key, SC2_SYNC_DEFAULT_ROOM) == 0) {
            have_general = true;
        }
        if (strncmp(key, SOLAR_OS_SC2_PUBLIC_ROOM_PREFIX, strlen(SOLAR_OS_SC2_PUBLIC_ROOM_PREFIX)) != 0) {
            continue; /* private rooms: not in this build */
        }
        (void)sc2c_room_add(s.c, key, solar_os_messaging_provider_cursor_masked(s.id, key, SC2_SYNC_TAG_MASK,
                                                                                 SC2_SYNC_TAG(s.rp.relay)));
    }
    if (!have_general && !s.auto_joined) {
        /* #general by default: through Messaging so it shows in the chat UI
         * and is saved; the config change brings it back here. */
        s.auto_joined = true;
        (void)solar_os_messaging_join(s.id, SC2_SYNC_DEFAULT_ROOM, NULL);
    }
}

static void client_close(void)
{
    drop(ESP_OK, NULL);
    if (s.client_ok) {
        sc2c_free(s.c);
        s.client_ok = false;
    }
}

static bool same_link(const solar_os_sc2_relay_config_t *a, const solar_os_sc2_relay_config_t *b)
{
    return a->enabled == b->enabled && a->tls == b->tls && a->port == b->port && strcmp(a->url, b->url) == 0 &&
           strcmp(a->pin, b->pin) == 0 && strcmp(a->admit, b->admit) == 0 &&
           memcmp(a->fallback, b->fallback, sizeof(a->fallback)) == 0 && a->key_set == b->key_set &&
           memcmp(a->keys, b->keys, sizeof(a->keys)) == 0;
}

/* (Re)build the client for relay s.rp.relay. Same store tag on every relay: the
 * persisted outbox follows whichever relay is in use. Fallback relays get the
 * signed prekey only (no one-time prekey may be claimable on two relays). */
static bool build_client(void)
{
    client_close();
    (void)solar_os_sc2_relay_endpoint(s.cfg, s.rp.relay, s.ep);
    const char *h = strstr(s.ep->url, "://");
    h = h != NULL ? h + 3 : s.ep->url;
    size_t hl = strcspn(h, ":");
    if (hl >= sizeof(s.host)) {
        hl = sizeof(s.host) - 1u;
    }
    memcpy(s.host, h, hl);
    s.host[hl] = '\0';
    snprintf(s.tag, sizeof(s.tag), "r%u", (unsigned)(s.id % 10000000u));
    sc2c_host_t host = {h_send, h_event, h_lock, h_unlock, h_alloc, h_release, h_mono, h_wall, NULL};
    sc2c_config_t cc = {
        .tag = s.tag,
        .nick = s.nick,
        .admit = s.cfg->admit[0] != '\0' ? s.cfg->admit : NULL,
        .no_opks = s.rp.relay != 0,
    };
    uint8_t key[SOLAR_OS_SC2_RELAY_KEY_LEN];
    if (solar_os_sc2_relay_key(s.cfg, s.rp.relay, key)) {
        cc.relay_key = key; /* the relay must prove this key at login */
    }
    int rc = sc2c_init(s.c, &host, &cc);
    s.rebuild = false;
    if (rc != SC2_OK) {
        status(false, ESP_ERR_NO_MEM, "client init failed: %s", sc2_strerror(rc));
        s.retry_at = mono_ms() + 10000u;
        return false;
    }
    s.client_ok = true;
    s.auto_joined = false;
    add_rooms();
    return true;
}

/* Pick the first enabled sc2 instance; (re)build the client on changes. */
static void check_config(void)
{
    solar_os_messaging_provider_id_t cursor = 0, found = 0;
    solar_os_messaging_provider_status_t st;
    while (solar_os_messaging_provider_next(&cursor, &st)) {
        if (st.removed || !solar_os_messaging_provider_is_class(st.id, "sc2")) {
            continue;
        }
        if (solar_os_sc2_relay_get_config(st.id, s.next) == ESP_OK && s.next->enabled) {
            found = st.id;
            snprintf(s.nick, sizeof(s.nick), "%s", st.name); /* public post name: the instance name */
            break;
        }
    }
    if (found == 0) {
        if (s.id != 0) {
            client_close();
            s.id = 0;
        }
        return;
    }
    bool link_changed = found != s.id || !s.client_ok || !same_link(s.cfg, s.next);
    bool rooms_changed = memcmp(s.cfg->rooms, s.next->rooms, sizeof(s.next->rooms)) != 0;
    if (link_changed) {
        client_close();
    }
    *s.cfg = *s.next;
    s.id = found;
    if (link_changed) {
        sc2rp_reset(&s.rp, (unsigned)solar_os_sc2_relay_endpoint(s.cfg, 0, NULL), mono_ms());
        s.attempt = 0;
        s.retry_at = 0;
        (void)build_client();
        return;
    }
    if (rooms_changed) {
        add_rooms();
    }
}

/* Visits (probe the primary, take keys and mailbox to the fallbacks), only
 * when nothing is waiting to be sent. */
static void schedule_visit(uint64_t now)
{
    bool idle = s.rp.visiting ||
                (sc2c_outbox_count(s.c) == 0 && solar_os_messaging_outbox_peek(s.id, s.ob) != ESP_OK);
    if (sc2rp_tick(&s.rp, now, idle) != SC2RP_STAY) {
        drop(ESP_OK, NULL);
        s.rebuild = true;
        s.retry_at = now;
    }
}

static void worker(void *arg)
{
    (void)arg;
    esp_err_t err = solar_os_sc2_init();
    if (err != ESP_OK) {
        SOLAR_OS_LOGE(TAG, "SC2 service unavailable: %s", solar_os_sc2_err_text(err));
    }
    while (!s.stop && err == ESP_OK) {
        uint64_t now = mono_ms();
        if (now - s.checked_at >= SC2_SYNC_CHECK_MS || s.checked_at == 0) {
            s.checked_at = now;
            check_config();
        }
        if (s.id != 0 && s.rebuild && !s.connected) {
            (void)build_client();
        }
        if (s.id == 0 || !s.client_ok) {
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(SC2_SYNC_CHECK_MS));
            continue;
        }
        if (!s.connected) {
            if (now < s.retry_at) {
                (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
                continue;
            }
            try_connect();
            continue;
        }
        pump_input(100);
        if (!s.connected) {
            continue;
        }
        if (sc2c_tick(s.c) != SC2_OK) {
            drop(ESP_ERR_TIMEOUT, "relay went silent");
            continue;
        }
        push_outbox();
        if (s.ready) {
            schedule_visit(now);
        }
    }
    client_close();
    s.id = 0;
    s.running = false;
    s.done = true;
    solar_os_task_delete(NULL);
}

static bool alloc_state(void)
{
    if (s.c == NULL) {
        s.c = solar_os_memory_calloc(1, sizeof(*s.c), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "sc2.client");
    }
    if (s.cfg == NULL) {
        s.cfg = solar_os_memory_calloc(1, sizeof(*s.cfg), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "sc2.sync.cfg");
    }
    if (s.next == NULL) {
        s.next = solar_os_memory_calloc(1, sizeof(*s.next), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "sc2.sync.cfg");
    }
    if (s.ep == NULL) {
        s.ep = solar_os_memory_calloc(1, sizeof(*s.ep), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "sc2.sync.ep");
    }
    if (s.ob == NULL) {
        s.ob = solar_os_memory_calloc(1, sizeof(*s.ob), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "sc2.sync.ob");
    }
    return s.c != NULL && s.cfg != NULL && s.next != NULL && s.ep != NULL && s.ob != NULL;
}

esp_err_t solar_os_sc2_sync_start(void)
{
    if (s.running) {
        if (s.task != NULL) {
            xTaskNotifyGive(s.task);
        }
        return ESP_OK;
    }
    if (s.task != NULL && !s.done) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        return err;
    }
    if (!alloc_state()) {
        return ESP_ERR_NO_MEM;
    }
    s.stop = false;
    s.done = false;
    s.running = true;
    s.checked_at = 0;
    if (solar_os_task_create_pinned(worker, "sc2_sync", SOLAR_OS_SC2_SYNC_STACK, NULL, tskIDLE_PRIORITY + 1, &s.task,
                                    tskNO_AFFINITY, SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) {
        s.running = false;
        s.task = NULL;
        return ESP_ERR_NO_MEM;
    }
    (void)solar_os_jobs_note_resource("sc2-sync", SOLAR_OS_JOB_RESOURCE_NET, "SC2 relay", "sc2 messaging instance");
    return ESP_OK;
}

void solar_os_sc2_sync_stop(void)
{
    s.stop = true;
    if (s.task != NULL && !s.done) {
        xTaskNotifyGive(s.task);
    }
    if (solar_os_task_wait_done(s.task, &s.done, 8000)) {
        s.task = NULL;
    } else {
        SOLAR_OS_LOGW(TAG, "worker is still stopping");
    }
}

static void provider_changed(solar_os_messaging_provider_id_t id, bool enabled)
{
    (void)id;
    if (enabled && s.running) {
        s.checked_at = 0; /* pick up the new or changed instance now */
        (void)solar_os_sc2_sync_start();
    } else if (enabled) {
        /* Through the job manager: `job` shows sc2-sync running, and a start
         * that finds no SRAM for the worker stack is queued and retried
         * instead of being dropped. */
        esp_err_t err = solar_os_jobs_start(NULL, "sc2-sync", 0, NULL);
        if (err == ESP_ERR_NOT_FOUND) {
            err = solar_os_sc2_sync_start();
        }
        if (err != ESP_OK) {
            SOLAR_OS_LOGW(TAG, "cannot start: %s", esp_err_to_name(err));
        }
    } else if (s.running && s.task != NULL) {
        s.checked_at = 0;
        xTaskNotifyGive(s.task);
    }
}

void solar_os_sc2_sync_install(void)
{
    solar_os_sc2_provider_set_transport(provider_changed);
}

void solar_os_sc2_sync_get_status(solar_os_sc2_sync_status_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->running = s.running;
    out->connected = s.connected;
    out->ready = s.ready;
    out->instance = (unsigned)s.id;
    out->attempt = (unsigned)s.attempt;
    if (s.client_ok) {
        out->outbox = (unsigned)sc2c_outbox_count(s.c);
        snprintf(out->relay_id, sizeof(out->relay_id), "%s", s.c->relay_id);
    }
    snprintf(out->host, sizeof(out->host), "%s", s.host);
    if (s.ep != NULL) {
        out->port = s.ep->port;
        out->tls = s.ep->tls;
    }
    out->relay = s.rp.relay;
    out->relays = s.rp.relays;
    out->home = s.rp.home;
    out->visiting = s.rp.visiting;
}
