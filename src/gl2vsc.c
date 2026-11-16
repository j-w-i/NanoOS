#include "gl2vsc.h"
#define MAXTOK 700
#define TLEN 24
typedef struct { int f, i, w, hascv, mat; int sw[4]; float cv[4]; } V;           /* f: 0 input 1 const 2 temp 3 zero */
typedef struct { int op, dst, mask, hi, neg[3]; V s[3]; } Ins;
enum { MOV = 1, ADD = 2, MAD = 3, MUL = 4, RCP = 5, RSQ = 6, DP3 = 7, DP4 = 8, MIN = 9, MAX = 10, SGE = 12, EX2 = 0x13 };
static char tk[MAXTOK][TLEN]; static int ntk, tp;
static struct { char n[TLEN]; V v; int local; } vars[48]; static int nvars;
static struct { char n[TLEN]; int w, slot; } outs[8]; static int nouts;
static vsc_result *R; static int nc, nreg, ncode, written_pos, err_flag;
static Ins code[VSC_MAX_INSTR];
static int seq(const char *a, const char *b) { while (*a && *a == *b) { ++a; ++b; } return *a == *b; }
static void scpy(char *d, const char *s, int n) { int i = 0; for (; i < n - 1 && s[i]; ++i) d[i] = s[i]; d[i] = 0; }
static void fail(const char *m) { if (!err_flag) { err_flag = 1; scpy(R->err, m, 64); } }
static int isid(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int isdig(char c) { return c >= '0' && c <= '9'; }
static void tokenize(const char *s) {
    ntk = 0;
    while (*s) {
        if (*s == ' ' || *s == '\n' || *s == '\t' || *s == '\r') { ++s; continue; }
        if (s[0] == '/' && s[1] == '/') { while (*s && *s != '\n') ++s; continue; }
        if (ntk >= MAXTOK - 1) { fail("too many tokens"); return; }
        int n = 0;
        if (isid(*s)) { while ((isid(*s) || isdig(*s)) && n < TLEN - 1) tk[ntk][n++] = *s++; }
        else if (isdig(*s) || (*s == '.' && isdig(s[1]))) {
            while ((isdig(*s) || *s == '.') && n < TLEN - 1) tk[ntk][n++] = *s++;
            if ((*s == 'e' || *s == 'E') && n < TLEN - 3) { tk[ntk][n++] = *s++; if (*s == '-' || *s == '+') tk[ntk][n++] = *s++; while (isdig(*s) && n < TLEN - 1) tk[ntk][n++] = *s++; }
        } else tk[ntk][n++] = *s++;
        tk[ntk][n] = 0; ++ntk;
    }
    tk[ntk][0] = 0;
}
static const char *peek(int k) { return tp + k < ntk ? tk[tp + k] : ""; }
static const char *eat(const char *x) { const char *v = peek(0); if (!*v || (x && !seq(v, x))) { fail("unexpected token"); return ""; } ++tp; return v; }
static float atof_(const char *s) {
    float v = 0, f = 1; int dot = 0;
    for (; *s && *s != 'e' && *s != 'E'; ++s) { if (*s == '.') dot = 1; else if (dot) { f *= 0.1f; v += (float)(*s - '0') * f; } else v = v * 10 + (float)(*s - '0'); }
    if (*s) { ++s; int sg = 1, e = 0; if (*s == '-') { sg = -1; ++s; } else if (*s == '+') ++s; while (isdig(*s)) e = e * 10 + (*s++ - '0'); while (e--) v = sg > 0 ? v * 10 : v / 10; }
    return v;
}
/* ---- value helpers */
static V mkv(int f, int i, int w) { V v; v.f = f; v.i = i; v.w = w; v.hascv = 0; v.mat = 0; for (int k = 0; k < 4; ++k) { v.sw[k] = k < w ? k : w - 1; v.cv[k] = 0; } return v; }
static V cnst(const float *vals, int w) {
    float key[4] = { 0, 0, 0, 0 }; for (int k = 0; k < w; ++k) key[k] = vals[k];
    int idx = -1;
    for (uint32_t j = 0; j < R->nlit; ++j) { int same = 1; for (int k = 0; k < 4; ++k) if (R->lit_val[j][k] != key[k]) same = 0; if (same) { idx = R->lit_idx[j]; break; } }
    if (idx < 0) {
        if (R->nlit >= VSC_MAX_LIT || nc >= 112) { fail("out of constants"); idx = 0; }
        else { idx = nc++; R->lit_idx[R->nlit] = (uint16_t)idx; for (int k = 0; k < 4; ++k) R->lit_val[R->nlit][k] = key[k]; ++R->nlit; }
    }
    V v = mkv(1, idx, w); v.hascv = 1; for (int k = 0; k < w; ++k) v.cv[k] = vals[k]; return v;
}
static V swz(V v, const int *idx, int n) {
    V o = v; for (int k = 0; k < n; ++k) { o.sw[k] = v.sw[idx[k]]; o.cv[k] = v.cv[idx[k]]; }
    for (int k = n; k < 4; ++k) o.sw[k] = o.sw[n - 1];
    o.w = n; return o;
}
static int newtmp(void) { return nreg++; }
static void emit(int op, int dst, int mask, V *srcs, int nsrc, int neg0, int neg1) {
    Ins in; in.op = op; in.dst = dst; in.mask = mask; in.hi = 0; in.neg[0] = neg0; in.neg[1] = neg1; in.neg[2] = 0;
    for (int k = 0; k < 3; ++k) in.s[k] = k < nsrc ? srcs[k] : mkv(3, 0, 4);
    int first = -1;
    for (int k = 0; k < 3; ++k) if (in.s[k].f == 1) {
        if (first < 0) first = in.s[k].i;
        else if (in.s[k].i != first) {            /* hardware: one distinct constant per instruction -> copy the extra one to a temp first */
            int t = newtmp(); V full = mkv(1, in.s[k].i, 4); V sw = in.s[k];
            emit(MOV, 16 + t, 15, &full, 1, 0, 0);
            in.s[k] = mkv(2, t, sw.w); for (int j = 0; j < 4; ++j) in.s[k].sw[j] = sw.sw[j];
        }
    }
    for (int k = 0; k < 3; ++k) if (in.s[k].f == 1) in.hi = in.s[k].i >> 4;
    if (ncode >= VSC_MAX_INSTR) { fail("too many instructions"); return; }
    code[ncode++] = in;
}
static V tmp(int op, V *s, int ns, int w, int neg0, int neg1) { int r = newtmp(); emit(op, 16 + r, (1 << w) - 1, s, ns, neg0, neg1); return mkv(2, r, w); }
static V bc(V v, int w) { if (v.w == w) return v; if (v.w != 1) { fail("vector size mismatch"); return v; } V o = v; o.w = w; for (int k = 0; k < 4; ++k) { o.sw[k] = v.sw[0]; o.cv[k] = v.cv[0]; } return o; }
static int allval(const V *a, float x, int w) { if (!a->hascv) return 0; for (int k = 0; k < w; ++k) if (a->cv[k] != x) return 0; return 1; }
static V binop(char op, V a, V b) {
    if (a.mat || b.mat) { fail("bad matrix use"); return a; }
    int w = a.w > b.w ? a.w : b.w; a = bc(a, w); b = bc(b, w);
    if (a.hascv && b.hascv) {
        float r[4]; for (int k = 0; k < w; ++k) r[k] = op == '+' ? a.cv[k] + b.cv[k] : op == '-' ? a.cv[k] - b.cv[k] : op == '*' ? a.cv[k] * b.cv[k] : (b.cv[k] != 0 ? a.cv[k] / b.cv[k] : 3.0e38f);
        return cnst(r, w);
    }
    if (op == '*' && allval(&a, 1, w)) return b;
    if (op == '*' && allval(&b, 1, w)) return a;
    if ((op == '+' || op == '-') && allval(&b, 0, w)) return a;
    if (op == '*' && (allval(&a, 0, w) || allval(&b, 0, w))) { float z[4] = { 0, 0, 0, 0 }; return cnst(z, w); }
    if (op == '/') {
        if (b.hascv) { float r[4]; for (int k = 0; k < w; ++k) r[k] = b.cv[k] != 0 ? 1.0f / b.cv[k] : 3.0e38f; return binop('*', a, cnst(r, w)); }
        if (b.w != 1) { fail("1/vector unsupported"); return a; }
        V s[1] = { b }; V rc = tmp(RCP, s, 1, 1, 0, 0); return binop('*', a, rc);
    }
    V s[2] = { a, b }; return tmp(op == '*' ? MUL : ADD, s, 2, w, 0, op == '-');
}
static V dot_(V a, V b) {
    int n = a.w > b.w ? a.w : b.w; a = bc(a, n); b = bc(b, n);
    if (a.hascv && b.hascv) { float r = 0; for (int k = 0; k < n; ++k) r += a.cv[k] * b.cv[k]; return cnst(&r, 1); }
    if (n == 1) return binop('*', a, b);
    V s[2] = { a, b };
    if (n == 2) { V m = tmp(MUL, s, 2, 2, 0, 0); int i0[1] = { 0 }, i1[1] = { 1 }; V t[2] = { swz(m, i0, 1), swz(m, i1, 1) }; return tmp(ADD, t, 2, 1, 0, 0); }
    return tmp(n == 3 ? DP3 : DP4, s, 2, 1, 0, 0);
}
/* ---- parser */
static V expr(void);
static int ty_w(const char *t) { return seq(t, "float") ? 1 : seq(t, "vec2") ? 2 : seq(t, "vec3") ? 3 : seq(t, "vec4") ? 4 : 0; }
static V call(const char *name, V *a, int n) {
    int w; V z = mkv(3, 0, 1);
    if ((w = ty_w(name))) {
        V flat[16]; int nf = 0;
        for (int i = 0; i < n; ++i) for (int c = 0; c < a[i].w && nf < 16; ++c) { int ix[1] = { c }; flat[nf++] = swz(a[i], ix, 1); }
        if (n == 1 && a[0].w == 1) return bc(a[0], w);
        if (nf < w) { fail("too few constructor components"); return z; }
        if (nf > w && n != 1) { fail("too many constructor components"); return z; }
        int allc = 1; for (int k = 0; k < w; ++k) if (!flat[k].hascv) allc = 0;
        if (allc) { float r[4]; for (int k = 0; k < w; ++k) r[k] = flat[k].cv[0]; return cnst(r, w); }
        if (n == 1) { int ix[4] = { 0, 1, 2, 3 }; return swz(a[0], ix, w); }
        int r = newtmp(), pos = 0;
        for (int i = 0; i < n && pos < w; ++i) {
            int k = a[i].w < w - pos ? a[i].w : w - pos; V s = a[i]; s.w = 4; int sw[4];
            for (int j = 0; j < 4; ++j) sw[j] = a[i].sw[0];
            for (int j = 0; j < k; ++j) sw[pos + j] = a[i].sw[j];
            for (int j = 0; j < 4; ++j) s.sw[j] = sw[j];
            s.hascv = 0;
            emit(MOV, 16 + r, ((1 << k) - 1) << pos, &s, 1, 0, 0); pos += k;
        }
        return mkv(2, r, w);
    }
    if (seq(name, "dot")) { if (n != 2) { fail("dot args"); return z; } return dot_(a[0], a[1]); }
    if (seq(name, "min") || seq(name, "max")) {
        if (n != 2) { fail("min/max args"); return z; }
        int w2 = a[0].w > a[1].w ? a[0].w : a[1].w; V x = bc(a[0], w2), y = bc(a[1], w2);
        if (x.hascv && y.hascv) { float r[4]; for (int k = 0; k < w2; ++k) r[k] = seq(name, "min") ? (x.cv[k] < y.cv[k] ? x.cv[k] : y.cv[k]) : (x.cv[k] > y.cv[k] ? x.cv[k] : y.cv[k]); return cnst(r, w2); }
        V s[2] = { x, y }; return tmp(seq(name, "min") ? MIN : MAX, s, 2, w2, 0, 0);
    }
    if (seq(name, "abs")) {
        if (n != 1) { fail("abs args"); return z; }
        if (a[0].hascv) { float r[4]; for (int k = 0; k < a[0].w; ++k) r[k] = a[0].cv[k] < 0 ? -a[0].cv[k] : a[0].cv[k]; return cnst(r, a[0].w); }
        V s1[1] = { a[0] }; V ng = tmp(MOV, s1, 1, a[0].w, 1, 0); V s2[2] = { a[0], ng }; return tmp(MAX, s2, 2, a[0].w, 0, 0);
    }
    if (seq(name, "clamp")) {
        if (n != 3) { fail("clamp args"); return z; }
        V m1[2] = { a[0], bc(a[1], a[0].w) }; V lo = call("max", m1, 2); V m2[2] = { lo, bc(a[2], a[0].w) }; return call("min", m2, 2);
    }
    if (seq(name, "mix")) {
        if (n != 3) { fail("mix args"); return z; }
        int w2 = a[0].w > a[1].w ? a[0].w : a[1].w; V x = bc(a[0], w2), y = bc(a[1], w2), t = bc(a[2], w2); V d = binop('-', y, x);
        if (d.hascv && t.hascv) return binop('+', x, binop('*', d, t));
        V s[3] = { d, t, x }; return tmp(MAD, s, 3, w2, 0, 0);
    }
    if (seq(name, "step")) {
        if (n != 2) { fail("step args"); return z; }
        int w2 = a[0].w > a[1].w ? a[0].w : a[1].w; V s[2] = { bc(a[1], w2), bc(a[0], w2) }; return tmp(SGE, s, 2, w2, 0, 0);
    }
    if (seq(name, "inversesqrt")) { if (n != 1) { fail("args"); return z; } V s[1] = { a[0] }; return tmp(RSQ, s, 1, 1, 0, 0); }
    if (seq(name, "sqrt")) { if (n != 1 || a[0].w != 1) { fail("sqrt of non-scalar unsupported"); return z; } V s[1] = { a[0] }; V r = tmp(RSQ, s, 1, 1, 0, 0); return binop('*', a[0], r); }
    if (seq(name, "length")) { if (n != 1) { fail("args"); return z; } V d = dot_(a[0], a[0]); V s[1] = { d }; V r = tmp(RSQ, s, 1, 1, 0, 0); return binop('*', d, r); }
    if (seq(name, "normalize")) {
        if (n != 1 || (a[0].w != 3 && a[0].w != 4)) { fail("normalize needs vec3/vec4"); return z; }
        V d = dot_(a[0], a[0]); V s[1] = { d }; V r = tmp(RSQ, s, 1, 1, 0, 0); return binop('*', a[0], r);
    }
    if (seq(name, "exp2")) { if (n != 1) { fail("args"); return z; } V s[1] = { a[0] }; return tmp(EX2, s, 1, 1, 0, 0); }
    fail("unsupported function"); return z;
}
static V post(V v) {
    while (*peek(0) == '.' && !peek(0)[1]) {
        eat("."); const char *m = eat(0); int idx[4], n = 0; const char *sets[3] = { "xyzw", "rgba", "stpq" }; int ok = 0;
        for (int si = 0; si < 3 && !ok; ++si) { n = 0; ok = 1; for (const char *c = m; *c; ++c) { int f = -1; for (int j = 0; j < 4; ++j) if (sets[si][j] == *c) f = j; if (f < 0 || n >= 4) { ok = 0; break; } idx[n++] = f; } }
        if (!ok) { fail("bad swizzle"); return v; }
        for (int k = 0; k < n; ++k) if (idx[k] >= v.w) { fail("swizzle out of range"); return v; }
        v = swz(v, idx, n);
    }
    return v;
}
static V primary(void) {
    const char *t = eat(0); V z = mkv(3, 0, 1);
    if (seq(t, "(")) { V v = expr(); eat(")"); return post(v); }
    if (isdig(t[0]) || t[0] == '.') { float f = atof_(t); return post(cnst(&f, 1)); }
    if (seq(peek(0), "(")) {
        char name[TLEN]; scpy(name, t, TLEN); eat("("); V args[8]; int n = 0;
        while (!seq(peek(0), ")") && *peek(0) && n < 8) { args[n++] = expr(); if (seq(peek(0), ",")) eat(","); }
        eat(")"); return post(call(name, args, n));
    }
    for (int i = nvars - 1; i >= 0; --i) if (seq(vars[i].n, t)) return post(vars[i].v);
    fail("unknown identifier"); return z;
}
static V unary(void) {
    if (seq(peek(0), "-")) { eat("-"); V v = unary(); if (v.hascv) { float r[4]; for (int k = 0; k < v.w; ++k) r[k] = -v.cv[k]; return cnst(r, v.w); } V s[1] = { v }; return tmp(MOV, s, 1, v.w, 1, 0); }
    if (seq(peek(0), "+")) eat("+");
    return primary();
}
static V term(void) {
    V a = unary();
    while (seq(peek(0), "*") || seq(peek(0), "/")) {
        char op = *eat(0); V b = unary();
        if (a.mat) {
            if (op != '*' || b.w != 4) { fail("mat4 can only multiply vec4"); return a; }
            int r = newtmp(), rows[4];
            for (int k = 0; k < 4; ++k) { V m = mkv(1, a.i + k, 4); V s[2] = { m, b }; V d = tmp(DP4, s, 2, 1, 0, 0); rows[k] = d.i; }
            for (int k = 0; k < 4; ++k) { V s = mkv(2, rows[k], 1); for (int j = 0; j < 4; ++j) s.sw[j] = 0; emit(MOV, 16 + r, 1 << k, &s, 1, 0, 0); }
            a = mkv(2, r, 4); continue;
        }
        a = binop(op, a, b);
    }
    return a;
}
static V expr(void) { V a = term(); while (seq(peek(0), "+") || seq(peek(0), "-")) { char op = *eat(0); a = binop(op, a, term()); } return a; }
static int out_find(const char *n) { for (int i = 0; i < nouts; ++i) if (seq(outs[i].n, n)) return i; return -1; }
static void stmt(void) {
    const char *t = peek(0);
    if (seq(t, "if") || seq(t, "for") || seq(t, "while") || seq(t, "discard") || seq(t, "return")) { fail("control flow unsupported"); tp = ntk; return; }
    if (seq(t, "lowp") || seq(t, "mediump") || seq(t, "highp")) { eat(0); t = peek(0); }
    int w = ty_w(t);
    if (w && !seq(peek(1), "(") && !seq(peek(1), ".") && *peek(1)) {
        eat(0); char name[TLEN]; scpy(name, eat(0), TLEN); eat("="); V v = bc(expr(), w); eat(";");
        if (nvars < 48) { scpy(vars[nvars].n, name, TLEN); vars[nvars].v = v; vars[nvars].local = 1; ++nvars; } return;
    }
    char n[TLEN]; scpy(n, eat(0), TLEN);
    for (int i = nvars - 1; i >= 0; --i) if (vars[i].local && seq(vars[i].n, n) && seq(peek(0), "=")) { eat("="); V v = bc(expr(), vars[i].v.w); eat(";"); vars[i].v = v; return; }
    int mask[4], nm = 0; const char *sets[3] = { "xyzw", "rgba", "stpq" };
    if (seq(peek(0), ".")) { eat("."); const char *m = eat(0); for (const char *c = m; *c && nm < 4; ++c) { int f = -1; for (int s = 0; s < 3; ++s) for (int j = 0; j < 4; ++j) if (sets[s][j] == *c) f = j; if (f < 0) { fail("bad mask"); return; } mask[nm++] = f; } }
    eat("=");
    int pos = seq(n, "gl_Position"), oi = pos ? -1 : out_find(n);
    if (!pos && oi < 0) { fail("can only assign to varyings/gl_Position"); return; }
    V v = expr(); eat(";");
    int full = pos ? 4 : outs[oi].w, slot = pos ? 0 : outs[oi].slot;
    if (!nm) for (int k = 0; k < full; ++k) mask[nm++] = k;
    if (slot == 2) { nm = 1; mask[0] = 3; }
    if (v.w == 1 && nm > 1) v = bc(v, nm);
    if (v.w < nm) { fail("width mismatch"); return; }
    int sw[4]; for (int k = 0; k < 4; ++k) sw[k] = v.sw[0];
    for (int j = 0; j < nm; ++j) sw[mask[j]] = v.sw[j];
    int m4 = 0, o[4];
    if (slot == 1) { o[0] = sw[2]; o[1] = sw[1]; o[2] = sw[0]; o[3] = sw[3]; for (int j = 0; j < nm; ++j) m4 |= 1 << (mask[j] == 0 ? 2 : mask[j] == 2 ? 0 : mask[j]); }
    else { for (int k = 0; k < 4; ++k) o[k] = sw[k]; for (int j = 0; j < nm; ++j) m4 |= 1 << mask[j]; }
    V s = v; for (int k = 0; k < 4; ++k) s.sw[k] = o[k]; s.w = 4; s.hascv = 0;
    emit(MOV, slot, m4, &s, 1, 0, 0);
    if (pos) written_pos = 1;
}
/* ---- DCE, temp allocation, encoding */
static int finish(void) {
    static char live[64 + 16]; static int keep[VSC_MAX_INSTR]; int nk = 0;
    for (int i = 0; i < 80; ++i) live[i] = 0;
    for (int i = ncode - 1; i >= 0; --i) {
        int d = code[i].dst;
        if (d < 16 || (d - 16 < 64 && live[d - 16])) { keep[nk++] = i; for (int k = 0; k < 3; ++k) if (code[i].s[k].f == 2 && code[i].s[k].i < 64) live[code[i].s[k].i] = 1; }
    }
    for (int a = 0, b = nk - 1; a < b; ++a, --b) { int t = keep[a]; keep[a] = keep[b]; keep[b] = t; }
    static int first[64], last[64], map[64]; for (int i = 0; i < 64; ++i) first[i] = last[i] = map[i] = -1;
    for (int n = 0; n < nk; ++n) {
        Ins *in = &code[keep[n]];
        if (in->dst >= 16) { int r = in->dst - 16; if (r >= 64) return -1; if (first[r] < 0) first[r] = n; last[r] = last[r] > n ? last[r] : n; }
        for (int k = 0; k < 3; ++k) if (in->s[k].f == 2) { int r = in->s[k].i; if (r >= 64) return -1; last[r] = n; }
    }
    int freeregs[16], nf = 16, active[16], na = 0, maxr = -1; for (int i = 0; i < 16; ++i) freeregs[i] = i;
    for (int n = 0; n < nk; ++n) {
        for (int j = 0; j < na;) { if (last[active[j]] < n) { freeregs[nf++] = map[active[j]]; for (int q = j + 1; q < na; ++q) active[q - 1] = active[q]; --na; } else ++j; }
        for (int r = 0; r < 64; ++r) if (first[r] == n && map[r] < 0) {
            if (!nf) { fail("more than 16 live temporaries"); return -2; }
            map[r] = freeregs[0]; for (int q = 1; q < nf; ++q) freeregs[q - 1] = freeregs[q]; --nf;   /* FIFO, same policy as vsc.py */
            active[na++] = r; if (map[r] > maxr) maxr = map[r];
        }
    }
    R->ntemps = (uint32_t)(maxr + 1); R->ncode = 0;
    for (int n = 0; n < nk; ++n) {
        Ins *in = &code[keep[n]]; uint32_t A = (uint32_t)in->op, dst = (uint32_t)in->dst; if (dst >= 16) dst = 16 + (uint32_t)map[dst - 16];
        uint32_t B = (dst << 26) | ((uint32_t)in->mask << 22);
        for (int k = 0; k < 3; ++k) {
            V *v = &in->s[k]; uint32_t ix = (uint32_t)(v->f == 2 ? map[v->i] : v->i), sv = 0;
            for (int j = 0; j < 4; ++j) sv |= (uint32_t)v->sw[j] << (2 * j);
            A |= sv << (8 + 8 * k); if (in->neg[k]) A |= 1u << (5 + k);
            B |= (uint32_t)v->f << (16 + 2 * k) | (ix & 0xf) << (4 * k);
        }
        if (in->hi) B |= (uint32_t)in->hi << 12;
        R->code[R->ncode][0] = A; R->code[R->ncode][1] = B; ++R->ncode;
    }
    R->nconst = (uint32_t)nc; return 0;
}
int gl2vsc_compile(const char *src, vsc_result *out) {
    R = out; R->err[0] = 0; R->nlit = R->nuni = R->nattr = R->nvary = 0; err_flag = 0; nc = 0; nreg = 0; ncode = 0; nvars = 0; nouts = 0; tp = 0; written_pos = 0;
    tokenize(src); if (err_flag) return -1;
    while (tp < ntk && !err_flag) {
        const char *t = eat(0);
        if (seq(t, "precision")) { while (tp < ntk && !seq(eat(0), ";")) {} }
        else if (seq(t, "uniform") || seq(t, "attribute") || seq(t, "varying")) {
            int kind = seq(t, "uniform") ? 0 : seq(t, "attribute") ? 1 : 2;
            if (seq(peek(0), "lowp") || seq(peek(0), "mediump") || seq(peek(0), "highp")) eat(0);
            char ty[TLEN], name[TLEN]; scpy(ty, eat(0), TLEN); scpy(name, eat(0), TLEN); eat(";");
            if (seq(ty, "mat4") && kind == 0) {
                if (nvars >= 48 || R->nuni >= VSC_MAX_UNI || nc + 4 > 112) { fail("out of constants"); break; }
                V v = mkv(1, nc, 4); v.mat = 1; scpy(vars[nvars].n, name, TLEN); vars[nvars].v = v; vars[nvars].local = 0; ++nvars;
                scpy(R->uni_name[R->nuni], name, 24); R->uni_idx[R->nuni] = (uint8_t)nc; R->uni_cnt[R->nuni] = 4; ++R->nuni; nc += 4; continue;
            }
            int w = ty_w(ty); if (!w) { fail("unsupported type"); break; }
            if (kind == 0) {
                if (nvars >= 48 || R->nuni >= VSC_MAX_UNI || nc >= 112) { fail("out of constants"); break; }
                scpy(vars[nvars].n, name, TLEN); vars[nvars].v = mkv(1, nc, w); vars[nvars].local = 0; ++nvars;
                scpy(R->uni_name[R->nuni], name, 24); R->uni_idx[R->nuni] = (uint8_t)nc; R->uni_cnt[R->nuni] = 1; ++R->nuni; ++nc;
            } else if (kind == 1) {
                static const char *an[] = { "aPosition", "aNormal", "aColor", "aTexCoord", "aTexCoord1" }; static const int as[] = { 0, 1, 2, 3, 4 };
                int slot = -1; for (int i = 0; i < 5; ++i) if (seq(name, an[i])) slot = as[i];
                if (slot < 0) {
                    int used[8] = { 0 }; for (int i = 0; i < nvars; ++i) if (vars[i].v.f == 0) used[vars[i].v.i] = 1;
                    const int *pref = w == 4 ? (const int[]){ 0, 2, -1 } : w == 3 ? (const int[]){ 0, 1, -1 } : w == 2 ? (const int[]){ 3, 4, -1 } : (const int[]){ 2, -1, -1 };
                    for (int i = 0; pref[i] >= 0; ++i) if (!used[pref[i]]) { slot = pref[i]; break; }
                    if (slot < 0) { fail("no free input slot"); break; }
                }
                scpy(vars[nvars].n, name, TLEN); vars[nvars].v = mkv(0, slot, w); vars[nvars].local = 0; ++nvars;
                if (R->nattr < 8) { scpy(R->attr_name[R->nattr], name, 24); R->attr_slot[R->nattr] = (uint8_t)slot; R->attr_w[R->nattr] = (uint8_t)w; ++R->nattr; }
            } else {
                static const char *on[] = { "vColor", "vFog", "vTexCoord", "vTexCoord1" }; static const int os[] = { 1, 2, 3, 4 };
                int slot = -1; for (int i = 0; i < 4; ++i) if (seq(name, on[i])) slot = os[i];
                int used[8] = { 0 }; for (int i = 0; i < nouts; ++i) used[outs[i].slot] = 1;
                if (slot >= 0 && used[slot]) { fail("varying collides on an output slot"); break; }
                if (slot < 0) {
                    const int *pref = w == 4 || w == 3 ? (const int[]){ 1, -1, -1 } : w == 2 ? (const int[]){ 3, 4, -1 } : (const int[]){ 2, -1, -1 };
                    for (int i = 0; pref[i] >= 0; ++i) if (!used[pref[i]]) { slot = pref[i]; break; }
                    if (slot < 0) { fail("no free output slot"); break; }
                }
                if (nouts >= 8) { fail("too many varyings"); break; }
                scpy(outs[nouts].n, name, TLEN); outs[nouts].w = w; outs[nouts].slot = slot; ++nouts;
                if (R->nvary < 8) { scpy(R->vary_name[R->nvary], name, 24); R->vary_slot[R->nvary] = (uint8_t)slot; R->vary_w[R->nvary] = (uint8_t)w; ++R->nvary; }
            }
        } else if (seq(t, "void")) {
            eat("main"); eat("("); eat(")"); eat("{");
            while (!seq(peek(0), "}") && tp < ntk && !err_flag) stmt();
            eat("}");
        } else fail("unexpected token");
    }
    if (err_flag) return -1;
    if (!written_pos) { fail("gl_Position not written"); return -2; }
    return finish();
}
