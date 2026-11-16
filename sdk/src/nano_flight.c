#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include "nano_flight.h"
#include "nano_app.h"
#include "report.h"
#include "gl2_egl.h"
static const uint32_t flight_base[2] = { 0x0ba00000u, 0x0be60000u };
#define FL_STATUS 0x5000u
#define FL_SHOT   0x6000u
#define FL_TEXT   12288u
#define FL_STATUS_MAGIC 0x54415453u   /* "STAT" */
#define FL_SHOT_MAGIC   0x544f4853u   /* "SHOT" */
static char ring[FL_TEXT];
static uint32_t ring_len;
static void clean_range(uintptr_t a, uint32_t n) {
#if defined(__arm__)
    for (uintptr_t l = a & ~31u; l < a + n; l += 32u) __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" :: "r"(l) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
#else
    (void)a; (void)n;
#endif
}
void nano_flight_status(uint32_t frame, uint32_t phase, const uint32_t user[4]) {
    for (unsigned m = 0; m < 2; ++m) {
        volatile uint32_t *s = (volatile uint32_t *)(flight_base[m] + FL_STATUS);
        s[0] = FL_STATUS_MAGIC; s[1] = frame; s[2] = phase; s[3] = nano_app_usec();
        for (unsigned i = 0; i < 4; ++i) s[4 + i] = user ? user[i] : 0u;
        clean_range((uintptr_t)s, 32u);
    }
}
void nano_flight_log(const char *fmt, ...) {
    char tmp[160];
    va_list ap; va_start(ap, fmt); int n = vsnprintf(tmp, sizeof tmp, fmt, ap); va_end(ap);
    if (n <= 0) return;
    if ((uint32_t)n >= sizeof tmp) n = (int)sizeof tmp - 1;
    if (ring_len + (uint32_t)n >= FL_TEXT) {               /* keep the newest half */
        uint32_t keep = FL_TEXT / 2u, from = ring_len - keep;
        for (uint32_t i = 0; i < keep; ++i) ring[i] = ring[from + i];
        ring_len = keep;
    }
    for (int i = 0; i < n; ++i) ring[ring_len++] = tmp[i];
}
void nano_flight_commit(void) {
    uint32_t len = ring_len, sum = rpt_sum(ring, len);
    for (unsigned m = 0; m < 2; ++m) {
        volatile struct rpt_hdr *h = (volatile struct rpt_hdr *)flight_base[m];
        volatile char *d = (volatile char *)(flight_base[m] + RPT_DATA);
        h->magic[0] = 0;                                   /* invalid while being rewritten */
        for (uint32_t i = 0; i < len; ++i) d[i] = ring[i];
        for (uint32_t i = 0; i < sizeof *h / 4; ++i) if (i) ((volatile uint32_t *)h)[i] = 0;
        h->slot_size = RPT_MIR_SIZE; h->id[RPT_M8_SLOT] = RPT_M8_ID; h->len[RPT_M8_SLOT] = len; h->sum[RPT_M8_SLOT] = sum;
        h->magic[1] = RPT_MAGIC1; h->magic[0] = RPT_MAGIC0;
        h->hdr_sum = rpt_sum(h, __builtin_offsetof(struct rpt_hdr, hdr_sum));
        clean_range(flight_base[m], RPT_DATA + len);
        clean_range(flight_base[m], sizeof *h);
    }
}
void nano_flight_screenshot(void) {
    const uint32_t *fb = gl2_egl_last_frame();
    if (!fb) return;
    for (unsigned m = 0; m < 2; ++m) {
        volatile uint32_t *hd = (volatile uint32_t *)(flight_base[m] + FL_SHOT);
        volatile uint16_t *px = (volatile uint16_t *)(flight_base[m] + FL_SHOT + 64u);
        hd[0] = 0;
        uint32_t sum = 0x811c9dc5u;
        for (uint32_t y = 0; y < 216u; ++y) for (uint32_t x = 0; x < 120u; ++x) {
            uint32_t v = fb[(2u * y) * 240u + 2u * x];       /* top-left origin, ARGB8888 */
            uint16_t p = (uint16_t)(((v >> 8) & 0xf800u) | ((v >> 5) & 0x07e0u) | ((v >> 3) & 0x001fu));
            px[y * 120u + x] = p;
            sum = (sum ^ (p & 255u)) * 16777619u; sum = (sum ^ (p >> 8)) * 16777619u;
        }
        hd[1] = 120u; hd[2] = 216u; hd[3] = nano_app_usec(); hd[4] = sum; hd[0] = FL_SHOT_MAGIC;
        clean_range(flight_base[m] + FL_SHOT, 64u + 120u * 216u * 2u);
    }
}
