#include "solar_os_shell_commands.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_sc2.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"

static void sc2_usage(solar_os_shell_io_t *io)
{
    solar_os_shell_io_writeln(io, "sc2 id                show this device's SC2 identity");
    solar_os_shell_io_writeln(io, "sc2 status            service, store and session counts");
    solar_os_shell_io_writeln(io, "sc2 safety <device>   safety number with a stored peer");
    solar_os_shell_io_writeln(io, "sc2 selftest          run known-answer and session tests");
    solar_os_shell_io_writeln(io, "sc2 bench [n]         time the primitives (n iterations, default 10)");
}

static void sc2_fail(solar_os_shell_io_t *io, esp_err_t err)
{
    solar_os_shell_io_printf(io, "sc2: %s\n", solar_os_sc2_err_text(err));
}

static void sc2_print_identity(solar_os_shell_io_t *io)
{
    sc2e_identity_info_t info;
    esp_err_t err = solar_os_sc2_identity(&info);
    if (err != ESP_OK) {
        sc2_fail(io, err);
        return;
    }
    solar_os_shell_io_printf(io, "device:      %s\n", info.device_id);
    solar_os_shell_io_printf(io, "fingerprint: %s\n", info.fingerprint);
    solar_os_shell_io_printf(io, "identity:    %s\n", info.ik_b64);
    solar_os_shell_io_printf(io, "device key:  %s\n", info.dk_b64);
    solar_os_shell_io_printf(io, "prekeys:     spk %" PRIu32 ", %" PRIu32 " one-time\n", info.spk_id, info.opk_count);
    solar_os_shell_io_writeln(io, "Keys are stored unencrypted on internal flash unless flash encryption is on.");
}

static void sc2_print_status(solar_os_shell_io_t *io)
{
    solar_os_sc2_status_t st;
    esp_err_t err = solar_os_sc2_get_status(&st);
    if (err != ESP_OK) {
        solar_os_shell_io_printf(io, "core:    %s, hash %s\n", st.core_version ? st.core_version : SC2_VERSION_STRING,
                                 st.hash_backend ? st.hash_backend : sc2_hash_backend());
        sc2_fail(io, err);
        return;
    }
    solar_os_shell_io_printf(io, "core:    %s, hash %s\n", st.core_version, st.hash_backend);
    solar_os_shell_io_printf(io, "device:  %s%s\n", st.device_id, st.created ? " (created this boot)" : "");
    solar_os_shell_io_printf(io, "store:   %s\n", st.store_path);
    solar_os_shell_io_printf(io, "memory:  work buffers in %s\n", st.work_in_psram ? "PSRAM" : "internal RAM");
    solar_os_shell_io_printf(io, "prekeys: spk %" PRIu32 ", %" PRIu32 " one-time\n", st.spk_id, st.opk_count);
    solar_os_shell_io_printf(io, "state:   %u peers, %u sessions, %u rooms\n", (unsigned)st.peers,
                             (unsigned)st.sessions, (unsigned)st.rooms);
}

static void sc2_print_safety(solar_os_shell_io_t *io, const char *peer)
{
    sc2e_safety_t safety;
    esp_err_t err = solar_os_sc2_safety(peer, &safety);
    if (err != ESP_OK) {
        sc2_fail(io, err);
        return;
    }
    solar_os_shell_io_printf(io, "short: %s\n", safety.short_form);
    solar_os_shell_io_printf(io, "words: %s\n", safety.words);
    solar_os_shell_io_printf(io, "full:  %s\n", safety.full_form);
}

static void sc2_selftest_line(void *ctx, const char *name, int rc)
{
    solar_os_shell_io_t *io = ctx;
    solar_os_shell_io_printf(io, "  %-20s %s\n", name, rc == SC2_OK ? "ok" : sc2_strerror(rc));
}

static void sc2_bench_line(void *ctx, const sc2e_bench_result_t *r)
{
    solar_os_shell_io_t *io = ctx;
    if (r->rc != SC2_OK || r->iterations == 0) {
        solar_os_shell_io_printf(io, "  %-24s failed: %s\n", r->name, sc2_strerror(r->rc));
        return;
    }
    uint64_t per = r->total_us / r->iterations;
    solar_os_shell_io_printf(io, "  %-24s %6" PRIu32 " x %8" PRIu64 " us  (total %" PRIu64 " ms)\n", r->name,
                             r->iterations, per, r->total_us / 1000u);
}

void solar_os_shell_cmd_sc2(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (!io) {
        return;
    }
    if (argc < 2 || strcmp(argv[1], "help") == 0) {
        sc2_usage(io);
        return;
    }
    if (argc == 2 && strcmp(argv[1], "id") == 0) {
        sc2_print_identity(io);
        return;
    }
    if (argc == 2 && strcmp(argv[1], "status") == 0) {
        sc2_print_status(io);
        return;
    }
    if (argc == 3 && strcmp(argv[1], "safety") == 0) {
        sc2_print_safety(io, argv[2]);
        return;
    }
    if (argc == 2 && strcmp(argv[1], "selftest") == 0) {
        unsigned passed = 0, failed = 0;
        solar_os_shell_io_printf(io, "%s, hash %s\n", SC2_VERSION_STRING, sc2_hash_backend());
        esp_err_t err = solar_os_sc2_selftest(sc2_selftest_line, io, &passed, &failed);
        solar_os_shell_io_printf(io, "selftest: %u passed, %u failed\n", passed, failed);
        if (err != ESP_OK && failed == 0) {
            sc2_fail(io, err);
        }
        return;
    }
    if ((argc == 2 || argc == 3) && strcmp(argv[1], "bench") == 0) {
        uint32_t n = 10;
        if (argc == 3) {
            char *end = NULL;
            unsigned long v = strtoul(argv[2], &end, 10);
            if (argv[2][0] == '\0' || *end != '\0' || v == 0 || v > 1000) {
                solar_os_shell_io_writeln(io, "sc2: iterations must be 1..1000");
                return;
            }
            n = (uint32_t)v;
        }
        solar_os_shell_io_printf(io, "%s, hash %s, %" PRIu32 " iterations\n", SC2_VERSION_STRING,
                                 sc2_hash_backend(), n);
        esp_err_t err = solar_os_sc2_bench(n, sc2_bench_line, io);
        if (err != ESP_OK) {
            sc2_fail(io, err);
        }
        return;
    }
    sc2_usage(io);
}
