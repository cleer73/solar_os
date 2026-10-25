/* Host tests for the portable mini-relay (protocol/sc2 sc2_minirelay):
 * caps and refusals that the TCP tests of the solar_os_sc2-relay repository
 * do not reach.
 * SPDX-License-Identifier: MIT */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sc2.h"
#include "sc2_minirelay.h"
#include "sc2_test_util.h"

static char g_last[SC2_MR_SESSIONS_MAX][8192];
static int g_count[SC2_MR_SESSIONS_MAX];
static int tx(void *ctx, unsigned s, const char *line, size_t len)
{
    (void)ctx;
    snprintf(g_last[s], sizeof(g_last[s]), "%.*s", (int)len, line);
    g_count[s]++;
    return 0;
}
static uint64_t now_s(void *ctx) { (void)ctx; return 1700000000u; }
static void *al(void *ctx, size_t n) { (void)ctx; return calloc(1, n); }
static void rl(void *ctx, void *p, size_t n) { (void)ctx; (void)n; free(p); }
static const sc2_rng_t RNG = {sc2t_xs_fn, NULL};
static sc2_mr_t *mr;
static char buf[8192];

static int L(unsigned s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static int L(unsigned s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return sc2_mr_line(mr, s, buf, (size_t)n);
}
static bool has(unsigned s, const char *needle) { return strstr(g_last[s], needle) != NULL; }

static void b64(char *out, size_t cap, const uint8_t *in, size_t n)
{
    size_t w = 0;
    (void)sc2_b64_encode(out, cap, &w, in, n);
}

/* hello + auth for a fresh identity on session s */
static void login(unsigned s, sc2_identity_t *id, char hex[33], const char *admit)
{
    CHECK_RC(sc2_identity_generate(id, &RNG), SC2_OK);
    sc2_mr_open(mr, s);
    CHECK_RC(admit ? L(s, "{\"type\":\"hello\",\"v\":2,\"proto\":\"sc2\",\"admit\":\"%s\"}", admit)
                   : L(s, "{\"type\":\"hello\",\"v\":2,\"proto\":\"sc2\"}"), SC2_MR_OK);
    CHECK(has(s, "\"type\":\"welcome\""));
    sc2_welcome_t w;
    CHECK_RC(sc2_frame_welcome_decode(g_last[s], strlen(g_last[s]), &w), SC2_OK);
    uint8_t sig[SC2_SIG_LEN];
    CHECK_RC(sc2_auth_sign(sig, id, w.relay_id, strlen(w.relay_id), w.challenge), SC2_OK);
    size_t n = 0;
    CHECK_RC(sc2_frame_auth(buf, sizeof(buf), &n, "a", id->ik_pub, sig, false), SC2_OK);
    (void)sc2_mr_line(mr, s, buf, n);
    CHECK_RC(sc2_device_id_hex(hex, id->ik_pub), SC2_OK);
}

static void publish(unsigned s, const sc2_identity_t *id, unsigned opks, uint32_t first)
{
    sc2_spk_t spk;
    CHECK_RC(sc2_spk_generate(&spk, 7, id, &RNG), SC2_OK);
    char dk[48], dks[96], sp[48], sps[96];
    b64(dk, sizeof(dk), id->dk_pub, 32);
    b64(dks, sizeof(dks), id->dk_sig, 64);
    b64(sp, sizeof(sp), spk.pub, 32);
    b64(sps, sizeof(sps), spk.sig, 64);
    int n = snprintf(buf, sizeof(buf),
                     "{\"type\":\"keys.publish\",\"rid\":\"kp\",\"dk\":\"%s\",\"dk_sig\":\"%s\",\"spk\":{\"id\":7,"
                     "\"pub\":\"%s\",\"sig\":\"%s\"},\"opks\":[", dk, dks, sp, sps);
    for (unsigned i = 0; i < opks; i++) {
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%s{\"id\":%u,\"pub\":\"%s\"}", i ? "," : "",
                      (unsigned)(first + i), sp);
    }
    n += snprintf(buf + n, sizeof(buf) - (size_t)n, "]}");
    CHECK_RC(sc2_mr_line(mr, s, buf, (size_t)n), SC2_MR_OK);
}

static int dm(unsigned s, const char *from, const char *to, unsigned msg_n)
{
    uint8_t rk[32] = {1}, nonce[24] = {2}, ct[272] = {3};
    char rkb[48], nb[40], cb[400];
    b64(rkb, sizeof(rkb), rk, 32);
    b64(nb, sizeof(nb), nonce, 24);
    b64(cb, sizeof(cb), ct, sizeof(ct));
    return L(s, "{\"type\":\"dm.send\",\"rid\":\"o\",\"to\":\"%s\",\"env\":{\"s\":1,\"from\":\"%s\",\"rk\":\"%s\","
                "\"pn\":0,\"n\":%u,\"nonce\":\"%s\",\"ct\":\"%s\"}}", to, from, rkb, msg_n, nb, cb);
}

int main(void)
{
    uint8_t seed[32] = {9};
    sc2_mr_config_t cfg = {.relay_id = "mr.test", .seed = seed, .rng = RNG, .now_s = now_s, .send = tx,
                           .alloc = al, .release = rl, .admit = "code", .legacy_v1 = true, .sessions = 4};
    CHECK_RC(sc2_mr_init(&mr, &cfg), SC2_OK);
    printf("store %zu bytes\n", sc2_mr_size());
    CHECK(sc2_mr_size() < 200u * 1024u);

    /* bad configs */
    sc2_mr_t *x = NULL;
    sc2_mr_config_t bad = cfg;
    bad.relay_id = "bad id\"";
    CHECK_RC(sc2_mr_init(&x, &bad), SC2_ERR_ARG);
    bad = cfg;
    bad.room = "pub:Upper";
    CHECK_RC(sc2_mr_init(&x, &bad), SC2_ERR_ARG);
    bad = cfg;
    bad.sessions = SC2_MR_SESSIONS_MAX + 1u;
    CHECK_RC(sc2_mr_init(&x, &bad), SC2_ERR_ARG);

    /* v1 hand-off, hello first, junk */
    sc2_mr_open(mr, 0);
    CHECK_RC(L(0, "{\"type\":\"hello\",\"user\":\"u\",\"token\":\"t\"}"), SC2_MR_LEGACY);
    CHECK(!sc2_mr_active(mr, 0));
    CHECK_RC(L(0, "{\"type\":\"sync\"}"), SC2_MR_CLOSE); /* not ours any more */
    sc2_mr_open(mr, 0);
    CHECK_RC(L(0, "{\"type\":\"sync\"}"), SC2_MR_CLOSE);
    CHECK(has(0, "\"code\":401"));
    sc2_mr_open(mr, 0);
    CHECK_RC(L(0, "not json"), SC2_MR_CLOSE);
    sc2_mr_open(mr, 0);
    CHECK_RC(L(0, "{\"type\":\"hello\",\"v\":2,\"proto\":\"sc2\",\"rid\":\"a\\\"b\"}"), SC2_MR_CLOSE);

    /* admission */
    sc2_identity_t a, b;
    char ah[33], bh[33];
    login(0, &a, ah, NULL);
    CHECK(has(0, "admit_required"));
    login(0, &a, ah, "code");
    CHECK(has(0, "\"type\":\"authed\"") && has(0, "\"new\":true"));
    login(1, &b, bh, "code");
    CHECK(has(1, "\"type\":\"authed\""));

    /* keys: 10 published, 8 kept, each handed out once, then 410 */
    publish(0, &a, 10, 100);
    CHECK(has(0, "\"opks\":8"));
    publish(0, &a, 2, 100); /* same ids again: updated, not added */
    CHECK(has(0, "\"opks\":8"));
    for (unsigned i = 0; i < 8; i++) {
        char want[32];
        snprintf(want, sizeof(want), "\"opk\":{\"id\":%u,", 100u + i);
        g_count[1] = 0;
        L(1, "{\"type\":\"keys.claim\",\"rid\":\"k\",\"device\":\"%s\"}", ah);
        CHECK(g_count[1] == 1 && has(1, want)); /* lowest id first, never twice */
    }
    L(1, "{\"type\":\"keys.claim\",\"rid\":\"k\",\"device\":\"%s\"}", ah);
    CHECK(has(1, "prekey_exhausted"));
    /* a bundle without a valid spk signature is refused */
    CHECK_RC(L(0, "{\"type\":\"keys.publish\",\"dk\":\"%s\"}", "AAAA"), SC2_MR_OK);
    CHECK(has(0, "\"code\":400"));

    /* mailbox: forged from, cap per device, dedup, ack frees */
    CHECK_RC(dm(1, ah, ah, 0), SC2_MR_OK);
    CHECK(has(1, "\"code\":403"));
    CHECK_RC(dm(1, bh, "0123456789abcdef0123456789abcdef", 0), SC2_MR_OK);
    CHECK(has(1, "unknown_device"));
    for (unsigned i = 0; i < SC2_MR_INBOX_PER_DEVICE; i++) {
        CHECK_RC(dm(1, bh, ah, i), SC2_MR_OK);
        CHECK(has(1, "\"type\":\"ack\""));
    }
    CHECK_RC(dm(1, bh, ah, 3), SC2_MR_OK); /* resend: same seq */
    CHECK(has(1, "\"seq\":\"4\""));
    CHECK_RC(dm(1, bh, ah, 99), SC2_MR_OK);
    CHECK(has(1, "inbox_full"));
    sc2_mr_stats_t st;
    sc2_mr_stats(mr, &st);
    CHECK(st.inbox == SC2_MR_INBOX_PER_DEVICE && st.devices == 2 && st.dm_total == SC2_MR_INBOX_PER_DEVICE);
    /* a: sync gets them in order, ack frees them */
    g_count[0] = 0;
    L(0, "{\"type\":\"sync\",\"rid\":\"y\",\"inbox\":\"0\",\"max\":5}");
    CHECK(g_count[0] == 6 && has(0, "\"more\":true") && has(0, "\"inbox\":\"5\""));
    L(0, "{\"type\":\"ack\",\"inbox\":\"5\"}");
    sc2_mr_stats(mr, &st);
    CHECK(st.inbox == SC2_MR_INBOX_PER_DEVICE - 5u);
    CHECK_RC(dm(1, bh, ah, 99), SC2_MR_OK);
    CHECK(has(1, "\"seq\":\"13\""));

    /* private rooms and moderation are not here */
    L(0, "{\"type\":\"room.create\",\"rid\":\"c\",\"room\":\"00112233445566778899aabbccddeeff\"}");
    CHECK(has(0, "not_supported"));
    /* post before join */
    L(0, "{\"type\":\"room.post\",\"rid\":\"p\",\"room\":\"pub:general\",\"post\":{}}");
    CHECK(has(0, "not_member"));

    sc2_mr_close(mr, 0);
    sc2_mr_close(mr, 1);
    sc2_mr_free(mr);
    return sc2t_report("sc2_minirelay_test: handshake, admission, keys, mailbox caps");
}
