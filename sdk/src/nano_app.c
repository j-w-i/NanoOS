#include "nano_app.h"
#include "gl2_egl.h"

static const struct nanoos_api *g_api;
static int g_was_down;
static int g_last_x;
static int g_last_y;

int nano_app_init(const struct nanoos_api *api) {
    if (!api || api->abi != NGAME_ABI || NGAME_API_TABLE(api) < 3) return -1;
    g_api = api;
    g_was_down = 0;
    g_last_x = 0;
    g_last_y = 0;
    gl2_egl_set_platform(api);
    return 0;
}

static uint32_t g_orient;
int nano_app_set_orientation(uint32_t o) {
    if (gl2_egl_set_orientation(o)) return -1;
    g_orient = o;
    return 0;
}
int nano_app_screen_w(void) { return g_orient ? (int)NANO_SCREEN_H : (int)NANO_SCREEN_W; }
int nano_app_screen_h(void) { return g_orient ? (int)NANO_SCREEN_W : (int)NANO_SCREEN_H; }

const struct nanoos_api *nano_app_api(void) {
    return g_api;
}

uint32_t nano_app_usec(void) {
    return (g_api && g_api->usec) ? g_api->usec() : 0u;
}

void nano_app_dcache_clean(void) {
    if (g_api && g_api->dcache_clean) g_api->dcache_clean();
}

void nano_app_poll_input(nano_input_t *out) {
    if (!out) return;
    out->down = 0;
    out->pressed = 0;
    out->released = 0;
    out->moved = 0;
    out->x = g_last_x;
    out->y = g_last_y;
    out->prev_x = g_last_x;
    out->prev_y = g_last_y;
    out->buttons = 0;
    out->home_pressed = 0;

    if (!g_api) return;
    if (g_api->input_poll) {
        out->buttons = g_api->input_poll();
        out->home_pressed = (out->buttons & NGAME_HOME) ? 1 : 0;
    }

    if (g_api->touch_poll && g_api->touch) {
        g_api->touch_poll();
        unsigned st = g_api->touch->state;
        if (st == 3u || st == 4u) {
            int dx = 239 - g_api->touch->y;
            int dy = g_api->touch->x;
            if (dx < 0) dx = 0;
            if (dx >= (int)NANO_SCREEN_W) dx = (int)NANO_SCREEN_W - 1;
            if (dy < 0) dy = 0;
            if (dy >= (int)NANO_SCREEN_H) dy = (int)NANO_SCREEN_H - 1;
            if (g_orient == NANO_ORIENT_LANDSCAPE) {            /* panel (dx, dy) -> logical, see gl2nano.c nano_read */
                int lx = dy, ly = (int)NANO_SCREEN_W - 1 - dx; dx = lx; dy = ly;
            } else if (g_orient == NANO_ORIENT_LANDSCAPE_LEFT) {
                int lx = (int)NANO_SCREEN_H - 1 - dy, ly = dx; dx = lx; dy = ly;
            }
            out->down = 1;
            out->x = dx;
            out->y = dy;
            if (!g_was_down) {
                out->pressed = 1;
            } else if (dx != g_last_x || dy != g_last_y) {
                out->moved = 1;
            }
            g_last_x = dx;
            g_last_y = dy;
            g_was_down = 1;
        } else if (g_was_down) {
            out->released = 1;
            g_was_down = 0;
        }
    }
}

/* kernel logical landscape (x = panel y, y = 239 - panel x) -> display coordinates of the
 * current orientation; the same mapping nano_app_poll_input applies */
static void to_display(int kx, int ky, int *ox, int *oy) {
    int dx = 239 - ky;
    int dy = kx;
    if (dx < 0) dx = 0;
    if (dx >= (int)NANO_SCREEN_W) dx = (int)NANO_SCREEN_W - 1;
    if (dy < 0) dy = 0;
    if (dy >= (int)NANO_SCREEN_H) dy = (int)NANO_SCREEN_H - 1;
    if (g_orient == NANO_ORIENT_LANDSCAPE) {
        int lx = dy, ly = (int)NANO_SCREEN_W - 1 - dx; dx = lx; dy = ly;
    } else if (g_orient == NANO_ORIENT_LANDSCAPE_LEFT) {
        int lx = (int)NANO_SCREEN_H - 1 - dy, ly = dx; dx = lx; dy = ly;
    }
    *ox = dx;
    *oy = dy;
}

static struct { int active, id, x, y; } g_slot[NANO_TOUCH_MAX];

int nano_app_poll_touches(nano_touch_t out[NANO_TOUCH_MAX]) {
    struct ngame_touch_contact c[NANO_TOUCH_MAX];
    int n = 0, i, j, downs = 0;
    for (i = 0; i < NANO_TOUCH_MAX; i++) {
        out[i].down = out[i].pressed = out[i].released = out[i].moved = 0;
        out[i].x = out[i].prev_x = g_slot[i].x;
        out[i].y = out[i].prev_y = g_slot[i].y;
        out[i].id = g_slot[i].id;
    }
    if (!g_api || !g_api->touch_poll || !g_api->touch) return 0;
    for (i = 0; i < 4; i++) if (!g_api->touch_poll()) break;   /* drain queued reports */
    if (NGAME_API_TABLE(g_api) >= 27 && g_api->touch_contacts) {
        n = g_api->touch_contacts(c, NANO_TOUCH_MAX);
    } else if (g_api->touch->state == 3u || g_api->touch->state == 4u) {
        c[0].id = 0; c[0].x = g_api->touch->x; c[0].y = g_api->touch->y; c[0].state = g_api->touch->state;
        n = 1;
    }
    int seen[NANO_TOUCH_MAX] = {0, 0, 0, 0, 0};
    int ccur[NANO_TOUCH_MAX][2];
    for (j = 0; j < n; j++) to_display(c[j].x, c[j].y, &ccur[j][0], &ccur[j][1]);
    for (i = 0; i < NANO_TOUCH_MAX; i++) {        /* fingers that stayed */
        if (!g_slot[i].active) continue;
        for (j = 0; j < n; j++) if (!seen[j] && c[j].id == g_slot[i].id) break;
        if (j == n) {                              /* gone: released this call */
            out[i].released = 1;
            g_slot[i].active = 0;
            continue;
        }
        seen[j] = 1;
        out[i].down = 1;
        out[i].x = ccur[j][0]; out[i].y = ccur[j][1];
        out[i].moved = out[i].x != g_slot[i].x || out[i].y != g_slot[i].y;
        g_slot[i].x = out[i].x; g_slot[i].y = out[i].y;
    }
    for (j = 0; j < n; j++) {                      /* new fingers */
        if (seen[j]) continue;
        for (i = 0; i < NANO_TOUCH_MAX; i++) if (!g_slot[i].active && !out[i].released) break;
        if (i == NANO_TOUCH_MAX) continue;
        g_slot[i].active = 1; g_slot[i].id = c[j].id;
        g_slot[i].x = out[i].x = out[i].prev_x = ccur[j][0];
        g_slot[i].y = out[i].y = out[i].prev_y = ccur[j][1];
        out[i].id = c[j].id;
        out[i].down = out[i].pressed = 1;
    }
    for (i = 0; i < NANO_TOUCH_MAX; i++) downs += out[i].down;
    return downs;
}

int nano_app_touch_max_seen(void) {
    if (g_api && NGAME_API_TABLE(g_api) >= 27 && g_api->touch_max_seen) return (int)g_api->touch_max_seen();
    return 0;
}

int nano_app_home_pressed(void) {
    if (!g_api) return 0;
    if (g_api->input_poll && (g_api->input_poll() & NGAME_HOME)) return 1;
    if (g_api->input_held && (g_api->input_held() & NGAME_HOME)) return 1;
    return 0;
}

void nano_app_gfx_stats(uint32_t *frames, uint32_t *software_frames, uint32_t *fault) {
    gl2_egl_stats(frames, software_frames, fault);
}
