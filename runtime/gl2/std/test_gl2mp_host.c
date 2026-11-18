/* Host-only platform for the multi-pass fragment regression: software target, larger program pool. */
#include "../gl2api.h"
#include "gl2std.h"
static gl2_ctx ctx;static gl2_prog_obj progs[GL2_MAX_PROGRAMS];static uint32_t segment[1024];static uint32_t target[16*16];static float depth[16*16];static uint8_t tex_arena[GL2_TEXTURE_ARENA_BYTES];
int gl2mp_host_init(int mode){gl2_init(&ctx,progs,segment,1024,0,0);gl2_texture_arena(&ctx,tex_arena,0x01000000,sizeof tex_arena,0,0);ctx.soft_target=target;ctx.soft_depth=depth;ctx.soft_width=ctx.soft_height=ctx.soft_stride=16;ctx.soft_depth_initialized=0;gl2_fragment_passes(&ctx,mode);gl2std_set_context(&ctx);return 0;}
uint32_t *gl2mp_host_target(void){return target;}
unsigned gl2mp_err_site(void){return ctx.err_site;}
int gl2mp_passes_add(unsigned program){return program<GL2_MAX_PROGRAMS?progs[program].mp_acc_n:0;}
int gl2mp_passes(unsigned program){return program<GL2_MAX_PROGRAMS?progs[program].mp_n:0;}
