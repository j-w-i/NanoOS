#ifndef NANOOS_SDK_APP_H
#define NANOOS_SDK_APP_H

#include <stdint.h>
#include "gameabi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NANO_SCREEN_W 240u
#define NANO_SCREEN_H 432u

typedef struct {
    int down;          /* 1 while finger is touching screen */
    int pressed;       /* 1 on transition from up -> down */
    int released;      /* 1 on transition from down -> up */
    int moved;         /* 1 when down and coordinates changed */
    int x;             /* Display X in [0, 239] (landscape: [0, 431]) */
    int y;             /* Display Y in [0, 431] (landscape: [0, 239]) */
    int prev_x;        /* Previous display X in [0, 239] */
    int prev_y;        /* Previous display Y in [0, 431] */
    uint32_t buttons;  /* Raw NGAME_* button bitmask */
    int home_pressed;  /* 1 if NGAME_HOME is currently pressed */
} nano_input_t;

/* Initialize NanoOS application SDK (heap, EGL platform, input state).
 * Call once at the start of nano_example_main / game entry. */
int nano_app_init(const struct nanoos_api *api);

/* Return the active NanoOS system services table (or NULL before init). */
const struct nanoos_api *nano_app_api(void);

/* Monotonic microsecond timer. */
uint32_t nano_app_usec(void);

/* Clean CPU D-cache to DRAM. */
void nano_app_dcache_clean(void);

/* Screen orientation. Call before eglCreateWindowSurface. LANDSCAPE: the EGL window is
 * 432x240 (home button on the right), LANDSCAPE_LEFT: home button on the left. Touch
 * coordinates from nano_app_poll_input follow the orientation (top-left origin). */
#define NANO_ORIENT_PORTRAIT       0u
#define NANO_ORIENT_LANDSCAPE      1u
#define NANO_ORIENT_LANDSCAPE_LEFT 2u
int nano_app_set_orientation(uint32_t orientation);
/* Logical screen size for the current orientation. */
int nano_app_screen_w(void);
int nano_app_screen_h(void);

/* Poll touchscreen (display coords for the current orientation: 240x432 portrait,
 * 432x240 landscape) and physical buttons. */
void nano_app_poll_input(nano_input_t *out);

/* Return 1 if NGAME_HOME button is pressed. */
int nano_app_home_pressed(void);

/* Query EGL/GL2 rendering stats (frames, software_frames, fault). */
void nano_app_gfx_stats(uint32_t *frames, uint32_t *software_frames, uint32_t *fault);

#ifdef __cplusplus
}
#endif

#endif /* NANOOS_SDK_APP_H */
