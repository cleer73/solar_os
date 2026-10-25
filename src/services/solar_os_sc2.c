/*
 * SolarOS SC2 security service: ESP-IDF glue around the SC2 session engine.
 * Platform pieces: hardware RNG mixed into a persisted HMAC pool, A/B record
 * files on internal flash, FreeRTOS mutexes, PSRAM work buffers. The SHA-256
 * and HMAC backend is mbedTLS (SC2_USE_MBEDTLS, set by the package build).
 */
#include "solar_os_sc2.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_memory.h"
#include "solar_os_storage.h"

static const char *TAG = "sc2";

#define SC2_SLOT_HEADER 12u          /* generation:u64 | crc32(generation):u32 */
#define SC2_FILE_MAX (4096u + SC2_SLOT_HEADER)
#define SC2_PATH_MAX SOLAR_OS_STORAGE_PATH_MAX
#define SC2_SEED_LEN 32u
#define SC2_MIN_WALL_CLOCK 1704067200u /* 2024-01-01: earlier means no wall clock */

typedef struct {
    SemaphoreHandle_t lock;     /* serialises the engine and its store */
    SemaphoreHandle_t rng_lock; /* serialises the entropy pool */
    sc2e_t *engine;
    bool ready;
    bool created;
    char base[SC2_PATH_MAX];
    uint8_t pool[32];
    uint64_t pool_counter;
} sc2_service_t;

static sc2_service_t s_sc2;
static portMUX_TYPE s_sc2_init_mux = portMUX_INITIALIZER_UNLOCKED;

/* ---------------------------------------------------------------------- */
/* Errors                                                                  */
/* ---------------------------------------------------------------------- */

static esp_err_t sc2_esp(int rc)
{
    return rc == SC2_OK ? ESP_OK : SOLAR_OS_SC2_ESP_ERR(rc);
}

const char *solar_os_sc2_err_text(esp_err_t err)
{
    if (err > SOLAR_OS_SC2_ERR_BASE && err <= SOLAR_OS_SC2_ERR_BASE - SC2_ERR_NOT_FOUND) {
        return sc2_strerror((int)(SOLAR_OS_SC2_ERR_BASE - err));
    }
    return esp_err_to_name(err);
}

/* ---------------------------------------------------------------------- */
/* Memory                                                                  */
/* ---------------------------------------------------------------------- */

void *solar_os_sc2_buffer_alloc(size_t len)
{
    return solar_os_memory_calloc(1, len, SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "sc2.work");
}

void solar_os_sc2_buffer_free(void *ptr, size_t len)
{
    if (ptr != NULL) {
        sc2_memzero(ptr, len);
        solar_os_memory_free(ptr);
    }
}

static void *pf_alloc(void *ctx, size_t len)
{
    (void)ctx;
    return solar_os_sc2_buffer_alloc(len);
}

static void pf_release(void *ctx, void *ptr, size_t len)
{
    (void)ctx;
    solar_os_sc2_buffer_free(ptr, len);
}

/* ---------------------------------------------------------------------- */
/* Randomness: HMAC-SHA-256 pool keyed by the hardware RNG and a seed that */
/* is carried across boots. Every output block mixes in fresh esp_fill_random */
/* bytes, so the output is never weaker than the hardware RNG alone.         */
/* ---------------------------------------------------------------------- */

static int pool_block(uint8_t out[32])
{
    uint8_t in[8 + 32 + 8];
    sc2_store_be64(in, ++s_sc2.pool_counter);
    esp_fill_random(in + 8, 32);
    sc2_store_be64(in + 40, (uint64_t)esp_timer_get_time());
    int rc = sc2_hmac_sha256(out, s_sc2.pool, sizeof(s_sc2.pool), in, sizeof(in));
    if (rc == SC2_OK) {
        /* Ratchet the pool key forward so earlier output cannot be rebuilt. */
        uint8_t next[32];
        rc = sc2_hmac_sha256(next, s_sc2.pool, sizeof(s_sc2.pool), out, 32);
        if (rc == SC2_OK) {
            memcpy(s_sc2.pool, next, sizeof(next));
        }
        sc2_memzero(next, sizeof(next));
    }
    sc2_memzero(in, sizeof(in));
    return rc;
}

static int sc2_rng(void *ctx, uint8_t *out, size_t len)
{
    (void)ctx;
    uint8_t block[32];
    int rc = SC2_OK;
    if (s_sc2.rng_lock == NULL || xSemaphoreTake(s_sc2.rng_lock, portMAX_DELAY) != pdTRUE) {
        return -1;
    }
    while (len > 0 && rc == SC2_OK) {
        rc = pool_block(block);
        size_t n = len < sizeof(block) ? len : sizeof(block);
        memcpy(out, block, n);
        out += n;
        len -= n;
    }
    xSemaphoreGive(s_sc2.rng_lock);
    sc2_memzero(block, sizeof(block));
    return rc == SC2_OK ? 0 : -1;
}

static const sc2_rng_t SC2_RNG = {sc2_rng, NULL};

static void seed_path(char *out, size_t cap)
{
    snprintf(out, cap, "%s/seed.bin", s_sc2.base);
}

/* Key the pool from the hardware RNG, the boot timer and the stored seed,
 * then immediately replace the stored seed. */
static void pool_init(void)
{
    uint8_t ikm[64 + SC2_SEED_LEN + 8];
    uint8_t seed[SC2_SEED_LEN];
    char path[SC2_PATH_MAX];
    size_t len = 0;
    esp_fill_random(ikm, 64);
    memset(ikm + 64, 0, SC2_SEED_LEN);
    seed_path(path, sizeof(path));
    if (solar_os_storage_read_file(path, ikm + 64, SC2_SEED_LEN, &len) != ESP_OK || len != SC2_SEED_LEN) {
        memset(ikm + 64, 0, SC2_SEED_LEN);
    }
    sc2_store_be64(ikm + 64 + SC2_SEED_LEN, (uint64_t)esp_timer_get_time());
    (void)sc2_hkdf(s_sc2.pool, sizeof(s_sc2.pool), (const uint8_t *)"SC2-POOL", 8, ikm, sizeof(ikm),
                   (const uint8_t *)"pool", 4);
    if (sc2_rng(NULL, seed, sizeof(seed)) == 0) {
        if (solar_os_storage_write_file(path, seed, sizeof(seed), false) != ESP_OK) {
            ESP_LOGW(TAG, "could not refresh %s", path);
        }
    }
    sc2_memzero(ikm, sizeof(ikm));
    sc2_memzero(seed, sizeof(seed));
}

/* ---------------------------------------------------------------------- */
/* Storage: each record is kept in two slot files, <name>.a and <name>.b.  */
/* A save writes the slot that does not hold the newest generation, so a   */
/* torn write always leaves the previous good copy readable.              */
/* ---------------------------------------------------------------------- */

static bool name_ok(const char *name)
{
    size_t n = strlen(name);
    if (n == 0 || n >= SC2E_NAME_MAX || name[0] == '/' || name[0] == '.') {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || (c == '/' && name[i + 1] != '/');
        if (!ok) {
            return false;
        }
    }
    return true;
}

static void slot_path(char *out, size_t cap, const char *name, char slot)
{
    snprintf(out, cap, "%s/%s.%c", s_sc2.base, name, slot);
}

/* Read one slot; returns its generation (0 when missing or invalid). */
static uint64_t slot_read(const char *name, char slot, uint8_t *file, size_t *len)
{
    char path[SC2_PATH_MAX];
    size_t n = 0;
    slot_path(path, sizeof(path), name, slot);
    if (solar_os_storage_read_file(path, file, SC2_FILE_MAX, &n) != ESP_OK || n <= SC2_SLOT_HEADER ||
        n >= SC2_FILE_MAX) {
        return 0;
    }
    uint64_t gen = sc2_load_be64(file);
    if (sc2_crc32(0, file, 8) != sc2_load_be32(file + 8)) {
        return 0;
    }
    *len = n;
    return gen;
}

static int pf_load(void *ctx, const char *name, uint8_t *buf, size_t cap, size_t *len)
{
    (void)ctx;
    if (!name_ok(name)) {
        return SC2_ERR_ARG;
    }
    uint8_t *a = solar_os_sc2_buffer_alloc(SC2_FILE_MAX);
    uint8_t *b = solar_os_sc2_buffer_alloc(SC2_FILE_MAX);
    int rc = SC2_ERR_NOT_FOUND;
    if (a == NULL || b == NULL) {
        rc = SC2_ERR_BOUNDS;
    } else {
        size_t la = 0, lb = 0;
        uint64_t ga = slot_read(name, 'a', a, &la);
        uint64_t gb = slot_read(name, 'b', b, &lb);
        /* Prefer the newest slot whose record also unwraps; fall back to the
         * other one if the newest is damaged. */
        const uint8_t *order[2] = {ga >= gb ? a : b, ga >= gb ? b : a};
        size_t lens[2] = {ga >= gb ? la : lb, ga >= gb ? lb : la};
        uint64_t gens[2] = {ga >= gb ? ga : gb, ga >= gb ? gb : ga};
        bool any = ga != 0 || gb != 0;
        for (int i = 0; i < 2 && rc != SC2_OK; i++) {
            if (gens[i] == 0) {
                continue;
            }
            const uint8_t *rec = order[i] + SC2_SLOT_HEADER;
            size_t rec_len = lens[i] - SC2_SLOT_HEADER;
            const uint8_t *payload;
            size_t payload_len;
            if (rec_len < SC2E_RECORD_OVERHEAD || rec[0] != 'S' ||
                sc2e_record_unwrap(rec, rec_len, rec[4], rec[5], &payload, &payload_len) != SC2_OK) {
                continue;
            }
            if (rec_len > cap) {
                rc = SC2_ERR_BOUNDS;
                break;
            }
            memcpy(buf, rec, rec_len);
            *len = rec_len;
            rc = SC2_OK;
            if (i == 1) {
                ESP_LOGW(TAG, "%s: newest copy damaged, using previous", name);
            }
        }
        if (rc == SC2_ERR_NOT_FOUND && any) {
            rc = SC2_ERR_FORMAT;
        }
    }
    solar_os_sc2_buffer_free(a, SC2_FILE_MAX);
    solar_os_sc2_buffer_free(b, SC2_FILE_MAX);
    return rc;
}

static int ensure_parent(const char *name)
{
    const char *slash = strchr(name, '/');
    if (slash == NULL) {
        return SC2_OK;
    }
    char dir[SC2_PATH_MAX];
    int n = snprintf(dir, sizeof(dir), "%s/%.*s", s_sc2.base, (int)(slash - name), name);
    if (n <= 0 || (size_t)n >= sizeof(dir)) {
        return SC2_ERR_BOUNDS;
    }
    return solar_os_storage_makedirs(dir, true) == ESP_OK ? SC2_OK : SC2_ERR_STATE;
}

static int pf_save(void *ctx, const char *name, const uint8_t *buf, size_t len)
{
    (void)ctx;
    if (!name_ok(name)) {
        return SC2_ERR_ARG;
    }
    if (len + SC2_SLOT_HEADER >= SC2_FILE_MAX) {
        return SC2_ERR_BOUNDS;
    }
    uint8_t *file = solar_os_sc2_buffer_alloc(SC2_FILE_MAX);
    if (file == NULL) {
        return SC2_ERR_BOUNDS;
    }
    size_t la = 0, lb = 0;
    uint64_t ga = slot_read(name, 'a', file, &la);
    uint64_t gb = slot_read(name, 'b', file, &lb);
    char slot = ga > gb ? 'b' : 'a';
    uint64_t gen = (ga > gb ? ga : gb) + 1u;
    sc2_store_be64(file, gen);
    sc2_store_be32(file + 8, sc2_crc32(0, file, 8));
    memcpy(file + SC2_SLOT_HEADER, buf, len);
    char path[SC2_PATH_MAX];
    slot_path(path, sizeof(path), name, slot);
    int rc = ensure_parent(name);
    if (rc == SC2_OK && solar_os_storage_write_file(path, file, len + SC2_SLOT_HEADER, false) != ESP_OK) {
        ESP_LOGE(TAG, "write %s failed", path);
        rc = SC2_ERR_STATE;
    }
    solar_os_sc2_buffer_free(file, SC2_FILE_MAX);
    return rc;
}

/* Overwrite a slot file in place with zeros before unlinking it. This is
 * best effort: the flash wear-levelling layer may still hold older copies of
 * the sectors until they are erased (see the manual page on keys at rest). */
static void slot_scrub(const char *path)
{
    static const uint8_t zeros[256];
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        return;
    }
    FILE *f = fopen(path, "r+b");
    if (f == NULL) {
        return;
    }
    size_t left = (size_t)st.st_size;
    while (left > 0) {
        size_t n = left < sizeof(zeros) ? left : sizeof(zeros);
        if (fwrite(zeros, 1, n, f) != n) {
            break;
        }
        left -= n;
    }
    (void)solar_os_storage_sync_file(f);
    fclose(f);
}

static int pf_remove(void *ctx, const char *name)
{
    (void)ctx;
    char path[SC2_PATH_MAX];
    bool exists = false;
    if (!name_ok(name)) {
        return SC2_ERR_ARG;
    }
    for (int i = 0; i < 2; i++) {
        slot_path(path, sizeof(path), name, i == 0 ? 'a' : 'b');
        if (solar_os_storage_exists(path, &exists) == ESP_OK && exists) {
            slot_scrub(path);
            if (solar_os_storage_remove(path) != ESP_OK) {
                return SC2_ERR_STATE;
            }
        }
    }
    return SC2_OK;
}

static int pf_count(void *ctx, const char *dir, size_t *count)
{
    (void)ctx;
    char path[SC2_PATH_MAX];
    char sibling[SC2_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", s_sc2.base, dir);
    *count = 0;
    DIR *d = opendir(path);
    if (d == NULL) {
        return SC2_OK;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t n = strlen(ent->d_name);
        if (n < 3 || ent->d_name[n - 2] != '.') {
            continue;
        }
        char slot = ent->d_name[n - 1];
        if (slot == 'a') {
            (*count)++;
        } else if (slot == 'b') {
            /* Count a .b slot only when there is no matching .a slot. */
            struct stat st;
            snprintf(sibling, sizeof(sibling), "%s/%.*sa", path, (int)(n - 1), ent->d_name);
            if (stat(sibling, &st) != 0) {
                (*count)++;
            }
        }
    }
    closedir(d);
    return SC2_OK;
}

static uint64_t pf_now(void *ctx)
{
    (void)ctx;
    time_t now = time(NULL);
    return now >= (time_t)SC2_MIN_WALL_CLOCK ? (uint64_t)now : 0u;
}

static uint64_t host_clock_us(void *ctx)
{
    (void)ctx;
    return (uint64_t)esp_timer_get_time();
}

/* ---------------------------------------------------------------------- */
/* Init                                                                    */
/* ---------------------------------------------------------------------- */

static esp_err_t sc2_locks(void)
{
    portENTER_CRITICAL(&s_sc2_init_mux);
    bool need = s_sc2.lock == NULL;
    portEXIT_CRITICAL(&s_sc2_init_mux);
    if (!need) {
        return ESP_OK;
    }
    SemaphoreHandle_t lock = xSemaphoreCreateMutex();
    SemaphoreHandle_t rng_lock = xSemaphoreCreateMutex();
    if (lock == NULL || rng_lock == NULL) {
        if (lock) {
            vSemaphoreDelete(lock);
        }
        if (rng_lock) {
            vSemaphoreDelete(rng_lock);
        }
        return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&s_sc2_init_mux);
    if (s_sc2.lock == NULL) {
        s_sc2.lock = lock;
        s_sc2.rng_lock = rng_lock;
        lock = NULL;
        rng_lock = NULL;
    }
    portEXIT_CRITICAL(&s_sc2_init_mux);
    if (lock) {
        vSemaphoreDelete(lock);
        vSemaphoreDelete(rng_lock);
    }
    return ESP_OK;
}

static esp_err_t sc2_init_locked(void)
{
    if (s_sc2.ready) {
        return ESP_OK;
    }
    if (!solar_os_storage_flash_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }
    const char *mount = solar_os_storage_flash_mount_point();
    esp_err_t err = solar_os_storage_join_path(mount, SOLAR_OS_SC2_STORE_DIR, s_sc2.base, sizeof(s_sc2.base));
    if (err != ESP_OK) {
        return err;
    }
    err = solar_os_storage_makedirs(s_sc2.base, true);
    if (err != ESP_OK) {
        return err;
    }
    pool_init();
    sc2e_t *engine = solar_os_sc2_buffer_alloc(sizeof(*engine));
    if (engine == NULL) {
        return ESP_ERR_NO_MEM;
    }
    sc2e_platform_t pf = {
        .load = pf_load,
        .save = pf_save,
        .remove = pf_remove,
        .count = pf_count,
        .alloc = pf_alloc,
        .release = pf_release,
        .now = pf_now,
        .rng = SC2_RNG,
        .ctx = NULL,
    };
    int rc = sc2e_open(engine, &pf);
    if (rc != SC2_OK) {
        ESP_LOGE(TAG, "open %s: %s", s_sc2.base, sc2_strerror(rc));
        solar_os_sc2_buffer_free(engine, sizeof(*engine));
        return sc2_esp(rc);
    }
    s_sc2.engine = engine;
    s_sc2.created = engine->created;
    s_sc2.ready = true;
    ESP_LOGI(TAG, "%s device %s (%s, hash %s)", engine->created ? "created" : "loaded", engine->self_hex,
             SC2_VERSION_STRING, sc2_hash_backend());
    return ESP_OK;
}

static esp_err_t sc2_enter(void)
{
    esp_err_t err = sc2_locks();
    if (err != ESP_OK) {
        return err;
    }
    xSemaphoreTake(s_sc2.lock, portMAX_DELAY);
    err = sc2_init_locked();
    if (err != ESP_OK) {
        xSemaphoreGive(s_sc2.lock);
    }
    return err;
}

static void sc2_leave(void)
{
    xSemaphoreGive(s_sc2.lock);
}

esp_err_t solar_os_sc2_init(void)
{
    esp_err_t err = sc2_enter();
    if (err == ESP_OK) {
        sc2_leave();
    }
    return err;
}

#define SC2_CALL(expr)                \
    do {                              \
        esp_err_t enter_ = sc2_enter(); \
        if (enter_ != ESP_OK) {       \
            return enter_;            \
        }                             \
        int rc_ = (expr);             \
        sc2_leave();                  \
        return sc2_esp(rc_);          \
    } while (0)

/* ---------------------------------------------------------------------- */
/* API                                                                     */
/* ---------------------------------------------------------------------- */

esp_err_t solar_os_sc2_get_status(solar_os_sc2_status_t *status)
{
    sc2e_identity_info_t info;
    sc2e_counts_t counts;
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(status, 0, sizeof(*status));
    status->core_version = SC2_VERSION_STRING;
    status->hash_backend = sc2_hash_backend();
    esp_err_t err = sc2_enter();
    if (err != ESP_OK) {
        return err;
    }
    int rc = sc2e_identity(s_sc2.engine, &info);
    if (rc == SC2_OK) {
        rc = sc2e_counts(s_sc2.engine, &counts);
    }
    if (rc == SC2_OK) {
        status->ready = true;
        status->created = s_sc2.created;
        memcpy(status->device_id, info.device_id, sizeof(status->device_id));
        memcpy(status->fingerprint, info.fingerprint, sizeof(status->fingerprint));
        status->spk_id = info.spk_id;
        status->spk_created = info.spk_created;
        status->opk_count = info.opk_count;
        status->peers = counts.peers;
        status->sessions = counts.sessions;
        status->rooms = counts.rooms;
        strlcpy(status->store_path, s_sc2.base, sizeof(status->store_path));
        status->work_in_psram = solar_os_memory_is_external(s_sc2.engine);
    }
    sc2_leave();
    return sc2_esp(rc);
}

esp_err_t solar_os_sc2_identity(sc2e_identity_info_t *info)
{
    SC2_CALL(sc2e_identity(s_sc2.engine, info));
}

esp_err_t solar_os_sc2_peer_info(const char *peer, sc2e_peer_info_t *info)
{
    SC2_CALL(sc2e_peer_info(s_sc2.engine, peer, info));
}

esp_err_t solar_os_sc2_safety(const char *peer, sc2e_safety_t *safety)
{
    SC2_CALL(sc2e_safety(s_sc2.engine, peer, safety));
}

esp_err_t solar_os_sc2_forget_peer(const char *peer)
{
    SC2_CALL(sc2e_forget_peer(s_sc2.engine, peer));
}

esp_err_t solar_os_sc2_keys_publish(const char *rid, uint32_t opk_target, char *out, size_t cap, size_t *out_len)
{
    SC2_CALL(sc2e_keys_publish(s_sc2.engine, rid, opk_target, out, cap, out_len));
}

esp_err_t solar_os_sc2_add_bundle(const char *line, size_t len, char device[SC2_DEVICE_ID_HEX_LEN + 1])
{
    SC2_CALL(sc2e_add_bundle(s_sc2.engine, line, len, device));
}

esp_err_t solar_os_sc2_dm_encrypt(const char *peer, const char *rid, const char *inner, size_t inner_len, char *out,
                                  size_t cap, size_t *out_len)
{
    SC2_CALL(sc2e_dm_encrypt(s_sc2.engine, peer, rid, inner, inner_len, out, cap, out_len));
}

esp_err_t solar_os_sc2_dm_decrypt(const char *line, size_t len, sc2e_dm_result_t *result, char *pt, size_t pt_cap,
                                  size_t *pt_len)
{
    SC2_CALL(sc2e_dm_decrypt(s_sc2.engine, line, len, result, pt, pt_cap, pt_len));
}

esp_err_t solar_os_sc2_room_create(const char *room, char room_out[SC2_DEVICE_ID_HEX_LEN + 1], uint32_t *epoch)
{
    SC2_CALL(sc2e_room_create(s_sc2.engine, room, room_out, epoch));
}

esp_err_t solar_os_sc2_room_rotate(const char *room, uint32_t *epoch)
{
    SC2_CALL(sc2e_room_rotate(s_sc2.engine, room, epoch));
}

esp_err_t solar_os_sc2_room_share(const char *room, const char *peer, const char *rid, char *out, size_t cap,
                                  size_t *out_len)
{
    SC2_CALL(sc2e_room_share(s_sc2.engine, room, peer, rid, out, cap, out_len));
}

esp_err_t solar_os_sc2_room_encrypt(const char *room, const char *rid, const char *inner, size_t inner_len, char *out,
                                    size_t cap, size_t *out_len)
{
    SC2_CALL(sc2e_room_encrypt(s_sc2.engine, room, rid, inner, inner_len, out, cap, out_len));
}

esp_err_t solar_os_sc2_room_decrypt(const char *line, size_t len, sc2e_room_result_t *result, char *pt, size_t pt_cap,
                                    size_t *pt_len)
{
    SC2_CALL(sc2e_room_decrypt(s_sc2.engine, line, len, result, pt, pt_cap, pt_len));
}

esp_err_t solar_os_sc2_room_info(const char *room, sc2e_room_info_t *info)
{
    SC2_CALL(sc2e_room_info(s_sc2.engine, room, info));
}

esp_err_t solar_os_sc2_room_forget(const char *room)
{
    SC2_CALL(sc2e_room_forget(s_sc2.engine, room));
}

esp_err_t solar_os_sc2_post_sign(const char *relay_id, const char *room, const char *name, const char *body,
                                 size_t body_len, uint64_t ts, const char *ref, bool deleted, const char *rid,
                                 char *out, size_t cap, size_t *out_len)
{
    SC2_CALL(sc2e_post_sign(s_sc2.engine, relay_id, room, name, body, body_len, ts, ref, deleted, rid, out, cap,
                            out_len));
}

esp_err_t solar_os_sc2_post_verify(const char *relay_id, const char *line, size_t len, size_t max_body,
                                   sc2e_post_result_t *result, char *body, size_t body_cap)
{
    return sc2_esp(sc2e_post_verify(relay_id, line, len, max_body, result, body, body_cap));
}

static sc2e_test_host_t test_host(void)
{
    sc2e_test_host_t host = {
        .alloc = pf_alloc,
        .release = pf_release,
        .clock_us = host_clock_us,
        .rng = SC2_RNG,
        .ctx = NULL,
    };
    return host;
}

esp_err_t solar_os_sc2_selftest(sc2e_report_fn report, void *ctx, unsigned *passed, unsigned *failed)
{
    /* The selftest uses its own in-memory devices; only the RNG pool is
     * shared, and it has its own lock. It still needs the pool keyed. */
    esp_err_t err = solar_os_sc2_init();
    if (err != ESP_OK) {
        return err;
    }
    sc2e_test_host_t host = test_host();
    return sc2_esp(sc2e_selftest(&host, report, ctx, passed, failed));
}

esp_err_t solar_os_sc2_bench(uint32_t iterations, sc2e_bench_fn report, void *ctx)
{
    esp_err_t err = solar_os_sc2_init();
    if (err != ESP_OK) {
        return err;
    }
    sc2e_test_host_t host = test_host();
    return sc2_esp(sc2e_bench(&host, iterations, report, ctx));
}
