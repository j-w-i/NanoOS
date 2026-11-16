#include "gl2api.h"
static int scmp(const char *a, const char *b) { while (*a && *a == *b) { ++a; ++b; } return *a - *b; }
static void scpy(char *d, const char *s, int n) { int i = 0; for (; i < n - 1 && s[i]; ++i) d[i] = s[i]; d[i] = 0; }
uint32_t gl2_hash(const char *s) { uint32_t h = 2166136261u; while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; } return h; }
static void err(gl2_ctx *c, uint32_t e) { if (!c->err) c->err = e; }
void gl2_init(gl2_ctx *c, gl2_prog_obj *progs, uint32_t *segbuf, uint32_t seg_cap, gl2_submit_fn fn, void *user) {
    uint8_t *p = (uint8_t *)c; for (uint32_t i = 0; i < sizeof *c; ++i) p[i] = 0;
    c->pr = progs; for (int i = 0; i < 4; ++i) { progs[i].used = 0; progs[i].ok = 0; }
    c->seg = segbuf; c->seg_cap = seg_cap; c->submit = fn; c->submit_user = user;
    c->st.depth_func = 0x201; c->st.depth_write = 1; c->st.src = 1; c->st.dst = 0; c->vp_w = 240; c->vp_h = 432;
}
void gl2_set_fs_cache(gl2_ctx *c, const gl2_fs_entry *e, uint32_t n) { c->cache = e; c->ncache = n; }
uint32_t glGetError(gl2_ctx *c) { uint32_t e = c->err; c->err = 0; return e; }
uint32_t glCreateShader(gl2_ctx *c, uint32_t type) {
    if (type != GL_VERTEX_SHADER && type != GL_FRAGMENT_SHADER) { err(c, GL_INVALID_ENUM); return 0; }
    for (uint32_t i = 1; i < 8; ++i) if (!c->sh[i].used) { c->sh[i].used = 1; c->sh[i].type = type; c->sh[i].ok = 0; c->sh[i].log[0] = 0; c->sh[i].vs = 0; c->sh[i].fs = 0; return i; }
    err(c, GL_INVALID_OPERATION); return 0;     /* (real GL would be OUT_OF_MEMORY) */
}
static gl2_shader *shp(gl2_ctx *c, uint32_t sh) { if (sh == 0 || sh >= 8 || !c->sh[sh].used) { err(c, GL_INVALID_VALUE); return 0; } return &c->sh[sh]; }
static const char *src_of[8];
void glShaderSource(gl2_ctx *c, uint32_t sh, const char *src) { gl2_shader *s = shp(c, sh); if (!s) return; src_of[sh] = src; s->src_hash = gl2_hash(src); }
void glCompileShader(gl2_ctx *c, uint32_t sh) {
    gl2_shader *s = shp(c, sh); if (!s) return; s->ok = 0; s->log[0] = 0; s->vs = 0; s->fs = 0;
    if (s->type == GL_VERTEX_SHADER) {
        vsc_result *r = &c->sh_vs[sh & 3];      /* up to 4 live vertex shaders */
        if (gl2vsc_compile(src_of[sh], r) == 0) { s->ok = 1; s->vs = r; } else scpy(s->log, r->err, 64);
    } else {
        for (uint32_t i = 0; i < c->ncache; ++i) if (c->cache[i].hash == s->src_hash) { s->fs = &c->cache[i]; s->ok = 1; return; }
        scpy(s->log, "fragment shader not in the precompiled cache", 64);
    }
}
int glGetShaderiv(gl2_ctx *c, uint32_t sh, uint32_t pname) { gl2_shader *s = shp(c, sh); if (!s) return 0; return pname == GL_COMPILE_STATUS ? (int)s->ok : 0; }
const char *glGetShaderInfoLog(gl2_ctx *c, uint32_t sh) { gl2_shader *s = shp(c, sh); return s ? s->log : ""; }
uint32_t glCreateProgram(gl2_ctx *c) {
    for (uint32_t i = 1; i < 4; ++i) if (!c->pr[i].used) { c->pr[i].used = 1; c->pr[i].ok = 0; c->pr[i].vsh = c->pr[i].fsh = 0; c->pr[i].log[0] = 0; return i; }
    err(c, GL_INVALID_OPERATION); return 0;
}
static gl2_prog_obj *prp(gl2_ctx *c, uint32_t p) { if (p == 0 || p >= 4 || !c->pr[p].used) { err(c, GL_INVALID_VALUE); return 0; } return &c->pr[p]; }
void glAttachShader(gl2_ctx *c, uint32_t p, uint32_t sh) {
    gl2_prog_obj *P = prp(c, p); gl2_shader *S = shp(c, sh); if (!P || !S) return;
    uint32_t *slot = S->type == GL_VERTEX_SHADER ? &P->vsh : &P->fsh;
    if (*slot) { err(c, GL_INVALID_OPERATION); return; }
    *slot = sh;
}
void glLinkProgram(gl2_ctx *c, uint32_t p) {
    gl2_prog_obj *P = prp(c, p); if (!P) return; P->ok = 0;
    if (!P->vsh || !P->fsh || !c->sh[P->vsh].ok || !c->sh[P->fsh].ok) { scpy(P->log, "missing or uncompiled shader", 64); return; }
    P->vs = *c->sh[P->vsh].vs; P->fs = c->sh[P->fsh].fs;
    uint32_t has_col = 0, has_uv = 0; for (uint32_t i = 0; i < P->vs.nvary; ++i) { if (P->vs.vary_slot[i] == 1) has_col = 1; if (P->vs.vary_slot[i] == 3) has_uv = 1; }
    if (P->fs->needs_colour && !has_col) { scpy(P->log, "fragment shader reads a colour varying the vertex shader does not write", 64); return; }
    if (P->fs->nsampler && !has_uv) { scpy(P->log, "fragment shader samples a texture but the vertex shader writes no texcoord", 64); return; }
    if (P->fs->has_constp && !has_col) { scpy(P->log, "vertex shader needs a vec4 colour varying for this fragment shader", 64); return; }
    /* assemble a gl2_program directly from the compiled pieces (no serialised blob on the device) */
    gl2_program *g = &P->prog; g->nstage = P->vs.ncode; g->nplan = P->fs->nplan; g->code = &P->vs.code[0][0];
    g->nlit = P->vs.nlit; g->lit_idx = P->vs.lit_idx; g->lit_val = &P->vs.lit_val[0][0]; g->nuni = P->vs.nuni;
    static uint8_t uidx[4][16], ucnt[4][16]; for (uint32_t i = 0; i < P->vs.nuni; ++i) { uidx[p][i] = P->vs.uni_idx[i]; ucnt[p][i] = P->vs.uni_cnt[i]; }
    g->uni_idx = uidx[p]; g->uni_cnt = ucnt[p]; g->uni_names = 0; g->plan = &P->fs->plan[0][0]; g->has_constp = P->fs->has_constp;
    for (int i = 0; i < 4; ++i) g->constp[i] = P->fs->constp[i];
    for (uint32_t i = 0; i < GL2_MAX_UNI; ++i) g->uset[i] = 0;
    P->ok = 1; scpy(P->log, "linked", 64);
}
int glGetProgramiv(gl2_ctx *c, uint32_t p, uint32_t pname) { gl2_prog_obj *P = prp(c, p); if (!P) return 0; return pname == GL_LINK_STATUS ? (int)P->ok : 0; }
const char *glGetProgramInfoLog(gl2_ctx *c, uint32_t p) { gl2_prog_obj *P = prp(c, p); return P ? P->log : ""; }
void glUseProgram(gl2_ctx *c, uint32_t p) { if (p && (p >= 4 || !c->pr[p].used || !c->pr[p].ok)) { err(c, GL_INVALID_OPERATION); return; } c->cur = p; }
int glGetUniformLocation(gl2_ctx *c, uint32_t p, const char *name) {
    gl2_prog_obj *P = prp(c, p); if (!P || !P->ok) { err(c, GL_INVALID_OPERATION); return -1; }
    for (uint32_t i = 0; i < P->vs.nuni; ++i) { if (!scmp(P->vs.uni_name[i], name)) return (int)i; }
    return -1;
}
int glGetAttribLocation(gl2_ctx *c, uint32_t p, const char *name) {
    gl2_prog_obj *P = prp(c, p); if (!P || !P->ok) { err(c, GL_INVALID_OPERATION); return -1; }
    for (uint32_t i = 0; i < P->vs.nattr; ++i) { if (!scmp(P->vs.attr_name[i], name)) return P->vs.attr_slot[i]; }
    return -1;
}
static gl2_prog_obj *cur(gl2_ctx *c) { if (!c->cur) { err(c, GL_INVALID_OPERATION); return 0; } return &c->pr[c->cur]; }
void glUniform4f(gl2_ctx *c, int loc, float x, float y, float z, float w) { gl2_prog_obj *P = cur(c); if (!P) return; if (loc == -1) return; if (loc < 0 || (uint32_t)loc >= P->vs.nuni || P->vs.uni_cnt[loc] != 1) { err(c, GL_INVALID_OPERATION); return; } gl2_uniform4f(&P->prog, loc, x, y, z, w); }
void glUniform1f(gl2_ctx *c, int loc, float x) { glUniform4f(c, loc, x, 0, 0, 0); }
void glUniformMatrix4fv(gl2_ctx *c, int loc, int count, int transpose, const float *m) {
    gl2_prog_obj *P = cur(c); if (!P || loc == -1) return;
    if (transpose) { err(c, GL_INVALID_VALUE); return; }
    if (loc < 0 || (uint32_t)loc >= P->vs.nuni || P->vs.uni_cnt[loc] != 4 || count != 1) { err(c, GL_INVALID_OPERATION); return; }
    gl2_uniform_matrix4(&P->prog, loc, m);
}
void glEnableVertexAttribArray(gl2_ctx *c, uint32_t loc) { if (loc > 4) { err(c, GL_INVALID_VALUE); return; } c->attr[loc].enabled = 1; }
void glDisableVertexAttribArray(gl2_ctx *c, uint32_t loc) { if (loc > 4) { err(c, GL_INVALID_VALUE); return; } c->attr[loc].enabled = 0; }
void glVertexAttribPointer(gl2_ctx *c, uint32_t loc, int size, uint32_t type, int normalized, int stride, const void *ptr) {
    if (loc > 4 || size < 1 || size > 4 || stride < 0) { err(c, GL_INVALID_VALUE); return; }
    if (type != 0x1406 || normalized) { err(c, GL_INVALID_ENUM); return; }      /* floats only */
    c->attr[loc].ptr = (const float *)ptr; c->attr[loc].size = (uint32_t)size; c->attr[loc].stride = stride ? (uint32_t)stride / 4 : (uint32_t)size;
}
void glBlendFunc(gl2_ctx *c, uint32_t s, uint32_t d) { c->st.src = s; c->st.dst = d; }
void glDepthFunc(gl2_ctx *c, uint32_t f) { if (f < 0x200 || f > 0x207) { err(c, GL_INVALID_ENUM); return; } c->st.depth_func = f; }
void glDepthMask(gl2_ctx *c, int flag) { c->st.depth_write = flag != 0; }
void glEnable(gl2_ctx *c, uint32_t cap) { if (cap == 0xBE2) c->st.blend = 1; else if (cap == 0xB71) c->st.depth_test = 1; else err(c, GL_INVALID_ENUM); }
void glDisable(gl2_ctx *c, uint32_t cap) { if (cap == 0xBE2) c->st.blend = 0; else if (cap == 0xB71) c->st.depth_test = 0; else err(c, GL_INVALID_ENUM); }
void glViewport(gl2_ctx *c, int x, int y, int w, int h) { (void)x; (void)y; if (w < 0 || h < 0) { err(c, GL_INVALID_VALUE); return; } c->vp_w = (uint32_t)w; c->vp_h = (uint32_t)h; }
void gl2_bind_texture(gl2_ctx *c, uint32_t unit, uint32_t va, uint32_t l2) { if (unit < 2) { c->tex[unit].va = va; c->tex[unit].log2size = l2; } }
void glDrawArrays(gl2_ctx *c, uint32_t mode, int first, int count) {
    if (mode < 4 || mode > 6) { err(c, GL_INVALID_ENUM); return; }
    if (first < 0 || count < 0) { err(c, GL_INVALID_VALUE); return; }
    gl2_prog_obj *P = cur(c); if (!P) return;
    if (!c->attr[0].enabled || !c->attr[0].ptr) { err(c, GL_INVALID_OPERATION); return; }
    if (count == 0) return;
    static float pos[1024], col[1024], uv0[512], uv1[512];
    if (count > 256) { err(c, GL_INVALID_VALUE); return; }          /* per-call vertex cap of this prototype */
    for (int i = 0; i < count; ++i) {
        const float *p = c->attr[0].ptr + (uint32_t)(first + i) * c->attr[0].stride;
        for (uint32_t k = 0; k < 4; ++k) pos[4 * i + k] = k < c->attr[0].size ? p[k] : (k == 3 ? 1.0f : 0.0f);
        for (uint32_t k = 0; k < 4; ++k) col[4 * i + k] = 1.0f;
        if (c->attr[2].enabled && c->attr[2].ptr) { const float *q = c->attr[2].ptr + (uint32_t)(first + i) * c->attr[2].stride; for (uint32_t k = 0; k < 4; ++k) col[4 * i + k] = k < c->attr[2].size ? q[k] : (k == 3 ? 1.0f : 0.0f); }
        uv0[2 * i] = uv0[2 * i + 1] = 0; uv1[2 * i] = uv1[2 * i + 1] = 0;
        if (c->attr[3].enabled && c->attr[3].ptr) { const float *q = c->attr[3].ptr + (uint32_t)(first + i) * c->attr[3].stride; uv0[2 * i] = q[0]; uv0[2 * i + 1] = c->attr[3].size > 1 ? q[1] : 0; }
        if (c->attr[4].enabled && c->attr[4].ptr) { const float *q = c->attr[4].ptr + (uint32_t)(first + i) * c->attr[4].stride; uv1[2 * i] = q[0]; uv1[2 * i + 1] = c->attr[4].size > 1 ? q[1] : 0; }
    }
    int n = gl2_build_draw(&P->prog, &c->st, (int)mode, (uint32_t)count, pos, col, uv0, c->attr[4].enabled ? uv1 : 0, c->tex, c->vp_w, c->vp_h, c->seg, c->seg_cap);
    if (n < 0) { err(c, GL_INVALID_OPERATION); return; }
    if (c->submit) c->submit(c->seg, (uint32_t)n, c->submit_user);
}
