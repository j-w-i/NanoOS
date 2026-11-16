#ifndef GL2_EGL_H
#define GL2_EGL_H
#include <stdint.h>
struct nanoos_api;
void gl2_egl_set_platform(const struct nanoos_api *api);
void gl2_egl_stats(uint32_t *frames, uint32_t *software, uint32_t *fault);
/* The most recently presented 240x432 ARGB8888 frame (top-left origin), or 0. */
const uint32_t *gl2_egl_last_frame(void);
/* Landscape window surface. Call before eglCreateWindowSurface: the surface then reports
 * 432x240 and the GLES2 default framebuffer (viewport, scissor, glReadPixels) is landscape;
 * the runtime rotates positions onto the portrait panel in the GPU header at no per-vertex
 * cost. 0 portrait (default), 1 landscape with the home button on the right, 2 home left.
 * FBO rendering is unaffected. Returns 0, or -1 for a bad mode / while a window is current. */
int gl2_egl_set_orientation(uint32_t mode);
/* Number of GPU/display faults the runtime recovered from (gl2_nano_recover). */
uint32_t gl2_egl_recoveries(void);
uint32_t gl2_egl_orientation(void);
#endif
