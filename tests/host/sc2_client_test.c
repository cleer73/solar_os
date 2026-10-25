/*
 * Host tests for the device-side relay client (src/services/solar_os_sc2_client.c)
 * against the portable mini-relay (protocol/sc2 sc2_minirelay), both in
 * process: how the client treats a session the relay closes on purpose.
 * SPDX-License-Identifier: MIT
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sc2_minirelay.h"
#include "sc2_test_util.h"
#include "solar_os_sc2_client.h"
#include "solar_os_sc2_engine.h"
#include "solar_os_sc2_selftest.h"

/* ---------------- clock, memory ---------------- */

static uint64_t g_ms = 1000u;
static uint64_t g_s = 1700000000u;

static void *t_alloc(void *ctx, size_t len)
{
    (void)ctx;
    return calloc(1, len);
}

static void t_release(void *ctx, void *ptr, size_t len)
{
    (void)ctx;
    if (ptr != NULL) {
        sc2_memzero(ptr, len);
        free(ptr);
    }
}

static uint64_t t_clock_us(void *ctx)
{
    (void)ctx;
    return g_ms * 1000u;
}

/* ---------------- line queues (no re-entry between the two sides) ---------------- */

typedef struct line {
    struct line *next;
    size_t len;
    char s[];
} line_t;

typedef struct {
    line_t *head, *tail;
} queue_t;

static void q_push(queue_t *q, const char *s, size_t len)
{
    line_t *l = malloc(sizeof(*l) + len + 1u);
    if (l == NULL) {
        exit(2);
    }
    l->next = NULL;
    l->len = len;
    memcpy(l->s, s, len);
    l->s[len] = '\0';
    if (q->tail != NULL) {
        q->tail->next = l;
    } else {
        q->head = l;
    }
    q->tail = l;
}

static line_t *q_pop(queue_t *q)
{
    line_t *l = q->head;
    if (l != NULL) {
        q->head = l->next;
        if (q->head == NULL) {
            q->tail = NULL;
        }
    }
    return l;
}

static void q_clear(queue_t *q)
{
    line_t *l;
    while ((l = q_pop(q)) != NULL) {
        free(l);
    }
}

/* ---------------- mini-relay ---------------- */

#define NSESS 4u
static sc2_mr_t *mr;
static queue_t to_client[NSESS];

static int mr_send(void *ctx, unsigned s, const char *line, size_t len)
{
    (void)ctx;
    q_push(&to_client[s], line, len);
    return 0;
}

static uint64_t mr_now(void *ctx)
{
    (void)ctx;
    return g_s;
}

/* ---------------- devices ---------------- */

typedef struct {
    const char *name;
    sc2e_memstore_t *store;
    sc2e_t e;
    sc2e_t *engine; /* own engine, or another device's (same identity) */
    sc2c_t c;
    int sess;       /* mini-relay session, -1 offline */
    queue_t to_relay;
    int notices, last_notice;
} tdev_t;

static int d_send(void *ctx, const char *line, size_t len)
{
    tdev_t *d = ctx;
    if (d->sess < 0) {
        return -1;
    }
    q_push(&d->to_relay, line, len);
    return 0;
}

static void d_event(void *ctx, const sc2c_event_t *ev)
{
    tdev_t *d = ctx;
    if (ev->type == SC2C_EV_NOTICE) {
        d->notices++;
        d->last_notice = ev->code;
    }
}

static sc2e_t *d_lock(void *ctx)
{
    return ((tdev_t *)ctx)->engine;
}

static void d_unlock(void *ctx)
{
    (void)ctx;
}

static uint64_t d_mono(void *ctx)
{
    (void)ctx;
    return g_ms;
}

static uint64_t d_wall(void *ctx)
{
    (void)ctx;
    return g_s;
}

static const sc2e_test_host_t TH = {t_alloc, t_release, t_clock_us, {sc2t_xs_fn, NULL}, NULL};

static void d_init(tdev_t *d, const char *name, const char *tag, tdev_t *same_identity)
{
    memset(d, 0, sizeof(*d));
    d->name = name;
    d->sess = -1;
    if (same_identity != NULL) {
        d->engine = same_identity->engine;
    } else {
        sc2e_platform_t pf;
        CHECK_RC(sc2e_memstore_create(&d->store, &TH), SC2_OK);
        sc2e_memstore_platform(d->store, &pf);
        CHECK_RC(sc2e_open(&d->e, &pf), SC2_OK);
        d->engine = &d->e;
    }
    sc2c_host_t h = {d_send, d_event, d_lock, d_unlock, t_alloc, t_release, d_mono, d_wall, d};
    sc2c_config_t cfg = {.tag = tag, .nick = name, .expect_relay = "mr.test"};
    CHECK_RC(sc2c_init(&d->c, &h, &cfg), SC2_OK);
}

static void d_free(tdev_t *d)
{
    sc2c_free(&d->c);
    q_clear(&d->to_relay);
    if (d->store != NULL) {
        sc2e_close(&d->e);
        sc2e_memstore_destroy(d->store);
    }
}

static tdev_t *devs[4];
static unsigned ndevs;

static tdev_t *dev_on(unsigned s)
{
    for (unsigned i = 0; i < ndevs; i++) {
        if (devs[i]->sess == (int)s) {
            return devs[i];
        }
    }
    return NULL;
}

/* The host side of a closed connection, as sc2-sync and chatd do it. */
static void disconnect(tdev_t *d)
{
    if (d->sess < 0) {
        return;
    }
    unsigned s = (unsigned)d->sess;
    sc2_mr_close(mr, s);
    q_clear(&to_client[s]);
    q_clear(&d->to_relay);
    d->sess = -1;
    sc2c_on_disconnected(&d->c);
}

/* Deliver queued lines both ways until nothing moves. */
static void pump(void)
{
    for (int round = 0; round < 10000; round++) {
        bool moved = false;
        for (unsigned i = 0; i < ndevs; i++) {
            tdev_t *d = devs[i];
            line_t *l;
            while (d->sess >= 0 && (l = q_pop(&d->to_relay)) != NULL) {
                int rc = sc2_mr_line(mr, (unsigned)d->sess, l->s, l->len);
                free(l);
                moved = true;
                if (rc == SC2_MR_CLOSE) {
                    disconnect(d);
                }
            }
        }
        for (unsigned s = 0; s < NSESS; s++) {
            line_t *l;
            while ((l = q_pop(&to_client[s])) != NULL) {
                tdev_t *d = dev_on(s);
                moved = true;
                if (d != NULL) {
                    char *nl = malloc(l->len + 1u);
                    memcpy(nl, l->s, l->len);
                    nl[l->len] = '\n';
                    if (sc2c_feed(&d->c, (const uint8_t *)nl, l->len + 1u) != SC2_OK) {
                        disconnect(d);
                    }
                    free(nl);
                }
                free(l);
            }
        }
        if (!moved) {
            return;
        }
    }
    fprintf(stderr, "pump: no quiet point\n");
    exit(2);
}

static void connect_to(tdev_t *d, unsigned s)
{
    sc2_mr_open(mr, s);
    d->sess = (int)s;
    CHECK_RC(sc2c_on_connected(&d->c), SC2_OK);
    pump();
}

static void feed_line(tdev_t *d, const char *line)
{
    char buf[512];
    int n = snprintf(buf, sizeof(buf), "%s\n", line);
    CHECK_RC(sc2c_feed(&d->c, (const uint8_t *)buf, (size_t)n), SC2_OK);
}

/* ---------------- tests ---------------- */

/* Close reasons the relay announces with an error frame (no rid). */
static void announced_closes(tdev_t *a)
{
    static const struct {
        const char *frame;
        sc2c_close_t want;
    } cases[] = {
        {"{\"type\":\"error\",\"code\":408,\"err\":\"idle\",\"text\":\"closed after a period without activity\"}",
         SC2C_CLOSE_IDLE},
        {"{\"type\":\"error\",\"code\":503,\"err\":\"reconnect\",\"text\":\"session lifetime reached\"}",
         SC2C_CLOSE_RECONNECT},
        {"{\"type\":\"error\",\"code\":503,\"err\":\"evicted\",\"text\":\"relay full\"}", SC2C_CLOSE_EVICTED},
        /* a full relay is a failure: backoff and failover as before */
        {"{\"type\":\"error\",\"code\":503,\"err\":\"server_full\",\"text\":\"server full\"}", SC2C_CLOSE_NONE},
        /* the answer to one request never ends the session */
        {"{\"type\":\"error\",\"rid\":\"o1\",\"code\":503,\"err\":\"evicted\",\"text\":\"x\"}", SC2C_CLOSE_NONE},
        {"{\"type\":\"error\",\"code\":409,\"err\":\"epoch_conflict\",\"text\":\"x\"}", SC2C_CLOSE_NONE},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        connect_to(a, 0);
        CHECK(sc2c_ready(&a->c));
        CHECK(sc2c_close_reason(&a->c) == SC2C_CLOSE_NONE);
        int notices = a->notices;
        feed_line(a, cases[i].frame);
        CHECK(a->notices == notices + 1);
        CHECK(sc2c_close_reason(&a->c) == cases[i].want);
        disconnect(a);
        /* still readable after the disconnect, for the reconnect decision */
        CHECK(sc2c_close_reason(&a->c) == cases[i].want);
        CHECK(sc2c_close_soft(cases[i].want) == (cases[i].want != SC2C_CLOSE_NONE));
    }
    /* during login a refusal is never a soft close */
    sc2_mr_open(mr, 0);
    a->sess = 0;
    CHECK_RC(sc2c_on_connected(&a->c), SC2_OK);
    CHECK(sc2c_close_reason(&a->c) == SC2C_CLOSE_NONE); /* cleared on connect */
    feed_line(a, "{\"type\":\"error\",\"code\":408,\"err\":\"idle\",\"text\":\"x\"}");
    CHECK(sc2c_close_reason(&a->c) == SC2C_CLOSE_NONE);
    disconnect(a);
}

static void reconnect_delays(tdev_t *a)
{
    /* soft: a short pause, the attempt counter is not touched */
    connect_to(a, 0);
    feed_line(a, "{\"type\":\"error\",\"code\":408,\"err\":\"idle\",\"text\":\"x\"}");
    disconnect(a);
    uint32_t attempt = 3;
    for (uint32_t r = 0; r < 5000; r += 97) {
        uint32_t ms = sc2c_reconnect_ms(&a->c, &attempt, r);
        CHECK(ms >= SC2C_SOFT_RECONNECT_MS && ms <= SC2C_SOFT_RECONNECT_MS + 2000u);
    }
    CHECK(attempt == 3);
    /* a plain drop: the exponential backoff, counted */
    connect_to(a, 0);
    disconnect(a);
    attempt = 3;
    uint32_t ms = sc2c_reconnect_ms(&a->c, &attempt, 0);
    CHECK(attempt == 4 && ms >= 6000u && ms <= 10000u);
}

int main(void)
{
    uint8_t seed[32] = {9};
    sc2_mr_config_t cfg = {.relay_id = "mr.test", .seed = seed, .rng = {sc2t_xs_fn, NULL}, .now_s = mr_now,
                           .send = mr_send, .alloc = t_alloc, .release = t_release, .sessions = NSESS};
    CHECK_RC(sc2_mr_init(&mr, &cfg), SC2_OK);

    static tdev_t a;
    d_init(&a, "alice", "a", NULL);
    devs[ndevs++] = &a;

    announced_closes(&a);
    reconnect_delays(&a);

    d_free(&a);
    for (unsigned s = 0; s < NSESS; s++) {
        q_clear(&to_client[s]);
    }
    sc2_mr_free(mr);
    return sc2t_report("sc2_client_test: announced closes, reconnect delays");
}
