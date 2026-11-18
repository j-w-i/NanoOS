/* Multi-pass fragment lowering. Included privately by gl2api.c after gl2fsc.inc.
 *
 * The MBX pixel stage is a fixed two-stage combiner with one texture per stage and one vertex
 * colour. A fragment shader that does not fit one combiner pass is rewritten as several GPU passes
 * over the same geometry, accumulated in the framebuffer with blending:
 *
 *   colour = sum over terms of  V * product(texture factors)
 *
 * - V is everything that does not depend on a texture sample (varyings, uniforms, constants, any
 *   builtin). It runs per vertex in that pass's vertex program and reaches the combiner as the
 *   vertex colour, so each pass gets its own.
 * - A texture factor is a sample T, T.a, 1-T or 1-T.a at any per-vertex coordinate. Each pass
 *   samples one lookup ("site"); its coordinate is computed by the pass's vertex program, so
 *   offset taps and scaled or scrolling coordinates are real, not approximated.
 * - A dot product of a texture with a per-vertex vector, clamped (clamp(dot(t * 2 - 1, l), 0, 1),
 *   max(..., 0)), is a factor too: its pass runs the combiner's DOT3 with the vector in the vertex
 *   colour (normal mapping).
 * - Passes combine with blend states proven on the device: first pass opaque, then ONE/ONE (add)
 *   or DST_COLOR/ZERO (multiply, for a product of two different lookups). Negative terms use two
 *   ONE_MINUS_DST_COLOR/ZERO invert passes around an add (GL2_MP_INVERT).
 *
 * Not expressible, so the lowering fails and the caller keeps its old behaviour: a nonlinear
 * function of a texture sample (pow, step, a clamp that can bite inside the expression), a texture
 * coordinate that depends on another sample, more than one term that multiplies two lookups.
 * 8 bits per channel and a clamp to [0,1] after every pass apply, as on any fixed-function target. */
#ifndef GL2_MP_INVERT
#define GL2_MP_INVERT 1
#endif
#ifndef GL2_MP_ACC_RESTORE
#define GL2_MP_ACC_RESTORE 1   /* additive list: end with alpha := 1, as GL leaves it after ONE/ONE with an opaque shader (one more pass) */
#endif
#define MP_MAX_PASS 6    /* per pass list (the ordinary one and the additive one, MP_SETA..MP_ADDDA) */
#define MP_POOL 192
#define MP_LANE 12
#define MP_GROUPS 8
enum{MP_FIRST=0,MP_ADD,MP_MUL,MP_INV,MP_SETA,MP_MULA,MP_ADDDA,MP_SQ};
#define MP_ACC_MAX 16   /* additive list length: a specular pow(x, k) costs k/2 passes there */
/* MP_SQ  ZERO/DST_ALPHA: the framebuffer times its own alpha. With rgb = alpha = x (a squared dot
 *        product written as (s, s, s, s)) that is x^2 in every channel: pow(x, 2^m) in m passes. */
/* Additive draws (the application blends ONE/ONE, as per-light accumulation does) cannot multiply in
 * the framebuffer's colour, so the product goes through destination alpha instead:
 *   MP_SETA  ONE/ONE, (0,0,0,1)          alpha := 1, colour unchanged
 *   MP_MULA  ZERO/SRC_COLOR, (1,1,1,f)   alpha *= f (a dot product or a scalar lookup)
 *   MP_ADDDA DST_ALPHA/ONE, (T * V, 0)   colour += T * V * alpha
 *   MP_SETA again (GL2_MP_ACC_RESTORE)    alpha back to 1 */
typedef struct{float c;uint16_t v;uint8_t n,at[4];}mp_mono;            /* c * v * product(at); at = site<<3 | complement<<2 | lane */
typedef struct{uint8_t n,at[4],neg;float c[3];uint16_t v[3];}mp_group;  /* one RGB term; at = site<<3 | complement<<2 | (3 alpha replicate, 0 rgb) */
typedef struct{uint8_t op,site,nat,at[2],scale,amode,usek;uint16_t v[3],va;}mp_pass; /* site 0xff: no texture; at = complement<<2 | sel */
static struct{unsigned npass,nacc;mp_pass pass[MP_MAX_PASS+MP_ACC_MAX];   /* pass[npass .. npass+nacc): the additive list */uint8_t blend_ok,nsampler,sampler[2];char sampler_name[2][32];
 gl2_mp_lut lut;uint8_t lut_vary,lut_site;uint32_t lut_uniused;}mp;   /* lut.on: site lut_site is a lookup table over normalize(varying lut_vary) */
static mp_mono mp_pool[MP_POOL],mp_lane[4][MP_LANE];static unsigned mp_top,mp_lane_n[4],mp_ng;static int mp_bad;static mp_group mp_g[MP_GROUPS];
static gl2_fs_owned mp_scratch;
/* Dot products of a texture with a per-vertex vector, sum_k a * (T_k - 0.5) * B_k: the combiner's DOT3
 * (MBX_GPU.md "DOT3"). Each is its own lookup site, sampling the texture of the T it reads; its pass
 * gets 0.5 + a/4 * B in the vertex colour and outputs clamp(the sum), grey. bare: not under a clamp
 * or max(x, 0); only exact where it is the lane's only term. */
/* mp_dot_hw: the pass programs run on the MBX (not the host's float rasteriser): encode the light
 * vector for the hardware's measured conversion (hw-reports/run-dark2, run-dark3):
 *   DOT3 = 4 * sum (t - 127.5)/255 * (p - 128)/255, rounded down: the colour operand's zero is 128;
 *   vertex colour float -> byte: round(255 v) up to 0.5, about floor(255.56 v - 0.56) above.
 * So p = 128 + 255 x (x = a/4 * B) needs v = 0.505009 + 0.997809 x for x >= 0, else min(0.5, x + 128/255). */
static int mp_dot_hw;
static uint8_t mp_dot_is[F_MP_SITES],mp_dot_bare[F_MP_SITES],mp_dot_k[F_MP_SITES];   /* k: pow(clamped dot, k), 1 or even */static float mp_dot_a[F_MP_SITES];static uint16_t mp_dot_b[F_MP_SITES][3],mp_dot_node[F_MP_SITES];
static int mp_dot_match(uint16_t id,unsigned *site_out,float *a_out,uint16_t B[3]){
 uint16_t terms[32];unsigned nt=f_terms(F_ADD,id,terms,0),seen=0;int site=-1;float al=0;
 if(f.error||nt>3)return -1;
 for(unsigned k=0;k<3;++k)B[k]=1;   /* const 0: a lane the sum does not read */
 for(unsigned i=0;i<nt;++i){
  uint16_t fac[32];unsigned nf=f_terms(F_MUL,terms[i],fac,0);int ai=-1;
  if(f.error)return -1;
  for(unsigned j=0;j<nf;++j)if(f.nodes[fac[j]].deps&0xffu){if(ai>=0)return -1;ai=(int)j;}
  if(ai<0)return -1;
  f_node A=f.nodes[fac[ai]];if(A.op!=F_SUB||f.nodes[A.b].op!=F_CONST)return -1;
  uint16_t x=A.a;float a=1;
  if(f.nodes[x].op==F_MUL){
   uint16_t xt[32];unsigned nx=f_terms(F_MUL,x,xt,0);
   if(nx!=2)return -1;
   if(f.nodes[xt[0]].op==F_CONST){a=f.nodes[xt[0]].value;x=xt[1];}
   else if(f.nodes[xt[1]].op==F_CONST){a=f.nodes[xt[1]].value;x=xt[0];}
   else return -1;
  }
  f_node X=f.nodes[x];
  if(X.op!=F_INPUT||X.a<F_MP_SITE||a==0)return -1;
  unsigned s=(X.a-F_MP_SITE)/4u,lane=(X.a-F_MP_SITE)&3u;
  float c=f.nodes[A.b].value,e=c-0.5f*a;if(lane>2||e>1e-6f||e<-1e-6f||mp_dot_is[s])return -1;   /* A = a * (T - 0.5) */
  if(site<0){site=(int)s;al=a;}else if(site!=(int)s||al!=a)return -1;
  if(seen&(1u<<lane))return -1;
  seen|=1u<<lane;
  uint16_t b=2;for(unsigned j=0;j<nf;++j)if((int)j!=ai)b=f_op(F_MUL,b,fac[j]);
  if(f.error||(f.nodes[b].deps&0xffu))return -1;
  B[lane]=b;
 }
 if(site<0)return -1;
 *site_out=(unsigned)site;*a_out=al;return 0;
}
static int mp_dot_site(uint16_t id,int bare,unsigned k){
 for(unsigned s=0;s<f_mp_nsite;++s)if(mp_dot_is[s]&&mp_dot_node[s]==id&&mp_dot_k[s]==k){if(!bare)mp_dot_bare[s]=0;return (int)s;}
 unsigned ts;float a;uint16_t B[3];
 if(mp_dot_match(id,&ts,&a,B))return -1;
 if(f_mp_nsite>=F_MP_SITES)return -1;
 unsigned s=f_mp_nsite++;
 f_mp_site_sampler[s]=f_mp_site_sampler[ts];f_mp_site_uv[s][0]=f_mp_site_uv[ts][0];f_mp_site_uv[s][1]=f_mp_site_uv[ts][1];
 mp_dot_is[s]=1;mp_dot_bare[s]=(uint8_t)bare;mp_dot_k[s]=(uint8_t)k;mp_dot_a[s]=a;mp_dot_node[s]=id;for(unsigned k=0;k<3;++k)mp_dot_b[s][k]=B[k];
 return (int)s;
}
static unsigned mp_push(float c,uint16_t v,unsigned n,const uint8_t *at){
 if(mp_top>=MP_POOL){mp_bad=1;return 0;}
 mp_mono *m=&mp_pool[mp_top++];m->c=c;m->v=v;m->n=(uint8_t)n;for(unsigned i=0;i<n;++i)m->at[i]=at[i];return 1;
}
/* Appends the monomials of node id to the pool; returns how many. */
static unsigned mp_expand(uint16_t id,unsigned depth){
 if(mp_bad)return 0;
 if(depth>64){mp_bad=1;return 0;}
 f_node n=f.nodes[id];
 if(!(n.deps&0xffu)){if(n.op==F_CONST)return n.value!=0?mp_push(n.value,2,0,0):0;return mp_push(1,id,0,0);}
 if(n.op==F_INPUT){uint8_t a=(uint8_t)(((n.a-F_MP_SITE)/4u)<<3|((n.a-F_MP_SITE)&3u));return mp_push(1,2,1,&a);}
 /* clamp(dot, 0, 1), max(dot, 0) or a bare dot: one grey factor (lane 3) */
 if(n.op==F_CLAMP||n.op==F_ADD||n.op==F_SUB||(n.op==F_FN&&(unsigned)n.value==FN_MAX)){
  int si=-1;
  if(n.op==F_CLAMP)si=mp_dot_site(n.a,0,1);
  else if(n.op==F_FN){if(f_is(n.b,0))si=mp_dot_site(n.a,0,1);else if(f_is(n.a,0))si=mp_dot_site(n.b,0,1);}
  else if(n.op==F_ADD)si=mp_dot_site(id,1,1);
  if(si>=0){uint8_t a=(uint8_t)(si<<3|3);return mp_push(1,2,1,&a);}
  if(f.error){mp_bad=1;return 0;}
 }
 /* pow(clamp(dot, 0, 1), k) or pow(max(dot, 0), k), k 1 or even: specular */
 if(n.op==F_FN&&(unsigned)n.value==FN_POW&&f.nodes[n.b].op==F_CONST){
  float kf=f.nodes[n.b].value;int k=(int)kf;f_node x=f.nodes[n.a];uint16_t inner=0;
  if((float)k==kf&&k>=1&&k<=64&&(k==1||!(k&1))){
   if(x.op==F_CLAMP)inner=x.a;
   else if(x.op==F_FN&&(unsigned)x.value==FN_MAX){if(f_is(x.b,0))inner=x.a;else if(f_is(x.a,0))inner=x.b;}
  }
  if(inner){int si=mp_dot_site(inner,0,(unsigned)k);if(si>=0){uint8_t a=(uint8_t)(si<<3|3);return mp_push(1,2,1,&a);}}
  if(f.error){mp_bad=1;return 0;}
 }
 if(n.op==F_CLAMP){f_node x=f.nodes[n.a];if(x.lo<0||x.hi>1){mp_bad=1;return 0;}return mp_expand(n.a,depth+1);}
 if(n.op==F_SUB){
  f_node b=f.nodes[n.b];
  if(f_is(n.a,1)&&b.op==F_INPUT&&b.a>=F_MP_SITE){uint8_t a=(uint8_t)(((b.a-F_MP_SITE)/4u)<<3|4u|((b.a-F_MP_SITE)&3u));return mp_push(1,2,1,&a);}
  unsigned na=mp_expand(n.a,depth+1),s=mp_top,nb=mp_expand(n.b,depth+1);
  for(unsigned i=0;i<nb;++i)mp_pool[s+i].c=-mp_pool[s+i].c;
  return na+nb;
 }
 if(n.op==F_ADD){unsigned na=mp_expand(n.a,depth+1);return na+mp_expand(n.b,depth+1);}
 if(n.op==F_MUL){
  unsigned s=mp_top,na=mp_expand(n.a,depth+1),nb=mp_expand(n.b,depth+1),t=mp_top,cnt=0;
  for(unsigned i=0;i<na&&!mp_bad;++i)for(unsigned j=0;j<nb&&!mp_bad;++j){
   const mp_mono *x=&mp_pool[s+i],*y=&mp_pool[s+na+j];uint8_t at[4];unsigned k=0;
   if(x->n+y->n>4){mp_bad=1;break;}
   for(unsigned q=0;q<x->n;++q)at[k++]=x->at[q];
   for(unsigned q=0;q<y->n;++q)at[k++]=y->at[q];
   for(unsigned q=1;q<k;++q){uint8_t v=at[q];unsigned r=q;while(r&&at[r-1]>v){at[r]=at[r-1];--r;}at[r]=v;}
   cnt+=mp_push(x->c*y->c,f_op(F_MUL,x->v,y->v),k,at);
  }
  if(mp_bad||f.error){mp_bad=1;return 0;}
  for(unsigned i=0;i<cnt;++i)mp_pool[s+i]=mp_pool[t+i];
  mp_top=s+cnt;return cnt;
 }
 mp_bad=1;return 0;   /* a builtin of a per-pixel value */
}
static int mp_same(const mp_mono *a,const mp_mono *b){if(a->n!=b->n)return 0;for(unsigned i=0;i<a->n;++i)if(a->at[i]!=b->at[i])return 0;return 1;}
static uint16_t mp_scaled(float c,uint16_t v){return c==1?v:f_op(F_MUL,f_const(c),v);}
static int mp_lane_build(uint16_t id,unsigned lane){
 mp_top=0;unsigned n=mp_expand(id,0),out=0;if(mp_bad)return -1;
 mp_mono *L=mp_lane[lane];
 for(unsigned i=0;i<n;++i){
  mp_mono m=mp_pool[i];unsigned j=0;
  for(;j<out;++j)if(mp_same(&L[j],&m))break;
  if(j==out){if(out>=MP_LANE)return -1;L[out++]=m;continue;}
  if(L[j].c<0&&m.c<0){L[j].v=f_op(F_ADD,mp_scaled(-L[j].c,L[j].v),mp_scaled(-m.c,m.v));L[j].c=-1;}
  else{L[j].v=f_op(F_ADD,mp_scaled(L[j].c,L[j].v),mp_scaled(m.c,m.v));L[j].c=1;}
 }
 unsigned k=0;
 for(unsigned j=0;j<out;++j){f_node v=f.nodes[L[j].v];if(v.op==F_CONST){L[j].c*=v.value;L[j].v=2;}if(L[j].c!=0)L[k++]=L[j];}
 /* the hardware clamps a dot product at 0 and 1: a bare one is exact only as the lane's single term */
 if(k>1)for(unsigned j=0;j<k;++j)for(unsigned q=0;q<L[j].n;++q)if(mp_dot_is[L[j].at[q]>>3]&&mp_dot_bare[L[j].at[q]>>3])return -1;
 mp_lane_n[lane]=k;return f.error?-1:0;
}
static unsigned mp_sites(const mp_group *g,uint8_t *sites){
 unsigned n=0;
 for(unsigned i=0;i<g->n;++i){unsigned s=g->at[i]>>3,j=0;for(;j<n;++j)if(sites[j]==s)break;if(j==n)sites[n++]=(uint8_t)s;}
 return n;
}
static int mp_gdot(const mp_group *g){for(unsigned i=0;i<g->n;++i)if(mp_dot_is[g->at[i]>>3])return (int)(g->at[i]>>3);return -1;}
static int mp_emit(unsigned op,const mp_group *g,int site,int withv){
 if(mp.npass>=MP_MAX_PASS+MP_ACC_MAX)return -1;
 mp_pass *p=&mp.pass[mp.npass++];p->op=(uint8_t)op;p->site=(uint8_t)(site<0?0xff:site);p->nat=0;p->scale=1;p->amode=0;p->usek=0;p->va=(uint16_t)(op==MP_ADD||op==MP_FIRST?1:2);
 for(unsigned k=0;k<3;++k)p->v[k]=2;
 if(!g)return 0;
 for(unsigned i=0;i<g->n;++i)if(site>=0&&(g->at[i]>>3)==(unsigned)site){if(p->nat>=2)return -1;p->at[p->nat++]=g->at[i]&7u;}
 if(withv){
  float m=0;for(unsigned k=0;k<3;++k)if(g->c[k]>m)m=g->c[k];
  if(m>4.0001f)return -1;
  p->scale=(uint8_t)(m>2.0001f?4:m>1.0001f?2:1);
  for(unsigned k=0;k<3;++k)p->v[k]=g->c[k]==0?1:mp_scaled(g->c[k]/(float)p->scale,g->v[k]);
 }
 return f.error?-1:0;
}
/* ---- lookup tables: per-pixel functions of a direction ----
 * A term's per-vertex factor V normally reaches the pixel as an interpolated vertex colour, which is
 * only right when V is linear across the triangle. When V depends on a single vec3 varying v and
 * only on its direction (V(t*v) = V(v): the shader wrote normalize(v), as lighting does), the term
 * is evaluated per pixel instead: the CPU bakes V over all directions into a small sphere-map
 * texture (again whenever a uniform it reads changes), the vertex program outputs the sphere-map
 * coordinate of v, and the pass multiplies by the texture. The table holds V/2, the pass doubles. */
static uint16_t mp_st_v[F_NODES],mp_st_d[F_NODES],mp_st_now,mp_lut_map[F_NODES];static uint16_t mp_vm[F_NODES];static float mp_dg[F_NODES];
#define MP_NODEG 1e9f    /* not homogeneous in the varying */
#define MP_ANYDEG 2e9f   /* identically zero: fits any degree */
/* Varyings a node reads (bit j), bit 8: reads a texture lookup. */
static unsigned mp_vmask(uint16_t id,unsigned depth){
 if(mp_st_v[id]==mp_st_now)return mp_vm[id];
 f_node n=f.nodes[id];unsigned m=0;
 if(depth>64)m=0x100u;
 else if(n.op==F_INPUT){if(n.a>=F_MP_SITE)m=0x100u;else if(n.a>=F_MP_VARY)m=1u<<((n.a-F_MP_VARY)/4u);}
 else if(n.op!=F_CONST){m=mp_vmask(n.a,depth+1);if(n.op!=F_CLAMP&&n.b)m|=mp_vmask(n.b,depth+1);}
 mp_st_v[id]=mp_st_now;mp_vm[id]=(uint16_t)m;return m;
}
/* Degree of homogeneity of a node in varying j: f(t*v) = t^deg * f(v). */
static float mp_deg(uint16_t id,unsigned j,unsigned depth){
 if(mp_st_d[id]==mp_st_now)return mp_dg[id];
 f_node n=f.nodes[id];float r=MP_NODEG;
 if(depth>64)r=MP_NODEG;
 else if(n.op==F_CONST)r=n.value==0?MP_ANYDEG:0;
 else if(n.op==F_INPUT)r=(n.a>=F_MP_VARY&&n.a<F_MP_SITE)?((n.a-F_MP_VARY)/4u==j?1.f:MP_NODEG):n.a>=F_MP_SITE?MP_NODEG:0;
 else{
  float a=mp_deg(n.a,j,depth+1),b=(n.op!=F_CLAMP&&n.b)?mp_deg(n.b,j,depth+1):0;
  unsigned fn=n.op==F_FN?(unsigned)n.value:0;
  if(a==MP_NODEG||b==MP_NODEG)r=MP_NODEG;
  else if(n.op==F_ADD||n.op==F_SUB||fn==FN_MIN||fn==FN_MAX)r=a==MP_ANYDEG?b:b==MP_ANYDEG?a:a==b?a:MP_NODEG;
  else if(n.op==F_MUL)r=(a==MP_ANYDEG||b==MP_ANYDEG)?MP_ANYDEG:a+b;
  else if(n.op==F_CLAMP||fn==FN_EXP||fn==FN_FLOOR||fn==FN_FRACT||fn==FN_SIN||fn==FN_COS)r=(a==0||a==MP_ANYDEG)?0:MP_NODEG;
  else if(fn==FN_DIV)r=b==MP_ANYDEG?MP_NODEG:a==MP_ANYDEG?MP_ANYDEG:a-b;
  else if(fn==FN_ABS)r=a;
  else if(fn==FN_SQRT)r=a==MP_ANYDEG?a:a*0.5f;
  else if(fn==FN_RSQ)r=a==MP_ANYDEG?MP_NODEG:-a*0.5f;
  else if(fn==FN_SIGN)r=0;
  else if(fn==FN_STEP)r=(a==b||a==MP_ANYDEG||b==MP_ANYDEG)?0:MP_NODEG;
  else if(fn==FN_POW)r=f.nodes[n.b].op!=F_CONST?((a==0||a==MP_ANYDEG)&&b==0?0:MP_NODEG):a==MP_ANYDEG?a:a*f.nodes[n.b].value;
 }
 mp_st_d[id]=mp_st_now;mp_dg[id]=r;return r;
}
static uint16_t mp_lut_len2;
/* Serialise the expression of node id into mp.lut (inputs: direction lanes of varying j, uniforms). Index or -1. */
static int mp_lut_emit(uint16_t id,unsigned j,unsigned depth){
 if(mp_lut_map[id])return mp_lut_map[id]-1;
 if(depth>64)return -1;
 f_node n=f.nodes[id];gl2_mp_lut_insn in;in.op=n.op;in.fn=0;in.a=in.b=0;in.value=n.value;
 /* the table is evaluated on unit vectors: the shader's own 1/length and length of the varying are 1 */
 if(n.op==F_FN&&n.a==mp_lut_len2&&((unsigned)n.value==FN_RSQ||(unsigned)n.value==FN_SQRT)){in.op=F_CONST;in.value=1;}
 if(in.op==F_CONST){}
 else if(n.op==F_INPUT){
  if(n.a>=F_MP_VARY&&n.a<F_MP_SITE){if((n.a-F_MP_VARY)/4u!=j||((n.a-F_MP_VARY)&3u)>2u)return -1;in.a=(uint16_t)((n.a-F_MP_VARY)&3u);}
  else if(n.a>=GL2_SOFT_UNIFORM_BASE&&n.a<GL2_SOFT_UNIFORM_BASE+64u){in.a=(uint16_t)(16u+(n.a-GL2_SOFT_UNIFORM_BASE));mp.lut_uniused|=1u<<((n.a-GL2_SOFT_UNIFORM_BASE)/4u);}
  else return -1;
 }else{
  int a=mp_lut_emit(n.a,j,depth+1);if(a<0)return -1;in.a=(uint16_t)a;
  if(n.op==F_FN){in.fn=(uint8_t)n.value;if(in.fn==FN_SIN||in.fn==FN_COS)return -1;}
  if(n.op!=F_CLAMP&&n.b){int b=mp_lut_emit(n.b,j,depth+1);if(b<0)return -1;in.b=(uint16_t)b;}
 }
 if(mp.lut.n>=GL2_MP_LUT_INS)return -1;
 mp.lut.code[mp.lut.n]=in;mp_lut_map[id]=(uint16_t)(++mp.lut.n);return mp.lut.n-1;
}
static unsigned mp_norm_atom(unsigned at,unsigned lane){unsigned l=at&3u;return l==3u?(at|3u):l==lane?(at&~3u):0xffu;}
/* Turn one direction-only term into a lookup-table factor (rewrites mp_lane). */
static void mp_lut_pick(void){
 mp.lut.on=0;mp.lut.n=0;mp.lut_uniused=0;
 if(f_mp_nsite>=F_MP_SITES)return;
 if(!++mp_st_now){for(unsigned i=0;i<F_NODES;++i)mp_st_v[i]=mp_st_d[i]=0;mp_st_now=1;}
 for(unsigned lane=0;lane<3;++lane)for(unsigned i=0;i<mp_lane_n[lane];++i){
  const mp_mono *m=&mp_lane[lane][i];
  if(m->c<=0||m->n>=4||f.nodes[m->v].op==F_CONST)continue;
  unsigned vm=mp_vmask(m->v,0),j=0;
  while(j<8&&vm!=(1u<<j))++j;
  if(j==8||f_mp_vw[j]<3)continue;
  /* the degree memo is per varying */
  if(!++mp_st_now){for(unsigned q=0;q<F_NODES;++q)mp_st_v[q]=mp_st_d[q]=0;mp_st_now=1;}
  if(mp_deg(m->v,j,0)!=0)continue;
  uint16_t outv[3]={1,1,1};int idx[3]={-1,-1,-1};
  for(unsigned l2=0;l2<3;++l2)for(unsigned i2=0;i2<mp_lane_n[l2];++i2){
   const mp_mono *m2=&mp_lane[l2][i2];
   if(m2->c<=0||m2->n!=m->n)continue;
   unsigned q=0;for(;q<m->n;++q)if(mp_norm_atom(m2->at[q],l2)!=mp_norm_atom(m->at[q],lane)||mp_norm_atom(m->at[q],lane)==0xffu)break;
   if(q!=m->n)continue;
   unsigned vm2=mp_vmask(m2->v,0);
   if(vm2&&(vm2!=(1u<<j)||mp_deg(m2->v,j,0)!=0))continue;
   idx[l2]=(int)i2;outv[l2]=mp_scaled(m2->c*0.5f,m2->v);break;
  }
  if(f.error)return;
  for(unsigned q=0;q<F_NODES;++q)mp_lut_map[q]=0;
  {uint16_t d2=1;for(unsigned q=0;q<3;++q){uint16_t vq=f_intern(F_INPUT,F_MP_VARY+4u*j+q,0,0);d2=f_op(F_ADD,d2,f_op(F_MUL,vq,vq));}mp_lut_len2=d2;}
  mp.lut.n=0;mp.lut_uniused=0;int ok=1;
  for(unsigned k=0;k<3&&ok;++k){int o=mp_lut_emit(outv[k],j,0);if(o<0)ok=0;else mp.lut.out[k]=(uint16_t)o;}
  if(!ok){mp.lut.n=0;continue;}
  unsigned si=f_mp_nsite++;f_mp_site_sampler[si]=0xfe;f_mp_site_uv[si][0]=f_mp_site_uv[si][1]=1;
  mp.lut_vary=(uint8_t)j;mp.lut_site=(uint8_t)si;mp.lut.on=1;
  for(unsigned k=0;k<3;++k)if(idx[k]>=0){
   mp_mono *t=&mp_lane[k][idx[k]];t->c=2;t->v=2;t->at[t->n++]=(uint8_t)(si<<3|k);
   for(unsigned q=1;q<t->n;++q){uint8_t v=t->at[q];unsigned r=q;while(r&&t->at[r-1]>v){t->at[r]=t->at[r-1];--r;}t->at[r]=v;}
  }
  return;
 }
}
/* The ordinary list: the one term that multiplies two lookups (or a dot product, or a term on the
 * alpha lookup) first, then adds, then subtractions. 0 when built. */
static int mp_build_ordinary(int alpha_ok,int asite,uint16_t va,int nneg,unsigned max_pass){
 int hard=-1,first=-1;uint8_t sites[4];int restore=0;
 mp.npass=0;
 for(unsigned i=0;i<mp_ng;++i){unsigned ns=mp_sites(&mp_g[i],sites);if(ns>1||mp_gdot(&mp_g[i])>=0){if(hard>=0||mp_g[i].neg)return -1;hard=(int)i;}}
 if(asite>=0&&mp_dot_is[asite]){alpha_ok=0;va=2;asite=-1;}   /* a dot pass's alpha is the vertex alpha */
 if(nneg&&!GL2_MP_INVERT)return -1;
 if(hard>=0)first=hard;
 else{for(unsigned i=0;i<mp_ng&&first<0;++i)if(!mp_g[i].neg&&asite>=0&&mp_sites(&mp_g[i],sites)==1&&sites[0]==asite)first=(int)i;
  for(unsigned i=0;i<mp_ng&&first<0;++i)if(!mp_g[i].neg)first=(int)i;}
 if(first<0){if(mp_ng)return -1;if(mp_emit(MP_FIRST,0,-1,0))return -1;for(unsigned k=0;k<3;++k)mp.pass[0].v[k]=1;}   /* black */
 else{
  unsigned ns=mp_sites(&mp_g[first],sites);
  if(ns>1&&asite>=0)for(unsigned q=1;q<ns;++q)if(sites[q]==asite){sites[q]=sites[0];sites[0]=(uint8_t)asite;}
  /* the pass that samples the lookup table carries the term's x2 (the table holds V/2): it must come before the multiplies */
  if(ns>1&&mp.lut.on)for(unsigned q=1;q<ns;++q)if(sites[q]==mp.lut_site){sites[q]=sites[0];sites[0]=mp.lut_site;}
  /* a dot pass carries the light vector in its vertex colour: dot passes first (a pow chain at the very
   * front: it squares the framebuffer), the per-vertex factor (and with it the lookup table's x2)
   * rides on the first other pass, or on a pass of its own */
  for(unsigned q=0,w=0;q<ns;++q)if(mp_dot_is[sites[q]]){uint8_t t=sites[q];for(unsigned r=q;r>w;--r)sites[r]=sites[r-1];sites[w++]=t;}
  for(unsigned q=1;q<ns;++q)if(mp_dot_is[sites[q]]&&mp_dot_k[sites[q]]>1){uint8_t t=sites[q];sites[q]=sites[0];sites[0]=t;}
  if(!ns){if(mp_emit(MP_FIRST,&mp_g[first],-1,1))return -1;}
  else{
   int vplaced=0;
   for(unsigned q=0;q<ns;++q){
    const unsigned st=sites[q];
    if(mp_dot_is[st]&&mp_dot_k[st]>1){
     /* writes d^2 in all four channels; then square (power of two) or multiply by d^2 again */
     unsigned k=mp_dot_k[st];
     if(q||(va!=2&&alpha_ok))return -1;   /* the chain owns the framebuffer and its alpha */
     if(mp_emit(MP_FIRST,&mp_g[first],(int)st,0))return -1;
     if(!(k&(k-1))){for(unsigned e=4;e<=k;e*=2)if(mp_emit(MP_SQ,0,-1,0))return -1;}
     else for(unsigned e=4;e<=k;e+=2)if(mp_emit(MP_MUL,&mp_g[first],(int)st,0))return -1;
     restore=GL2_MP_ACC_RESTORE;continue;
    }
    int wv=!vplaced&&!mp_dot_is[st];if(mp_emit(q?MP_MUL:MP_FIRST,&mp_g[first],(int)st,wv))return -1;if(wv)vplaced=1;
   }
   const mp_group *g=&mp_g[first];
   if(!vplaced&&!(g->c[0]==1&&g->c[1]==1&&g->c[2]==1&&g->v[0]==2&&g->v[1]==2&&g->v[2]==2))if(mp_emit(MP_MUL,g,-1,1))return -1;
  }
 }
 for(unsigned i=0;i<mp_ng;++i)if((int)i!=first&&!mp_g[i].neg){unsigned ns=mp_sites(&mp_g[i],sites);if(mp_emit(MP_ADD,&mp_g[i],ns?sites[0]:-1,1))return -1;}
 if(nneg){
  if(mp_emit(MP_INV,0,-1,0))return -1;
  for(unsigned i=0;i<mp_ng;++i)if(mp_g[i].neg){unsigned ns=mp_sites(&mp_g[i],sites);if(mp_emit(MP_ADD,&mp_g[i],ns?sites[0]:-1,1))return -1;}
  if(mp_emit(MP_INV,0,-1,0))return -1;
 }
 if(restore){if(mp_emit(MP_SETA,0,-1,0))return -1;for(unsigned k=0;k<3;++k)mp.pass[mp.npass-1].v[k]=1;mp.pass[mp.npass-1].va=2;}
 if(mp.npass>max_pass)return -1;
 /* alpha: the first pass writes it; adds contribute 0 (or the same vertex alpha when the draw blends) */
 mp_pass *p0=&mp.pass[0];
 if(alpha_ok&&asite>=0&&p0->site!=asite){alpha_ok=0;va=2;asite=-1;}
 p0->va=va;p0->amode=(uint8_t)(asite>=0);
 mp.blend_ok=(uint8_t)(alpha_ok&&asite<0);
 for(unsigned i=1;i<mp.npass;++i){mp_pass *p=&mp.pass[i];if(p->op!=MP_ADD)mp.blend_ok=0;else if(alpha_ok&&asite<0){p->va=va;p->usek=1;}}
 for(unsigned i=1;i<mp.npass;++i)if(mp.pass[i].op!=MP_ADD)for(unsigned j=1;j<mp.npass;++j)mp.pass[j].usek=0,mp.pass[j].va=(uint16_t)(mp.pass[j].op==MP_ADD?1:2);
 return f.error?-1:0;
}
static int mp_emit_seta(void){if(mp_emit(MP_SETA,0,-1,0))return -1;for(unsigned k=0;k<3;++k)mp.pass[mp.npass-1].v[k]=1;mp.pass[mp.npass-1].va=2;return 0;}
/* The additive list (the draw blends ONE/ONE, as per-light accumulation does), built after the
 * ordinary one: every term goes through destination alpha. A term without dot products or scalar
 * lookups is a plain add (alpha 0); every other term: alpha := 1, alpha *= each dot product (a
 * pow(d, k) chain: k/2 times d^2) and scalar lookup, colour += its one per-channel lookup * V *
 * alpha. Ends with alpha := 1 (GL2_MP_ACC_RESTORE), as GL leaves it with an opaque shader. */
static void mp_build_additive(int nneg){
 const unsigned base=mp.npass;int ok=1,anydot=0;uint8_t sites[4];
 mp.nacc=0;
 if(!mp_ng||nneg||mp.lut.on)return;
 for(unsigned i=0;i<mp_ng;++i)if(mp_gdot(&mp_g[i])>=0)anydot=1;
 if(!anydot)return;   /* plain sums: the ordinary list blends them */
 /* the shader's alpha must be 1: GL then leaves alpha 1 after ONE/ONE, as the list does */
 if(mp_lane_n[3]!=1||mp_lane[3][0].n||f.nodes[mp_lane[3][0].v].op!=F_CONST||mp_lane[3][0].c*f.nodes[mp_lane[3][0].v].value<1)return;
 for(unsigned pass=0;pass<2&&ok;++pass)for(unsigned i=0;i<mp_ng&&ok;++i){
  const mp_group *g=&mp_g[i];unsigned ns=mp_sites(g,sites);int rgb=-1,factors=0;
  for(unsigned q=0;q<ns;++q){
   if(mp_dot_is[sites[q]]){factors=1;continue;}
   int scalar=1;for(unsigned a=0;a<g->n;++a)if((g->at[a]>>3)==sites[q]&&(g->at[a]&3u)!=3u)scalar=0;
   if(!scalar){if(rgb>=0)ok=0;rgb=sites[q];}else factors=1;
  }
  if(!ok)break;
  if(!factors){if(!pass){if(mp_emit(MP_ADD,g,rgb,1))ok=0;else mp.pass[mp.npass-1].va=1;}continue;}   /* plain adds first */
  if(!pass)continue;
  if(mp_emit_seta()){ok=0;break;}
  for(unsigned q=0;q<ns&&ok;++q)if(mp_dot_is[sites[q]]){unsigned k=mp_dot_k[sites[q]],n=k>1?k/2:1;for(unsigned e=0;e<n&&ok;++e)if(mp_emit(MP_MULA,g,sites[q],0))ok=0;}
  for(unsigned q=0;q<ns&&ok;++q)if(!mp_dot_is[sites[q]]&&(int)sites[q]!=rgb&&mp_emit(MP_MULA,g,sites[q],0))ok=0;
  if(ok&&mp_emit(MP_ADDDA,g,rgb,1))ok=0;
  if(ok)mp.pass[mp.npass-1].va=1;
 }
 if(ok&&GL2_MP_ACC_RESTORE&&mp_emit_seta())ok=0;
 if(ok&&mp.npass-base<=MP_ACC_MAX)mp.nacc=mp.npass-base;
 mp.npass=base;
}
/* Parse source in multi-pass mode and build mp.pass[]. 0 on success; the node ids in mp stay valid
 * until the next fragment parse. */
static int mp_analyse(const char *source,unsigned max_pass){
 static char log[64];
 f.cursor=0;f.n=f.ns=f.nvary=f.nsampler=f.nuniform=f.depth=f.error=0;f_tex_sampler=-1;f_nfuncs=0;f_call_depth=0;f_scope=0;f_extra_n=0;f_fold_lane=4;
 for(unsigned k=0;k<4;++k)f_fold_j[k]=-1;
 f.log=log;f.out=&mp_scratch;for(unsigned i=0;i<F_TABLE;++i)f.table[i]=0;
 f_mp=1;f_mp_nvary=f_mp_nsite=0;mp.npass=mp.nacc=0;mp.nsampler=0;mp.blend_ok=0;mp_bad=0;mp_ng=0;
 for(unsigned s=0;s<F_MP_SITES;++s)mp_dot_is[s]=mp_dot_bare[s]=mp_dot_k[s]=0;
 f_const(0);f_const(1);
 f_value target=f_parse(source);f_mp=0;
 if(f.error)return -1;
 for(unsigned k=0;k<4;++k){if(f.nodes[target.v[k]].op==F_CLAMP)target.v[k]=f.nodes[target.v[k]].a;if(mp_lane_build(target.v[k],k))return -1;}
 mp_lut_pick();
 if(f.error)return -1;
 /* RGB lanes -> vector terms */
 for(unsigned lane=0;lane<3;++lane)for(unsigned i=0;i<mp_lane_n[lane];++i){
  const mp_mono *m=&mp_lane[lane][i];uint8_t key[4];
  for(unsigned q=0;q<m->n;++q){unsigned l=m->at[q]&3u,base=m->at[q]&~3u;if(l==3)key[q]=(uint8_t)(base|3u);else if(l==lane)key[q]=(uint8_t)base;else return -1;}
  for(unsigned q=1;q<m->n;++q){uint8_t v=key[q];unsigned r=q;while(r&&key[r-1]>v){key[r]=key[r-1];--r;}key[r]=v;}
  unsigned gi=0;
  for(;gi<mp_ng;++gi){mp_group *g=&mp_g[gi];if(g->n!=m->n||g->neg!=(m->c<0))continue;unsigned q=0;for(;q<m->n;++q)if(g->at[q]!=key[q])break;if(q==m->n)break;}
  if(gi==mp_ng){if(mp_ng>=MP_GROUPS)return -1;mp_group *g=&mp_g[mp_ng++];g->n=m->n;g->neg=m->c<0;for(unsigned q=0;q<m->n;++q)g->at[q]=key[q];for(unsigned k=0;k<3;++k){g->c[k]=0;g->v[k]=2;}}
  mp_group *g=&mp_g[gi];if(g->c[lane]!=0)return -1;
  g->c[lane]=m->c<0?-m->c:m->c;g->v[lane]=m->v;
 }
 /* alpha lane: one term, at most the alpha of one lookup */
 int alpha_ok=1,asite=-1;uint16_t va=1;
 if(mp_lane_n[3]==1){const mp_mono *m=&mp_lane[3][0];
  if(m->c<0||m->n>1||(m->n==1&&(m->at[0]&7u)!=3u))alpha_ok=0;
  else{va=mp_scaled(m->c,m->v);if(m->n)asite=m->at[0]>>3;}
 }else if(mp_lane_n[3]>1)alpha_ok=0;
 if(!alpha_ok){va=2;asite=-1;}
 int nneg=0;for(unsigned i=0;i<mp_ng;++i)if(mp_g[i].neg)++nneg;
 if(mp_build_ordinary(alpha_ok,asite,va,nneg,max_pass)){mp.npass=0;mp.blend_ok=0;}
 mp_build_additive(nneg);
 if(!mp.npass&&!mp.nacc)return -1;
 if(f.error)return -1;
 /* samplers in use (the GL state has two texture units) */
 for(unsigned i=0;i<mp.npass+mp.nacc;++i)if(mp.pass[i].site!=0xff){
  unsigned s=f_mp_site_sampler[mp.pass[i].site],j=0;
  if(s==0xfeu)continue;   /* the lookup table is the driver's own texture */
  for(;j<mp.nsampler;++j)if(mp.sampler[j]==s)break;
  if(j==mp.nsampler){if(j>=2)return -1;mp.sampler[mp.nsampler++]=(uint8_t)s;
   mp.sampler_name[j][0]=0;for(unsigned q=0;q<f.ns;++q)if(f.symbols[q].storage==2&&f.symbols[q].value.kind==2&&f.symbols[q].value.index==s)scpy(mp.sampler_name[j],f.symbols[q].name,32);}
 }
 return f.error?-1:0;
}
/* ---- text ---- */
static char *mp_o;static unsigned mp_k,mp_cap;static int mp_ovf;static uint32_t mp_uniused;
static void mp_s(const char *s){while(*s){if(mp_k+1>=mp_cap){mp_ovf=1;return;}mp_o[mp_k++]=*s++;}mp_o[mp_k]=0;}
static void mp_dec(uint32_t v,unsigned digits){char b[12];unsigned n=0;do{b[n++]=(char)('0'+v%10u);v/=10u;}while(v||n<digits);while(n){char c[2]={b[--n],0};mp_s(c);}}
static void mp_num(float v){
 if(v<0){mp_s("(-");mp_num(-v);mp_s(")");return;}
 if(!(v<1000000000.f)){mp_ovf=1;return;}
 uint32_t ip=(uint32_t)v;float fr=v-(float)ip;uint32_t fp=(uint32_t)(fr*1000000000.f+0.5f);
 if(fp>=1000000000u){fp-=1000000000u;++ip;}
 mp_dec(ip,1);mp_s(".");mp_dec(fp,9);
}
static const char *mp_sym(uint16_t a,unsigned *w,unsigned *comp){
 if(a>=F_MP_VARY&&a<F_MP_SITE){unsigned j=(a-F_MP_VARY)/4u;*w=f_mp_vw[j];*comp=(a-F_MP_VARY)&3u;return f_mp_vname[j];}
 if(a>=GL2_SOFT_UNIFORM_BASE&&a<GL2_SOFT_UNIFORM_BASE+64u){unsigned j=(a-GL2_SOFT_UNIFORM_BASE)/4u;*w=f_mp_uw[j];*comp=(a-GL2_SOFT_UNIFORM_BASE)&3u;mp_uniused|=1u<<j;return f_mp_uname[j];}
 return 0;
}
static void mp_vec(const uint16_t *ids,unsigned w,unsigned d){
 static const char *const vn[5]={"","","vec2(","vec3(","vec4("};
 static const char *const fn[]={"","","min","max","abs","exp","sqrt","inversesqrt","floor","fract","sin","cos","sign","step"};
 if(mp_ovf)return;
 if(d>48){mp_ovf=1;return;}
 f_node n0=f.nodes[ids[0]];int same=1,sameop=1;uint16_t a[4],b[4];
 for(unsigned k=0;k<w;++k){f_node n=f.nodes[ids[k]];if(ids[k]!=ids[0])same=0;if(n.op!=n0.op||(n.op==F_FN&&n.value!=n0.value))sameop=0;a[k]=n.a;b[k]=n.b;}
 if(w>1&&same){mp_s(vn[w]);mp_vec(ids,1,d+1);mp_s(")");return;}
 if(sameop&&n0.op==F_CONST){if(w>1)mp_s(vn[w]);for(unsigned k=0;k<w;++k){if(k)mp_s(",");mp_num(f.nodes[ids[k]].value);}if(w>1)mp_s(")");return;}
 if(sameop&&n0.op==F_INPUT){
  unsigned sw=0,comp=0;const char *name=mp_sym(n0.a,&sw,&comp);char sz[6];unsigned q=0;int one=name!=0;
  for(unsigned k=0;k<w&&one;++k){unsigned w2=0,c2=0;if(mp_sym(f.nodes[ids[k]].a,&w2,&c2)!=name)one=0;sz[q++]="xyzw"[c2];}
  if(!name){mp_ovf=1;return;}
  if(one){mp_s(name);if(sw>1){sz[q]=0;mp_s(".");mp_s(sz);}return;}
 }
 else if(sameop&&(n0.op==F_ADD||n0.op==F_SUB||n0.op==F_MUL)){mp_s("(");mp_vec(a,w,d+1);mp_s(n0.op==F_ADD?"+":n0.op==F_SUB?"-":"*");mp_vec(b,w,d+1);mp_s(")");return;}
 else if(sameop&&n0.op==F_CLAMP){mp_s("clamp(");mp_vec(a,w,d+1);mp_s(",0.0,1.0)");return;}
 else if(sameop&&n0.op==F_FN){
  unsigned code=(unsigned)n0.value;
  if(code==FN_DIV){mp_s("(");mp_vec(a,w,d+1);mp_s("/max(");mp_vec(b,w,d+1);mp_s(",0.0000305))");return;}   /* a raw x/0 hung the device (run518) */
  if(code<2||code>FN_STEP){mp_ovf=1;return;}
  mp_s(fn[code]);mp_s("(");mp_vec(a,w,d+1);if(code==FN_MIN||code==FN_MAX||code==FN_STEP){mp_s(",");mp_vec(b,w,d+1);}mp_s(")");return;
 }
 if(w==1){mp_ovf=1;return;}
 mp_s(vn[w]);for(unsigned k=0;k<w;++k){if(k)mp_s(",");mp_vec(ids+k,1,d+1);}mp_s(")");
}
/* Fragment source of pass i. col / uv: the varyings in VGP output slots 1 and 3 (uvw: uv's width). */
static int mp_fs_text(unsigned i,const char *col,const char *uv,unsigned uvw,char *out,unsigned cap){
 const mp_pass *p=&mp.pass[i];mp_o=out;mp_k=0;mp_cap=cap;mp_ovf=0;out[0]=0;
 mp_s("precision mediump float;varying vec4 ");mp_s(col);mp_s(";");
 if(p->site!=0xff){
  unsigned s=f_mp_site_sampler[p->site],j=0;for(;j<mp.nsampler;++j)if(mp.sampler[j]==s)break;
  const char *sn=s==0xfeu?"gl2_lut":mp.sampler_name[j<2?j:0];
  mp_s(uvw==4?"varying vec4 ":"varying vec2 ");mp_s(uv);mp_s(";uniform sampler2D ");mp_s(sn);
  mp_s(";void main(){vec4 t=texture2D(");mp_s(sn);mp_s(",");mp_s(uv);mp_s(".xy);");
  if(mp_dot_is[p->site]){   /* DOT3 against the light vector in the vertex colour (compiles to the combiner's DOT3) */
   mp_s("float d=dot(t.rgb-vec3(0.5),");mp_s(col);mp_s(".rgb-vec3(0.5))*4.0;");
   if(mp_dot_k[p->site]>1){   /* d^2 from the second combiner stage: C * C */
    mp_s("float e=clamp(d,0.0,1.0);float s=e*e;");
    if(p->op==MP_MULA)mp_s("gl_FragColor=vec4(1.0,1.0,1.0,s);}");else mp_s("gl_FragColor=vec4(vec3(s),s);}");
   }
   else if(p->op==MP_MULA)mp_s("gl_FragColor=vec4(1.0,1.0,1.0,d);}");
   else{mp_s("gl_FragColor=vec4(vec3(d),");mp_s(col);mp_s(".a);}");}
   return mp_ovf?-1:0;
  }
  if(p->op==MP_MULA){   /* a scalar lookup into destination alpha */
   static const char *const at[8]={"","","","t.a","","","","(1.0-t.a)"};
   mp_s("gl_FragColor=vec4(1.0,1.0,1.0,");
   for(unsigned q=0;q<p->nat;++q){if(q)mp_s("*");mp_s(at[p->at[q]&7u][0]?at[p->at[q]&7u]:"1.0");}
   mp_s(");}");return mp_ovf?-1:0;
  }
  mp_s("gl_FragColor=vec4(");
  for(unsigned q=0;q<p->nat;++q){static const char *const at[8]={"t.rgb","","","t.a","(1.0-t.rgb)","","","(1.0-t.a)"};mp_s(at[p->at[q]&7u]);mp_s("*");}
 }else mp_s("void main(){gl_FragColor=vec4(");
 mp_s(col);mp_s(".rgb");
 if(p->scale>1)mp_s(p->scale==2?"*2.0":"*4.0");
 mp_s(",");if(p->amode){mp_s("t.a*");}mp_s(col);mp_s(".a);}");
 return mp_ovf?-1:0;
}
/* Statements appended to the vertex shader's main() for pass i (decl: globals to add in front).
 * vs_has(name): the vertex shader already declares that uniform. */
static int mp_vs_text(unsigned i,const char *col,int decl_col,const char *uv,unsigned uvw,int decl_uv,int (*vs_has)(void *,const char *),void *user,char *decl,unsigned dcap,char *add,unsigned acap,uint32_t *uniused){
 const mp_pass *p=&mp.pass[i];mp_uniused=0;
 mp_o=add;mp_k=0;mp_cap=acap;mp_ovf=0;add[0]=0;
 const int dotp=p->site!=0xff&&mp_dot_is[p->site];
 if(dotp){mp_s("vec3 gl2x=");mp_num(mp_dot_a[p->site]*0.25f);mp_s("*");mp_vec(mp_dot_b[p->site],3,0);mp_s(";");}
 mp_s("vec4 gl2c=clamp(vec4(");
 if(dotp){   /* the light vector: 0.5 + a/4 * B, so 4 * dot(T - .5, P - .5) is the shader's sum */
  if(mp_dot_hw)mp_s("mix(min(gl2x+vec3(0.501961),vec3(0.5)),gl2x*0.997809+vec3(0.505009),step(vec3(0.0),gl2x))");
  else mp_s("vec3(0.5)+gl2x");
 }else mp_vec(p->v,3,0);
 mp_s(",");
 if(p->usek){mp_s("(");mp_vec(&p->va,1,0);mp_s("*gl2_mpk.x)");}else mp_vec(&p->va,1,0);
 mp_s("),0.0,1.0);");
 int direct=0;
 if(p->site!=0xff){
  const uint16_t *q=f_mp_site_uv[p->site];f_node u=f.nodes[q[0]],v=f.nodes[q[1]];
  if(!decl_uv&&u.op==F_INPUT&&v.op==F_INPUT&&u.a>=F_MP_VARY&&u.a<F_MP_SITE&&v.a==u.a+1u&&!((u.a-F_MP_VARY)&3u)&&!scmp(f_mp_vname[(u.a-F_MP_VARY)/4u],uv))direct=1;
  if(f_mp_site_sampler[p->site]==0xfeu){   /* sphere-map coordinate of the varying's direction */
   const char *nm=f_mp_vname[mp.lut_vary];direct=0;
   mp_s("vec3 gl2n=");mp_s(nm);mp_s(".xyz*inversesqrt(max(dot(");mp_s(nm);mp_s(".xyz,");mp_s(nm);mp_s(".xyz),0.000001))+vec3(0.0,0.0,1.0);");
   mp_s("vec2 gl2u=gl2n.xy*(0.5*inversesqrt(max(dot(gl2n,gl2n),0.000001)))+0.5;");
  }else if(!direct){mp_s("vec2 gl2u=");mp_vec(q,2,0);mp_s(";");}
 }
 /* the same conversion loses up to 1.75/255 above 0.5 in every channel: v = 0.997809 c + 0.004148 lands on round(255 c) */
 if(mp_dot_hw){
  if(dotp)mp_s("gl2c.a=mix(gl2c.a,gl2c.a*0.997809+0.004148,step(0.501,gl2c.a));");
  else mp_s("gl2c=mix(gl2c,gl2c*0.997809+vec4(0.004148),step(vec4(0.501),gl2c));");
 }
 mp_s(col);mp_s("=gl2c;");
 if(p->site!=0xff&&!direct){mp_s(uv);mp_s(uvw==2?"=gl2u;":".xy=gl2u;");}
 if(mp_ovf)return -1;
 mp_o=decl;mp_k=0;mp_cap=dcap;decl[0]=0;
 if(decl_col){mp_s("varying vec4 ");mp_s(col);mp_s(";");}
 if(decl_uv&&p->site!=0xff){mp_s("varying vec2 ");mp_s(uv);mp_s(";");}
 for(unsigned j=0;j<16;++j)if((mp_uniused>>j&1u)&&!vs_has(user,f_mp_uname[j])){
  static const char *const ty[5]={"","uniform float ","uniform vec2 ","uniform vec3 ","uniform vec4 "};
  mp_s(ty[f_mp_uw[j]]);mp_s(f_mp_uname[j]);mp_s(";");
 }
 if(p->usek)mp_s("uniform vec4 gl2_mpk;");
 if(uniused)*uniused=mp_uniused;
 return mp_ovf?-1:0;
}
/* Compile-time check and bookkeeping: can this source be drawn in passes, and does every pass
 * compile to an exact combiner plan? On success the entry is marked (e.mp = pass count) and lists
 * every sampler and uniform the passes need. failed: the single-pass compile produced no entry. */
static int compile_fragment(const char *source,gl2_fs_owned *out,char *log);
static int mp_adopt(const char *source,gl2_fs_owned *o,int failed){
 static char fst[MP_MAX_PASS][512];static gl2_fs_owned fo;static char log[64];
 if(mp_analyse(source,MP_MAX_PASS))return 0;
 /* the ordinary list, or (a shader only an additive draw can run) the additive one */
 const unsigned first=0,n=mp.npass?mp.npass:mp.nacc;   /* both lists start at pass 0 when the ordinary one is empty */unsigned ns=mp.nsampler,nu=f.nuniform;char sn[2][32],un[16][32];uint8_t uw[16];
 static char ft[MP_ACC_MAX][512];
 for(unsigned i=0;i<ns;++i)scpy(sn[i],mp.sampler_name[i],32);
 for(unsigned i=0;i<nu&&i<16;++i){scpy(un[i],f_mp_uname[i],32);uw[i]=f_mp_uw[i];}
 (void)fst;
 for(unsigned i=0;i<n;++i)if(mp_fs_text(first+i,"vColor","vTexCoord",2,ft[i],sizeof ft[i]))return 0;
 for(unsigned i=0;i<n;++i)if(compile_fragment(ft[i],&fo,log)||!fo.e.nplan||fo.e.approx)return 0;
 if(failed){for(unsigned i=0;i<sizeof *o;++i)((uint8_t *)o)[i]=0;o->e.hash=gl2_hash(source);o->e.source=source;}
 for(unsigned i=0;i<ns;++i){unsigned j=0;for(;j<o->e.nsampler;++j)if(!scmp(o->e.sampler_names[j],sn[i]))break;
  if(j==o->e.nsampler){if(j>=2)return 0;scpy(o->samplers[j],sn[i],32);o->e.sampler_names[j]=o->samplers[j];++o->e.nsampler;}}
 for(unsigned i=0;i<nu&&i<16;++i){unsigned j=0;for(;j<o->e.nuniform;++j)if(!scmp(o->e.uniform_names[j],un[i]))break;
  if(j==o->e.nuniform){if(j>=16)return 0;scpy(o->uniforms[j],un[i],32);o->e.uniform_names[j]=o->uniforms[j];o->e.uniform_widths[j]=uw[i];++o->e.nuniform;}}
 o->e.mp=(uint8_t)n;
 return 1;
}
