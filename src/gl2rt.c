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
    p->uval[loc][0][0] = x; p->uval[loc][0][1] = y; p->uval[loc][0][2] = z; p->uval[loc][0][3] = w; p->uset[loc] = 1;
}
void gl2_uniform_matrix4(gl2_program *p, int loc, const float *m) {
    if (loc < 0 || (uint32_t)loc >= p->nuni) return;
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) p->uval[loc][r][c] = m[c * 4 + r];   /* column-major in -> rows */
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
int gl2_build_draw(const gl2_program *p, const gl2_state *st, int mode, uint32_t n, const float *pos, const float *col, const float *uv0,
                   const float *uv1, const gl2_tex *tex, uint32_t vp_w, uint32_t vp_h, uint32_t *out, uint32_t cap) {
    uint32_t k = 0, two = p->nplan > 1;
    uint32_t rows = 0;
    for (uint32_t u = 0; u < p->nuni; ++u) if (p->uset[u]) rows += p->uni_cnt[u];
    /* upper bound: header 3-4, program 2*nstage, constants 5 words per literal/row, state/layers/viewport/prim ~ 40, vertices */
    uint32_t need = 4 + 2 * p->nstage + 5 * (p->nlit + rows) + 40 + n * (4 + 1 + 2 + (two ? 2 : 0));
    if (need > cap) return -1;
    if (mode < 4 || mode > 6) return -2;
    uint32_t desc = (0x05u << 24) | ((p->nstage - 1) << 8);
    out[k++] = 0x80000004u | (two ? 1u << 13 : 0); out[k++] = desc; if (two) out[k++] = desc; out[k++] = 0x90000000u | (p->nstage - 1);
    for (uint32_t i = 0; i < p->nstage; ++i) { out[k++] = p->code[2 * i]; out[k++] = p->code[2 * i + 1]; }
    /* constants: literals and uniform rows, ascending index (the Python builder emits sorted(cst)) */
    {
        uint32_t idx[GL2_MAX_UNI * 4 + 64], cnt = 0; const float *val[GL2_MAX_UNI * 4 + 64];
        for (uint32_t i = 0; i < p->nlit; ++i) { idx[cnt] = p->lit_idx[i]; val[cnt] = p->lit_val + 4 * i; ++cnt; }
        for (uint32_t u = 0; u < p->nuni; ++u) if (p->uset[u])
            for (uint32_t r = 0; r < p->uni_cnt[u]; ++r) {
                uint32_t ci = p->uni_idx[u] + r, j;
                for (j = 0; j < cnt; ++j) if (idx[j] == ci) break;           /* a uniform row overrides a literal at the same index */
                if (j == cnt) ++cnt;
                idx[j] = ci; val[j] = p->uval[u][r];
            }
        for (uint32_t a = 1; a < cnt; ++a) { uint32_t ti = idx[a]; const float *tv = val[a]; uint32_t b = a; while (b && idx[b - 1] > ti) { idx[b] = idx[b - 1]; val[b] = val[b - 1]; --b; } idx[b] = ti; val[b] = tv; }
        for (uint32_t i = 0; i < cnt; ++i) { out[k++] = 0xa0000000u | (idx[i] << 16) | idx[i]; for (int c = 0; c < 4; ++c) out[k++] = f2u(val[i][c]); }
    }
    uint32_t isp = two ? 0x22650200u : 0x22620200u;
    isp &= ~(0xFC000000u | 0xE00u | 0x80u | 0x2000u);
    uint32_t zc = st->depth_test ? st->depth_func - 0x200u : 7u;
    isp |= zc << 9;
    if (!st->depth_test || !st->depth_write) isp |= 0x80u;
    if (st->blend) { int s = tsp(st->src, 1), d = tsp(st->dst, 0); if (s < 0 || d < 0) return -3; isp |= ((uint32_t)s << 29) | ((uint32_t)d << 26) | 0x2000u; }
    else isp |= 1u << 29;
    out[k++] = 0x10000000u | (0xf7u | (two ? 0x08u : 0)); out[k++] = isp; out[k++] = 0xe0000000u;
    for (uint32_t u = 0; u < (two ? 2u : 1u); ++u) {
        uint32_t l2 = tex[u].log2size - 3;
        out[k++] = (l2 << 24) | (l2 << 20) | 0x00019600u; out[k++] = 0x12000000u | (tex[u].va >> 7); out[k++] = w2_of(p->plan + 12 * u);
    }
    out[k++] = 0x2003801au; out[k++] = 0x4u | 5u << 8 | 2u << 12 | (two ? 2u << 16 : 0); out[k++] = 0;
    out[k++] = f2u((float)vp_w / 2); out[k++] = f2u((float)vp_w / 2); out[k++] = f2u((float)vp_h / 2); out[k++] = f2u(-(float)vp_h / 2); out[k++] = f2u(0.5f); out[k++] = f2u(0.5f);
    out[k++] = mode == 4 ? 0x70000000u : mode == 5 ? 0x40020000u : 0x50020000u; out[k++] = 0xf0020004u | (two ? 1u << 13 : 0);
    for (uint32_t i = 0; i < n; ++i) {
        for (int c = 0; c < 4; ++c) out[k++] = f2u(pos[4 * i + c]);
        const float *cc = p->has_constp ? p->constp : col + 4 * i;
        out[k++] = round_u8(cc[3]) << 24 | round_u8(cc[2]) << 16 | round_u8(cc[1]) << 8 | round_u8(cc[0]);
        out[k++] = f2u(uv0[2 * i]); out[k++] = f2u(uv0[2 * i + 1]);
        if (two) { const float *u1 = uv1 ? uv1 : uv0; out[k++] = f2u(u1[2 * i]); out[k++] = f2u(u1[2 * i + 1]); }
    }
    return (int)k;
}
