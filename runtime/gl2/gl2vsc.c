#include "gl2vsc.h"
#define MAXTOK 2400
#define MAXVTEMP (VSC_MAX_INSTR * 2)
#define TLEN 32
typedef struct { int f, i, w, hascv, mat; int sw[4]; float cv[4]; } V;           /* f: 0 input 1 const 2 temp 3 zero */
typedef struct { int op, dst, mask, hi, neg[3]; V s[3]; } Ins;
enum { MOV = 1, ADD = 2, MAD = 3, MUL = 4, RCP = 5, RSQ = 6, DP3 = 7, DP4 = 8, MIN = 9, MAX = 10, SGE = 12, EX2 = 0x13 };
extern int vsc_copyprop_enabled;
static char tk[MAXTOK][TLEN]; static int ntk, tp;
static struct { char n[TLEN]; V v; int local; } vars[160]; static int nvars;
typedef struct { char n[TLEN]; int np, is_void; char pn[4][TLEN]; int bs,be; } UserFn;
static UserFn ufn[8]; static int nufn;
static int seq(const char*,const char*);static void scpy(char*,const char*,int);static void stmt(void);
static char scope_o[128][TLEN],scope_n[128][TLEN];static int nscope;static unsigned fnid,fn_serial;
static int fn_depth,fn_control,fn_ret,fn_body_end;static V fn_val;
static const char *map_name(const char*n){for(int i=nscope-1;i>=0;--i)if(seq(scope_o[i],n))return scope_n[i];return n;}
static void push_scope(const char*o,const char*u){if(nscope<128){scpy(scope_o[nscope],o,TLEN);scpy(scope_n[nscope],u,TLEN);++nscope;}}
static void make_uniq(char*out,const char*name){unsigned s=fn_serial;int p=0;out[p++]='f';char d[12];int n=0;if(!s)d[n++]='0';else{while(s){d[n++]=(char)('0'+s%10);s/=10;}}while(n)out[p++]=d[--n];out[p++]='_';int i=0;while(name[i]&&p<TLEN-2)out[p++]=name[i++];out[p]=0;}
static struct { char n[TLEN]; int w, slot; V val; } outs[8]; static int nouts;
static vsc_result *R; static int nc, nreg, ncode, written_pos, err_flag;
static Ins code[VSC_MAX_INSTR];
static V position_value;
static int seq(const char *a, const char *b) { while (*a && *a == *b) { ++a; ++b; } return *a == *b; }
static void scpy(char *d, const char *s, int n) { int i = 0; for (; i < n - 1 && s[i]; ++i) d[i] = s[i]; d[i] = 0; }
static void fail(const char *m) { if (!err_flag) { err_flag = 1; scpy(R->err, m, 64); } }
static int isid(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int isdig(char c) { return c >= '0' && c <= '9'; }
static void tokenize(const char *s) {
    ntk=0;
    while(*s){
        if(*s==' '||*s=='\n'||*s=='\t'||*s=='\r'){++s;continue;}
        if(s[0]=='/'&&s[1]=='/'){while(*s&&*s!='\n')++s;continue;}
        if(s[0]=='/'&&s[1]=='*'){s+=2;while(*s&&!(s[0]=='*'&&s[1]=='/'))++s;if(!*s){fail("unterminated block comment");return;}s+=2;continue;}
        if(ntk>=MAXTOK-1){fail("too many tokens");return;}
        unsigned n=0;
#define TOKEN_CHAR() do { if(n>=TLEN-1){fail("token exceeds compiler limit");return;}tk[ntk][n++]=*s++; }while(0)
        if(isid(*s)){while(isid(*s)||isdig(*s))TOKEN_CHAR();}
        else if(isdig(*s)||(*s=='.'&&isdig(s[1]))){
            while(isdig(*s))TOKEN_CHAR();
            if(*s=='.'){TOKEN_CHAR();while(isdig(*s))TOKEN_CHAR();}
            if(*s=='e'||*s=='E'){TOKEN_CHAR();if(*s=='-'||*s=='+')TOKEN_CHAR();if(!isdig(*s)){fail("missing numeric exponent");return;}while(isdig(*s))TOKEN_CHAR();}
        }else{const char *allowed="+-*/=;(),.{}<>!&|[]";int found=0;for(unsigned i=0;allowed[i];++i)found|=*s==allowed[i];if(!found){fail("unsupported source character");return;}TOKEN_CHAR();}
#undef TOKEN_CHAR
        tk[ntk][n]=0;++ntk;
    }
    tk[ntk][0]=0;
}
static const char *peek(int k) { return tp + k < ntk ? tk[tp + k] : ""; }
static const char *eat(const char *x) { const char *v = peek(0); if (!*v || (x && !seq(v, x))) { fail("unexpected token"); return ""; } ++tp; return v; }
static float atof_(const char *s) {
    float v = 0, f = 1; int dot = 0;
    for (; *s && *s != 'e' && *s != 'E'; ++s) { if (*s == '.') dot = 1; else if (dot) { f *= 0.1f; v += (float)(*s - '0') * f; } else v = v * 10 + (float)(*s - '0'); }
    if (*s) { ++s; int sg = 1, e = 0; if (*s == '-') { sg = -1; ++s; } else if (*s == '+') ++s; while (isdig(*s)) {e=e*10+(*s++-'0');if(e>255){fail("numeric exponent exceeds range");return 0;}} while(e--)v=sg>0?v*10:v/10; }
    union {float f;uint32_t u;} bits={v};if((bits.u&0x7f800000u)==0x7f800000u){fail("numeric literal exceeds float range");return 0;}
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
static int newtmp(void) { if(nreg>=MAXVTEMP){fail("too many intermediate values");return 0;}return nreg++; }
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
static V snap(V v, int w) { v = bc(v, w); if (v.f == 2) { V s[1] = { v }; return tmp(MOV, s, 1, w, 0, 0); } return v; }
static int allval(const V *a, float x, int w) { if (!a->hascv) return 0; for (int k = 0; k < w; ++k) if (a->cv[k] != x) return 0; return 1; }
static V binop(char op, V a, V b) {
    if (a.mat || b.mat) { fail("bad matrix use"); return a; }
    V original_b=b;
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
        if(original_b.w==1){V s[1]={original_b};return binop('*',a,tmp(RCP,s,1,1,0,0));}
        int r=newtmp();for(int k=0;k<w;++k){int ix[1]={k};V lane=swz(b,ix,1);emit(RCP,16+r,1<<k,&lane,1,0,0);}return binop('*',a,mkv(2,r,w));
    }
    V s[2] = { a, b }; return tmp(op == '*' ? MUL : ADD, s, 2, w, 0, op == '-');
}
static V dot_(V a, V b) {
    if(a.w!=b.w){fail("dot operands have different widths");return a;}
    int n=a.w;
    if (a.hascv && b.hascv) { float r = 0; for (int k = 0; k < n; ++k) r += a.cv[k] * b.cv[k]; return cnst(&r, 1); }
    if (n == 1) return binop('*', a, b);
    V s[2] = { a, b };
    if (n == 2) { V m = tmp(MUL, s, 2, 2, 0, 0); int i0[1] = { 0 }, i1[1] = { 1 }; V t[2] = { swz(m, i0, 1), swz(m, i1, 1) }; return tmp(ADD, t, 2, 1, 0, 0); }
    return tmp(n == 3 ? DP3 : DP4, s, 2, 1, 0, 0);
}
/* ---- parser */
static V expr(void);
static int ty_w(const char *t) { return seq(t, "float") ? 1 : seq(t, "vec2") ? 2 : seq(t, "vec3") ? 3 : seq(t, "vec4") ? 4 : 0; }
static V scalar_math(int op,V a){
 if(a.w==1){V s[1]={a};return tmp(op,s,1,1,0,0);}
 int r=newtmp();for(int k=0;k<a.w;++k){int ix[1]={k};V lane=swz(a,ix,1);emit(op,16+r,1<<k,&lane,1,0,0);}return mkv(2,r,a.w);
}
static V call(const char *name, V *a, int n) {
    int w; V z = mkv(3, 0, 1);
    for (int fi = 0; fi < nufn; ++fi) if (seq(name, ufn[fi].n)) {
        if (n != ufn[fi].np) { fail("function argument count"); return z; }
        unsigned serial = ++fnid;
        int save_tp = tp, save_nv = nvars, save_ns = nscope, save_depth = fn_depth, save_ctrl = fn_control, save_ret = fn_ret, save_end = fn_body_end;
        unsigned save_serial = fn_serial; V save_val = fn_val;
        fn_serial = serial;
        for (int p = 0; p < n; ++p) { if (nvars >= 160) { fail("too many function locals"); goto fn_done; } char uniq[TLEN]; make_uniq(uniq, ufn[fi].pn[p]); push_scope(ufn[fi].pn[p], uniq); scpy(vars[nvars].n, uniq, TLEN); vars[nvars].v = a[p]; vars[nvars].local = 1; ++nvars; }
        tp = ufn[fi].bs; fn_depth++; fn_control = 0; fn_ret = 0; fn_body_end = ufn[fi].be;
        while (tp < ufn[fi].be && !fn_ret && !err_flag) stmt();
        if (!ufn[fi].is_void && !fn_ret) { fail("function body must return a value"); goto fn_done; }
        { V r = fn_val;
          tp = save_tp; nvars = save_nv; nscope = save_ns; fn_depth = save_depth; fn_control = save_ctrl; fn_ret = save_ret; fn_body_end = save_end; fn_serial = save_serial; fn_val = save_val;
          return r; }
    fn_done:
        tp = save_tp; nvars = save_nv; nscope = save_ns; fn_depth = save_depth; fn_control = save_ctrl; fn_ret = save_ret; fn_body_end = save_end; fn_serial = save_serial; fn_val = save_val;
        return z;
    }
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
    if (seq(name, "sign")) {
        if (n != 1) { fail("sign args"); return z; }
        int w2 = a[0].w; V x = a[0];
        if (x.hascv) { float r[4]; for (int k = 0; k < w2; ++k) r[k] = x.cv[k] < 0 ? -1.f : x.cv[k] > 0 ? 1.f : 0.f; return cnst(r, w2); }
        float zero4[4] = {0,0,0,0}; V zc = cnst(zero4, w2);
        V neg = tmp(MOV, &x, 1, w2, 1, 0);
        V p[2] = { x, zc }, q[2] = { neg, zc };
        V pos = tmp(SGE, p, 2, w2, 0, 0), ngt = tmp(SGE, q, 2, w2, 0, 0);
        return binop('-', pos, ngt);
    }
    if (seq(name, "smoothstep")) {
        if (n != 3) { fail("smoothstep args"); return z; }
        int w2 = a[0].w > a[1].w ? a[0].w : a[1].w; if (a[2].w > w2) w2 = a[2].w;
        V e0 = bc(a[0], w2), e1 = bc(a[1], w2), x = bc(a[2], w2);
        V num = binop('-', x, e0), den = binop('-', e1, e0), t = binop('/', num, den);
        V cz[2] = { t, cnst((float[]){0,0,0,0}, w2) };
        V lo = call("max", cz, 2); V one[2] = { lo, cnst((float[]){1,1,1,1}, w2) }; V hi = call("min", one, 2);
        V t2 = binop('*', hi, hi), three = cnst((float[]){3,3,3,3}, w2), two = cnst((float[]){2,2,2,2}, w2);
        return binop('*', t2, binop('-', three, binop('*', two, hi)));
    }
    if (seq(name, "distance")) {
        if (n != 2) { fail("distance args"); return z; }
        V d = binop('-', a[0], a[1]); V arg[1] = { d }; return call("length", arg, 1);
    }
    if (seq(name, "cross")) {
        if (n != 2 || a[0].w != 3 || a[1].w != 3) { fail("cross needs two vec3"); return z; }
        if (vsc_copyprop_enabled) {
            /* Vector form: a.yzx*b.zxy - a.zxy*b.yzx (same per-lane operations as the
             * scalar form; 2 instructions after MAD fusion instead of 9). */
            int i1[3] = {1, 2, 0}, i2[3] = {2, 0, 1};
            V a1 = swz(a[0], i1, 3), b2 = swz(a[1], i2, 3), a2 = swz(a[0], i2, 3), b1 = swz(a[1], i1, 3);
            return binop('-', binop('*', a1, b2), binop('*', a2, b1));
        }
        int ix[3][1] = {{0},{1},{2}}; V ax = swz(a[0], ix[0], 1), ay = swz(a[0], ix[1], 1), az = swz(a[0], ix[2], 1);
        V bx = swz(a[1], ix[0], 1), by = swz(a[1], ix[1], 1), bz = swz(a[1], ix[2], 1);
        V c[3] = { binop('-', binop('*', ay, bz), binop('*', az, by)),
                   binop('-', binop('*', az, bx), binop('*', ax, bz)),
                   binop('-', binop('*', ax, by), binop('*', ay, bx)) };
        return call("vec3", c, 3);
    }
    if (seq(name, "reflect")) {
        if (n != 2) { fail("reflect args"); return z; }
        V d = dot_(a[1], a[0]); V two[1] = { cnst((float[]){2,2,2,2}, d.w) };
        V s = binop('*', two[0], d); V sc = binop('*', s, a[1]); return binop('-', a[0], sc);
    }
    if (seq(name, "faceforward")) {
        if (n != 3) { fail("faceforward args"); return z; }
        V d = dot_(a[2], a[1]); V zero = cnst((float[]){0,0,0,0}, 1); V neg = tmp(MOV, &d, 1, 1, 1, 0);
        V sgn = call("sign", &neg, 1); V low = call("max", (V[]){sgn, zero}, 2);
        V negn = binop('*', a[0], cnst((float[]){-1}, 1));
        V m[3] = { negn, a[0], low }; return call("mix", m, 3);
    }
    if (seq(name, "exp")) {
        if (n != 1) { fail("exp args"); return z; }
        V k = cnst((float[]){1.4426950408889634f}, 1);
        V arg = binop('*', a[0], k); return call("exp2", &arg, 1);
    }
    if (seq(name, "radians") || seq(name, "degrees")) {
        if (n != 1) { fail("angle-conversion args"); return z; }
        float k = seq(name, "radians") ? 0.017453292519943295f : 57.29577951308232f;
        V c = cnst(&k, 1); return binop('*', a[0], c);
    }
    if (seq(name, "inversesqrt")) { if (n != 1) { fail("args"); return z; } return scalar_math(RSQ,a[0]); }
    if (seq(name, "sqrt")) { if(n!=1){fail("sqrt args");return z;} return scalar_math(RCP,scalar_math(RSQ,a[0])); }
    if (seq(name, "length")) { if (n != 1) { fail("args"); return z; } V d = dot_(a[0], a[0]); return scalar_math(RCP,scalar_math(RSQ,d)); }
    if (seq(name, "normalize")) {
        if (n != 1 || (a[0].w<2 || a[0].w>4)) { fail("normalize needs vec2/vec3/vec4"); return z; }
        V d = dot_(a[0], a[0]); V s[1] = { d }; V r = tmp(RSQ, s, 1, 1, 0, 0); return binop('*', a[0], r);
    }
    if (seq(name, "exp2")) { if (n != 1) { fail("args"); return z; } return scalar_math(EX2,a[0]); }
    /* No FRC/FLR on the VGP: round to nearest with the IEEE single trick (x + 1.5*2^23) - 1.5*2^23
     * (|x| < 2^22), then floor = r - 1 + step(r, x). sin/cos reduce to [-pi, pi] and use the odd
     * Taylor series to y^11 (max error ~5e-4). */
    if (seq(name, "floor") || seq(name, "fract") || seq(name, "sin") || seq(name, "cos")) {
        if (n != 1) { fail("args"); return z; }
        int w = a[0].w; V x = a[0];
        #define KV(val) bc(cnst((float[]){ (val) }, 1), w)
        if (seq(name, "sin") || seq(name, "cos")) {
            if (seq(name, "cos")) x = binop('+', x, KV(1.5707963267948966f));
            V t = binop('*', x, KV(0.15915494309189535f));
            V r = binop('-', binop('+', t, KV(12582912.f)), KV(12582912.f));
            V y = binop('*', binop('-', t, r), KV(6.283185307179586f));
            /* bounded even if the hardware rounds differently from IEEE (2 instructions) */
            { V lo[2] = { y, KV(-3.141592653589793f) }; y = call("max", lo, 2); V hi[2] = { y, KV(3.141592653589793f) }; y = call("min", hi, 2); }
            V y2 = binop('*', y, y);
            static const float c[6] = { -2.505210838544172e-08f, 2.7557319223985893e-06f, -0.0001984126984126984f, 0.008333333333333333f, -0.16666666666666666f, 1.f };
            V p = KV(c[0]);
            for (int i = 1; i < 6; ++i) p = binop('+', binop('*', p, y2), KV(c[i]));
            return binop('*', p, y);
        }
        V r = binop('-', binop('+', x, KV(12582912.f)), KV(12582912.f));
        V st[2] = { r, x }; V fl = binop('+', binop('-', r, KV(1.f)), call("step", st, 2));
        #undef KV
        return seq(name, "floor") ? fl : binop('-', x, fl);
    }
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
static int atoi_(const char *s){int n=0;while(*s>='0'&&*s<='9'){n=n*10+(*s-'0');++s;}return n;}
static V primary(void) {
    const char *t = eat(0); V z = mkv(3, 0, 1); const char *mt = map_name(t);
    if (seq(t, "(")) { V v = expr(); eat(")"); return post(v); }
    if (isdig(t[0]) || t[0] == '.') { float f = atof_(t); return post(cnst(&f, 1)); }
    if (seq(peek(0), "(")) {
        char name[TLEN]; scpy(name, t, TLEN); eat("("); V args[8]; int n = 0;
        while (!seq(peek(0), ")") && *peek(0) && n < 8) { args[n++] = expr(); if (seq(peek(0), ",")) eat(","); }
        eat(")"); return post(call(name, args, n));
    }
    if (seq(peek(0), "[")) {
        eat("["); V ix = expr(); eat("]");
        if (!ix.hascv || ix.w != 1) { fail("array index must be constant"); return z; }
        int idx = (int)(ix.cv[0] + 0.5f);
        char full[TLEN + 16]; int p = 0; while (mt[p]) { full[p] = mt[p]; ++p; }
        full[p++] = '['; char dg[8]; int dn = 0, v = idx; if (v == 0) dg[dn++] = '0'; else { if (v < 0) { fail("array index out of range"); return z; } while (v) { dg[dn++] = (char)('0' + v % 10); v /= 10; } }
        while (dn) full[p++] = dg[--dn];
        full[p++] = ']'; full[p] = 0;
        for (int i = nvars - 1; i >= 0; --i) if (seq(vars[i].n, full)) return post(vars[i].v);
        fail("unknown array element"); return z;
    }
    if (seq(mt, "gl_Position") && written_pos) return post(position_value);
    for (int i = nvars - 1; i >= 0; --i) if (vars[i].n[0] && seq(vars[i].n, mt)) return post(vars[i].v);
    for (int i = 0; i < nouts; ++i) if (seq(outs[i].n, mt)) return post(outs[i].val);
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
            int dim = a.mat;
            if (op != '*' || b.w != dim) { fail(dim == 3 ? "mat3 can only multiply vec3" : "mat4 can only multiply vec4"); return a; }
            int r = newtmp(), rows[4];
            for (int k = 0; k < dim; ++k) {
                if (dim == 2) { V m = mkv(1, a.i + k, 2); V d = dot_(m, b); rows[k] = d.i; }
                else { V m = mkv(1, a.i + k, 4); V s[2] = { m, b }; V d = tmp(dim == 3 ? DP3 : DP4, s, 2, 1, 0, 0); rows[k] = d.i; }
            }
            for (int k = 0; k < dim; ++k) { V s = mkv(2, rows[k], 1); for (int j = 0; j < 4; ++j) s.sw[j] = 0; emit(MOV, 16 + r, 1 << k, &s, 1, 0, 0); }
            a = mkv(2, r, dim); continue;
        }
        a = binop(op, a, b);
    }
    return a;
}
static V expr(void) { V a = term(); while (seq(peek(0), "+") || seq(peek(0), "-")) { char op = *eat(0); a = binop(op, a, term()); } return a; }
static int local_index(const char *n){for(int i=nvars-1;i>=0;--i)if(vars[i].local&&seq(vars[i].n,n))return i;return -1;}
static V cond_primary(void){
    V a=expr();const char *op=peek(0);int kind=-1;
    if(seq(op,"<")){eat("<");if(seq(peek(0),"=")){eat("=");kind=1;}else kind=0;}
    else if(seq(op,">")){eat(">");if(seq(peek(0),"=")){eat("=");kind=3;}else kind=2;}
    else if(seq(op,"=")&&seq(peek(1),"=")){eat("=");eat("=");kind=4;}
    else if(seq(op,"!")&&seq(peek(1),"=")){eat("!");eat("=");kind=5;}
    else return a;
    V b=expr();int w=a.w>b.w?a.w:b.w;V aa=bc(a,w),bb=bc(b,w);
    V s1[2]={aa,bb},s2[2]={bb,aa};V geab=tmp(SGE,s1,2,w,0,0),geba=tmp(SGE,s2,2,w,0,0);
    float ones[4]={1,1,1,1};V one=cnst(ones,w);
    if(kind==0)return binop('-',one,geab);
    if(kind==1)return geba;
    if(kind==2)return binop('*',geab,binop('-',one,geba));
    if(kind==3)return geab;
    V eq=binop('*',geab,geba);
    return kind==4?eq:binop('-',one,eq);
}
static V cond_and(void){
    V a=cond_primary();float ones[4]={1,1,1,1};
    while(seq(peek(0),"&")&&seq(peek(1),"&")){eat("&");eat("&");V b=cond_primary();int w=a.w>b.w?a.w:b.w;a=binop('*',bc(a,w),bc(b,w));}
    (void)ones;return a;
}
static V cond_expr(void){
    V a=cond_and();float ones[4]={1,1,1,1};
    while(seq(peek(0),"|")&&seq(peek(1),"|")){eat("|");eat("|");V b=cond_and();int w=a.w>b.w?a.w:b.w;V aa=bc(a,w),bb=bc(b,w);V one=cnst(ones,w);a=binop('-',one,binop('*',binop('-',one,aa),binop('-',one,bb)));}
    return a;
}
static int out_find(const char *n);
typedef struct { char n[TLEN]; int kind; int mask[4],nm; V v; } BOut;
typedef struct { int li; V v; } BLoc;
typedef struct { BOut out[8]; int no; BLoc loc[16]; int nl; } Block;
static BOut *bfind_out(Block *b,const char *n,int kind){for(int i=0;i<b->no;++i)if(b->out[i].kind==kind&&seq(b->out[i].n,n))return &b->out[i];return 0;}
static void bput_out(Block *b,const char *n,int kind,const int *mask,int nm,V v){
    BOut *o=bfind_out(b,n,kind);
    if(!o){if(b->no>=8){fail("too many branch outputs");return;}o=&b->out[b->no++];scpy(o->n,n,TLEN);o->kind=kind;}
    o->nm=nm;for(int i=0;i<nm;++i)o->mask[i]=mask[i];o->v=v;
}
static BLoc *bfind_loc(Block *b,int li){for(int i=0;i<b->nl;++i)if(b->loc[i].li==li)return &b->loc[i];return 0;}
static void bput_loc(Block *b,int li,V v){
    BLoc *o=bfind_loc(b,li);
    if(!o){if(b->nl>=16){fail("too many branch locals");return;}o=&b->loc[b->nl++];o->li=li;}
    o->v=v;
}
static int parse_if_chain(Block *dst,int allow_outputs);
static int parse_block(Block *b){
    while (!seq(peek(0), "}")) {
        if(seq(peek(0),"if")){
            Block r;r.no=0;r.nl=0;
            if(!parse_if_chain(&r,0))return 0;
            for(int i=0;i<r.nl;++i)bput_loc(b,r.loc[i].li,r.loc[i].v);
            for(int i=0;i<r.no;++i)bput_out(b,r.out[i].n,r.out[i].kind,r.out[i].mask,r.out[i].nm,r.out[i].v);
            continue;
        }
        if(seq(peek(0),"for")||seq(peek(0),"while")||seq(peek(0),"return")||seq(peek(0),"discard")){fail("nested control flow in if branch unsupported");return 0;}
        int w=ty_w(peek(0));
        if(w&&!seq(peek(1),"(")&&!seq(peek(1),".")&&*peek(1)){fail("declarations in if branch unsupported");return 0;}
        char name[TLEN];scpy(name,eat(0),TLEN);
        const char *mn=map_name(name);int li=local_index(mn);
        /* Locals: any assignment form stmt() accepts (=, op=, swizzle masks); both sides of an
         * if are computed and selected afterwards, so the branch only records the new value. */
        if(li>=0){--tp;stmt();if(err_flag)return 0;bput_loc(b,li,vars[li].v);continue;}
        if(seq(name,"gl_Position")){
            int mask[4],nm=0;const char*sets[3]={"xyzw","rgba","stpq"};
            if(seq(peek(0),".")){
                eat(".");const char*m=eat(0);int set=-1;
                for(const char*c=m;*c;++c){int f=-1,fs=-1;for(int q=0;q<3;++q)for(int j=0;j<4;++j)if(sets[q][j]==*c){f=j;fs=q;}if(f<0||nm>=4||(set>=0&&set!=fs)){fail("bad output mask");return 0;}for(int j=0;j<nm;++j)if(mask[j]==f){fail("duplicate output component");return 0;}set=fs;mask[nm++]=f;}
            }
            eat("=");V v=expr();eat(";");for(int k=0;k<nm;++k)if(mask[k]>=4){fail("output mask out of range");return 0;}
            if(!nm)for(int k=0;k<4;++k)mask[nm++]=k;
            if(v.w==1&&nm>1)v=bc(v,nm);
            if(v.w<nm){fail("width mismatch");return 0;}
            bput_out(b,name,2,mask,nm,v);continue;
        }
        int oi=out_find(name);if(oi<0){fail("can only assign to locals/varyings/gl_Position");return 0;}
        int mask[4],nm=0;const char*sets[3]={"xyzw","rgba","stpq"};
        if(seq(peek(0),".")){
            eat(".");const char*m=eat(0);int set=-1;
            for(const char*c=m;*c;++c){int f=-1,fs=-1;for(int q=0;q<3;++q)for(int j=0;j<4;++j)if(sets[q][j]==*c){f=j;fs=q;}if(f<0||nm>=4||(set>=0&&set!=fs)){fail("bad output mask");return 0;}for(int j=0;j<nm;++j)if(mask[j]==f){fail("duplicate output component");return 0;}set=fs;mask[nm++]=f;}
        }
        eat("=");V v=expr();eat(";");int full=outs[oi].w,slot=outs[oi].slot;
        for(int k=0;k<nm;++k)if(mask[k]>=full){fail("output mask out of range");return 0;}
        if(!nm)for(int k=0;k<full;++k)mask[nm++]=k;
        if(slot==2){nm=1;mask[0]=3;}
        if(v.w==1&&nm>1)v=bc(v,nm);
        if(v.w<nm){fail("width mismatch");return 0;}
        bput_out(b,name,1,mask,nm,v);
    }
    eat("}");return 1;
}
static int emit_position_mask(const int *mask,int nm,V v){
    if(nm<=0){fail("empty position mask");return 0;}
    if(v.w==1&&nm>1)v=bc(v,nm);
    if(v.w<nm){fail("width mismatch");return 0;}
    int sw[4];for(int k=0;k<4;++k)sw[k]=v.sw[0];for(int j=0;j<nm;++j)sw[mask[j]]=v.sw[j];
    int m4=0,o[4];for(int k=0;k<4;++k)o[k]=sw[k];for(int j=0;j<nm;++j)m4|=1<<mask[j];
    V s=v;for(int k=0;k<4;++k)s.sw[k]=o[k];s.w=4;s.hascv=0;
    if(m4!=15){
        int t=newtmp();V previous=written_pos?position_value:mkv(3,0,4);
        emit(MOV,16+t,15,&previous,1,0,0);
        emit(MOV,16+t,m4,&s,1,0,0);
        position_value=mkv(2,t,4);
    }else position_value=s;
    emit(MOV,0,15,&position_value,1,0,0);
    V d[2]={position_value,position_value};
    for(int k=0;k<4;++k){d[0].sw[k]=position_value.sw[3];d[1].sw[k]=position_value.sw[2];}
    emit(ADD,5,1,d,2,0,0);emit(ADD,5,2,d,2,0,1);written_pos=1;return 1;
}
static int emit_varying(const char*n,const int*mask,int nm,V v){
    int oi=out_find(n);if(oi<0){fail("bad varying");return 0;}int slot=outs[oi].slot;
    int sw[4];for(int k=0;k<4;++k)sw[k]=v.sw[0];for(int j=0;j<nm;++j)sw[mask[j]]=v.sw[j];
    int m4=0,o[4];
    if(slot==1){o[0]=sw[2];o[1]=sw[1];o[2]=sw[0];o[3]=sw[3];for(int j=0;j<nm;++j)m4|=1<<(mask[j]==0?2:mask[j]==2?0:mask[j]);}
    else{for(int k=0;k<4;++k)o[k]=sw[k];for(int j=0;j<nm;++j)m4|=1<<mask[j];}
    V s=v;for(int k=0;k<4;++k)s.sw[k]=o[k];s.w=4;s.hascv=0;
    emit(MOV,slot,m4,&s,1,0,0);return 1;
}
static int if_depth;
static int parse_if_chain_(Block *dst,int allow_outputs);
static int parse_if_chain(Block *dst,int allow_outputs){++if_depth;int r=parse_if_chain_(dst,allow_outputs);--if_depth;return r;}
static int parse_if_chain_(Block *dst,int allow_outputs){
    eat("if");eat("(");V c0=snap(cond_expr(),1);eat(")");
    if(c0.w!=1){fail("if condition must be scalar");return 0;}
    static V pre_stack[4][160];if(if_depth>4){fail("if nesting too deep");return 0;}V *pre=pre_stack[if_depth-1];int npre=nvars;for(int i=0;i<npre;++i)pre[i]=vars[i].v;
    Block blk[8];V cd[8];int nb=0;
    for(int i=0;i<8;++i){blk[i].no=0;blk[i].nl=0;}
    if(!seq(eat(0),"{")){fail("if body must be a block");return 0;}
    cd[0]=c0;if(!parse_block(&blk[0]))return 0;nb=1;
    for(int i=0;i<npre;++i)vars[i].v=pre[i];
    int fin=0;
    while(seq(peek(0),"else")){
        eat("else");
        if(seq(peek(0),"if")){
            if(nb>=8){fail("too many else-if branches");return 0;}
            eat("if");eat("(");V cc=snap(cond_expr(),1);eat(")");
            if(cc.w!=1){fail("else-if condition must be scalar");return 0;}
            if(!seq(eat(0),"{")){fail("else-if body must be a block");return 0;}
            cd[nb]=cc;if(!parse_block(&blk[nb]))return 0;nb++;
            for(int i=0;i<npre;++i)vars[i].v=pre[i];
        } else if(seq(peek(0),"{")) {
            eat("{");if(!parse_block(&blk[nb]))return 0;nb++;fin=1;break;
        } else { fail("else must be a block or another if"); return 0; }
    }
    if(!fin){
        /* No final else: an implicit empty one keeps the previous values. Locals only; an
         * output assigned in only some branches has no tracked previous value here. */
        if(nb>=8){fail("too many else-if branches");return 0;}
        for(int j=0;j<nb;++j)if(blk[j].no){fail("if without else cannot assign outputs");return 0;}
        blk[nb].no=0;blk[nb].nl=0;nb++;
    }
    int nfin=nb-1;
    for(int i=0;i<nvars;++i)if(vars[i].local){
        int any=0;for(int j=0;j<nb;++j)if(bfind_loc(&blk[j],i))any=1;
        if(!any)continue;
        BLoc *fl=bfind_loc(&blk[nfin],i);V val=fl?fl->v:pre[i];
        for(int j=nfin-1;j>=0;--j){
            BLoc *l=bfind_loc(&blk[j],i);V t=l?l->v:pre[i];int w=vars[i].v.w;
            V tt=snap(t,w),vv=snap(val,w);V diff=binop('-',tt,vv);V scaled=binop('*',diff,cd[j]);
            val=binop('+',vv,scaled);
        }
        vars[i].v=val;bput_loc(dst,i,val);
    }
    int no=blk[0].no;
    for(int j=1;j<nb;++j)if(blk[j].no!=no){fail("if branches must assign the same outputs");return 0;}
    for(int i=0;i<no;++i){
        BOut *x=&blk[0].out[i];BOut *finout=bfind_out(&blk[nfin],x->n,x->kind);
        if(!finout){fail("if branches must assign the same outputs");return 0;}
        for(int j=1;j<nb;++j){BOut*y=bfind_out(&blk[j],x->n,x->kind);if(!y||y->nm!=x->nm){fail("if branches must use the same output mask");return 0;}for(int k=0;k<x->nm;++k)if(y->mask[k]!=x->mask[k]){fail("if branches must use the same output mask");return 0;}}
        V val=finout->v;
        for(int j=nfin-1;j>=0;--j){
            BOut*y=bfind_out(&blk[j],x->n,x->kind);int w=x->nm;
            V tt=snap(y->v,w),vv=snap(val,w);V diff=binop('-',tt,vv);V scaled=binop('*',diff,cd[j]);
            val=binop('+',vv,scaled);
        }
        if(allow_outputs){if(x->kind==2){if(!emit_position_mask(x->mask,x->nm,val))return 0;}else if(!emit_varying(x->n,x->mask,x->nm,val))return 0;}
        else bput_out(dst,x->n,x->kind,x->mask,x->nm,val);
    }
    return 1;
}
static void if_stmt(void){Block r;r.no=0;r.nl=0;fn_control++;(void)parse_if_chain(&r,1);fn_control--;}
static int symbol_exists(const char *name){for(int i=0;i<nvars;++i)if(seq(vars[i].n,name))return 1;for(int i=0;i<nouts;++i)if(seq(outs[i].n,name))return 1;return seq(name,"gl_Position");}
static int out_find(const char *n) { for (int i = 0; i < nouts; ++i) if (seq(outs[i].n, n)) return i; return -1; }
static void stmt(void);
static int int_lit(const char *s){int v=0;while(*s>='0'&&*s<='9')v=v*10+(*s++-'0');return v;}
static void for_stmt(void) {
    eat("for"); eat("(");
    if (seq(peek(0), "int")) eat("int");
    char var[TLEN]; scpy(var, eat(0), TLEN); char vuniq[TLEN]; const char *mv = var;
    if (fn_depth > 0) { make_uniq(vuniq, var); push_scope(var, vuniq); mv = vuniq; }
    eat("=");
    int init = int_lit(eat(0)); eat(";");
    if (!seq(eat(0), var)) { fail("unsupported for initializer"); return; }
    const char *op = eat(0); int le = 0;
    if (seq(op, "<")) { if (seq(peek(0), "=")) { eat("="); le = 1; } }
    else { fail("only < and <= loop bounds supported"); return; }
    int bound = int_lit(eat(0)); eat(";");
    int inc = 1;
    if (seq(peek(0), "+") && seq(peek(1), "+")) { eat("+"); eat("+"); if (!seq(eat(0), var)) { fail("bad for increment"); return; } }
    else if (seq(peek(0), var)) { eat(var); if (seq(peek(0), "+") && seq(peek(1), "+")) { eat("+"); eat("+"); }
        else if (seq(peek(0), "+") && seq(peek(1), "=")) { eat("+"); eat("="); inc = int_lit(eat(0)); }
        else { fail("unsupported for increment"); return; } }
    else { fail("unsupported for increment"); return; }
    eat(")"); if (!seq(eat(0), "{")) { fail("for body must be a block"); return; }
    int body_start = tp, depth = 1, body_end = -1;
    for (int i = tp; i < ntk; ++i) {
        if (seq(tk[i], "{")) ++depth;
        else if (seq(tk[i], "}")) { --depth; if (!depth) { body_end = i; break; } }
    }
    if (body_end < 0) { fail("unterminated for body"); return; }
    int iterations = 0;
    if (le) { for (int v = init; v <= bound; v += inc) ++iterations; }
    else { for (int v = init; v < bound; v += inc) ++iterations; }
    if (iterations <= 0 || iterations > 64 || inc <= 0) { fail("unsupported loop iteration count"); return; }
    for (int it = 0; it < iterations; ++it) {
        int save = nvars;
        float value = (float)(init + it * inc); V nv = cnst(&value, 1);
        scpy(vars[nvars].n, mv, TLEN); vars[nvars].v = nv; vars[nvars].local = 1; ++nvars;
        tp = body_start; fn_control++;
        while (tp < body_end && !err_flag) stmt();
        fn_control--; tp = body_end + 1;
        nvars = save;
    }
}
static void stmt(void) {
    const char *t = peek(0);
    if (seq(t, "return")) {
        if (fn_depth <= 0) { fail("return outside function"); return; }
        if (fn_control > 0) { fail("early return unsupported"); return; }
        eat("return"); fn_val = expr(); eat(";"); fn_ret = 1; tp = fn_body_end; return;
    }
    if (seq(t, "for")) { for_stmt(); return; }
    if (seq(t, "if")) { if_stmt(); return; }
    if (seq(t, "while") || seq(t, "discard")) { fail("control flow unsupported"); tp = ntk; return; }
    if (seq(t, "lowp") || seq(t, "mediump") || seq(t, "highp")) { eat(0); t = peek(0); }
    int w = ty_w(t);
    if (w && !seq(peek(1), "(") && !seq(peek(1), ".") && *peek(1)) {
        eat(0);
        for (;;) {
            char name[TLEN]; scpy(name, eat(0), TLEN);
            if (fn_depth > 0) { char uniq[TLEN]; make_uniq(uniq, name); push_scope(name, uniq); scpy(name, uniq, TLEN); }
            if (symbol_exists(name)) { fail("duplicate local symbol"); return; }
            if (seq(peek(0), "[")) {
                eat("["); char cb[TLEN]; scpy(cb, eat(0), TLEN); eat("]"); int count = atoi_(cb);
                if (count < 1 || count > 16 || nvars + count > 96) { fail("bad local array size"); return; }
                eat(";"); float zero[4] = { 0, 0, 0, 0 };
                for (int ai = 0; ai < count; ++ai) {
                    char en[TLEN]; int p = 0; while (name[p]) { en[p] = name[p]; ++p; }
                    en[p++] = '['; char dg[8]; int dn = 0, v = ai; if (v == 0) dg[dn++] = '0'; else while (v) { dg[dn++] = (char)('0' + v % 10); v /= 10; }
                    while (dn) en[p++] = dg[--dn];
                    en[p++] = ']'; en[p] = 0;
                    scpy(vars[nvars].n, en, TLEN); vars[nvars].v = cnst(zero, w); vars[nvars].local = 1; ++nvars;
                }
                return;
            }
            if (nvars >= 160) { fail("too many local symbols"); return; }
            float zero[4] = { 0, 0, 0, 0 };
            V v = cnst(zero, w);
            if (seq(peek(0), "=")) { eat("="); v = bc(expr(), w); }
            scpy(vars[nvars].n, name, TLEN); vars[nvars].v = v; vars[nvars].local = 1; ++nvars;
            if (seq(peek(0), ",")) { eat(","); continue; }
            eat(";");
            return;
        }
    }
    char n[TLEN]; scpy(n, eat(0), TLEN);
    if (seq(peek(0), "(")) {
        eat("("); V a[8]; int na = 0;
        if (!seq(peek(0), ")")) for (;;) {
            if (na >= 8) { fail("too many call args"); return; }
            a[na++] = expr();
            if (!seq(peek(0), ",")) break;
            eat(",");
        }
        eat(")"); eat(";");
        (void)call(n, a, na);
        return;
    }
    const char *mn = map_name(n);
    if (seq(peek(0), "[")) {
        eat("["); V ix = expr(); eat("]");
        if (!ix.hascv || ix.w != 1) { fail("array index must be constant"); return; }
        int idx = (int)(ix.cv[0] + 0.5f);
        char full[TLEN + 16]; int p = 0; while (mn[p]) { full[p] = mn[p]; ++p; }
        full[p++] = '['; char dg[8]; int dn = 0, v = idx; if (v == 0) dg[dn++] = '0'; else { if (v < 0) { fail("array index out of range"); return; } while (v) { dg[dn++] = (char)('0' + v % 10); v /= 10; } }
        while (dn) full[p++] = dg[--dn];
        full[p++] = ']'; full[p] = 0;
        for (int i = nvars - 1; i >= 0; --i) if (vars[i].local && seq(vars[i].n, full) && seq(peek(0), "=")) { eat("="); V v = bc(expr(), vars[i].v.w); eat(";"); vars[i].v = v; return; }
        fail("unknown local array element"); return;
    }
    int mask[4], nm = 0; const char *sets[3] = { "xyzw", "rgba", "stpq" };
    if (seq(peek(0), ".")) {
        eat("."); const char *m = eat(0); int set=-1;
        for (const char *c=m;*c;++c){
            int f=-1,foundset=-1;
            for(int q=0;q<3;++q)for(int j=0;j<4;++j)if(sets[q][j]==*c){f=j;foundset=q;}
            if(f<0||nm>=4||(set>=0&&set!=foundset)){fail("bad output mask");return;}
            for(int j=0;j<nm;++j)if(mask[j]==f){fail("duplicate output component");return;}
            set=foundset;mask[nm++]=f;
        }
    }
    char cop = 0;
    if ((seq(peek(0), "*") || seq(peek(0), "+") || seq(peek(0), "-") || seq(peek(0), "/")) && seq(peek(1), "=")) {
        cop = eat(0)[0];
    }
    eat("=");
    int li = -1;
    for (int i = nvars - 1; i >= 0; --i) if (vars[i].local && seq(vars[i].n, mn)) { li = i; break; }
    if (li >= 0) {
        V rhs = expr(); eat(";");
        V *dst = &vars[li].v;
        if (!nm) {
            if (cop) rhs = binop(cop, *dst, rhs);
            *dst = bc(rhs, dst->w);
        } else {
            if (rhs.w == 1 && nm > 1) rhs = bc(rhs, nm);
            if (rhs.w < nm) { fail("width mismatch"); return; }
            V cur = *dst;
            if (cop) {
                V sub = swz(cur, mask, nm);
                rhs = binop(cop, sub, bc(rhs, nm));
            }
            if (rhs.f != 2 || rhs.i >= MAXVTEMP) { V tmpv = tmp(MOV, &rhs, 1, rhs.w, 0, 0); rhs = tmpv; }
            int t = newtmp();
            emit(MOV, 16 + t, 15, &cur, 1, 0, 0);
            int m4 = 0, sw4[4] = { 0, 0, 0, 0 };
            for (int j = 0; j < nm; ++j) { m4 |= 1 << mask[j]; sw4[mask[j]] = rhs.sw[j]; }
            V wv = rhs; wv.w = 4; for (int k = 0; k < 4; ++k) wv.sw[k] = sw4[k];
            emit(MOV, 16 + t, m4, &wv, 1, 0, 0);
            *dst = mkv(2, t, cur.w);
        }
        return;
    }
    int pos = seq(n, "gl_Position"), oi = pos ? -1 : out_find(n);
    if (!pos && oi < 0) { fail("can only assign to varyings/gl_Position"); return; }
    V v = expr(); eat(";");
    if (cop) {
        V base = pos ? (written_pos ? position_value : mkv(3, 0, 4)) : outs[oi].val;
        if (nm) base = swz(base, mask, nm);
        v = binop(cop, base, v);
    }
    int full = pos ? 4 : outs[oi].w, slot = pos ? 0 : outs[oi].slot;
    for(int k=0;k<nm;++k)if(mask[k]>=full){fail("output mask out of range");return;}
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
    if (pos) {
        /* VGP output 5.xy carries the native near/far clip distances.
         * Keep an SSA position value so masked/reordered writes use the final
         * z and w, without reading an output register as a temporary. */
        if (m4 != 15) {
            int t = newtmp(); V previous = written_pos ? position_value : mkv(3, 0, 4);
            emit(MOV, 16 + t, 15, &previous, 1, 0, 0);
            emit(MOV, 16 + t, m4, &s, 1, 0, 0);
            position_value = mkv(2, t, 4);
        } else position_value = s;
        emit(MOV, 0, 15, &position_value, 1, 0, 0);
        V distances[2] = {position_value, position_value};
        for (int k = 0; k < 4; ++k) {
            distances[0].sw[k] = position_value.sw[3];
            distances[1].sw[k] = position_value.sw[2];
        }
        emit(ADD, 5, 1, distances, 2, 0, 0); /* w + z */
        emit(ADD, 5, 2, distances, 2, 0, 1); /* w - z */
        written_pos = 1;
    } else {
        int t = newtmp();
        V prev = outs[oi].val;
        emit(MOV, 16 + t, 15, &prev, 1, 0, 0);
        V wv = v; wv.w = 4; for (int k = 0; k < 4; ++k) wv.sw[k] = sw[k];
        int vm = 0; for (int j = 0; j < nm; ++j) vm |= 1 << mask[j];
        emit(MOV, 16 + t, vm, &wv, 1, 0, 0);
        outs[oi].val = mkv(2, t, outs[oi].w);
        emit(MOV, slot, m4, &s, 1, 0, 0);
    }
}
/* ---- DCE, temp allocation, encoding */
/* Copy propagation. A full-mask MOV into a temporary that is written exactly
 * once is a pure alias: its uses read the same values from the MOV's source
 * (swizzles compose, negation XORs). Applied only when the source is another
 * single-write temp, a vertex input, or one constant, and only when the user
 * still has at most one distinct non-temporary operand (the hardware limit this
 * compiler already respects). The existing dead-code pass then removes the MOV. */
int vsc_copyprop_enabled = 1;
static void copy_propagate(void) {
    static unsigned char ndef[MAXVTEMP];
    for (int i = 0; i < MAXVTEMP; ++i) ndef[i] = 0;
    for (int i = 0; i < ncode; ++i) if (code[i].dst >= 16 && code[i].dst - 16 < MAXVTEMP && ndef[code[i].dst - 16] < 2) ++ndef[code[i].dst - 16];
    for (int i = 0; i < ncode; ++i) {
        Ins *m = &code[i];
        if (m->op != MOV || m->dst < 16 || m->mask != 15) continue;
        int r = m->dst - 16; if (r >= MAXVTEMP || ndef[r] != 1) continue;
        V src = m->s[0];
        if (src.f == 3 || src.f > 3) continue;
        if (src.f == 2 && (src.i >= MAXVTEMP || ndef[src.i] != 1)) continue;
        for (int j = i + 1; j < ncode; ++j) {
            Ins *u = &code[j];
            for (int k = 0; k < 3; ++k) {
                if (u->s[k].f != 2 || u->s[k].i != r) continue;
                if (src.f != 2) {
                    int ok = 1;
                    for (int q = 0; q < 3; ++q) {
                        if (q == k || u->s[q].f == 2 || u->s[q].f == 3) continue;
                        if (u->s[q].f != src.f || u->s[q].i != src.i) ok = 0;
                    }
                    if (!ok) continue;
                }
                V nv = src; for (int l = 0; l < 4; ++l) nv.sw[l] = src.sw[u->s[k].sw[l]];
                nv.w = u->s[k].w;
                u->s[k] = nv; u->neg[k] ^= m->neg[0];
                if (src.f == 1) u->hi = src.i >> 4;
            }
        }
    }
}
/* Scalar-into-lane folding. "OP S.x <- ..." whose only use is "MOV D.<lane> <- S.x"
 * can write D.<lane> directly: the write mask does the placement. The producer
 * must be lane-uniform (every non-zero source swizzles the same component into
 * all four lanes, or the op ignores the destination lane), have no negation on
 * the MOV, and nothing between the two instructions may read or write D. */
static void fold_scalar_lanes(void) {
    static unsigned char ndef[MAXVTEMP], nuse[MAXVTEMP]; static int defat[MAXVTEMP];
    for (int i = 0; i < MAXVTEMP; ++i) { ndef[i] = nuse[i] = 0; defat[i] = -1; }
    for (int i = 0; i < ncode; ++i) {
        if (code[i].dst >= 16 && code[i].dst - 16 < MAXVTEMP) { int r = code[i].dst - 16; if (ndef[r] < 2) ++ndef[r]; defat[r] = i; }
        for (int k = 0; k < 3; ++k) if (code[i].s[k].f == 2 && code[i].s[k].i < MAXVTEMP && nuse[code[i].s[k].i] < 2) ++nuse[code[i].s[k].i];
    }
    for (int i = 0; i < ncode; ++i) {
        Ins *m = &code[i];
        if (m->op != MOV || m->dst < 16 || m->neg[0] || m->s[0].f != 2 || !m->mask) continue;
        int mk = m->mask;
        int S = m->s[0].i; if (S >= MAXVTEMP || ndef[S] != 1 || nuse[S] != 1) continue;
        int pi = defat[S]; if (pi < 0 || pi >= i) continue;
        Ins *pr = &code[pi];
        int uniform = pr->op == DP3 || pr->op == DP4 || pr->op == RCP || pr->op == RSQ || pr->op == EX2;
        int lanewise = pr->op == ADD || pr->op == MUL || pr->op == MAD || pr->op == MIN || pr->op == MAX || pr->op == SGE || pr->op == MOV;
        if (!uniform && !lanewise) continue;
        int ok = 1;
        for (int k = 0; k < 4 && ok; ++k) {
            if (!(mk & (1 << k))) continue;
            int j = m->s[0].sw[k];                       /* lane of S this lane copies */
            if (!(pr->mask & (1 << j))) { ok = 0; break; }
            if (lanewise) for (int q = 0; q < 3; ++q) { V *v = &pr->s[q]; if (v->f == 3) continue; if (v->sw[k] != v->sw[j]) ok = 0; }
        }
        if (!ok) continue;
        int D = m->dst - 16;
        for (int j = pi + 1; j < i && ok; ++j) {
            if (code[j].dst == m->dst) ok = 0;
            for (int k = 0; k < 3; ++k) if (code[j].s[k].f == 2 && code[j].s[k].i == D) ok = 0;
        }
        if (!ok) continue;
        pr->dst = m->dst; pr->mask = mk;
        m->dst = 16 + MAXVTEMP - 1; m->mask = 0;          /* unread scratch temp: removed by DCE */
        ++ndef[D];
    }
}
/* Dead store: a full-mask write to a temporary that is overwritten by another
 * full-mask write before any read is never observed. */
static void drop_dead_full_writes(void) {
    for (int i = 0; i < ncode; ++i) {
        Ins *w = &code[i];
        if (w->dst < 16 || w->mask != 15 || w->dst - 16 >= MAXVTEMP - 1) continue;
        for (int j = i + 1; j < ncode; ++j) {
            int read = 0; for (int k = 0; k < 3; ++k) if (code[j].s[k].f == 2 && code[j].s[k].i == w->dst - 16) read = 1;
            if (read) break;
            if (code[j].dst == w->dst) { if (code[j].mask == 15) { w->dst = 16 + MAXVTEMP - 1; w->mask = 0; } break; }
        }
    }
}
/* MUL feeding ADD -> MAD. "MUL T = a*b" (single definition, single use) followed
 * by "ADD d = T + c" becomes "MAD d = a*b + c". The product's sources must not
 * change in between, lane swizzles must match, and the result keeps the
 * one-distinct-constant rule. Differs from separate MUL+ADD by at most one
 * rounding step. */
static void fuse_mad(void) {
    static unsigned char ndef[MAXVTEMP], nuse[MAXVTEMP]; static int defat[MAXVTEMP];
    for (int i = 0; i < MAXVTEMP; ++i) { ndef[i] = nuse[i] = 0; defat[i] = -1; }
    for (int i = 0; i < ncode; ++i) {
        if (code[i].dst >= 16 && code[i].dst - 16 < MAXVTEMP) { int r = code[i].dst - 16; if (ndef[r] < 2) ++ndef[r]; defat[r] = i; }
        for (int k = 0; k < 3; ++k) if (code[i].s[k].f == 2 && code[i].s[k].i < MAXVTEMP && nuse[code[i].s[k].i] < 2) ++nuse[code[i].s[k].i];
    }
    for (int i = 0; i < ncode; ++i) {
        Ins *ad = &code[i];
        if (ad->op != ADD || !ad->mask) continue;
        for (int t = 0; t < 2; ++t) {
            V *tv = &ad->s[t], *cv = &ad->s[1 - t];
            if (tv->f != 2 || tv->i >= MAXVTEMP || ndef[tv->i] != 1 || nuse[tv->i] != 1) continue;
            int pi = defat[tv->i]; if (pi < 0 || pi >= i) continue;
            Ins *mu = &code[pi];
            if (mu->op != MUL || mu->neg[0] || mu->neg[1]) { if (mu->op != MUL) continue; }
            int ok = 1;
            for (int l = 0; l < 4 && ok; ++l) {
                if (!(ad->mask & (1 << l))) continue;
                int j = tv->sw[l];
                if (!(mu->mask & (1 << j))) { ok = 0; break; }
                for (int q = 0; q < 2; ++q) { V *v = &mu->s[q]; if (v->f != 3 && v->sw[l] != v->sw[j]) ok = 0; }
            }
            if (!ok) continue;
            /* operand rules after fusion: a, b, c non-temp operands must name at most one distinct register */
            V a = mu->s[0], b = mu->s[1], c = *cv;
            int nf = -1, ni = -1, bad = 0;
            V *ops[3] = { &a, &b, &c };
            for (int q = 0; q < 3; ++q) if (ops[q]->f == 0 || ops[q]->f == 1) { if (nf < 0) { nf = ops[q]->f; ni = ops[q]->i; } else if (nf != ops[q]->f || ni != ops[q]->i) bad = 1; }
            if (bad) continue;
            /* sources of the product must be unchanged between the MUL and the ADD */
            for (int j = pi + 1; j < i && ok; ++j) for (int q = 0; q < 2; ++q) if (mu->s[q].f == 2 && code[j].dst == 16 + mu->s[q].i) ok = 0;
            if (!ok) continue;
            if (ad->neg[t] ) { /* -(a*b) + c: fold into a's negation */ }
            int na = mu->neg[0] ^ ad->neg[t], nb = mu->neg[1], nc = ad->neg[1 - t];
            ad->op = MAD; ad->s[0] = a; ad->s[1] = b; ad->s[2] = c;
            ad->neg[0] = na; ad->neg[1] = nb; ad->neg[2] = nc;
            ad->hi = 0; for (int q = 0; q < 3; ++q) if (ad->s[q].f == 1) ad->hi = ad->s[q].i >> 4;
            mu->dst = 16 + MAXVTEMP - 1; mu->mask = 0;
            break;
        }
    }
}
static int finish(void) {
    if (vsc_copyprop_enabled) { copy_propagate(); fold_scalar_lanes(); drop_dead_full_writes(); fuse_mad(); }
    static char live[MAXVTEMP]; static int keep[VSC_MAX_INSTR]; int nk = 0;
    for (int i = 0; i < MAXVTEMP; ++i) live[i] = 0;
    for (int i = ncode - 1; i >= 0; --i) {
        int d = code[i].dst;
        if (d < 16 || (d - 16 < MAXVTEMP && live[d - 16])) { keep[nk++] = i; for (int k = 0; k < 3; ++k) if (code[i].s[k].f == 2 && code[i].s[k].i < MAXVTEMP) live[code[i].s[k].i] = 1; }
    }
    for (int a = 0, b = nk - 1; a < b; ++a, --b) { int t = keep[a]; keep[a] = keep[b]; keep[b] = t; }
    static int first[MAXVTEMP], last[MAXVTEMP], map[MAXVTEMP]; for (int i = 0; i < MAXVTEMP; ++i) first[i] = last[i] = map[i] = -1;
    for (int n = 0; n < nk; ++n) {
        Ins *in = &code[keep[n]];
        if (in->dst >= 16) { int r = in->dst - 16; if (r >= MAXVTEMP) {fail("intermediate value out of range");return -1;} if (first[r] < 0) first[r] = n; last[r] = last[r] > n ? last[r] : n; }
        for (int k = 0; k < 3; ++k) if (in->s[k].f == 2) { int r = in->s[k].i; if (r >= MAXVTEMP) {fail("intermediate value out of range");return -1;} last[r] = n; }
    }
    /* Device register-rotation run77: indices14/15 corrupt output. Use the
     * 14 verified scratch slots; FIFO reuse must not cycle into unsafe slots. */
    int freeregs[14], nf = 14, active[14], na = 0, maxr = -1; for (int i = 0; i < 14; ++i) freeregs[i] = i;
    for (int n = 0; n < nk; ++n) {
        for (int j = 0; j < na;) { if (last[active[j]] < n) { freeregs[nf++] = map[active[j]]; for (int q = j + 1; q < na; ++q) active[q - 1] = active[q]; --na; } else ++j; }
        for (int r = 0; r < nreg; ++r) if (first[r] == n && map[r] < 0) {
            if (!nf) { fail("more than 14 verified live temporaries"); return -2; }
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
/* ---- freestanding GLSL ES 1.00 preprocessor ---- */
#define PP_MAX_MACROS 64
typedef struct { char name[32]; char val[96]; uint8_t used; } pp_macro;
static pp_macro pp_macros[PP_MAX_MACROS];
static const char *pp_p;
static int pp_find(const char *name) {
    for (int i = 0; i < PP_MAX_MACROS; ++i)
        if (pp_macros[i].used && seq(pp_macros[i].name, name)) return i;
    return -1;
}
static void pp_set(const char *name, const char *val) {
    int idx = pp_find(name);
    if (idx < 0) {
        for (int i = 0; i < PP_MAX_MACROS; ++i) if (!pp_macros[i].used) { idx = i; break; }
    }
    if (idx < 0) return;
    pp_macros[idx].used = 1;
    scpy(pp_macros[idx].name, name, 32);
    scpy(pp_macros[idx].val, val && *val ? val : "1", 96);
}
static void pp_unset(const char *name) {
    int idx = pp_find(name);
    if (idx >= 0) pp_macros[idx].used = 0;
}
static void pp_skip_ws(void) { while (*pp_p == ' ' || *pp_p == '\t') ++pp_p; }
static int pp_expr(void);
static int pp_primary(void) {
    pp_skip_ws();
    if (*pp_p == '(') { ++pp_p; int v = pp_expr(); pp_skip_ws(); if (*pp_p == ')') ++pp_p; return v; }
    if (*pp_p == '!') { ++pp_p; return !pp_primary(); }
    if (*pp_p == '-') { ++pp_p; return -pp_primary(); }
    if (*pp_p == '+') { ++pp_p; return pp_primary(); }
    if (isid(*pp_p)) {
        char id[32]; int n = 0;
        while ((isid(*pp_p) || isdig(*pp_p)) && n < 31) id[n++] = *pp_p++;
        while (isid(*pp_p) || isdig(*pp_p)) ++pp_p;
        id[n] = 0;
        if (seq(id, "defined")) {
            pp_skip_ws();
            int par = 0; if (*pp_p == '(') { par = 1; ++pp_p; pp_skip_ws(); }
            char target[32]; int tn = 0;
            while ((isid(*pp_p) || isdig(*pp_p)) && tn < 31) target[tn++] = *pp_p++;
            while (isid(*pp_p) || isdig(*pp_p)) ++pp_p;
            target[tn] = 0;
            if (par) { pp_skip_ws(); if (*pp_p == ')') ++pp_p; }
            return pp_find(target) >= 0 ? 1 : 0;
        }
        int idx = pp_find(id);
        if (idx < 0) return 0;
        const char *saved = pp_p; pp_p = pp_macros[idx].val;
        int v = pp_expr(); pp_p = saved;
        return v;
    }
    if (isdig(*pp_p)) {
        int v = 0;
        if (pp_p[0] == '0' && (pp_p[1] == 'x' || pp_p[1] == 'X')) {
            pp_p += 2;
            while (isdig(*pp_p) || (*pp_p >= 'a' && *pp_p <= 'f') || (*pp_p >= 'A' && *pp_p <= 'F')) {
                char c = *pp_p++; int d = isdig(c) ? (c - '0') : ((c >= 'a' ? c - 'a' : c - 'A') + 10);
                v = v * 16 + d;
            }
        } else {
            while (isdig(*pp_p)) v = v * 10 + (*pp_p++ - '0');
        }
        return v;
    }
    return 0;
}
static int pp_mul(void) {
    int v = pp_primary();
    for (;;) {
        pp_skip_ws();
        if (*pp_p == '*') { ++pp_p; v *= pp_primary(); }
        else if (*pp_p == '/') { ++pp_p; int d = pp_primary(); v = d ? (v / d) : 0; }
        else break;
    }
    return v;
}
static int pp_add(void) {
    int v = pp_mul();
    for (;;) {
        pp_skip_ws();
        if (*pp_p == '+') { ++pp_p; v += pp_mul(); }
        else if (*pp_p == '-') { ++pp_p; v -= pp_mul(); }
        else break;
    }
    return v;
}
static int pp_rel(void) {
    int v = pp_add();
    for (;;) {
        pp_skip_ws();
        if (pp_p[0] == '<' && pp_p[1] == '=') { pp_p += 2; v = v <= pp_add(); }
        else if (pp_p[0] == '>' && pp_p[1] == '=') { pp_p += 2; v = v >= pp_add(); }
        else if (*pp_p == '<') { ++pp_p; v = v < pp_add(); }
        else if (*pp_p == '>') { ++pp_p; v = v > pp_add(); }
        else break;
    }
    return v;
}
static int pp_eq(void) {
    int v = pp_rel();
    for (;;) {
        pp_skip_ws();
        if (pp_p[0] == '=' && pp_p[1] == '=') { pp_p += 2; v = v == pp_rel(); }
        else if (pp_p[0] == '!' && pp_p[1] == '=') { pp_p += 2; v = v != pp_rel(); }
        else break;
    }
    return v;
}
static int pp_and(void) {
    int v = pp_eq();
    while (1) { pp_skip_ws(); if (pp_p[0] == '&' && pp_p[1] == '&') { pp_p += 2; int r = pp_eq(); v = v && r; } else break; }
    return v;
}
static int pp_expr(void) {
    int v = pp_and();
    while (1) { pp_skip_ws(); if (pp_p[0] == '|' && pp_p[1] == '|') { pp_p += 2; int r = pp_and(); v = v || r; } else break; }
    return v;
}
static int pp_eval_str(const char *s) { const char *saved = pp_p; pp_p = s; int v = pp_expr(); pp_p = saved; return v; }
static void pp_emit_expanded(const char *s, int len, char *out, uint32_t *op, uint32_t cap, int depth) {
    int i = 0;
    while (i < len) {
        if (isid(s[i])) {
            char id[32]; int n = 0; int start = i;
            while (i < len && (isid(s[i]) || isdig(s[i]))) { if (n < 31) id[n++] = s[i]; ++i; }
            id[n] = 0;
            int m = (depth < 6) ? pp_find(id) : -1;
            if (m >= 0 && (i - start) < 32) {
                int vlen = 0; while (pp_macros[m].val[vlen]) ++vlen;
                pp_emit_expanded(pp_macros[m].val, vlen, out, op, cap, depth + 1);
            } else {
                for (int k = start; k < i; ++k) if (*op + 1 < cap) out[(*op)++] = s[k];
            }
        } else {
            if (*op + 1 < cap) out[(*op)++] = s[i];
            ++i;
        }
    }
}
int gl2_preprocess(const char *src, char *out, uint32_t cap) {
    if (!src || !out || cap == 0) return -1;
    for (int i = 0; i < PP_MAX_MACROS; ++i) pp_macros[i].used = 0;
    struct { uint8_t parent_active, active, seen_true; } stk[16];
    int sp = 0; uint32_t op = 0; int in_comment = 0;
    const char *p = src;
    while (*p) {
        const char *ls = p;
        while (*p && *p != '\n') ++p;
        int llen = (int)(p - ls);
        if (*p == '\n') ++p;
        if (llen > 0 && ls[llen - 1] == '\r') --llen;
        int cur_active = (sp == 0) ? 1 : stk[sp - 1].active;
        int k = 0;
        while (k < llen && (ls[k] == ' ' || ls[k] == '\t')) ++k;
        if (!in_comment && k < llen && ls[k] == '#') {
            ++k;
            while (k < llen && (ls[k] == ' ' || ls[k] == '\t')) ++k;
            char dir[16]; int dn = 0;
            while (k < llen && (isid(ls[k]) || isdig(ls[k])) && dn < 15) dir[dn++] = ls[k++];
            dir[dn] = 0;
            while (k < llen && (ls[k] == ' ' || ls[k] == '\t')) ++k;
            char rest[256]; int rn = 0;
            while (k < llen && rn < 255) {
                if (k + 1 < llen && ls[k] == '/' && ls[k + 1] == '/') break;
                rest[rn++] = ls[k++];
            }
            while (rn > 0 && (rest[rn - 1] == ' ' || rest[rn - 1] == '\t')) --rn;
            rest[rn] = 0;
            if (seq(dir, "ifdef") || seq(dir, "ifndef")) {
                if (sp < 16) {
                    char id[32]; int n = 0;
                    while (rest[n] && (isid(rest[n]) || isdig(rest[n])) && n < 31) { id[n] = rest[n]; ++n; }
                    id[n] = 0;
                    int cond = pp_find(id) >= 0; if (seq(dir, "ifndef")) cond = !cond;
                    stk[sp].parent_active = (uint8_t)cur_active;
                    stk[sp].active = (uint8_t)(cur_active && cond);
                    stk[sp].seen_true = (uint8_t)cond;
                    ++sp;
                }
            } else if (seq(dir, "if")) {
                if (sp < 16) {
                    int cond = cur_active ? (pp_eval_str(rest) != 0) : 0;
                    stk[sp].parent_active = (uint8_t)cur_active;
                    stk[sp].active = (uint8_t)(cur_active && cond);
                    stk[sp].seen_true = (uint8_t)cond;
                    ++sp;
                }
            } else if (seq(dir, "elif")) {
                if (sp > 0) {
                    int cond = (stk[sp - 1].parent_active && !stk[sp - 1].seen_true) ? (pp_eval_str(rest) != 0) : 0;
                    stk[sp - 1].active = (uint8_t)cond;
                    if (cond) stk[sp - 1].seen_true = 1;
                }
            } else if (seq(dir, "else")) {
                if (sp > 0) {
                    int cond = stk[sp - 1].parent_active && !stk[sp - 1].seen_true;
                    stk[sp - 1].active = (uint8_t)cond;
                    stk[sp - 1].seen_true = 1;
                }
            } else if (seq(dir, "endif")) {
                if (sp > 0) --sp;
            } else if (cur_active && seq(dir, "define")) {
                char name[32]; int n = 0, rk = 0;
                while (rest[rk] && (isid(rest[rk]) || isdig(rest[rk])) && n < 31) name[n++] = rest[rk++];
                name[n] = 0;
                if (rest[rk] != '(') {
                    while (rest[rk] == ' ' || rest[rk] == '\t') ++rk;
                    if (n > 0) pp_set(name, rest + rk);
                }
            } else if (cur_active && seq(dir, "undef")) {
                char name[32]; int n = 0, rk = 0;
                while (rest[rk] && (isid(rest[rk]) || isdig(rest[rk])) && n < 31) name[n++] = rest[rk++];
                name[n] = 0;
                if (n > 0) pp_unset(name);
            }
            if (op + 1 < cap) out[op++] = '\n';
            continue;
        }
        if (cur_active) {
            int i = 0;
            while (i < llen) {
                if (in_comment) {
                    if (i + 1 < llen && ls[i] == '*' && ls[i + 1] == '/') { in_comment = 0; i += 2; }
                    else ++i;
                } else if (i + 1 < llen && ls[i] == '/' && ls[i + 1] == '/') {
                    break;
                } else if (i + 1 < llen && ls[i] == '/' && ls[i + 1] == '*') {
                    in_comment = 1; i += 2;
                } else {
                    int seg_start = i;
                    while (i < llen && !(i + 1 < llen && ls[i] == '/' && (ls[i + 1] == '/' || ls[i + 1] == '*'))) ++i;
                    pp_emit_expanded(ls + seg_start, i - seg_start, out, &op, cap, 0);
                }
            }
            if (op + 1 < cap) out[op++] = '\n';
        } else {
            for (int i = 0; i < llen; ++i) {
                if (in_comment && i + 1 < llen && ls[i] == '*' && ls[i + 1] == '/') { in_comment = 0; ++i; }
                else if (!in_comment && i + 1 < llen && ls[i] == '/' && ls[i + 1] == '*') { in_comment = 1; ++i; }
            }
            if (op + 1 < cap) out[op++] = '\n';
        }
    }
    out[op] = 0;
    return 0;
}

int gl2vsc_compile(const char *src, vsc_result *out) {
    static char pp_buf[16384];
    R = out; R->ncode=R->nconst=R->ntemps=0; R->err[0] = 0; R->nlit = R->nuni = R->nattr = R->nvary = 0; err_flag = 0; nc = 0; nreg = 0; ncode = 0; nvars = 0; nouts = 0; tp = 0; written_pos = 0; nufn = 0;
    gl2_preprocess(src, pp_buf, sizeof(pp_buf));
    tokenize(pp_buf); if (err_flag) return -1;
    while (tp < ntk && !err_flag) {
        const char *t = eat(0);
        if (seq(t, "precision")) { while (tp < ntk && !seq(eat(0), ";")) {} }
        else if (seq(t, "uniform") || seq(t, "attribute") || seq(t, "varying")) {
            int kind = seq(t, "uniform") ? 0 : seq(t, "attribute") ? 1 : 2;
            if (seq(peek(0), "lowp") || seq(peek(0), "mediump") || seq(peek(0), "highp")) eat(0);
            char ty[TLEN], name[TLEN]; scpy(ty, eat(0), TLEN); scpy(name, eat(0), TLEN);
            if(symbol_exists(name)){fail("duplicate global symbol");break;}if(nvars>=96){fail("too many global symbols");break;}
            if ((seq(ty, "mat4") || seq(ty, "mat3") || seq(ty, "mat2")) && kind == 0) {
                eat(";");
                int md = seq(ty, "mat4") ? 4 : seq(ty, "mat3") ? 3 : 2;
                if (nvars >= 96 || R->nuni >= VSC_MAX_UNI || nc + (unsigned)md > 112) { fail("out of constants"); break; }
                V v = mkv(1, nc, 4); v.mat = md; scpy(vars[nvars].n, name, TLEN); vars[nvars].v = v; vars[nvars].local = 0; ++nvars;
                scpy(R->uni_name[R->nuni], name, TLEN); R->uni_idx[R->nuni] = (uint8_t)nc; R->uni_cnt[R->nuni] = (uint8_t)md;R->uni_width[R->nuni]=(uint8_t)(md*md); ++R->nuni; nc += (unsigned)md; continue;
            }
            int w = ty_w(ty); if (!w) { fail("unsupported type"); break; }
            if (kind == 0) {
                int count = 1;
                if (seq(peek(0), "[")) {
                    eat("[");
                    char exprbuf[64]; int ep = 0;
                    while (!seq(peek(0), "]") && tp < ntk) {
                        const char *pt = eat(0);
                        for (int k = 0; pt[k] && ep < 63; ++k) exprbuf[ep++] = pt[k];
                        if (ep < 63) exprbuf[ep++] = ' ';
                    }
                    exprbuf[ep] = 0;
                    eat("]");
                    count = pp_eval_str(exprbuf);
                    if (count < 1 || count > 64) { fail("bad uniform array size"); break; }
                }
                eat(";");
                if (count > 1) {
                    for (int ai = 0; ai < count; ++ai) {
                        if (nvars >= 96 || R->nuni >= VSC_MAX_UNI || nc >= 112) { fail("out of constants"); break; }
                        char en[TLEN]; int p = 0; while (name[p]) { en[p] = name[p]; ++p; }
                        en[p++] = '['; char dg[8]; int dn = 0, v = ai; if (v == 0) dg[dn++] = '0'; else while (v) { dg[dn++] = (char)('0' + v % 10); v /= 10; }
                        while (dn) en[p++] = dg[--dn];
                        en[p++] = ']'; en[p] = 0;
                        scpy(vars[nvars].n, en, TLEN); vars[nvars].v = mkv(1, nc, w); vars[nvars].local = 0; ++nvars;
                        scpy(R->uni_name[R->nuni], en, TLEN); R->uni_idx[R->nuni] = (uint8_t)nc; R->uni_cnt[R->nuni] = 1; R->uni_width[R->nuni] = (uint8_t)w; ++R->nuni; ++nc;
                    }
                    continue;
                }
                if (nvars >= 96 || R->nuni >= VSC_MAX_UNI || nc >= 112) { fail("out of constants"); break; }
                scpy(vars[nvars].n, name, TLEN); vars[nvars].v = mkv(1, nc, w); vars[nvars].local = 0; ++nvars;
                scpy(R->uni_name[R->nuni], name, TLEN); R->uni_idx[R->nuni] = (uint8_t)nc; R->uni_cnt[R->nuni] = 1;R->uni_width[R->nuni]=(uint8_t)w; ++R->nuni; ++nc;
            } else if (kind == 1) {
                eat(";");
                static const char *an[] = { "aPosition", "aNormal", "aColor", "aTexCoord", "aTexCoord1" }; static const int as[] = { 0, 1, 2, 3, 4 };
                int slot = -1; for (int i = 0; i < 5; ++i) if (seq(name, an[i])) slot = as[i];
                if (slot < 0) {
                    int used[8] = { 0 }; for (int i = 0; i < nvars; ++i) if (vars[i].v.f == 0) used[vars[i].v.i] = 1;
                    const int *pref = w == 4 ? (const int[]){ 0, 2, 1, 3, 4, -1 } : w == 3 ? (const int[]){ 0, 1, 2, 3, 4, -1 } : w == 2 ? (const int[]){ 3, 4, 1, 2, -1 } : (const int[]){ 2, 1, 3, 4, -1 };
                    for (int i = 0; pref[i] >= 0; ++i) if (!used[pref[i]]) { slot = pref[i]; break; }
                    if (slot < 0) { fail("no free input slot"); break; }
                }
                scpy(vars[nvars].n, name, TLEN); vars[nvars].v = mkv(0, slot, w); vars[nvars].local = 0; ++nvars;
                if (R->nattr < 8) { scpy(R->attr_name[R->nattr], name, TLEN); R->attr_slot[R->nattr] = (uint8_t)slot; R->attr_w[R->nattr] = (uint8_t)w; ++R->nattr; }
            } else {
                eat(";");
                static const char *on[] = { "vColor", "vDiffuse", "vFog", "vTexCoord", "vTexCoord1" }; static const int os[] = { 1, 1, 2, 3, 4 };
                int slot = -1; for (int i = 0; i < 5; ++i) if (seq(name, on[i])) slot = os[i];
                int used[8] = { 0 }; for (int i = 0; i < nouts; ++i) used[outs[i].slot] = 1;
                if (slot >= 0 && used[slot]) slot = -1;
                if (slot < 0) {
                    const int *pref = (w == 4 || w == 3) ? (const int[]){ 6, 7, 1, 2, 4, 3, -1 } : w == 2 ? (const int[]){ 3, 4, 6, 7, 1, 2, -1 } : (const int[]){ 2, 6, 7, 1, 4, 3, -1 };
                    for (int i = 0; pref[i] >= 0; ++i) if (!used[pref[i]]) { slot = pref[i]; break; }
                    if (slot < 0) { fail("no free output slot"); break; }
                }
                if (nouts >= 8) { fail("too many varyings"); break; }
                scpy(outs[nouts].n, name, TLEN); outs[nouts].w = w; outs[nouts].slot = slot; outs[nouts].val = mkv(3, 0, w); ++nouts;
                if (R->nvary < 8) { scpy(R->vary_name[R->nvary], name, TLEN); R->vary_slot[R->nvary] = (uint8_t)slot; R->vary_w[R->nvary] = (uint8_t)w; ++R->nvary; }
            }
        } else if ((ty_w(t) || (seq(t, "void") && !seq(peek(0), "main"))) && !seq(peek(0), "(")) {
            if (nufn >= 8) { fail("too many user functions"); break; }
            scpy(ufn[nufn].n, eat(0), TLEN); ufn[nufn].is_void = seq(t, "void"); eat("(");
            int np = 0;
            while (!seq(peek(0), ")")) {
                if (np >= 4) { fail("too many function parameters"); break; }
                eat(0); scpy(ufn[nufn].pn[np], eat(0), TLEN); ++np;
                if (seq(peek(0), ",")) eat(",");
            }
            eat(")"); eat("{"); ufn[nufn].np = np; ufn[nufn].bs = tp;
            int depth = 0;
            while (tp < ntk) { const char *x = peek(0); if (seq(x, "{")) { ++depth; eat(0); } else if (seq(x, "}")) { if (depth == 0) break; --depth; eat(0); } else eat(0); }
            if (tp >= ntk || !seq(peek(0), "}")) { fail("unterminated function body"); break; }
            ufn[nufn].be = tp; eat("}"); ++nufn;
        } else if (seq(t, "void")) {
            eat("main"); eat("("); if (seq(peek(0), "void")) eat("void"); eat(")"); eat("{");
            while (!seq(peek(0), "}") && tp < ntk && !err_flag) stmt();
            eat("}");
        } else fail("unexpected token");
    }
    if (err_flag) return -1;
    if (!written_pos) { fail("gl_Position not written"); return -2; }
    return finish();
}
/* GPU-only pruning of encoded vertex code (component-level dead-code removal).
 * live_out: bit d = output register d is read downstream. Instructions whose
 * written lanes are never read (by a later instruction, or as a live output at
 * program end) are removed; partly dead writes keep only their live lanes.
 * Straight-line code on physical registers: outputs 0..15, temps 16..31.
 * Returns the new instruction count (code compacted in place). */
static unsigned vsc_reads(uint32_t op, unsigned need, uint32_t sw) {
    unsigned r = 0;
    if (op == DP3) { for (unsigned j = 0; j < 3; ++j) r |= 1u << (sw >> (2 * j) & 3u); return r; }
    if (op == DP4 || op == RCP || op == RSQ || op == EX2) { for (unsigned j = 0; j < 4; ++j) r |= 1u << (sw >> (2 * j) & 3u); return r; }
    for (unsigned l = 0; l < 4; ++l) if (need & (1u << l)) r |= 1u << (sw >> (2 * l) & 3u);
    return r;
}
uint32_t vsc_prune(uint32_t (*code)[2], uint32_t n, uint32_t live_out) {
    static uint8_t keep[VSC_MAX_INSTR];
    uint8_t live[32];
    if (n > VSC_MAX_INSTR) return n;
    for (unsigned r = 0; r < 32; ++r) live[r] = r < 16 && (live_out >> r & 1u) ? 15u : 0u;
    for (int i = (int)n - 1; i >= 0; --i) {
        uint32_t A = code[i][0], B = code[i][1], op = A & 31u, dst = B >> 26, mask = B >> 22 & 15u;
        unsigned known = op == MOV || op == ADD || op == MAD || op == MUL || op == RCP || op == RSQ || op == DP3 || op == DP4 || op == MIN || op == MAX || op == SGE || op == EX2;
        if (!known || dst >= 32) {            /* unknown encoding: keep it and treat every source as read */
            keep[i] = 1;
            for (unsigned k = 0; k < 3; ++k) if ((B >> (16 + 2 * k) & 3u) == 2u) live[16 + (B >> (4 * k) & 15u)] = 15u;
            continue;
        }
        unsigned need = mask & live[dst];
        keep[i] = need != 0;
        if (!need) continue;
        live[dst] &= (uint8_t)~mask;
        if (need != mask) code[i][1] = (B & ~(15u << 22)) | (uint32_t)need << 22;
        unsigned nsrc = op == MAD ? 3u : (op == MOV || op == RCP || op == RSQ || op == EX2) ? 1u : 2u;
        for (unsigned k = 0; k < nsrc; ++k) {
            if ((B >> (16 + 2 * k) & 3u) != 2u) continue;
            unsigned reg = 16u + (B >> (4 * k) & 15u);
            live[reg] |= (uint8_t)vsc_reads(op, need, A >> (8 + 8 * k) & 0xffu);
        }
    }
    uint32_t m = 0;
    for (uint32_t i = 0; i < n; ++i) if (keep[i]) { code[m][0] = code[i][0]; code[m][1] = code[i][1]; ++m; }
    return m;
}

/* ---- vsc_optimize: copy propagation + copy retargeting on native VGP code ----
 * The compiler builds most vectors in scratch registers and copies them into place
 * (OpenLara's lit room program: 36 of 89 instructions were MOVs). Two lane-exact
 * rewrites, then vsc_prune deletes what became dead:
 *  forward:  MOV D.m <- S.sw ; later reads of D lanes that still hold the copy read S
 *            (S unchanged meanwhile) with the swizzle composed and the negation folded;
 *  backward: p: T <- op ... ; MOV D.m <- T.sw where p's result is read only by that MOV:
 *            p writes D directly (component-wise ops get their source swizzles permuted,
 *            broadcast ops DP3/DP4/RCP/RSQ/EX2 just change destination).
 * Hardware rules kept: at most one distinct constant row and one distinct input
 * register per instruction; no new scratch registers; zero operands are not moved
 * into other instructions. Outputs (dst < 16) are only retargeted if allow_out. */
typedef struct { unsigned op, dst, mask, f[3], idx[3], sw[3][4], neg[3]; } vins;
static unsigned v_nsrc(unsigned op) { return op == MAD ? 3u : (op == MOV || op == RCP || op == RSQ || op == EX2) ? 1u : 2u; }
static int v_bcast(unsigned op) { return op == DP3 || op == DP4 || op == RCP || op == RSQ || op == EX2; }
static int v_known(unsigned op) { return op == MOV || op == ADD || op == MAD || op == MUL || op == RCP || op == RSQ || op == DP3 || op == DP4 || op == MIN || op == MAX || op == SGE || op == EX2; }
static void v_dec(const uint32_t *c, vins *v) {
    uint32_t A = c[0], B = c[1]; v->op = A & 31u; v->dst = B >> 26; v->mask = B >> 22 & 15u;
    for (unsigned k = 0; k < 3; ++k) {
        v->f[k] = B >> (16 + 2 * k) & 3u; v->idx[k] = B >> (4 * k) & 15u;
        if (v->f[k] == 1u) v->idx[k] |= (B >> 12 & 15u) << 4;
        for (unsigned l = 0; l < 4; ++l) v->sw[k][l] = A >> (8 + 8 * k + 2 * l) & 3u;
        v->neg[k] = A >> (5 + k) & 1u;
    }
}
static int v_enc(const vins *v, uint32_t *c) {
    uint32_t A = v->op, B = v->dst << 26 | v->mask << 22; int chi = -1;
    for (unsigned k = 0; k < 3; ++k) {
        unsigned sw = 0; for (unsigned l = 0; l < 4; ++l) sw |= v->sw[k][l] << (2 * l);
        A |= sw << (8 + 8 * k) | v->neg[k] << (5 + k);
        B |= v->f[k] << (16 + 2 * k);
        if (k < 3) {
            if (v->f[k] == 1u && k < v_nsrc(v->op)) { if (chi >= 0 && (unsigned)chi != v->idx[k] >> 4) return -1; chi = (int)(v->idx[k] >> 4); }
            if (k < 2 || v->f[k] != 1u || 1) B |= (v->idx[k] & 15u) << (4 * k);
        }
    }
    if (chi >= 0) B = (B & ~(15u << 12)) | (uint32_t)chi << 12;
    c[0] = A; c[1] = B; return 0;
}
static unsigned v_reads(const vins *v, unsigned k) {        /* lanes of source k read */
    unsigned sw = 0; for (unsigned l = 0; l < 4; ++l) sw |= v->sw[k][l] << (2 * l);
    return vsc_reads(v->op, v->mask, sw);
}
static unsigned v_reg(const vins *v, unsigned k) { return v->f[k] == 2u ? 16u + v->idx[k] : v->f[k] == 0u ? 100u + v->idx[k] : v->f[k] == 1u ? 200u + v->idx[k] : 999u; }
static int v_rules_ok(const vins *v) {                       /* one distinct constant row and input per instruction */
    int c = -1, in = -1;
    for (unsigned k = 0; k < v_nsrc(v->op); ++k) {
        if (v->f[k] == 1u) { if (c >= 0 && (unsigned)c != v->idx[k]) return 0; c = (int)v->idx[k]; }
        if (v->f[k] == 0u) { if (in >= 0 && (unsigned)in != v->idx[k]) return 0; in = (int)v->idx[k]; }
    }
    return 1;
}
static int v_fwd(vins *v, unsigned n) {
    int changed = 0;
    for (unsigned m = 0; m < n; ++m) {
        vins *c = &v[m];
        if (c->op != MOV || c->dst < 16 || c->f[0] == 3u) continue;
        unsigned D = c->dst, S = v_reg(c, 0), held = c->mask;   /* D lanes still holding the copy */
        unsigned sdirty = 0;                                     /* S lanes overwritten since m */
        for (unsigned j = m + 1; j < n && held; ++j) {
            vins *u = &v[j];
            if (!v_known(u->op)) break;
            for (unsigned k = 0; k < v_nsrc(u->op); ++k) {
                if (v_reg(u, k) != D) continue;
                unsigned L = v_reads(u, k); if (!L || (L & ~held)) continue;
                unsigned sl = 0; for (unsigned l = 0; l < 4; ++l) if (L >> l & 1u) sl |= 1u << c->sw[0][l];
                if (sl & sdirty) continue;
                vins t = *u; t.f[k] = c->f[0]; t.idx[k] = c->idx[0]; t.neg[k] ^= c->neg[0];
                for (unsigned l = 0; l < 4; ++l) t.sw[k][l] = c->sw[0][u->sw[k][l]];
                if (!v_rules_ok(&t)) continue;
                *u = t; changed = 1;
            }
            if (u->dst == D) held &= ~u->mask;
            if (S >= 16 && S < 32 && u->dst == S) sdirty |= u->mask;
        }
    }
    return changed;
}
static int v_back(vins *v, unsigned *pn, int allow_out) {
    unsigned n = *pn; int changed = 0;
    for (unsigned m = 0; m < n; ++m) {
        vins *c = &v[m];
        if (c->op != MOV || c->f[0] != 2u || c->neg[0]) continue;
        if (c->dst < 16 && !allow_out) continue;
        unsigned T = 16u + c->idx[0], D = c->dst, M = c->mask, R = 0;
        for (unsigned l = 0; l < 4; ++l) if (M >> l & 1u) R |= 1u << c->sw[0][l];
        int p = -1; unsigned cover = 0;                         /* last writer of the lanes R */
        for (int j = (int)m - 1; j >= 0; --j) { if (v[j].dst == T && (v[j].mask & R & ~cover)) { if (p < 0) p = j; else if ((unsigned)j != (unsigned)p) { p = -2; break; } cover |= v[j].mask & R; } if ((cover & R) == R) break; }
        if (p < 0 || (cover & R) != R) continue;
        vins *q = &v[p];
        if (!v_known(q->op) || (q->mask & R) != R) continue;
        int ok = 1;
        /* p's result (all lanes it writes to T) is read only by m */
        unsigned alive = q->mask;
        for (unsigned j = (unsigned)p + 1; j < n && alive && ok; ++j) {
            const vins *u = &v[j];
            if (!v_known(u->op)) { ok = 0; break; }
            for (unsigned k = 0; k < v_nsrc(u->op); ++k) if (v_reg(u, k) == T && (v_reads(u, k) & alive) && j != m) ok = 0;
            if (u->dst == T) alive &= ~u->mask;
        }
        if (!ok) continue;
        /* D lanes M neither read nor written between p and m */
        for (unsigned j = (unsigned)p + 1; j < m && ok; ++j) {
            const vins *u = &v[j];
            if (u->dst == D && (u->mask & M)) ok = 0;
            for (unsigned k = 0; k < v_nsrc(u->op); ++k) if (v_reg(u, k) == D && (v_reads(u, k) & M)) ok = 0;
        }
        if (!ok) continue;
        vins t = *q; t.dst = D; t.mask = M;
        if (!v_bcast(q->op)) for (unsigned k = 0; k < 3; ++k) for (unsigned l = 0; l < 4; ++l) t.sw[k][l] = (M >> l & 1u) ? q->sw[k][c->sw[0][l]] : q->sw[k][l];
        else if (q->op != DP3 && q->op != DP4) { /* scalar ops read lane w of the source swizzle: unchanged */ }
        v[p] = t;
        for (unsigned j = m; j + 1 < n; ++j) v[j] = v[j + 1];
        --n; --m; changed = 1;
    }
    *pn = n; return changed;
}
uint32_t vsc_optimize(uint32_t (*code)[2], uint32_t n, uint32_t live_out, int allow_out) {
    static vins v[VSC_MAX_INSTR];
    if (n > VSC_MAX_INSTR) return n;
    for (unsigned it = 0; it < 6; ++it) {
        for (unsigned i = 0; i < n; ++i) v_dec(code[i], &v[i]);
        int ch = v_fwd(v, n);
        unsigned nn = n; ch |= v_back(v, &nn, allow_out);
        static uint32_t tmpc[VSC_MAX_INSTR][2];
        for (unsigned i = 0; i < nn; ++i) if (v_enc(&v[i], tmpc[i])) return n;   /* should not happen: keep original */
        for (unsigned i = 0; i < nn; ++i) { code[i][0] = tmpc[i][0]; code[i][1] = tmpc[i][1]; }
        n = vsc_prune(code, nn, live_out);
        if (!ch) break;
    }
    return n;
}

/* ---- vsc_fold_zero: specialise native code for uniform lanes that are exactly zero ----
 * zero_rows[ci] = lanes of constant row ci known to be 0.0. Known-zero lanes are
 * propagated (x*0 = 0, x+0 = x, a*b+c with a zero product = c, dot of zero = 0);
 * instructions whose written lanes are all zero become MOV dst <- zero operand (the
 * form prune_native already uses on hardware), ADD/MAD with a dead term become MOVs,
 * then vsc_optimize forwards the copies and prunes everything that only fed zeros
 * (OpenLara rooms: lights 1-3 at colour 0 make their whole lighting chain dead).
 * Zero operands are never placed in non-MOV instructions. */
uint32_t vsc_fold_zero(uint32_t (*code)[2], uint32_t n, const uint8_t *zero_rows, uint32_t live_out, int allow_out) {
    static vins v[VSC_MAX_INSTR];
    if (n > VSC_MAX_INSTR) return n;
    uint8_t zr[32]; for (unsigned r = 0; r < 32; ++r) zr[r] = 0;   /* known-zero lanes of outputs (0..15) and temps (16..31) */
    for (unsigned i = 0; i < n; ++i) {
        v_dec(code[i], &v[i]); vins *u = &v[i];
        if (!v_known(u->op) || u->dst >= 32) return n;
        unsigned zl[3] = {0, 0, 0};                                  /* lanes (after swizzle) of each source known zero */
        for (unsigned k = 0; k < v_nsrc(u->op); ++k) {
            unsigned regz = u->f[k] == 3u ? 15u : u->f[k] == 1u ? (u->idx[k] < 256u ? zero_rows[u->idx[k]] : 0u) : u->f[k] == 2u ? zr[16 + u->idx[k]] : 0u;
            for (unsigned l = 0; l < 4; ++l) if (regz >> u->sw[k][l] & 1u) zl[k] |= 1u << l;
        }
        unsigned M = u->mask, res = 0;
        switch (u->op) {
        case MOV: res = zl[0]; break;
        case MUL: res = zl[0] | zl[1]; break;
        case ADD: res = zl[0] & zl[1];
            if ((zl[0] & 15u) != 15u && (zl[1] & M) == M) { u->op = MOV; }
            else if ((zl[0] & M) == M && (zl[1] & M) != M) { u->op = MOV; u->f[0] = u->f[1]; u->idx[0] = u->idx[1]; u->neg[0] = u->neg[1]; for (unsigned l = 0; l < 4; ++l) u->sw[0][l] = u->sw[1][l]; }
            break;
        case MAD: { unsigned prod = zl[0] | zl[1]; res = prod & zl[2];
            if ((prod & M) == M) { u->op = MOV; u->f[0] = u->f[2]; u->idx[0] = u->idx[2]; u->neg[0] = u->neg[2]; for (unsigned l = 0; l < 4; ++l) u->sw[0][l] = u->sw[2][l]; }
            break; }
        case DP3: { unsigned t = zl[0] | zl[1]; if ((t & 7u) == 7u) res = 15u; break; }
        case DP4: { unsigned t = zl[0] | zl[1]; if ((t & 15u) == 15u) res = 15u; break; }
        case MIN: case MAX: res = zl[0] & zl[1]; break;
        default: res = 0; break;
        }
        if ((res & M) == M && !(u->op == MOV && u->f[0] == 3u)) {   /* all written lanes are zero */
            u->op = MOV; u->f[0] = 3u; u->idx[0] = 0; u->neg[0] = 0; for (unsigned l = 0; l < 4; ++l) u->sw[0][l] = l;
        }
        if (u->op == MOV) for (unsigned k = 1; k < 3; ++k) { u->f[k] = 3u; u->idx[k] = 0; u->neg[k] = 0; for (unsigned l = 0; l < 4; ++l) u->sw[k][l] = l; }
        zr[u->dst] = (uint8_t)((zr[u->dst] & ~M) | (res & M));
    }
    for (unsigned i = 0; i < n; ++i) if (v_enc(&v[i], code[i])) return n;
    return vsc_optimize(code, n, live_out, allow_out);
}
