#include "nano_fs.h"
#include "nano_app.h"
#include "memmap.h"
#include "storage/nano_game.h"
#include <stdlib.h>

/* Place ns_game (~1.5 MiB) in MM_GAME_HEAP, clear of report mailbox/mirrors. */
static struct ns_game *const g_ns_game = (struct ns_game *)(uintptr_t)(MM_GAME_HEAP + 0x00400000u);
#include "handoff.h"
_Static_assert(MM_GAME_HEAP + 0x00400000u + sizeof(struct ns_game) <= NS_HANDOFF_ADDR,
               "ns_game overlaps the kernel volume handoff window");
static int g_mounted = 0;

struct nano_file {
    struct ns_file file;
    uint64_t pos;
#ifdef NANO_EMU_FILES_ADDR
    const uint8_t *emu;
#endif
};

#ifdef NANO_EMU_FILES_ADDR
/* Emulator-only file source (NanoEmu has no iPod disk). The host stages a table in RAM:
 *   u32 "NEMU", count, log_off, log_cap; count x { char path[120]; u32 off; u32 size }
 * (offsets from the table start; paths are full HFS+ paths). Every open is appended to
 * the log at log_off as "+path\n" (found) or "-path\n" (missing), so the host can stage
 * exactly the files a game reads. Files are served from RAM; nothing touches NAND. */
#include <string.h>
struct nemu_entry { char path[120]; uint32_t off, size; };
static const uint32_t *nemu(void) {
    const uint32_t *t = (const uint32_t *)(uintptr_t)(NANO_EMU_FILES_ADDR);
    return t[0] == 0x554d454eu ? t : 0;
}
static void nemu_log(const uint32_t *t, char mark, const char *path) {
    uint32_t *used = (uint32_t *)(uintptr_t)((uintptr_t)t + t[2]);
    char *log = (char *)(used + 1);
    uint32_t n = (uint32_t)strlen(path);
    if (*used + n + 2 > t[3] - 4u) return;
    log[(*used)++] = mark;
    memcpy(log + *used, path, n); *used += n;
    log[(*used)++] = '\n';
}
#endif

int nano_fs_mount(void) {
    if (g_mounted) return 0;
#ifdef NANO_EMU_FILES_ADDR
    if (nemu()) { g_mounted = 1; return 0; }
#endif
    const struct nanoos_api *api = nano_app_api();
    if (!api) return -1;
    int rc = ns_nano_game_mount(g_ns_game, api);
    if (rc == 0) g_mounted = 1;
    return rc;
}

nano_file_t *nano_fs_open(const char *path) {
    if (!path || !g_mounted) return 0;
    nano_file_t *f = (nano_file_t *)calloc(1, sizeof(nano_file_t));
    if (!f) return 0;
#ifdef NANO_EMU_FILES_ADDR
    const uint32_t *t = nemu();
    if (t) {
        const struct nemu_entry *e = (const struct nemu_entry *)(t + 4);
        for (uint32_t i = 0; i < t[1]; ++i, ++e) {
            if (strncmp(e->path, path, sizeof e->path) == 0) {
               
                f->file.fork.size = e->size;
                f->emu = (const uint8_t *)t + e->off;
                nemu_log(t, '+', path);
                return f;
            }
        }
        nemu_log(t, '-', path);
        free(f);
        return 0;
    }
#endif
    if (ns_game_open(g_ns_game, path, &f->file) != 0) {
        free(f);
        return 0;
    }
    f->pos = 0;
    return f;
}

uint64_t nano_fs_size(const nano_file_t *f) {
    return f ? f->file.fork.size : 0u;
}

int nano_fs_pread(nano_file_t *f, uint64_t off, void *buf, uint32_t len) {
    if (!f || !buf || !len || !g_mounted) return -1;
    if (off >= f->file.fork.size) return 0;
    uint64_t avail = f->file.fork.size - off;
    uint32_t want = (uint64_t)len > avail ? (uint32_t)avail : len;
    uint32_t got = 0;
#ifdef NANO_EMU_FILES_ADDR
    if (f->emu) { memcpy(buf, f->emu + off, want); return (int)want; }
#endif
    if (ns_game_read(g_ns_game, &f->file, off, buf, want, &got) != 0) return -1;
    return (int)got;
}

int nano_fs_read(nano_file_t *f, void *buf, uint32_t len) {
    int got = nano_fs_pread(f, f->pos, buf, len);
    if (got > 0) f->pos += (uint32_t)got;
    return got;
}

int nano_fs_seek(nano_file_t *f, int64_t offset, int whence) {
    if (!f) return -1;
    int64_t base = 0;
    if (whence == 0) base = 0;
    else if (whence == 1) base = (int64_t)f->pos;
    else if (whence == 2) base = (int64_t)f->file.fork.size;
    else return -1;
    int64_t next = base + offset;
    if (next < 0) next = 0;
    if ((uint64_t)next > f->file.fork.size) next = (int64_t)f->file.fork.size;
    f->pos = (uint64_t)next;
    return 0;
}

uint64_t nano_fs_tell(const nano_file_t *f) {
    return f ? f->pos : 0u;
}

void nano_fs_close(nano_file_t *f) {
    free(f);
}

void *nano_fs_read_all(const char *path, uint32_t *out_size) {
    if (out_size) *out_size = 0;
    nano_file_t *f = nano_fs_open(path);
    if (!f) return 0;
    uint64_t sz64 = nano_fs_size(f);
    if (sz64 > 0x01000000u) { nano_fs_close(f); return 0; }
    uint32_t sz = (uint32_t)sz64;
    uint8_t *buf = (uint8_t *)malloc(sz + 1u);
    if (!buf) { nano_fs_close(f); return 0; }
    uint32_t off = 0;
    while (off < sz) {
        int r = nano_fs_read(f, buf + off, sz - off);
        if (r <= 0) break;
        off += (uint32_t)r;
    }
    nano_fs_close(f);
    if (off != sz) { free(buf); return 0; }
    buf[sz] = 0;
    if (out_size) *out_size = sz;
    return buf;
}
