#ifndef GL2_STD_H
#define GL2_STD_H
#include <stdint.h>
struct gl2_ctx;
void gl2std_set_context(struct gl2_ctx *c);
int gl2std_host_init(void);
uint32_t *gl2std_host_target(void);
typedef void (*gl2std_frame_fn)(void);
void gl2std_set_frame_hooks(gl2std_frame_fn ensure, gl2std_frame_fn changed);
/* Multi-pass fragment shaders (gl2api.h gl2_fragment_passes): 0 off (default), 1 shaders with no single-pass plan, 2 also in place of approximated plans. */
void gl2std_fragment_passes(int mode);
int gl2std_program_passes(unsigned program);
int gl2std_program_passes_additive(unsigned program);
const uint32_t *gl2std_lut_stats(void);
#endif
