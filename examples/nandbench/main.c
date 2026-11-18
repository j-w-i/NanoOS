/* NAND driver benchmark (read-only). Mounts the volume four ways in one boot and compares:
 *   OLD  copy-out reads (nand_inspect_ppn_page), 4-sector survey: the pre-v25 path
 *   D4   v25 direct-DMA reads, 4-sector survey
 *   D1   v25 direct-DMA reads, 1-sector survey (the new default)
 *   ADOPT the kernel's own boot mount, handed off (what nano_fs_mount now does)
 * Every map must be identical to OLD. Then the same files are read through OLD and D1
 * disk paths (throughput + content hash), and single page reads are timed per method.
 * No program/erase call is reachable from this app. */
#include "nano_app.h"
#include "nano_fs.h"
#include "report.h"
#include "storage/game.h"
#include "storage/nano_game.h"
#include "handoff.h"
#include <stdlib.h>
#include <string.h>

static char report[8192];
static unsigned rused;
static void rtext(const char *s) { while (*s && rused + 1 < sizeof report) report[rused++] = *s++; }
static void rword(uint32_t x) {
    if (rused + 10 >= sizeof report) return;
    static const char h[] = "0123456789abcdef";
    for (unsigned i = 0; i < 8; ++i) report[rused++] = h[(x >> (28 - i * 4)) & 15u];
    report[rused++] = ' ';
}
static void rdec(uint32_t x) {
    char b[12]; int n = 0;
    do { b[n++] = (char)('0' + x % 10u); x /= 10u; } while (x && n < 11);
    while (n && rused + 2 < sizeof report) report[rused++] = b[--n];
    if (rused + 1 < sizeof report) report[rused++] = ' ';
}

static const struct nanoos_api *A;
static uint32_t clock_us(void *p) { (void)p; return A->usec(); }
static int inspect(void *p, uint32_t ce, uint32_t row, void *d, uint32_t *r) { (void)p; return A->nand_inspect_ppn_page(ce, row, d, r); }
static int context(void *p, unsigned b, unsigned pg, unsigned v, unsigned n, void *d, uint32_t *r) { (void)p; return A->nand_read_ppn_context(b, pg, v, n, d, r); }
static int direct(void *p, uint32_t ce, uint32_t row, unsigned n, void *d, uint32_t *r) { (void)p; return A->nand_read_ppn_direct(ce, row, n, d, r); }

static uint32_t mix(uint32_t s, const void *p, uint32_t n) {
    const uint8_t *b = p;
    for (uint32_t i = 0; i < n; ++i) { s ^= b[i]; s *= 0x01000193u; }
    return s;
}
/* Digest of everything a mount produces that later reads/writes depend on. */
static uint32_t map_digest(const struct ns_game *g) {
    const struct ns_mount *m = &g->mount;
    uint32_t s = 0x811c9dc5u;
    s = mix(s, &m->map.sectors, 4); s = mix(s, &m->map.nruns, 4); s = mix(s, &m->map.nupdates, 4);
    s = mix(s, &m->map.checkpoint_generation, 4); s = mix(s, &m->map.checkpoint_key, 4);
    s = mix(s, &m->root, 4); s = mix(s, &m->open_block, 4); s = mix(s, &m->open_count, 4);
    s = mix(s, &m->ready_block, 4); s = mix(s, &m->map.read_only, 4);
    s = mix(s, m->map.runs, m->map.nruns * (uint32_t)sizeof(struct ns_run));
    s = mix(s, m->map.updates, m->map.nupdates * (uint32_t)sizeof(struct ns_update));
    s = mix(s, g->mask, sizeof g->mask); s = mix(s, g->states, sizeof g->states);
    return s;
}
static struct ns_game *new_game(void) {
    uint8_t *p = malloc(sizeof(struct ns_game) + 64);
    return p ? (struct ns_game *)(void *)(((uintptr_t)p + 63u) & ~(uintptr_t)63u) : 0;
}

static const char *const files[] = {
    "/Apps/Data/OpenLara/DATA/LEVEL2.PHD", "/Apps/Data/OpenLara/game.bin",
    "/Apps/Data/Shmup/data/cameraPath/act1.cp.cp2b", "/Apps/Data/Shmup/game.bin",
};
#define NFILES (sizeof files / sizeof files[0])
static uint32_t file_hash[NFILES], file_size[NFILES];

/* Reads each file whole in 64 KiB pieces; returns total bytes, time in *us. */
static uint32_t read_files(struct ns_game *g, uint8_t *buf, uint32_t *us, int record, int *mismatch) {
    uint32_t total = 0, spent = 0;
    for (unsigned i = 0; i < NFILES; ++i) {
        struct ns_file f;
        if (ns_game_open(g, files[i], &f)) { if (record) file_size[i] = 0xffffffffu; continue; }
        uint32_t size = (uint32_t)f.fork.size, h = 0x811c9dc5u, pos = 0;
        if (size > 8u << 20) size = 8u << 20;
        while (pos < size) {
            uint32_t n = size - pos, got = 0;
            if (n > 65536u) n = 65536u;
            uint32_t t0 = A->usec();
            int rc = ns_game_read(g, &f, pos, buf, n, &got);
            spent += A->usec() - t0;
            if (rc || !got) break;
            h = mix(h, buf, got); pos += got;
        }
        total += pos;
        if (record) { file_hash[i] = h; file_size[i] = pos; }
        else if (file_hash[i] != h || file_size[i] != pos) *mismatch = 1;
    }
    *us = spent;
    return total;
}

static void mount_line(const char *name, struct ns_game *g, int rc, uint32_t us) {
    const struct ns_mount *m = &g->mount;
    rtext(name); rtext(" rc "); rword((uint32_t)rc); rtext("us "); rdec(us);
    rtext("stages "); for (unsigned i = 0; i < 6; ++i) rdec(m->stage_us[i]);
    rtext("reads "); rdec(m->reads); rtext("erased "); rdec(m->erased); rtext("replay "); rdec(m->replay_pages);
    rtext("runs "); rdec(m->map.nruns); rtext("upd "); rdec(m->map.nupdates);
    rtext("open "); rdec(m->open_count); rtext("digest "); rword(map_digest(g)); rtext("\n");
}

#ifdef NANDBENCH_WRITE
/* Write benchmark on the dedicated 32 KiB scratch save (/Apps/NandGameTest.bin, the run59
 * file): 40 full-size save versions through the SDK save API, no erases (reclaim off). The
 * first 10 use copy-out reads, the rest the v25 direct reads. Then a fresh full mount must
 * load the last version back. check_write.py decodes the stock-mounted file afterwards. */
static uint8_t payload[16320], loaded[16384];
static void pattern(uint32_t version, uint32_t nonce) {
    for (uint32_t i = 0; i < sizeof payload; ++i)
        payload[i] = (uint8_t)((i * 37u + (i / 4096u) * 53u + version * 11u + (nonce >> ((i & 3u) * 8u))) & 255u);
    for (unsigned k = 0; k < 4; ++k) { payload[k] = (uint8_t)(nonce >> (8 * k)); payload[4 + k] = (uint8_t)(version >> (8 * k)); }
}
static int write_bench(const struct nanoos_api *api) {
    struct ns_game *g = new_game();
    if (!g) { rtext("no memory\n"); return 0; }
    uint32_t t0 = api->usec();
    int rc = ns_nano_game_mount(g, api);
    rtext("W mount rc "); rword((uint32_t)rc); rtext("us "); rdec(api->usec() - t0); rtext("adopted "); rdec(g->adopted); rtext("\n");
    if (rc) return 0;
    rc = ns_game_save_open(g, "/Apps/NandGameTest.bin", 0x4e474431u, 1u);
    rtext("W open rc "); rword((uint32_t)rc);
    if (rc && rc != NS_SAVE_EMPTY) { rtext("\n"); return 0; }
    uint32_t n = 0, first = 0;
    if (!rc) { rc = ns_game_save_load(g, loaded, sizeof loaded, &n); first = (uint32_t)g->save.sequence; }
    rtext("load rc "); rword((uint32_t)rc); rtext("seq "); rdec(first); rtext("\n");
    if (rc) return 0;
    uint32_t nonce = api->usec() ^ 0x5a17c0deu, t[40], tmin = 0xffffffffu, tmax = 0, sum[2] = {0, 0};
    int (*direct_fn)(void *, uint32_t, uint32_t, unsigned, void *, uint32_t *) = g->mount.io.direct;
    for (unsigned v = 0; v < 40; ++v) {
        int dir = v >= 10;
        g->mount.io.direct = dir ? direct_fn : 0; g->io.direct = dir ? direct_fn : 0;
        if (g->append.active) g->writer.io.direct = g->io.direct;
        pattern(first + 1u + v, nonce);
        uint32_t s0 = api->usec();
        rc = ns_game_save_store(g, payload, sizeof payload);
        t[v] = api->usec() - s0;
        if (rc) { rtext("W store "); rdec(v); rtext("rc "); rword((uint32_t)rc); rtext("\n"); return 0; }
        if (v) { if (t[v] < tmin) tmin = t[v]; if (t[v] > tmax) tmax = t[v]; }
        sum[dir] += t[v];
    }
    rtext("W nonce "); rword(nonce); rtext("first_seq "); rdec(first); rtext("last_seq "); rdec((uint32_t)g->save.sequence); rtext("\n");
    rtext("W store_us first(open+blank scan) "); rdec(t[0]); rtext("min "); rdec(tmin); rtext("max "); rdec(tmax);
    rtext("avg_copyout(2-10) "); rdec((sum[0] - t[0]) / 9u); rtext("avg_direct(11-40) "); rdec(sum[1] / 30u); rtext("\n");
    rtext("W us "); for (unsigned v = 0; v < 40; ++v) rdec(t[v]); rtext("\n");
    rtext("W pages "); rdec(g->append.pages); rtext("blocks "); rdec(g->append.blocks); rtext("updates "); rdec(g->mount.map.nupdates); rtext("\n");
    /* Independent fresh full mount (no handoff) must return the last version. */
    struct ns_game *h = new_game();
    if (!h) return 0;
    struct ns_mount_io io = { 0, inspect, context, clock_us, direct, 1 };
    t0 = api->usec();
    rc = ns_game_mount_from(h, 0, &io, 0, 150000000u);
    rtext("W remount rc "); rword((uint32_t)rc); rtext("us "); rdec(api->usec() - t0); rtext("\n");
    if (rc) return 0;
    rc = ns_game_save_open(h, "/Apps/NandGameTest.bin", 0x4e474431u, 1u);
    if (!rc) rc = ns_game_save_load(h, loaded, sizeof loaded, &n);
    int same = !rc && n == sizeof payload && !memcmp(loaded, payload, n) && (uint32_t)h->save.sequence == first + 40u;
    rtext("W reload rc "); rword((uint32_t)rc); rtext("seq "); rdec((uint32_t)h->save.sequence); rtext(same ? "match\n" : "MISMATCH\n");
    return same;
}
#endif
int nano_example_main(const struct nanoos_api *api) {
    nano_app_init(api);
    A = api;
    rtext("NANOOS GL2 GAME build " __DATE__ " " __TIME__ "\nNANDBENCH api "); rdec(NGAME_API_TABLE(api)); rtext("\n");
    int pass = 1;
    if (NGAME_API_TABLE(api) < 25 || !api->nand_read_ppn_direct) { rtext("needs a v25 kernel\nNANDBENCH FAIL\n"); goto out; }
    if (!api->nand_probe()) { rtext("probe failed\nNANDBENCH FAIL\n"); goto out; }

#ifdef NANDBENCH_WRITE
    rtext("NANDWRITE\n");
    pass = write_bench(api);
    rtext(pass ? "NANDWRITE PASS\n" : "NANDWRITE FAIL\n");
    goto out;
#endif
    /* Kernel handoff (captured before any of our own allocations could touch RAM). */
    const void *ho = api->nand_volume_handoff();
    const struct ns_handoff *hh = ho;
    rtext("KERNEL handoff "); rword((uint32_t)(uintptr_t)ho);
    if (hh) { rtext("mount_us "); rdec(hh->mount_us); rtext("flags "); rword(hh->flags); rtext("bytes "); rdec(hh->bytes);
              rtext("runs "); rdec(hh->nruns); rtext("upd "); rdec(hh->nupdates); }
    rtext("\n");

    struct ns_game *g = new_game();
    uint8_t *buf = malloc(65536 + 64);
    if (!g || !buf) { rtext("no memory\nNANDBENCH FAIL\n"); goto out; }
    buf = (uint8_t *)(((uintptr_t)buf + 31u) & ~(uintptr_t)31u);

    struct ns_mount_io io_old = { 0, inspect, context, clock_us, 0, 0 };
    struct ns_mount_io io_d4 = { 0, inspect, context, clock_us, direct, 4 };
    struct ns_mount_io io_d1 = { 0, inspect, context, clock_us, direct, 1 };
    const struct ns_mount_io *ios[4] = { &io_old, &io_d4, &io_d1, &io_d1 };
    static const char *const names[4] = { "M_OLD", "M_D4", "M_D1", "M_ADOPT" };
    uint32_t ref = 0, rd_old = 0, rd_new = 0, by_old = 0, by_new = 0;
    int mism = 0;
    for (unsigned k = 0; k < 4; ++k) {
        if (k == 3 && !ho) { rtext("M_ADOPT skipped (no handoff)\n"); pass = 0; continue; }
        uint32_t t0 = api->usec();
        int rc = ns_game_mount_from(g, k == 3 ? ho : 0, ios[k], 0, 150000000u);
        uint32_t us = api->usec() - t0;
        mount_line(names[k], g, rc, us);
        if (rc) { pass = 0; continue; }
        if (k == 3 && !g->adopted) { rtext("  not adopted\n"); pass = 0; }
        uint32_t d = map_digest(g);
        if (!k) ref = d; else if (d != ref) { rtext("  MAP DIFFERS from M_OLD\n"); pass = 0; }
        if (k == 0) by_old = read_files(g, buf, &rd_old, 1, &mism);
        if (k == 2) by_new = read_files(g, buf, &rd_new, 0, &mism);
        if (k == 0 || k == 2) {
            rtext(k ? "  READ_NEW bytes " : "  READ_OLD bytes "); rdec(k ? by_new : by_old);
            rtext("us "); rdec(k ? rd_new : rd_old);
            rtext("KiB/s "); rdec((uint32_t)((uint64_t)(k ? by_new : by_old) * 1000000u / 1024u / ((k ? rd_new : rd_old) | 1u)));
            rtext("disk_reads "); rdec(g->disk.reads); rtext("hits "); rdec(g->disk.hits); rtext("\n");
        }
    }
    for (unsigned i = 0; i < NFILES; ++i) { rtext("  FILE "); rtext(files[i]); rtext(" "); rdec(file_size[i]); rword(file_hash[i]); rtext("\n"); }
    if (mism) { rtext("  READ CONTENT DIFFERS\n"); pass = 0; }
    if (!by_old) { rtext("  no test file opened\n"); pass = 0; }

    /* Single page reads, block 200.. bank 0 page 0, CE0. */
    {
        static uint32_t a[4096] __attribute__((aligned(32))), b[4096] __attribute__((aligned(32)));
        uint32_t ra[16], rb[16], t[4] = {0}, diff[4] = {0}, rcs[4] = {0};
        for (unsigned i = 0; i < 64; ++i) {
            uint32_t row = (200u + i) << 7;
            uint32_t t0 = api->usec(); int r0 = api->nand_inspect_ppn_page(0, row, a, ra); t[0] += api->usec() - t0;
            for (unsigned v = 1; v < 4; ++v) {
                memset(b, 0x5a, sizeof b);
                unsigned cnt = v == 2 ? 1u : 4u;
                t0 = api->usec(); int r = api->nand_read_ppn_direct(0, row, cnt, v == 3 ? 0 : b, rb); t[v] += api->usec() - t0;
                if (r != r0) ++rcs[v];
                if (r0 == 0 || r0 == -2) {
                    if (memcmp(ra, rb, cnt * 16u)) ++diff[v];
                    else if (v != 3 && memcmp(a, b, cnt * 4096u)) ++diff[v];
                }
            }
        }
        rtext("PAGE64 us inspect/d4/d1/records "); for (unsigned v = 0; v < 4; ++v) rdec(t[v]);
        rtext("rcdiff "); for (unsigned v = 1; v < 4; ++v) rdec(rcs[v]);
        rtext("datadiff "); for (unsigned v = 1; v < 4; ++v) rdec(diff[v]);
        rtext("\n");
        for (unsigned v = 1; v < 4; ++v) if (rcs[v] || diff[v]) pass = 0;
    }

    /* The public path: nano_fs_mount() adopts the handoff (or mounts). */
    {
        uint32_t t0 = api->usec(); int rc = nano_fs_mount(); uint32_t us = api->usec() - t0;
        rtext("NANO_FS_MOUNT rc "); rword((uint32_t)rc); rtext("us "); rdec(us); rtext("\n");
        if (rc) pass = 0;
    }
    rtext(pass ? "NANDBENCH PASS\n" : "NANDBENCH FAIL\n");
out:
    report[rused] = 0;
    rpt_mirror_write(report, rused, rpt_sum(report, rused));
    nano_app_dcache_clean();
    return 0;
}
