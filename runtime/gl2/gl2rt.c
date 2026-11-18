#include "gl2rt.h"
static uint32_t f2u(float f) { union { float f; uint32_t u; } c; c.f = f; return c.u; }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static float rdf(const uint8_t *p) { union { uint32_t u; float f; } c; c.u = rd32(p); return c.f; }
static int streq(const char *a, const char *b) { while (*a && *a == *b) { ++a; ++b; } return *a == *b; }
/* blob: magic, nstage, nplan, nlit, nuni, has_constp, constp[4], code[2*nstage], lit_idx(u16 padded to 4), lit_val[4*nlit],
 *       uni_idx[nuni] pad4, uni_cnt[nuni] pad4, names (NUL-separated, pad4), plan[12*nplan] pad4 */
static uint32_t pad4(uint32_t n) { return (n + 3u) & ~3u; }
int gl2_program_load(gl2_program *p, const void *blob, uint32_t len) {
    const uint8_t *b = blob; uint32_t o = 0;
    if (len < 40 || rd32(b) != GL2_BLOB_MAGIC) return -1;
    p->dot3_mask = 0;
    p->normal_input = p->rounding_bias = p->repeat_uv0 = p->omit_colour = p->custom_alpha = 0;p->alpha[0]=p->alpha[1]=0;p->uv0_input=0;p->pack_pos=p->pack_uv1=0;
    p->nstage = rd32(b + 4); p->nplan = rd32(b + 8); p->nlit = rd32(b + 12); p->nuni = rd32(b + 16); p->has_constp = rd32(b + 20);
    for (int i = 0; i < 4; ++i) p->constp[i] = rdf(b + 24 + 4 * i);
    if (p->nstage == 0 || p->nstage > 256 || p->nplan < 1 || p->nplan > 2 || p->nuni > GL2_MAX_UNI) return -2;
    o = 40; p->code = (const uint32_t *)(b + o); o += 8 * p->nstage;
    p->lit_idx = (const uint16_t *)(b + o); o += pad4(2 * p->nlit);
    p->lit_val = (const float *)(b + o); o += 16 * p->nlit;
    p->uni_idx = b + o; o += pad4(p->nuni); p->uni_cnt = b + o; o += pad4(p->nuni);
    p->uni_names = (const char *)(b + o);
    { uint32_t nb = 0, k = 0; while (k < p->nuni && o + nb < len) { while (b[o + nb]) ++nb; ++nb; ++k; } o += pad4(nb); }
    p->plan = b + o; o += 12 * p->nplan;
    if (o > len) return -3;
    p->cused_valid = 0; p->clay_valid = 0;
    for (uint32_t i = 0; i < GL2_MAX_UNI; ++i) { p->uset[i] = 0; for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) p->uval[i][r][c] = 0; }
    return 0;
}
int gl2_uniform_loc(const gl2_program *p, const char *name) {
    const char *s = p->uni_names;
    for (uint32_t i = 0; i < p->nuni; ++i) { if (streq(s, name)) return (int)i; while (*s) ++s; ++s; }
    return -1;
}
void gl2_uniform4f(gl2_program *p, int loc, float x, float y, float z, float w) {
    if (loc < 0 || (uint32_t)loc >= p->nuni) return;
    p->uval[loc][0][0] = x; p->uval[loc][0][1] = y; p->uval[loc][0][2] = z; p->uval[loc][0][3] = w; if (!p->uset[loc]) { p->clay_valid = 0; } p->uset[loc] = 1;
}
void gl2_uniform_matrix4(gl2_program *p, int loc, const float *m) {
    if (loc < 0 || (uint32_t)loc >= p->nuni) return;
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) p->uval[loc][r][c] = m[c * 4 + r];   /* column-major in -> rows */
    if (!p->uset[loc]) { p->clay_valid = 0; }
    p->uset[loc] = 1;
}
void gl2_uniform_matrix3(gl2_program *p, int loc, const float *m) {
    if (loc < 0 || (uint32_t)loc >= p->nuni) return;
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) p->uval[loc][r][c] = m[c * 3 + r];
    if (!p->uset[loc]) { p->clay_valid = 0; }
    p->uset[loc] = 1;
}
void gl2_uniform_matrix2(gl2_program *p, int loc, const float *m) {
    if (loc < 0 || (uint32_t)loc >= p->nuni) return;
    for (int r = 0; r < 2; ++r) for (int c = 0; c < 2; ++c) p->uval[loc][r][c] = m[c * 2 + r];
    if (!p->uset[loc]) { p->clay_valid = 0; }
    p->uset[loc] = 1;
}
static uint32_t w2_of(const uint8_t *c) {
    uint32_t x1 = c[0], cx1 = c[1], y1 = c[2], ay1 = c[3], cy1 = c[4], x2 = c[5], y2 = c[6], ay2 = c[7], cy2 = c[8], drop = c[9], bias = c[10], sc = c[11];
    uint32_t h = x1 << 14 | ay1 << 13 | y1 << 11 | x2 << 9 | ay2 << 8 | y2 << 6 | cx1 << 5 | cy1 << 4 | drop << 3 | cy2 << 2 | bias << 1 | (sc >> 1);
    return h << 16 | (sc & 1) << 15 | 0x4610u;
}
static int tsp(uint32_t gl, int is_src) {
    switch (gl) { case 0: return 0; case 1: return 1; case 0x302: return 4; case 0x303: return 5; case 0x304: return 6; case 0x305: return 7; }
    if (is_src) { if (gl == 0x306) return 2; if (gl == 0x307) return 3; }
    else { if (gl == 0x300) return 2; if (gl == 0x301) return 3; }
    return -1;
}
static uint32_t round_u8(float v) {            /* round half to even on the exact (double) product, like the Python reference */
    double x = (double)v * 255.0; if (x < 0) x = 0; if (x > 255) x = 255;
    uint32_t i = (uint32_t)x; double fr = x - (double)i;
    if (fr > 0.5 || (fr == 0.5 && (i & 1))) ++i;
    return i;
}
uint32_t gl2_native_vertex_words(const gl2_program *p,const gl2_vertex_view views[5],const void **source){
    uint32_t two=p->uv1_input>0,words=6u+p->normal_input+p->uv1_input;
    if(GL2_UV0W(p)!=2u)return 0;
    uintptr_t base=(uintptr_t)views[0].data;
    if(!base||!p->omit_colour||p->normal_input>4||p->nplan<1||p->nplan>2||(base&3u)||views[0].stride!=words)return 0;
    if(p->normal_input&&(views[1].stride!=words||(uintptr_t)views[1].data!=base+16u))return 0;
    if(views[3].stride!=words||(uintptr_t)views[3].data!=base+(4u+p->normal_input)*4u)return 0;
    if(two&&(views[4].stride!=words||(uintptr_t)views[4].data!=base+(6u+p->normal_input)*4u))return 0;
    if(p->uv1_input!=0&&p->uv1_input!=2)return 0;
    if(source)*source=views[0].data;
    return words;
}
static inline int row_sent(const gl2_program *p,uint32_t ci){return !p->cused_valid||(ci<128u&&(p->cused[ci>>3]>>(ci&7u)&1u));}
void gl2_program_scan_constants(gl2_program *p){
    for(unsigned i=0;i<16;++i)p->cused[i]=0;
    for(uint32_t i=0;i<p->nstage;++i){uint32_t b=p->code[2*i+1];
        for(unsigned q=0;q<3;++q)if(((b>>(16+2*q))&3u)==1u){uint32_t ci=((b>>12)&15u)<<4|((b>>(4*q))&15u);p->cused[ci>>3]|=(uint8_t)(1u<<(ci&7u));}}
    p->cused_valid=1;p->clay_valid=0;
}
static int count_stages(const gl2_program *p,uint32_t *stages_out,uint32_t *rows_out){
    uint32_t two = p->nplan > 1, stages = p->nstage, rows = 0;
    if (!stages || stages > 256) return -2;
    if (p->repeat_uv0) {
        if (!two) return -2;
        for (uint32_t i = 0; i < p->nstage; ++i)
            if ((p->code[2 * i + 1] >> 26) == 3) ++stages;
    }
    if (p->rounding_bias) {
        if (!two || p->rounding_bias > 112) return -2;
        ++stages;
    }
    if (!stages || stages > 256) return -2;
    for (uint32_t u = 0; u < p->nuni; ++u) if (p->uset[u]) for (uint32_t r = 0; r < p->uni_cnt[u]; ++r) if (row_sent(p, p->uni_idx[u] + r)) ++rows;
    *stages_out = stages; *rows_out = rows;
    return 0;
}
#ifndef GL2_TRILINEAR
#define GL2_TRILINEAR 0
#endif
extern uint32_t (*gl2_prof_clock)(void); extern uint32_t gl2_prof2[8];
const uint32_t *gl2_hdr_memo_out; static const uint32_t *gl2_hdr_memo_code; static uint32_t gl2_hdr_memo_n, gl2_hdr_memo_k, gl2_hdr_memo_rot;
static int build_header_i(const gl2_program *p,const gl2_state *st,int mode,
                   const gl2_tex *tex,uint32_t vp_w,uint32_t vp_h,uint32_t *out,uint32_t cap,uint32_t color_mask);
static int build_header(const gl2_program *p,const gl2_state *st,int mode,
                   const gl2_tex *tex,uint32_t vp_w,uint32_t vp_h,uint32_t *out,uint32_t cap,uint32_t color_mask) {
    uint32_t t=gl2_prof_clock?gl2_prof_clock():0;
    int r=build_header_i(p,st,mode,tex,vp_w,vp_h,out,cap,color_mask);
    if(gl2_prof_clock)gl2_prof2[0]+=gl2_prof_clock()-t;
    if(r>0)gl2_prof2[3]+=(uint32_t)r;
    return r;
}
static int build_header_i(const gl2_program *p,const gl2_state *st,int mode,
                   const gl2_tex *tex,uint32_t vp_w,uint32_t vp_h,uint32_t *out,uint32_t cap,uint32_t color_mask) {
    uint32_t k = 0, two = p->nplan > 1, stages = 0, rows = 0;
    int err = count_stages(p, &stages, &rows);
    if (err) return err;
    if (p->normal_input > 4) return -5;
    uint32_t need = (two ? 4u : 3u) + 2u * stages + 5u * (p->nlit + rows + (p->rounding_bias ? 1u : 0u)) + (two ? 20u : 17u);
    if (need > cap) return -1;
    if (mode < 4 || mode > 6) return -2;
    uint32_t desc = (0x05u << 24) | ((stages - 1) << 8);
    out[k++] = 0x80000004u | (two ? 1u << 13 : 0); out[k++] = desc; if (two) out[k++] = desc; out[k++] = 0x90000000u | (stages - 1);
    /* Landscape: the application renders a (vp_h x vp_w) surface that is shown rotated on
     * the portrait panel. Every write to VGP output 0 (gl_Position) gets lanes x/y swapped
     * (destination mask bits and the x/y entries of every source swizzle: exact for the
     * component-wise and dot-product ops the compiler emits for output 0), and one viewport
     * scale is negated, so swap + reflection = a 90 degree rotation at no per-vertex cost.
     * Winding is preserved (det +1), so TA culling is unchanged. */
    const uint32_t rot = p->rotate;
    if (rot) { uint32_t t = vp_w; vp_w = vp_h; vp_h = t; }
    /* The code words of the last header built into this buffer are still there unless
     * something else wrote it (gl2_hdr_memo_out is cleared by gl2_invalidate_state_cache). */
    if (gl2_hdr_memo_out == out && gl2_hdr_memo_code == p->code && gl2_hdr_memo_n == p->nstage && gl2_hdr_memo_k == k &&
        gl2_hdr_memo_rot == rot && !p->repeat_uv0) k += 2u * p->nstage;
    else {
        const uint32_t k0 = k;
        if (!rot && !p->repeat_uv0) { const uint32_t *src = p->code; for (uint32_t i = 0; i < 2u * p->nstage; ++i) out[k++] = src[i]; }
        else for (uint32_t i = 0; i < p->nstage; ++i) {
            uint32_t a=p->code[2*i],b=p->code[2*i+1];
            if(rot&&(b>>26)==0u){
                uint32_t sw=a>>8,o=0;
                for(unsigned s=0;s<3;++s){uint32_t z=sw>>(8*s)&0xffu;z=(z&0xf0u)|(z>>2&3u)|(z&3u)<<2;o|=z<<(8*s);}
                a=(a&0xffu)|o<<8;
                b=(b&~(3u<<22))|(b>>23&1u)<<22|(b>>22&1u)<<23;
            }
            out[k++]=a;out[k++]=b;
            if(p->repeat_uv0&&(b>>26)==3){out[k++]=a;out[k++]=(b&0x03ffffffu)|(4u<<26);}
        }
        if (!p->repeat_uv0) { gl2_hdr_memo_out = out; gl2_hdr_memo_code = p->code; gl2_hdr_memo_n = p->nstage; gl2_hdr_memo_k = k0; gl2_hdr_memo_rot = rot; }
        else gl2_hdr_memo_out = 0;
    }
    if(p->rounding_bias){uint32_t ci=p->rounding_bias-1;out[k++]=1u|(0xe4u<<8);out[k++]=(1u<<26)|(15u<<22)|(1u<<16)|(ci&15u)|((ci>>4)<<12);}
    /* constants: literals and uniform rows, ascending index (the Python builder emits sorted(cst));
     * the sorted layout is cached per program, only the values are read per draw */
    if (!p->clay_valid) {
        gl2_program *m = (gl2_program *)p;
        uint8_t *idx = m->clay_idx; const float **val = m->clay_val; uint32_t cnt = 0;
        for (uint32_t i = 0; i < p->nlit; ++i) { idx[cnt] = (uint8_t)p->lit_idx[i]; val[cnt] = p->lit_val + 4 * i; ++cnt; }
        for (uint32_t u = 0; u < p->nuni; ++u) if (p->uset[u])
            for (uint32_t r = 0; r < p->uni_cnt[u]; ++r) {
                uint32_t ci = p->uni_idx[u] + r, j;
                if (!row_sent(p, ci)) continue;
                for (j = 0; j < cnt; ++j) if (idx[j] == ci) break;           /* a uniform row overrides a literal at the same index */
                if (j == cnt) ++cnt;
                idx[j] = (uint8_t)ci; val[j] = p->uval[u][r];
            }
        static const float bias[4]={1.f/255,1.f/255,1.f/255,1.f/255};
        if(p->rounding_bias){uint32_t ci=p->rounding_bias-1;for(uint32_t i=0;i<cnt;++i)if(idx[i]==ci)return -2;idx[cnt]=(uint8_t)ci;val[cnt++]=bias;}
        for (uint32_t a = 1; a < cnt; ++a) { uint8_t ti = idx[a]; const float *tv = val[a]; uint32_t b = a; while (b && idx[b - 1] > ti) { idx[b] = idx[b - 1]; val[b] = val[b - 1]; --b; } idx[b] = ti; val[b] = tv; }
        m->clay_n = (uint16_t)cnt; m->clay_valid = 1;
    }
    for (uint32_t i = 0; i < p->clay_n; ++i) { const float *v = p->clay_val[i]; uint32_t ci = p->clay_idx[i]; out[k++] = 0xa0000000u | (ci << 16) | ci; out[k++] = f2u(v[0]); out[k++] = f2u(v[1]); out[k++] = f2u(v[2]); out[k++] = f2u(v[3]); }
    uint32_t isp = two ? 0x22650200u : 0x22620200u;
    isp &= ~(0xFC000000u | 0xE00u | 0x80u | 0x2000u);
    uint32_t cmask = color_mask & 0xfu, layer = 0x10000000u | (0xf7u | (two ? 0x08u : 0u));
    if (cmask != 0xfu) {
        uint32_t lcm = ((cmask & 4u) ? 1u : 0u) | ((cmask & 2u) ? 2u : 0u) |
                       ((cmask & 1u) ? 4u : 0u) | ((cmask & 8u) ? 8u : 0u);
        layer = 0x10000000u | ((0xf7u & ~0x1e0u) | (lcm << 5) | (two ? 0x08u : 0u));
        isp &= ~0x4100u;
        if (!cmask) isp |= 0x100u; else isp |= 0x4000u;
    }
    uint32_t zc = st->depth_test ? st->depth_func - 0x200u : 7u;
    isp |= zc << 9;
    if (!st->depth_test || !st->depth_write) isp |= 0x80u;
    if (st->blend) { int s = tsp(st->src, 1), d = tsp(st->dst, 0); if (s < 0 || d < 0) return -3; isp |= ((uint32_t)s << 29) | ((uint32_t)d << 26) | 0x2000u; }
    else isp |= 1u << 29;
    out[k++] = layer; out[k++] = isp; out[k++] = 0xe0000000u;
    for (uint32_t u = 0; u < (two ? 2u : 1u); ++u) {
        uint32_t l2w = tex[u].log2size & 0xffu;
        uint32_t l2h = (tex[u].log2size >> 8) ? ((tex[u].log2size >> 8) & 0xffu) : l2w;
        uint32_t lw = l2w - 3, lh = l2h - 3;
        uint32_t fmt=(tex[u].storage&(GL2_TEX_PVRTC4_RGB|GL2_TEX_PVRTC4_RGBA))?0x38u:(tex[u].storage & 0x1fu);if(!fmt)fmt=0x19u;
        if (fmt!=0x19u && fmt!=GL2_TEX_RGB565 && fmt!=GL2_TEX_ARGB4444 && fmt!=GL2_TEX_ARGB1555 && fmt!=GL2_TEX_AL88 && fmt!=0x20u && fmt!=0x38u) return -4;
        uint32_t cm=0;if(tex[u].storage&GL2_TEX_CLAMP_S)cm|=1u<<31;if(tex[u].storage&GL2_TEX_CLAMP_T)cm|=1u<<29;
        uint32_t filt=0x12;if(tex[u].storage&GL2_TEX_LINEAR)filt=0x92;
        if(tex[u].storage&GL2_TEX_MIP_NEAREST)filt=(tex[u].storage&GL2_TEX_LINEAR)?0xb2u:0x32u;
        /* Trilinear (0xce/0x4e) faulted every frame on hardware (run545, OpenLara filter MEDIUM):
         * GL2_TRILINEAR 0 samples the nearest mip level instead (0xb2/0x32). */
        else if(tex[u].storage&GL2_TEX_MIP_LINEAR)filt=GL2_TRILINEAR?((tex[u].storage&GL2_TEX_LINEAR)?0xceu:0x4eu):((tex[u].storage&GL2_TEX_LINEAR)?0xb2u:0x32u);
        out[k++] = (lw << 24) | (lh << 20) | (fmt<<12) | 0x600u | cm | ((p->dot3_mask&(1u<<u))?1u<<13:0); out[k++] = (filt<<24) | (tex[u].va >> 7); out[k++] = (w2_of(p->plan + 12 * u)&~0x7fffu)|(p->custom_alpha?p->alpha[u]:0x4610u);
    }
    out[k++] = 0x2003801au; out[k++] = (p->pack_pos ? 8u : 4u) | p->normal_input << 4 | (p->omit_colour ? 0 : 5u << 8) | GL2_UV0W(p) << 12 | ((p->pack_uv1 ? 5u : p->uv1_input) << 16); out[k++] = 0;
    {   /* viewport: panel x = ox + sx * ndc, panel y (rows from the top) = oy + sy * ndc. vp_w / vp_h
         * are already swapped for landscape; vp_x / vp_y and the target size are GL window
         * coordinates (origin bottom-left). Portrait: x = vx + w/2 (1 + ndc_x), row = H - vy - h/2 (1 + ndc_y).
         * Landscape home right (1): panel x = window y, panel y = window x; home left (2): both mirrored. */
        float hw = (float)vp_w / 2, hh = (float)vp_h / 2, vx = (float)p->vp_x, vy = (float)p->vp_y;
        float tw = p->tgt_w ? (float)p->tgt_w : (rot ? 2 * hh : 2 * hw), th = p->tgt_h ? (float)p->tgt_h : (rot ? 2 * hw : 2 * hh);
        float ox, sx, oy, sy;
        if (rot == 1u) { ox = vy + hw; sx = hw; oy = vx + hh; sy = hh; }
        else if (rot == 2u) { ox = th - vy - hw; sx = -hw; oy = tw - vx - hh; sy = -hh; }
        else { ox = vx + hw; sx = hw; oy = th - vy - hh; sy = -hh; }
        out[k++] = f2u(ox); out[k++] = f2u(sx); out[k++] = f2u(oy); out[k++] = f2u(sy); out[k++] = f2u(0.5f); out[k++] = f2u(0.5f);
    }
    out[k++] = mode == 4 ? 0x70000000u : mode == 5 ? 0x40020000u : 0x50020000u; out[k++] = 0xf0020004u | (two ? 1u << 13 : 0) | p->clip_ctl;
    return (int)k;
}
int gl2_build_draw_views_mask(const gl2_program *p,const gl2_state *st,int mode,uint32_t n,
                   const gl2_vertex_view views[5],const gl2_tex *tex,uint32_t vp_w,uint32_t vp_h,uint32_t *out,uint32_t cap,uint32_t color_mask) {
    const float *normal=views[1].data;
    uint32_t two = p->nplan > 1, stages = 0, rows = 0;
    int err = count_stages(p, &stages, &rows);
    if (err) return err;
    if (p->normal_input>4 || (p->normal_input && !normal)) return -5;
    /* Exact header size, matching gl2_build_draw_header. Divide before
     * multiplying so even an untrusted UINT32_MAX vertex count cannot wrap. */
    uint32_t header_words = (two ? 4u : 3u) + 2u * stages + 5u * (p->nlit + rows + (p->rounding_bias ? 1u : 0u)) + (two ? 20u : 17u);
    uint32_t vertex_size = 4u + p->normal_input + (p->omit_colour ? 0u : 1u) + GL2_UV0W(p) + p->uv1_input;
    if (header_words > cap || n > (cap - header_words) / vertex_size) return -1;
    int hdr = build_header(p, st, mode, tex, vp_w, vp_h, out, cap, color_mask);
    if (hdr < 0) return hdr;
    uint32_t k = (uint32_t)hdr;
    /* An interleaved float stream already has the exact native vertex layout
     * when colour is omitted. Copy complete vertices instead of revisiting
     * each attribute and multiplying five source strides per vertex. */
    uint32_t vertex_words=4u+p->normal_input+GL2_UV0W(p)+p->uv1_input;
    const void *native_source=0;
    if(gl2_native_vertex_words(p,views,&native_source)){
#define COPY_VERTEX(bytes) do{for(uint32_t i=0;i<n;++i){const void *src=__builtin_assume_aligned(views[0].data+i*vertex_words,4);void *dst=__builtin_assume_aligned(out+k,4);__builtin_memcpy(dst,src,bytes);k+=vertex_words;}return (int)k;}while(0)
        switch(vertex_words){case 6:COPY_VERTEX(24);case 7:COPY_VERTEX(28);case 8:COPY_VERTEX(32);case 9:COPY_VERTEX(36);case 10:COPY_VERTEX(40);case 11:COPY_VERTEX(44);case 12:COPY_VERTEX(48);}
#undef COPY_VERTEX
    }
    uint32_t packed_constant=0;
    if(!p->omit_colour&&p->has_constp){const float *cc=p->constp;packed_constant=round_u8(cc[3])<<24|round_u8(cc[2])<<16|round_u8(cc[1])<<8|round_u8(cc[0]);}
    for (uint32_t i = 0; i < n; ++i) {
        const float *pv=views[0].data+i*views[0].stride;
        out[k++]=f2u(pv[0]);out[k++]=f2u(pv[1]);out[k++]=f2u(pv[2]);out[k++]=f2u(pv[3]);
        for(uint32_t j=0;j<p->normal_input;++j)out[k++]=f2u(normal[i*views[1].stride+j]);
        if(!p->omit_colour){const float *cc = views[2].data ? views[2].data + views[2].stride * i : p->constp;
        out[k++] = p->has_constp ? packed_constant : round_u8(cc[3]) << 24 | round_u8(cc[2]) << 16 | round_u8(cc[1]) << 8 | round_u8(cc[0]);}
        const float *t0=views[3].data+i*views[3].stride;
        for(uint32_t j=0;j<GL2_UV0W(p);++j)out[k++]=f2u(t0[j]);
        if (p->uv1_input) {const float *t1=views[4].data+i*views[4].stride;for(uint32_t j=0;j<p->uv1_input;++j)out[k++]=f2u(t1[j]);}
    }
    return (int)k;
}

int gl2_build_draw_header(const gl2_program *p,const gl2_state *st,int mode,
 const gl2_tex *tex,uint32_t vp_w,uint32_t vp_h,uint32_t *out,uint32_t cap){
 return build_header(p,st,mode,tex,vp_w,vp_h,out,cap,0xfu);
}
int gl2_build_draw_header_mask(const gl2_program *p,const gl2_state *st,int mode,
 const gl2_tex *tex,uint32_t vp_w,uint32_t vp_h,uint32_t *out,uint32_t cap,uint32_t color_mask){
 return build_header(p,st,mode,tex,vp_w,vp_h,out,cap,color_mask);
}
int gl2_build_draw_views(const gl2_program *p,const gl2_state *st,int mode,uint32_t n,
 const gl2_vertex_view views[5],const gl2_tex *tex,uint32_t vp_w,uint32_t vp_h,uint32_t *out,uint32_t cap){
 return gl2_build_draw_views_mask(p,st,mode,n,views,tex,vp_w,vp_h,out,cap,0xfu);
}
int gl2_build_draw(const gl2_program *p,const gl2_state *st,int mode,uint32_t n,
 const float *pos,const float *col,const float *uv0,const float *uv1,const gl2_tex *tex,
 uint32_t vp_w,uint32_t vp_h,uint32_t *out,uint32_t cap){
 return gl2_build_draw_inputs(p,st,mode,n,pos,0,col,uv0,uv1,tex,vp_w,vp_h,out,cap);
}

int gl2_build_draw_inputs_mask(const gl2_program *p,const gl2_state *st,int mode,uint32_t n,const float *pos,const float *normal,const float *col,const float *uv0,const float *uv1,const gl2_tex *tex,uint32_t w,uint32_t h,uint32_t *out,uint32_t cap,uint32_t color_mask){
 const gl2_vertex_view views[5]={{pos,4},{normal,4},{col,4},{uv0,GL2_UV0W(p)},{uv1?uv1:uv0,p->uv1_input>2?p->uv1_input:2}};
 return gl2_build_draw_views_mask(p,st,mode,n,views,tex,w,h,out,cap,color_mask);
}
int gl2_build_draw_inputs(const gl2_program *p,const gl2_state *st,int mode,uint32_t n,const float *pos,const float *normal,const float *col,const float *uv0,const float *uv1,const gl2_tex *tex,uint32_t w,uint32_t h,uint32_t *out,uint32_t cap){
 return gl2_build_draw_inputs_mask(p,st,mode,n,pos,normal,col,uv0,uv1,tex,w,h,out,cap,0xfu);
}
