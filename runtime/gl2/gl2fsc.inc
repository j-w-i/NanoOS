/* On-device typed straight-line fragment frontend and exact combiner lowering.
 * Included privately by gl2api.c; serialized like the vertex compiler.
 * No sampling-based equivalence tests, source-string templates or CPU rendering.
 * Current lowering is one texture/up to two stages. Broader GLSL/execution is unfinished. */
#define F_NODES 768
#define F_TABLE 2048
#define F_SYMBOLS 48
enum { F_CONST=1,F_INPUT,F_ADD,F_MUL,F_SUB,F_CLAMP,F_FN };
typedef struct {uint16_t a,b,slot;uint32_t deps;uint8_t op;float value,lo,hi;} f_node;
typedef struct {uint16_t v[4];uint8_t w,kind,index;} f_value; /* kind0 expression,1 UV,2 sampler */
typedef struct {char name[32];f_value value;uint8_t storage,slot;} f_symbol;
static struct {const char *cursor;char token[32];char *log;int error,depth;uint16_t n,table[F_TABLE];f_node nodes[F_NODES];f_symbol symbols[F_SYMBOLS];unsigned ns,nvary,nsampler,nuniform;uint8_t uniform_map[16];gl2_fs_owned *out;} f;
/* Varyings other than the colour and UVs get their own inputs (GL2_SOFT_INPUTS+4j+k)
 * so a colour product by them (OpenLara: color.xyz *= vLight.xyz) is not mistaken
 * for the colour itself. The native plan cannot read them: f_strip_mix records the
 * product per colour lane as a fold, applied per vertex to the colour output at link. */
#define F_EXTRA_BASE GL2_SOFT_INPUTS
static char f_extra_name[4][32];static unsigned f_extra_n,f_fold_lane;static int f_fold_j[4],f_fold_k[4];static float f_fold_c[4];  /* per colour lane constant folded into vDiffuse (1 = none) */
static int f_tex_sampler=-1;  /* the one sampler texture2D uses */
/* Multi-pass mode (gl2fmp.inc): every varying and uniform is a plain named input, texture2D takes any
 * per-vertex UV expression (a "site"), builtins become exact F_FN nodes instead of approximations.
 * f_lossy: the single-pass parse approximated something (so its plan is not exact). */
#define F_MP_VARY 96u
#define F_MP_SITE 128u
#define F_MP_SITES 8u
enum{FN_DIV=1,FN_MIN,FN_MAX,FN_ABS,FN_EXP,FN_SQRT,FN_RSQ,FN_FLOOR,FN_FRACT,FN_SIN,FN_COS,FN_SIGN,FN_STEP,FN_POW};   /* FN_POW: lookup tables only (no vertex form) */
static int f_mp,f_lossy;static unsigned f_mp_nvary,f_mp_nsite;static char f_mp_vname[8][32],f_mp_uname[16][32];static uint8_t f_mp_vw[8],f_mp_uw[16],f_mp_site_sampler[F_MP_SITES];static uint16_t f_mp_site_uv[F_MP_SITES][2];
static int f_fog_j=-1,f_fog_k;static uint8_t f_fog_mask;  /* fog factor folded per vertex (see gl2_fs_entry.fog_name) */
/* t of a stripped mix(fog, colour, t) when it is one component of an extra varying: record it
 * as the fog fold (one varying component for all lanes) and report success. */
static int f_take_fog(uint16_t t){
 if(!GL2_FOLD_FOG||!t||f_fold_lane>=3u)return 0;
 f_node *x=&f.nodes[t];if(x->op!=F_INPUT)return 0;
 int j,k;
 if(x->a==3){j=-2;k=3;}   /* the colour varying's own alpha (OpenLara rooms: vDiffuse.w) */
 else if(x->a>=F_EXTRA_BASE&&x->a<F_EXTRA_BASE+16u){j=(int)((x->a-F_EXTRA_BASE)/4u);k=(int)((x->a-F_EXTRA_BASE)%4u);}
 else return 0;
 if(f_fog_j!=-1&&(f_fog_j!=j||f_fog_k!=k))return 0;
 f_fog_j=j;f_fog_k=k;f_fog_mask|=(uint8_t)(1u<<f_fold_lane);return 1;
}
static void f_fail(const char *s){if(!f.error){f.error=1;scpy(f.log,s,64);}}
static int f_id(char c){return(c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_';}
static int f_digit(char c){return c>='0'&&c<='9';}
static int f_eq(const char *s){return !scmp(f.token,s);}
static void f_next(void){
 const char *s=f.cursor;unsigned n=0;
 for(;;){while(*s==' '||*s=='\t'||*s=='\r'||*s=='\n')++s;
  if(s[0]=='/'&&s[1]=='/'){while(*s&&*s!='\n')++s;continue;}
  if(s[0]=='/'&&s[1]=='*'){s+=2;while(*s&&!(s[0]=='*'&&s[1]=='/'))++s;if(!*s){f_fail("unterminated fragment comment");break;}s+=2;continue;}break;
 }
#define F_CHAR() do{if(n>=31){f_fail("fragment token exceeds31bytes");goto done;}f.token[n++]=*s++;}while(0)
 if(f_id(*s)){while(f_id(*s)||f_digit(*s))F_CHAR();}
 else if(f_digit(*s)||(*s=='.'&&f_digit(s[1]))){while(f_digit(*s))F_CHAR();if(*s=='.'){F_CHAR();while(f_digit(*s))F_CHAR();}if(*s=='e'||*s=='E'){F_CHAR();if(*s=='+'||*s=='-')F_CHAR();if(!f_digit(*s))f_fail("missing fragment exponent");while(f_digit(*s))F_CHAR();}}
 else if(*s){const char *p="+-*/=;(),.{}<>!&|[]";while(*p&&*p!=*s)++p;if(!*p)f_fail("unsupported fragment character");F_CHAR();}
 done:f.token[n]=0;f.cursor=s;
#undef F_CHAR
}
static void f_eat(const char *s){if(!f_eq(s)){f_fail("unexpected fragment token");return;}f_next();}
static uint32_t f_bits(float x){union{float x;uint32_t u;}v={x};return v.u;}
static uint16_t f_intern(unsigned op,unsigned a,unsigned b,float value){
 uint32_t hash=(op*0x9e3779b9u)^(a*0x85ebca6bu)^(b*0xc2b2ae35u)^f_bits(value);unsigned slot=hash&(F_TABLE-1);
 while(f.table[slot]){unsigned i=f.table[slot];f_node *n=&f.nodes[i];if(n->op==op&&n->a==a&&n->b==b&&f_bits(n->value)==f_bits(value))return(uint16_t)i;slot=(slot+1)&(F_TABLE-1);}
 if(f.n+1>=F_NODES){f_fail("fragment expression exceeds node limit");return 1;}
 unsigned i=++f.n;f_node *n=&f.nodes[i];n->op=(uint8_t)op;n->a=(uint16_t)a;n->b=(uint16_t)b;n->value=value;n->slot=(uint16_t)slot;n->deps=op==F_INPUT?(a<8?1u<<a:a<F_MP_VARY?1u<<(8+(a-8)/4):a>=F_MP_SITE?1u<<((a-F_MP_SITE)/4):0):op==F_CONST?0:(f.nodes[a].deps|(op==F_CLAMP?0:f.nodes[b].deps));
 if(op==F_CONST)n->lo=n->hi=value;
 else if(op==F_INPUT){n->lo=0;n->hi=1;}
 else if(op==F_CLAMP){n->lo=0;n->hi=1;}
 else if(op==F_FN){n->lo=-1e30f;n->hi=1e30f;}
 else{f_node *x=&f.nodes[a],*y=&f.nodes[b];if(op==F_ADD){n->lo=x->lo+y->lo;n->hi=x->hi+y->hi;}else if(op==F_SUB){n->lo=x->lo-y->hi;n->hi=x->hi-y->lo;}else{float v[4]={x->lo*y->lo,x->lo*y->hi,x->hi*y->lo,x->hi*y->hi};n->lo=n->hi=v[0];for(unsigned k=1;k<4;++k){if(v[k]<n->lo)n->lo=v[k];if(v[k]>n->hi)n->hi=v[k];}}}
 f.table[slot]=(uint16_t)i;return(uint16_t)i;
}
static uint16_t f_const(float v){if((f_bits(v)&0x7f800000u)==0x7f800000u){f_fail("fragment constant out of range");return 1;}return f_intern(F_CONST,0,0,v==0?0:v);}
static int f_is(uint16_t id,float v){return f.nodes[id].op==F_CONST&&f.nodes[id].value==v;}
static unsigned f_terms(unsigned op,uint16_t id,uint16_t *terms,unsigned count){
 if(f.nodes[id].op==op){count=f_terms(op,f.nodes[id].a,terms,count);return f_terms(op,f.nodes[id].b,terms,count);}
 if(count>=32){f_fail("fragment arithmetic exceeds term limit");return count;}terms[count++]=id;return count;
}
static uint16_t f_op(unsigned op,uint16_t a,uint16_t b){
 if(f.nodes[a].op==F_CONST&&f.nodes[b].op==F_CONST){float x=f.nodes[a].value,y=f.nodes[b].value;return f_const(op==F_ADD?x+y:op==F_MUL?x*y:x-y);}
 if(op==F_SUB){if(a==b)return 1;if(f_is(b,0))return a;return f_intern(op,a,b,0);}
 if(op==F_ADD){if(f_is(a,0))return b;if(f_is(b,0))return a;}
 if(op==F_MUL){if(f_is(a,0)||f_is(b,0))return 1;if(f_is(a,1))return b;if(f_is(b,1))return a;}
 uint16_t terms[32];unsigned n=f_terms(op,a,terms,0);n=f_terms(op,b,terms,n);
 for(unsigned i=1;i<n;++i){uint16_t v=terms[i];unsigned j=i;while(j&&terms[j-1]>v){terms[j]=terms[j-1];--j;}terms[j]=v;}
 uint16_t result=terms[0];for(unsigned i=1;i<n;++i)result=f_intern(op,result,terms[i],0);return result;
}
static f_value f_zero(void){f_value v={{1,1,1,1},1,0,0};return v;}
static f_value f_binary(unsigned op,f_value a,f_value b){
 if(a.kind||b.kind){f_fail("fragment arithmetic on sampler/UV unsupported");return f_zero();}
 if(a.w!=b.w&&a.w!=1&&b.w!=1){f_fail("fragment vector width mismatch");return f_zero();}
 f_value v=f_zero();v.w=a.w>b.w?a.w:b.w;for(unsigned i=0;i<v.w;++i)v.v[i]=f_op(op,a.v[a.w==1?0:i],b.v[b.w==1?0:i]);return v;
}
static int f_width(const char *s){return !scmp(s,"float")?1:!scmp(s,"vec2")?2:!scmp(s,"vec3")?3:!scmp(s,"vec4")?4:0;}
static unsigned f_scope;   /* first symbol of the innermost function call (shadowing allowed above it) */
static int f_find(const char *s){for(unsigned i=f.ns;i-->0;)if(!scmp(s,f.symbols[i].name))return(int)i;return -1;}
/* User functions: recorded at global scope, inlined at each call (no recursion). */
#define F_FUNCS 12
static struct{char name[32];const char *params,*body;} f_funcs[F_FUNCS];static unsigned f_nfuncs,f_call_depth;
static int f_func_find(const char *s){for(unsigned i=0;i<f_nfuncs;++i)if(!scmp(s,f_funcs[i].name))return(int)i;return -1;}
static f_value f_call_user(int fi,const f_value *args,unsigned count);
static int f_valid_name(const char *name){
 static const char *const keywords[]={"attribute","const","uniform","varying","precision","lowp","mediump","highp","float","vec2","vec3","vec4","void","main","int","bool","sampler2D","if","else","for","while","return","discard","struct","in","out","inout"};
 if(!f_id(name[0])||(name[0]=='g'&&name[1]=='l'&&name[2]=='_'))return 0;
 for(unsigned i=0;i<sizeof keywords/sizeof *keywords;++i)if(!scmp(name,keywords[i]))return 0;
 return 1;
}
static void f_name(char *s){if(!f_valid_name(f.token)){f_fail("invalid fragment declaration name");s[0]=0;return;}scpy(s,f.token,32);f_next();}
static float f_number(void){
 const char *s=f.token;float v=0,unit=1;int dot=0;
 for(;*s&&*s!='e'&&*s!='E';++s){if(*s=='.')dot=1;else if(dot){unit*=.1f;v+=(*s-'0')*unit;}else v=v*10+(*s-'0');}
 if(*s){++s;int sign=1,e=0;if(*s=='+'||*s=='-'){if(*s=='-')sign=-1;++s;}while(f_digit(*s)){e=e*10+(*s++-'0');if(e>255){f_fail("fragment exponent out of range");return 0;}}while(e--)v=sign>0?v*10:v/10;}
 return v;
}
static f_value f_expr(void);
static f_value f_mp_fn2(unsigned code,f_value a,f_value b){
 f_value v=f_zero();if(a.kind||b.kind||(a.w!=b.w&&a.w!=1&&b.w!=1)){f_fail("fragment builtin argument mismatch");return v;}
 v.w=a.w>b.w?a.w:b.w;for(unsigned k=0;k<v.w;++k)v.v[k]=f_intern(F_FN,a.v[a.w==1?0:k],b.v[b.w==1?0:k],(float)code);return v;
}
static f_value f_mp_fn1(unsigned code,f_value a){f_value v=a;if(a.kind){f_fail("fragment builtin argument mismatch");return f_zero();}for(unsigned k=0;k<v.w;++k)v.v[k]=f_intern(F_FN,a.v[k],0,(float)code);return v;}
static f_value f_mp_dot(f_value a,f_value b){f_value d=f_zero();if(a.kind||b.kind||a.w!=b.w){f_fail("fragment dot argument mismatch");return d;}for(unsigned k=0;k<a.w;++k)d.v[0]=f_op(F_ADD,d.v[0],f_op(F_MUL,a.v[k],b.v[k]));return d;}
static f_value f_mp_k(float x){f_value v=f_zero();v.v[0]=f_const(x);return v;}
/* Exact builtins for multi-pass mode. Returns 1 when the name was handled (*out set, or an error raised). */
static int f_mp_call(const char *name,const f_value *a,unsigned n,f_value *out){
 static const char *const u1[]={"abs","exp","sqrt","inversesqrt","floor","fract","sin","cos","sign"};static const uint8_t c1[]={FN_ABS,FN_EXP,FN_SQRT,FN_RSQ,FN_FLOOR,FN_FRACT,FN_SIN,FN_COS,FN_SIGN};
 for(unsigned i=0;i<9;++i)if(!scmp(name,u1[i])){if(n!=1){f_fail("fragment builtin argument mismatch");return 1;}*out=f_mp_fn1(c1[i],a[0]);return 1;}
 if(!scmp(name,"min")||!scmp(name,"max")||!scmp(name,"step")){if(n!=2){f_fail("fragment builtin argument mismatch");return 1;}*out=f_mp_fn2(!scmp(name,"min")?FN_MIN:!scmp(name,"max")?FN_MAX:FN_STEP,a[0],a[1]);return 1;}
 if(!scmp(name,"length")){if(n!=1){f_fail("fragment builtin argument mismatch");return 1;}*out=f_mp_fn1(FN_SQRT,f_mp_dot(a[0],a[0]));return 1;}
 if(!scmp(name,"distance")){if(n!=2){f_fail("fragment builtin argument mismatch");return 1;}f_value d=f_binary(F_SUB,a[0],a[1]);*out=f_mp_fn1(FN_SQRT,f_mp_dot(d,d));return 1;}
 if(!scmp(name,"normalize")){if(n!=1){f_fail("fragment builtin argument mismatch");return 1;}*out=f_binary(F_MUL,a[0],f_mp_fn1(FN_RSQ,f_mp_dot(a[0],a[0])));return 1;}
 if(!scmp(name,"clamp")){if(n!=3){f_fail("fragment clamp argument mismatch");return 1;}
  int unit=1;for(unsigned k=0;k<a[1].w;++k)if(!f_is(a[1].v[k],0))unit=0;for(unsigned k=0;k<a[2].w;++k)if(!f_is(a[2].v[k],1))unit=0;
  if(unit)return 0;
  *out=f_mp_fn2(FN_MIN,f_mp_fn2(FN_MAX,a[0],a[1]),a[2]);return 1;}
 if(!scmp(name,"smoothstep")){if(n!=3){f_fail("fragment smoothstep argument mismatch");return 1;}
  f_value t=f_mp_fn2(FN_DIV,f_binary(F_SUB,a[2],a[0]),f_binary(F_SUB,a[1],a[0]));if(f.error)return 1;for(unsigned k=0;k<t.w;++k)t.v[k]=f_intern(F_CLAMP,t.v[k],0,0);
  *out=f_binary(F_MUL,f_binary(F_MUL,t,t),f_binary(F_SUB,f_mp_k(3),f_binary(F_MUL,f_mp_k(2),t)));return 1;}
 if(!scmp(name,"pow")){if(n!=2){f_fail("fragment builtin argument mismatch");return 1;}*out=f_mp_fn2(FN_POW,a[0],a[1]);return 1;}
 if(!scmp(name,"mod")){f_fail("fragment builtin has no exact GPU form");return 1;}
 return 0;
}
static f_value f_primary(void){
 f_value v=f_zero();
 if(f_eq("(")){f_next();v=f_expr();f_eat(")");}
 else if(f_digit(f.token[0])||(f.token[0]=='.'&&f_digit(f.token[1]))){v.v[0]=f_const(f_number());f_next();}
 else{char name[32];scpy(name,f.token,32);f_next();
  /* texture2D(s, uv): one sampler per program (unused sampler declarations are fine).
   * A UV that is not the plain UV0 varying (blur/downsample taps: vTexCoord + offset) is
   * approximated by the UV0 sample: offset taps of a normalised kernel collapse to the
   * centre sample (unblurred) instead of the whole program failing to link. */
  if(!scmp(name,"texture2D")&&f_eq("(")){f_next();f_value s0=f_expr();f_eat(",");
   if(!f.error&&s0.kind!=2){f_fail("texture2D needs a sampler");return v;}
   if(f_mp){f_value uv=f_expr();f_eat(")");if(f.error)return v;if(uv.kind||uv.w<2){f_fail("texture2D needs a vec2 coordinate");return v;}
    unsigned si=0;for(;si<f_mp_nsite;++si)if(f_mp_site_sampler[si]==s0.index&&f_mp_site_uv[si][0]==uv.v[0]&&f_mp_site_uv[si][1]==uv.v[1])break;
    if(si==f_mp_nsite){if(si>=F_MP_SITES){f_fail("too many texture lookups");return v;}f_mp_site_sampler[si]=s0.index;f_mp_site_uv[si][0]=uv.v[0];f_mp_site_uv[si][1]=uv.v[1];++f_mp_nsite;}
    v.w=4;for(unsigned k=0;k<4;++k)v.v[k]=f_intern(F_INPUT,F_MP_SITE+4*si+k,0,0);goto postfix;}
   if(f_tex_sampler<0)f_tex_sampler=s0.index;else if(f_tex_sampler!=s0.index){f_fail("fragment compiler currently supports one sampler");return v;}
   /* A single identifier (optionally swizzled) parses as before: a UV varying or a local
    * holding one. Anything longer is an offset tap. */
   int direct=0;if(f_find(f.token)>=0){const char *c=f.cursor;while(*c==' '||*c=='\t'||*c=='\n'||*c=='\r')++c;
    if(*c=='.'){++c;while((*c>='a'&&*c<='z')||(*c>='A'&&*c<='Z'))++c;while(*c==' '||*c=='\t'||*c=='\n'||*c=='\r')++c;}direct=*c==')';}
   if(direct){f_value uv=f_expr();if(f.error)return v;if(uv.kind!=1||uv.w!=2){f_fail("texture2D needs direct sampler and UV varying");return v;}if(uv.index!=3){f_fail("fragment lowering currently needs texture0/UV0");return v;}}
   else{int depth=1,uv0=0;while(!f.error&&f.token[0]){if(f_eq("("))++depth;else if(f_eq(")")&&--depth==0)break;{int si=f_find(f.token);if(si>=0&&f.symbols[si].storage==1&&f.symbols[si].slot==3)uv0=1;}f_next();}
    if(!uv0){f_fail("texture2D needs a UV0 varying");return v;}f.out->e.uv_approx=1;f_lossy=1;}
   f_eat(")");v.w=4;for(unsigned k=0;k<4;++k)v.v[k]=f_intern(F_INPUT,4+k,0,0);goto postfix;}
  /* textureCube: the MBX path has no cube maps; the environment term is approximated by
   * white, so the result is the colour it is multiplied by (OpenLara crystals, Midas gold). */
  if(!scmp(name,"textureCube")&&f_eq("(")){if(f_mp){f_fail("no cube maps");return v;}f_lossy=1;int depth=1;f_next();while(!f.error&&f.token[0]){if(f_eq("("))++depth;else if(f_eq(")")&&--depth==0)break;f_next();}
   f_eat(")");f.out->e.uv_approx=1;v.w=4;for(unsigned k=0;k<4;++k)v.v[k]=f_const(1);goto postfix;}
  if(f_eq("(")){f_next();f_value args[8];unsigned count=0;if(!f_eq(")"))for(;;){if(count>=8){f_fail("too many fragment arguments");break;}args[count++]=f_expr();if(!f_eq(","))break;f_next();}f_eat(")");int width=f_width(name);
   if(width){v.w=(uint8_t)width;unsigned n=0;for(unsigned a=0;a<count;++a){if(args[a].kind){f_fail("sampler/UV fragment constructor unsupported");break;}for(unsigned k=0;k<args[a].w;++k)if(n<4)v.v[n++]=args[a].v[k];else++n;}
    if(count==1&&args[0].w==1){for(int k=0;k<width;++k)v.v[k]=args[0].v[0];}
    else if(n<(unsigned)width||(n>(unsigned)width&&count!=1))f_fail("fragment constructor width mismatch");
   }else if(f_mp&&f_mp_call(name,args,count,&v)){
   }else if(!scmp(name,"texture2D")){if(count!=2||args[0].kind!=2||args[1].kind!=1||args[1].w!=2){f_fail("texture2D needs direct sampler and UV varying");return v;}if(args[0].index||args[1].index!=3){f_fail("fragment lowering currently needs texture0/UV0");return v;}v.w=4;for(unsigned k=0;k<4;++k)v.v[k]=f_intern(F_INPUT,4+k,0,0);}
   else if(!scmp(name,"reflect")){if(count!=2||args[0].kind||args[1].kind||args[0].w!=args[1].w){f_fail("fragment reflect argument mismatch");return v;}f_value d=f_zero();for(unsigned k=0;k<args[0].w;++k)d.v[0]=f_op(F_ADD,d.v[0],f_op(F_MUL,args[0].v[k],args[1].v[k]));v=args[0];for(unsigned k=0;k<v.w;++k)v.v[k]=f_op(F_SUB,args[0].v[k],f_op(F_MUL,f_op(F_MUL,f_const(2),d.v[0]),args[1].v[k]));}
   else if(!scmp(name,"dot")){if(count!=2||args[0].kind||args[1].kind||args[0].w!=args[1].w){f_fail("fragment dot argument mismatch");return v;}v=f_zero();for(unsigned k=0;k<args[0].w;++k)v.v[0]=f_op(F_ADD,v.v[0],f_op(F_MUL,args[0].v[k],args[1].v[k]));}
    else if(!scmp(name,"mix")){if(count!=3||args[0].w!=args[1].w||args[0].kind||args[1].kind||args[2].kind||(args[2].w!=1&&args[2].w!=args[0].w)){f_fail("fragment mix argument mismatch");return v;}f_value one=f_zero();one.v[0]=f_const(1);v=f_binary(F_ADD,f_binary(F_MUL,args[0],f_binary(F_SUB,one,args[2])),f_binary(F_MUL,args[1],args[2]));}
   else if(!scmp(name,"clamp")){if(count!=3||args[0].kind||args[1].kind||args[2].kind||(args[1].w!=1&&args[1].w!=args[0].w)||(args[2].w!=1&&args[2].w!=args[0].w)){f_fail("fragment clamp argument mismatch");return v;}v=args[0];for(unsigned k=0;k<v.w;++k){if(!f_is(args[1].v[args[1].w==1?0:k],0)||!f_is(args[2].v[args[2].w==1?0:k],1)){f_fail("fragment clamp needs0and1 limits");break;}v.v[k]=f_intern(F_CLAMP,v.v[k],0,0);}}
   else if(!scmp(name,"normalize")){f_lossy=1;if(count!=1||args[0].kind){f_fail("fragment normalize argument mismatch");return v;}v=args[0];}
   else if(!scmp(name,"length")||!scmp(name,"abs")||!scmp(name,"exp")){f_lossy=1;if(count!=1||args[0].kind){f_fail("fragment builtin argument mismatch");return v;}v=args[0];if(!scmp(name,"length"))v.w=1;}
   else if(!scmp(name,"min")||!scmp(name,"max")||!scmp(name,"step")||!scmp(name,"pow")||!scmp(name,"reflect")){f_lossy=1;if(count!=2||args[0].kind||args[1].kind){f_fail("fragment builtin argument mismatch");return v;}v=args[0];}
   else if(!scmp(name,"smoothstep")){f_lossy=1;if(count!=3||args[0].kind||args[1].kind||args[2].kind){f_fail("fragment smoothstep argument mismatch");return v;}v=args[2];}
   else if(f_func_find(name)>=0)v=f_call_user(f_func_find(name),args,count);
   else f_fail("fragment builtin needs broader execution path");
  }else{
   int lane=-1;
   if(f_eq("[")){f_next();if(f_digit(f.token[0])&&!f.token[1]){lane=f.token[0]-'0';}int bd=1;while(!f.error&&bd>0&&f.token[0]){if(f_eq("["))++bd;else if(f_eq("]"))--bd;f_next();}}
   int i=f_find(name);if(i<0)f_fail("unknown fragment identifier");else v=f.symbols[i].value;
   /* v[k] on a local vector selects a lane (uniform arrays keep their first element, as before) */
   if(i>=0&&lane>=0&&!f.symbols[i].storage&&!v.kind&&v.w>1){if(lane>=v.w)f_fail("fragment vector index out of range");else{f_value o=f_zero();o.v[0]=v.v[lane];v=o;}}
  }
 }
 postfix:
 while(f_eq(".")&&!f.error){f_next();
  if(v.kind==1&&(!scmp(f.token,"xy")||!scmp(f.token,"st")||!scmp(f.token,"zw")||!scmp(f.token,"pq"))){f_value out=v;out.w=2;v=out;f_next();continue;}
  if(v.kind){f_fail("UV/sampler swizzle unsupported");break;}f_value out=f_zero();out.w=0;int set=-1;
  for(const char *s=f.token;*s;++s){const char *sets[3]={"xyzw","rgba","stpq"};int lane=-1,which=-1;for(int q=0;q<3;++q)for(int k=0;k<4;++k)if(sets[q][k]==*s){lane=k;which=q;}
   if(lane<0||lane>=v.w||out.w>=4||(set>=0&&set!=which)){f_fail("invalid fragment swizzle");break;}set=which;out.v[out.w++]=v.v[lane];}
  if(!out.w){f_fail("empty fragment swizzle");}v=out;f_next();
 }
 return v;
}
static f_value f_unary(void){if(++f.depth>32){f_fail("fragment nesting exceeds32");--f.depth;return f_zero();}f_value v;if(f_eq("-")||f_eq("+")){int neg=f_eq("-");f_next();v=f_unary();if(neg){f_value zero=f_zero();v=f_binary(F_SUB,zero,v);}}else v=f_primary();--f.depth;return v;}
static f_value f_product(void){f_value v=f_unary();while(!f.error&&(f_eq("*")||f_eq("/"))){int div=f_eq("/");f_next();f_value b=f_unary();if(div){if(v.kind==1&&b.kind==1){if(v.index==b.index&&v.index>=3&&v.index<=4)f.out->e.uv_divide|=(uint8_t)(1u<<(v.index-3));continue;}if(b.kind){f_fail("fragment division by sampler/UV");break;}
  if(f_mp){int cst=1;for(unsigned k=0;k<b.w;++k)if(f.nodes[b.v[k]].op!=F_CONST||!f.nodes[b.v[k]].value)cst=0;if(!cst){v=f_mp_fn2(FN_DIV,v,b);continue;}}
  for(unsigned k=0;k<b.w;++k){f_node *n=&f.nodes[b.v[k]];if(n->op!=F_CONST||!n->value){f_fail("fragment division needs nonzero constants");break;}b.v[k]=f_const(1/n->value);}}v=f_binary(F_MUL,v,b);}return v;}
static f_value f_expr(void){f_value v=f_product();while(!f.error&&(f_eq("+")||f_eq("-"))){unsigned op=f_eq("+")?F_ADD:F_SUB;f_next();v=f_binary(op,v,f_product());}return v;}
static void f_precision(void){if(f_eq("lowp")||f_eq("mediump")||f_eq("highp"))f_next();}
static void f_symbol_add(const char *name,f_value value,unsigned storage,unsigned slot){if(!f_valid_name(name)){f_fail("reserved fragment declaration name");return;}{int d=f_find(name);if(d>=0&&(unsigned)d>=f_scope){f_fail("duplicate fragment symbol");return;}}if(f.ns>=F_SYMBOLS){f_fail("too many fragment symbols");return;}f_symbol *s=&f.symbols[f.ns++];scpy(s->name,name,32);s->value=value;s->storage=(uint8_t)storage;s->slot=(uint8_t)slot;}
/* Statements of main() or of an inlined function body (ret != 0: stops at return). */
static int f_statements(f_value *outp,unsigned *assignedp,f_value *ret){
 f_value output=*outp;unsigned assigned=*assignedp;
 while(!f.error&&!f_eq("}")&&f.token[0]){
  if(f_eq("{")){if(f_mp){f_fail("nested block");break;}f_lossy=1;f_next();int bd=1;while(!f.error&&bd>0&&f.token[0]){if(f_eq("{"))++bd;else if(f_eq("}"))--bd;f_next();}continue;}
  else if(f_eq("if")){f_next();f_eat("(");int pd=1;while(!f.error&&pd>0&&f.token[0]){if(f_eq("("))++pd;else if(f_eq(")"))--pd;f_next();}if(f_eq("discard")){f_next();f_eat(";");}else{f_lossy=1;if(f_mp){f_fail("conditional");break;}}continue;}
  if(f_eq("return")){if(!ret){f_fail("return outside a fragment function");break;}f_next();*ret=f_expr();f_eat(";");return 1;}
  if(f_eq("const"))f_next();
  f_precision();int width=f_width(f.token);char name[32];if(width)f_next();
  if(f_eq(";")){f_next();continue;}
  scpy(name,f.token,32);if(!f_id(name[0])){f_fail("invalid fragment statement");break;}f_next();
  if(width&&f_eq(";")){f_next();f_value z=f_zero();z.w=(uint8_t)width;f_symbol_add(name,z,0,0);continue;}
  int lanes[4]={0,1,2,3},nl=0;
  if(!width&&f_eq(".")){f_next();const char *sets[3]={"xyzw","rgba","stpq"};int set=-1;
   for(const char *s=f.token;*s;++s){int lane=-1,which=-1;for(int q=0;q<3;++q)for(int k=0;k<4;++k)if(sets[q][k]==*s){lane=k;which=q;}
    if(lane<0||nl>=4||(set>=0&&set!=which)){f_fail("invalid fragment lhs swizzle");break;}set=which;lanes[nl++]=lane;}
   f_next();
  }
  unsigned cop=0;if(f_eq("*"))cop=F_MUL;else if(f_eq("+"))cop=F_ADD;else if(f_eq("-"))cop=F_SUB;else if(f_eq("/"))cop=99;
  if(cop)f_next();
  f_eat("=");f_value v=f_expr();f_eat(";");
  if(!scmp(name,"gl_FragColor")){if(width||v.kind||v.w!=4||nl||cop){f_fail("gl_FragColor needs vec4");break;}output=v;assigned=1;}
  else if(width){if((v.kind&&v.kind!=1)||v.w!=width||nl||cop){f_fail("fragment local type mismatch");break;}f_symbol_add(name,v,0,0);}
  else{int i=f_find(name);if(i<0||f.symbols[i].storage||(v.kind&&!(cop==99&&f.symbols[i].value.kind==1&&v.kind==1))){f_fail("invalid fragment assignment");break;}
   f_value *dst=&f.symbols[i].value;
   if(!nl){
    if(cop==99&&dst->kind==1&&v.kind==1){if(dst->index==v.index&&v.index>=3&&v.index<=4)f.out->e.uv_divide|=(uint8_t)(1u<<(v.index-3));continue;}
    if(cop==99&&f_mp&&!v.kind&&!dst->kind&&(v.w==dst->w||v.w==1)){*dst=f_mp_fn2(FN_DIV,*dst,v);continue;}
    if(cop==99){f_fail("invalid fragment /= assignment");break;}
    if(cop){v=f_binary(cop,*dst,v);}
    if(v.w!=dst->w){f_fail("invalid fragment assignment");break;}
    *dst=v;
   }else{
    if(v.w!=1&&v.w!=nl){f_fail("fragment swizzle assign width mismatch");break;}
    for(int k=0;k<nl;++k){
     if(lanes[k]>=dst->w){f_fail("fragment swizzle lane out of range");break;}
     uint16_t rhs=v.v[v.w==1?0:k];
     dst->v[lanes[k]]=cop?f_op(cop,dst->v[lanes[k]],rhs):rhs;
    }
   }
  }
 }
 *outp=output;*assignedp=assigned;return 0;
}
static f_value f_call_user(int fi,const f_value *args,unsigned count){
 f_value r=f_zero();
 if(f_call_depth>=4){f_fail("fragment function nesting/recursion");return r;}
 const char *cur=f.cursor;char tok[32];scpy(tok,f.token,32);unsigned ns=f.ns,scope=f_scope;
 ++f_call_depth;f_scope=f.ns;
 f.cursor=f_funcs[fi].params;f_next();unsigned n=0;
 while(!f.error&&!f_eq(")")&&f.token[0]){
  if(f_eq("in")||f_eq("const")){f_next();}
  f_precision();int w=f_width(f.token);if(!w){f_fail("unsupported fragment parameter type");break;}f_next();
  if(n>=count){f_fail("fragment call argument count");break;}
  if(args[n].kind?args[n].w!=2&&args[n].kind==1:args[n].w!=w){f_fail("fragment call argument type");break;}
  char pn[32];scpy(pn,f.token,32);f_next();f_symbol_add(pn,args[n],0,0);++n;
  if(f_eq(","))f_next();
 }
 if(!f.error&&n!=count)f_fail("fragment call argument count");
 if(!f.error){f.cursor=f_funcs[fi].body;f_next();f_value out=f_zero();unsigned as=0;
  if(!f_statements(&out,&as,&r)&&!f.error)f_fail("fragment function without return");
  if(as)f_fail("gl_FragColor in a fragment function");}
 --f_call_depth;f_scope=scope;f.ns=ns;f.cursor=cur;scpy(f.token,tok,32);
 return r;
}
static f_value f_parse(const char *source){
 f.cursor=source;f_next();while(!f.error&&!f_eq("void")&&f.token[0]){
  if(f_eq("precision")){f_next();if(!f_eq("lowp")&&!f_eq("mediump")&&!f_eq("highp")){f_fail("missing fragment precision qualifier");break;}f_precision();if(!f_eq("float")&&!f_eq("int")&&!f_eq("sampler2D")&&!f_eq("samplerCube")){f_fail("invalid fragment precision type");break;}f_next();f_eat(";");continue;}
  if(f_width(f.token)){f_next();char fname[32];scpy(fname,f.token,32);f_next();if(f_eq("(")){const char *params=f.cursor;int pd=1;f_next();while(!f.error&&pd>0&&f.token[0]){if(f_eq("("))++pd;else if(f_eq(")"))--pd;f_next();}
   if(f_eq("{")&&f_nfuncs<F_FUNCS&&f_func_find(fname)<0){scpy(f_funcs[f_nfuncs].name,fname,32);f_funcs[f_nfuncs].params=params;f_funcs[f_nfuncs].body=f.cursor;++f_nfuncs;}
   f_eat("{");int bd=1;while(!f.error&&bd>0&&f.token[0]){if(f_eq("{"))++bd;else if(f_eq("}"))--bd;f_next();}continue;}f_fail("unsupported fragment global declaration");break;}
  unsigned storage=f_eq("uniform")?2:f_eq("varying")?1:0;if(!storage){f_fail("unsupported fragment global declaration");break;}f_next();f_precision();int ismat=(f_eq("mat4")||f_eq("mat3")||f_eq("mat2"));int sampler=f_eq("sampler2D")||f_eq("samplerCube"),width=f_width(f.token);f_next();char name[32];f_name(name);
  int isarr=0;if(f_eq("[")){isarr=1;while(!f.error&&!f_eq("]")&&f.token[0])f_next();f_eat("]");}
  f_eat(";");if(storage==2&&ismat)continue;if(f_mp&&isarr)continue;f_value v=f_zero();unsigned slot=0;
  if(f_mp&&storage==1){if(sampler||!width||f_mp_nvary>=8){f_fail("unsupported fragment varying type");break;}
   unsigned j=f_mp_nvary++;scpy(f_mp_vname[j],name,32);f_mp_vw[j]=(uint8_t)width;v.w=(uint8_t)width;for(int k=0;k<width;++k)v.v[k]=f_intern(F_INPUT,F_MP_VARY+4*j+(unsigned)k,0,0);f_symbol_add(name,v,1,0);continue;}
  if(storage==2){if(sampler){if(f.nsampler>=4){f_fail("fragment sampler limit4");break;}v.kind=2;v.index=(uint8_t)f.nsampler++;}
   else{if(!width){f_fail("unsupported fragment uniform type");break;}if(f.nuniform>=16){f_fail("fragment uniform limit16");break;}v.w=(uint8_t)width;v.index=(uint8_t)f.nuniform++;storage=3;scpy(f_mp_uname[v.index],name,32);f_mp_uw[v.index]=(uint8_t)width;for(int k=0;k<width;++k)v.v[k]=f_intern(F_INPUT,GL2_SOFT_UNIFORM_BASE+4*v.index+(unsigned)k,0,0);}
  }
  else{if(sampler||!width){f_fail("unsupported fragment varying type");break;}v.w=(uint8_t)width;
   if((!scmp(name,"vColor")&&width!=4)||((!scmp(name,"vTexCoord")||!scmp(name,"vTexCoord1"))&&width!=2&&width!=4)){f_fail("reserved fragment varying type mismatch");}
   int used[8]={0};for(unsigned i=0;i<f.ns;++i)if(f.symbols[i].storage==1&&f.symbols[i].slot<8)used[f.symbols[i].slot]=1;
   if(!scmp(name,"vColor")||!scmp(name,"vDiffuse"))slot=1;
   else if(!scmp(name,"vFog"))slot=2;
   else if(!scmp(name,"vTexCoord"))slot=3;
   else if(!scmp(name,"vTexCoord1"))slot=4;
   else if(width==2){slot=!used[3]?3:!used[4]?4:!used[6]?6:!used[7]?7:!used[1]?1:2;}
   else if(width==1){slot=!used[2]?2:!used[6]?6:!used[7]?7:!used[1]?1:!used[4]?4:3;}
   else{slot=!used[6]?6:!used[7]?7:!used[1]?1:!used[2]?2:!used[4]?4:3;}
   if(used[slot]){for(unsigned s=1;s<8;++s)if(s!=5&&!used[s]){slot=s;break;}}
   if(used[slot])f_fail("fragment varying slot occupied");
   if(!scmp(name,"vTexCoord")||!scmp(name,"vTexCoord1")){v.kind=1;v.index=(uint8_t)(!scmp(name,"vTexCoord")?3:4);}
   else if(width==2&&(slot==3||slot==4)){v.kind=1;v.index=(uint8_t)slot;}
   else if(slot!=1&&f_extra_n<4){scpy(f_extra_name[f_extra_n],name,32);for(unsigned k=0;k<(unsigned)width;++k)v.v[k]=f_intern(F_INPUT,F_EXTRA_BASE+4*f_extra_n+k,0,0);++f_extra_n;}
   else{for(unsigned k=0;k<(unsigned)width;++k)v.v[k]=f_intern(F_INPUT,k,0,0);}
  }f_symbol_add(name,v,storage,slot);
 }
 f_eat("void");f_eat("main");f_eat("(");if(f_eq("void"))f_next();f_eat(")");f_eat("{");f_value output=f_zero();unsigned assigned=0;
 f_statements(&output,&assigned,0);
 f_eat("}");if(f.token[0])f_fail("fragment text after main");if(!assigned)f_fail("gl_FragColor not assigned");return output;
}
static void f_rollback(unsigned mark){while(f.n>mark){f.table[f.nodes[f.n].slot]=0;--f.n;}}
static uint16_t f_previous[4];
static uint16_t f_source(unsigned code,unsigned alpha,unsigned channel){if(code==1)return f_previous[alpha?3:channel];if(code==3)return 1;return f_intern(F_INPUT,(code==2?4:0)+(alpha?3:channel),0,0);}
static uint16_t f_stage_base(const uint8_t *c,unsigned channel,int alpha){
 uint16_t one=f_const(1),x1=c[0]==3?one:f_source(c[0],0,channel),y1=f_source(c[2],c[3]||alpha,channel);
 if(alpha&&c[0]!=3)x1=f_source(c[0],1,channel);
 if(c[1]){x1=f_op(F_SUB,one,x1);}if(c[4]){y1=f_op(F_SUB,one,y1);}uint16_t out=f_op(F_MUL,x1,y1);
 if(!c[9]){uint16_t x2=c[5]==3?one:f_source(c[5],alpha,channel),y2=f_source(c[6],c[7]||alpha,channel);if(c[8])y2=f_op(F_SUB,one,y2);out=f_op(F_ADD,out,f_op(F_MUL,x2,y2));}
 return out;
}
/* Exhaust finite instruction operands; exact hash-consed AST identity is required.
 * Commutative/associative normalization handles reordered arithmetic. Candidate
 * nodes are rolled back; no numerical probes or hash-only acceptance. */
/* Strip instruction post-modifiers from the target before enumeration.
 * This is structural inversion, not algebraic approximation. It removes an
 * eightfold bias/scale loop for the overwhelmingly common unmodified target. */
static uint16_t f_unmodify(uint16_t id,unsigned bias,unsigned scale){
 if(scale==3){f_node n=f.nodes[id];if(n.op!=F_SUB||!f_is(n.a,1))return 0;id=n.b;}
 else if(scale){
  uint16_t terms[32];unsigned n=f_terms(F_MUL,id,terms,0),take=32;
  for(unsigned i=0;i<n;++i)if(f_is(terms[i],scale==1?2:4)){take=i;break;}
  if(take==32)return 0;
  id=f_const(1);for(unsigned i=0;i<n;++i)if(i!=take)id=f_op(F_MUL,id,terms[i]);
 }
 if(bias){f_node n=f.nodes[id];if(n.op!=F_SUB||!f_is(n.b,.5f))return 0;id=n.a;}
 return id;
}
static int f_lower(f_value target,uint8_t *plan,int alpha,int previous){
 const unsigned xs[4]={2,3,previous?1u:0u,0},ys[4]={0,2,3,1};
 unsigned outer=f.n,channels=alpha?1:3;
 for(unsigned bias=0;bias<2;++bias)for(unsigned scale=0;scale<4;++scale){
  f_value base=target;int valid=1;
  for(unsigned k=0;k<channels;++k){unsigned lane=alpha?3:k;base.v[lane]=f_unmodify(target.v[lane],bias,scale);if(!base.v[lane])valid=0;}
  if(!valid){f_rollback(outer);continue;}
  unsigned mark=f.n;uint8_t c[12]={0};c[10]=(uint8_t)bias;c[11]=(uint8_t)scale;
  for(unsigned products=1;products<=2;++products)
  for(unsigned xi=0;xi<(previous?(alpha?4u:3u):alpha?3u:2u);++xi)for(c[1]=0;c[1]<2;++c[1])for(unsigned yi=0;yi<(previous?4u:3u);++yi)for(c[3]=0;c[3]<(alpha?1:2);++c[3])for(c[4]=0;c[4]<2;++c[4])for(unsigned term=products==1?36:0;term<(products==1?37u:36u);++term){
   c[0]=(uint8_t)xs[xi];c[2]=(uint8_t)ys[yi];c[9]=term==36;c[5]=term==36?3:(uint8_t)(term/12==0?0:term/12==1?2:3);c[6]=term==36?0:(uint8_t)ys[(term%12)/4];c[7]=alpha?0:(uint8_t)((term%4)/2);c[8]=term==36?0:(uint8_t)(term&1);
   int match=1;for(unsigned k=0;k<channels&&match;++k)match=f_stage_base(c,alpha?3:k,alpha)==base.v[alpha?3:k];
   f_rollback(mark);if(f.error)return -1;if(match){for(unsigned k=0;k<12;++k)plan[k]=c[k];f_rollback(outer);return 0;}
  }
  f_rollback(outer);
 }
 return -1;
}
/* Bounded two-stage graph search. Candidate intermediates come from the
 * source DAG and associative product pairs, not source-expression templates.
 * Only texture-bounded values may cross the stage clamp. Primary/uniform
 * ranges are not a proof of a GLSL value bound. RGB and alpha are rematched
 * independently using the same finite instruction grammar as one-stage code. */
#define F_STAGE_CANDIDATES 16
static uint16_t f_lane(uint16_t id,unsigned lane){
 f_node n=f.nodes[id];
 if(n.op==F_CONST)return id;
 if(n.op==F_INPUT)return n.a==4?f_intern(F_INPUT,4+lane,0,0):n.a==0?f_intern(F_INPUT,lane,0,0):0;
 uint16_t a=f_lane(n.a,lane);if(!a)return 0;
 if(n.op==F_CLAMP)return f_intern(F_CLAMP,a,0,0);
 uint16_t b=f_lane(n.b,lane);return b?f_op(n.op,a,b):0;
}
static void f_candidate(uint16_t id,uint16_t *ids,unsigned *count){
 f_node *n=&f.nodes[id];
 if(n->op==F_CONST||n->op==F_INPUT||(n->deps&~0x1fu)||n->lo<0||n->hi>1)return;
 for(unsigned i=0;i<*count;++i)if(ids[i]==id)return;
 if(*count<F_STAGE_CANDIDATES)ids[(*count)++]=id;
}
static unsigned f_candidates(uint16_t root,unsigned channel,uint16_t *ids){
 unsigned count=0,original=f.n;uint8_t live[F_NODES]={0};live[root]=1;
 for(unsigned i=original;i;--i)if(live[i]){
  f_node n=f.nodes[i];
  if(channel==0)f_candidate((uint16_t)i,ids,&count);
  else if(n.op!=F_CONST&&n.op!=F_INPUT&&!(n.deps&~0x80u)&&n.lo>=0&&n.hi<=1&&count<F_STAGE_CANDIDATES)ids[count++]=(uint16_t)i;
  if(n.op!=F_CONST&&n.op!=F_INPUT){live[n.a]=1;if(n.op!=F_CLAMP)live[n.b]=1;}
 }
 for(unsigned i=1;i<=original&&count<F_STAGE_CANDIDATES;++i)if(live[i]&&f.nodes[i].op==F_MUL){
  uint16_t terms[32];unsigned n=f_terms(F_MUL,(uint16_t)i,terms,0);
  for(unsigned a=0;a<n&&count<F_STAGE_CANDIDATES;++a)for(unsigned b=a+1;b<n&&count<F_STAGE_CANDIDATES;++b){
   if((f.nodes[terms[a]].deps|f.nodes[terms[b]].deps)&~(1u<<(4+channel)))continue;
   uint16_t id=f_op(F_MUL,terms[a],terms[b]);
   if(channel==0)f_candidate(id,ids,&count);
   else if(f.nodes[id].lo>=0&&f.nodes[id].hi<=1){unsigned j=0;for(;j<count;++j)if(ids[j]==id)break;if(j==count)ids[count++]=id;}
  }
 }
 return count;
}
static int f_lower_two(f_value target,uint8_t (*plan)[12],uint8_t (*alpha)[12]){   /* two rows each */
 uint16_t ids[F_STAGE_CANDIDATES+1],aids[F_STAGE_CANDIDATES+2];
 unsigned count=f_candidates(target.v[0],0,ids),acount=f_candidates(target.v[3],3,aids);
 /* Pass-through is essential when only alpha requires two stages. */
 ids[count++]=f_intern(F_INPUT,4,0,0);aids[acount++]=f_intern(F_INPUT,7,0,0);
 for(unsigned i=0;i<count&&!f.error;++i){
  unsigned mark=f.n;f_value first=f_zero();first.w=4;int mapped=1;
  for(unsigned k=0;k<4;++k){first.v[k]=f_lane(ids[i],k);if(!first.v[k])mapped=0;}
  /* An explicit clamp on the intermediate is the stage's own output saturation: match its
   * operand (stage 2 still reads the clamped node as "previous"). */
  f_value body=first;if(mapped)for(unsigned k=0;k<4;++k)if(f.nodes[first.v[k]].op==F_CLAMP)body.v[k]=f.nodes[first.v[k]].a;
  if(mapped&&!f_lower(body,plan[0],0,0)){
   /* Add the corresponding alpha shape as the first choice for backward
    * precision stability; then independently enumerate alpha source DAGs. */
   uint16_t shared=first.v[3];
   for(unsigned mode=0;mode<=acount;++mode){
    first.v[3]=mode?aids[mode-1]:shared;body.v[3]=f.nodes[first.v[3]].op==F_CLAMP?f.nodes[first.v[3]].a:first.v[3];
    if(f_lower(body,alpha[0],1,0))continue;
    for(unsigned k=0;k<4;++k)f_previous[k]=first.v[k];
    if(!f_lower(target,plan[1],0,1)&&!f_lower(target,alpha[1],1,1))return 0;
   }
  }
  f_rollback(mark);
 }
 return -1;
}
/* DOT3 (layer word 0 bit 13), measured on the device (hw-reports/run-dot3-1, -2):
 *   colour = clamp(scale * sum_k (X1_k - .5) * (Y1_k - .5)), grey; the stage's result scale applies
 *   (x4 gives GL's 4 * dot). X2 must be "one" (X2 = P gave per-channel garbage), drop product 2.
 *   alpha  = clamp(sum ...) at x1 whatever the alpha half says.
 *   The next stage reads the result as C (previous).
 * So a DOT3 shader takes two stages: stage 1 the dot product, stage 2 the shader's colour as a
 * function of C (C, C * T, C * P, ...) and its alpha. Matched forms (exact AST identity, with T the
 * texture and P the vertex colour, or T with itself):
 *   4 * dot(T - .5, P - .5)      dot(T * 2 - 1, P * 2 - 1)      and either under clamp(x, 0, 1). */
#ifndef GL2_DOT3_SCALE
#define GL2_DOT3_SCALE 2   /* result scale code: 2 = x4 */
#endif
#ifndef GL2_DOT3
#define GL2_DOT3 1
#endif
#if GL2_DOT3
static int f_reads_prev(const uint8_t *c){return c[0]==1||c[2]==1||(!c[9]&&(c[5]==1||c[6]==1));}
static int f_lower_dot3(f_value target,uint8_t (*plan)[12],uint8_t (*alpha)[12]){
 unsigned mark=f.n;
 for(unsigned form=0;form<2&&!f.error;++form)for(unsigned y=0;y<2&&!f.error;++y){
  uint16_t d=f_const(0);
  for(unsigned k=0;k<3;++k){
   uint16_t a=f_intern(F_INPUT,4+k,0,0),b=f_intern(F_INPUT,(y?0:4)+k,0,0),pa,pb;
   if(!form){pa=f_op(F_SUB,a,f_const(.5f));pb=f_op(F_SUB,b,f_const(.5f));}
   else{pa=f_op(F_SUB,f_op(F_MUL,a,f_const(2)),f_const(1));pb=f_op(F_SUB,f_op(F_MUL,b,f_const(2)),f_const(1));}
   d=f_op(F_ADD,d,f_op(F_MUL,pa,pb));
  }
  if(!form)d=f_op(F_MUL,d,f_const(4));
  const uint16_t cd=f_intern(F_CLAMP,d,0,0);
  for(unsigned pass=0;pass<2;++pass){
   /* the stage output is clamp(d); plain d only where the shader outputs d itself in that channel (the final write clamps) */
   for(unsigned k=0;k<4;++k)f_previous[k]=pass?d:cd;
   if(f_lower(target,plan[1],0,1)||f_lower(target,alpha[1],1,1)){if(f.error)return -1;continue;}
   int rc=f_reads_prev(plan[1]),ra=f_reads_prev(alpha[1]);
   if(!rc&&!ra)continue;   /* stage 2 must read the dot */
   if(pass&&((rc&&!(target.v[0]==d&&target.v[1]==d&&target.v[2]==d))||(ra&&target.v[3]!=d)))continue;
   for(unsigned i=0;i<12;++i){plan[0][i]=0;alpha[0][i]=0;}
   /* X1 T, Y1 P or T, X2 one, Y2 field 1 (RetailOS's DOT3_RGBA): clamp(4 * sum) in colour and alpha (run-dot3-1 case 9) */
   plan[0][0]=2;plan[0][2]=y?0:2;plan[0][5]=3;plan[0][6]=1;plan[0][9]=1;plan[0][11]=0;
   alpha[0][0]=3;alpha[0][5]=3;alpha[0][9]=1;   /* one * P: ignored by the hardware in DOT3 mode */
   return 0;
  }
  f_rollback(mark>f.n?f.n:mark);
 }
 return -1;
}
#endif
static uint16_t f_alpha_word(const uint8_t *a){return(uint16_t)(a[0]<<13|a[2]<<11|a[5]<<9|a[6]<<7|a[1]<<6|a[4]<<5|a[8]<<3|a[9]<<4|a[10]<<2|a[11]);}
static unsigned f_dependencies(uint16_t id){return f.nodes[id].deps;}
static int f_software(f_value target,gl2_soft_program *p){
 uint8_t live[F_NODES]={0};uint16_t map[F_NODES];
 for(unsigned k=0;k<4;++k)live[target.v[k]]=1;
 for(unsigned i=f.n;i;--i)if(live[i]){f_node *n=&f.nodes[i];if(n->op!=F_CONST&&n->op!=F_INPUT){live[n->a]=1;if(n->op!=F_CLAMP)live[n->b]=1;}}
 p->count=0;for(unsigned i=1;i<=f.n;++i)if(live[i]){if(p->count>=GL2_SOFT_INSNS){f_fail("fragment software instruction limit");return -1;}f_node *n=&f.nodes[i];map[i]=p->count;gl2_soft_insn *s=&p->code[p->count++];s->op=n->op;s->value=n->value;s->a=n->op==F_INPUT?(n->a>=F_EXTRA_BASE?(unsigned)((n->a-F_EXTRA_BASE)%4u):(unsigned)n->a<(unsigned)GL2_SOFT_UNIFORM_BASE?(unsigned)n->a:(unsigned)GL2_SOFT_UNIFORM_BASE+f.uniform_map[(n->a-GL2_SOFT_UNIFORM_BASE)/4]*4+(n->a-GL2_SOFT_UNIFORM_BASE)%4):n->op==F_CONST?0:map[n->a];s->b=(n->op==F_ADD||n->op==F_MUL||n->op==F_SUB)?map[n->b]:0;}
 for(unsigned k=0;k<4;++k){p->output[k]=map[target.v[k]];}return 0;
}
static uint16_t f_drop_in3(uint16_t id){
 uint16_t terms[32];unsigned n=f_terms(F_MUL,id,terms,0),res=f_const(1);int dropped=0;
 for(unsigned i=0;i<n;++i){if(!dropped&&f.nodes[terms[i]].op==F_INPUT&&f.nodes[terms[i]].a==3){dropped=1;continue;}res=f_op(F_MUL,res,terms[i]);}
 return res;
}
/* mix(a,b,t) arrives as a*(1-t) + b*t: the factor t of a (1-t) term, or 0. */
static uint16_t f_mix_t(uint16_t id){
 uint16_t terms[32];unsigned n=f_terms(F_MUL,id,terms,0);
 for(unsigned i=0;i<n;++i){f_node *x=&f.nodes[terms[i]];if(x->op==F_SUB&&f_is(x->a,1))return x->b;}
 return 0;
}
static uint16_t f_drop_factor(uint16_t id,uint16_t t,int *ok){
 uint16_t terms[32];unsigned n=f_terms(F_MUL,id,terms,0),res=f_const(1);int dropped=0;
 for(unsigned i=0;i<n;++i){if(!dropped&&terms[i]==t){dropped=1;continue;}res=f_op(F_MUL,res,terms[i]);}
 *ok=dropped;return dropped?(uint16_t)res:id;
}
#define F_UNIFORM_DEPS 0x00ffff00u   /* deps bits 8..23: fragment uniforms (24..27: extra varyings) */
static uint16_t f_strip_mix(uint16_t id){
 f_node n=f.nodes[id];
 if(n.op==F_ADD){
  /* drop a uniform-only blend side (fog colour), keep the other; as before for
   * colour-only shaders, and also removes the blend factor of a mix(). */
  int da=(f.nodes[n.a].deps&F_UNIFORM_DEPS)&&!(f.nodes[n.a].deps&0xffu), db=(f.nodes[n.b].deps&F_UNIFORM_DEPS)&&!(f.nodes[n.b].deps&0xffu);
  /* also a constant fog colour (OpenLara underwater: mix(K, colour, fog)): a side with no
   * colour/texture input that carries a (1 - t) factor, against a side that has them */
  {int ca=f.nodes[n.a].deps&0xffu, cb=f.nodes[n.b].deps&0xffu;
   if(!da&&!db){if(!ca&&cb&&f_mix_t(n.a))da=1;else if(!cb&&ca&&f_mix_t(n.b))db=1;}}
  /* the blend factor goes before recursing, or a fold could take it for lighting */
  if(da&&!db){uint16_t t=f_mix_t(n.a);int ok=0;uint16_t keep=t?f_drop_factor(n.b,t,&ok):n.b;if(ok)f_take_fog(t);uint16_t r=f_strip_mix(keep);return ok?r:f_drop_in3(r);}
  if(db&&!da){uint16_t t=f_mix_t(n.b);int ok=0;uint16_t keep=t?f_drop_factor(n.a,t,&ok):n.a;if(ok)f_take_fog(t);uint16_t r=f_strip_mix(keep);return ok?r:f_drop_in3(r);}
  /* any other mix(x, y, t) with a varying t: take t = 1 (y), as for fog (OpenLara's
   * underwater entities tint everything below the water plane: the whole entity is tinted) */
  {uint16_t t=f_mix_t(n.a);if(t&&f.nodes[t].op!=F_CONST){int ok=0;uint16_t keep=f_drop_factor(n.b,t,&ok);if(ok)return f_strip_mix(keep);}
   t=f_mix_t(n.b);if(t&&f.nodes[t].op!=F_CONST){int ok=0;uint16_t keep=f_drop_factor(n.a,t,&ok);if(ok)return f_strip_mix(keep);}}
  return f_op(F_ADD,f_strip_mix(n.a),f_strip_mix(n.b));
 }
 if(n.op==F_MUL){
  uint16_t a=f_strip_mix(n.a),b=f_strip_mix(n.b);
  for(unsigned side=0;side<2;++side){f_node *x=&f.nodes[side?b:a];uint16_t other=side?a:b;
   if(x->op==F_INPUT&&x->a>=F_EXTRA_BASE&&x->a<F_EXTRA_BASE+16u&&!(f.nodes[other].deps&~255u)&&f_fold_lane<4u&&f_fold_j[f_fold_lane]<0){
    f_fold_j[f_fold_lane]=(int)((x->a-F_EXTRA_BASE)/4u);f_fold_k[f_fold_lane]=(int)((x->a-F_EXTRA_BASE)%4u);return other;}}
  /* a constant colour factor on a vertex-colour product (OpenLara underwater tint) is
   * applied per vertex too: the TSP has no per-lane constant multiply */
  for(unsigned side=0;side<2;++side){f_node *x=&f.nodes[side?b:a];uint16_t other=side?a:b;
   if(x->op==F_CONST&&x->value>0&&x->value<=1&&(f.nodes[other].deps&0x0fu)&&!(f.nodes[other].deps&~255u)&&f_fold_lane<3u){f_fold_c[f_fold_lane]*=x->value;return other;}}
  if(a==b&&f.nodes[a].op==F_INPUT&&f.nodes[a].a<4)return a;
  return f_op(F_MUL,a,b);
 }
 if(n.op==F_SUB)return f_op(F_SUB,f_strip_mix(n.a),f_strip_mix(n.b));
 return id;
}
static int compile_fragment(const char *source,gl2_fs_owned *out,char *log){
 int strip_used=0;f_mp=0;f_lossy=0;
 f.cursor=0;f.n=f.ns=f.nvary=f.nsampler=f.nuniform=f.depth=f.error=0;f_tex_sampler=-1;f_nfuncs=0;f_call_depth=0;f_scope=0;f_extra_n=0;f_fold_lane=4;for(unsigned k=0;k<4;++k)f_fold_j[k]=-1;f.log=log;f.out=out;for(unsigned i=0;i<F_TABLE;++i)f.table[i]=0;
 for(unsigned i=0;i<sizeof *out;++i)((uint8_t *)out)[i]=0;
 f_const(0);f_const(1);f_value target=f_parse(source);if(f.error)return -1;
 for(unsigned k=0;k<4;++k)if(f.nodes[target.v[k]].op==F_CLAMP)target.v[k]=f.nodes[target.v[k]].a;
 unsigned deps=0;for(unsigned k=0;k<4;++k)deps|=f_dependencies(target.v[k]);
 for(unsigned i=0;i<f.ns;++i){f_symbol *s=&f.symbols[i];if(s->storage==3&&(deps&(1u<<(8+s->value.index)))){unsigned u=out->e.nuniform++;f.uniform_map[s->value.index]=(uint8_t)u;scpy(out->uniforms[u],s->name,32);out->e.uniform_names[u]=out->uniforms[u];out->e.uniform_widths[u]=s->value.w;}}
 if(f_software(target,&out->software)){return -1;}out->e.software=&out->software;
 uint8_t alpha[2][12]={{0}};int gpu=0;for(unsigned k=0;k<4;++k)f_previous[k]=1;
 if((deps&0xffu)&&!(deps&~255u)){
  if(!f_lower(target,out->e.plan[0],0,0)&&!f_lower(target,alpha[0],1,0))gpu=1;
  else if(!f.error&&!f_lower_two(target,out->e.plan,alpha))gpu=2;
#if GL2_DOT3
  /* last: only what the per-channel stages cannot express (run-dot3-2 measured the encoding) */
  else if(!f.error){for(unsigned k=0;k<4;++k)f_previous[k]=1;if(!f_lower_dot3(target,out->e.plan,alpha)){gpu=2;out->e.has_alpha=2;}}
#endif
 }
 if(!gpu&&(deps&~255u)){
  f_value st=target;f_fog_j=-1;f_fog_k=0;f_fog_mask=0;for(unsigned k=0;k<4;++k){f_fold_j[k]=-1;f_fold_k[k]=0;}for(unsigned k=0;k<4;++k){f_fold_c[k]=1;f_fold_lane=k;st.v[k]=f_strip_mix(target.v[k]);}f_fold_lane=4;
  unsigned sdeps=0;for(unsigned k=0;k<4;++k)sdeps|=f_dependencies(st.v[k]);
  if((sdeps&0xffu)&&!(sdeps&~255u)){
   if(!f_lower(st,out->e.plan[0],0,0)&&!f_lower(st,alpha[0],1,0)){gpu=1;deps=sdeps;}
   else if(!f.error&&!f_lower_two(st,out->e.plan,alpha)){gpu=2;deps=sdeps;}
   if(gpu){strip_used=1;int j=-1;for(unsigned k=0;k<3;++k)if(f_fold_j[k]>=0){if(j<0)j=f_fold_j[k];if(f_fold_j[k]==j){out->e.fold_mask|=(uint8_t)(1u<<k);out->e.fold_comp[k]=(uint8_t)f_fold_k[k];}}
    if(j>=0){scpy(out->fold_vary,f_extra_name[j],32);out->e.fold_name=out->fold_vary;}
    for(unsigned k=0;k<3;++k)if(f_fold_c[k]!=1)out->e.fold_const[k]=f_fold_c[k];
    if(f_fog_j!=-1&&f_fog_mask){
     const char *nm=0;
     if(f_fog_j>=0)nm=f_extra_name[f_fog_j];
     else if(!(deps&8u))for(unsigned i=0;i<f.ns;++i)if(f.symbols[i].storage==1&&f.symbols[i].slot==1)nm=f.symbols[i].name;  /* colour alpha: only if the plan no longer reads it */
     if(nm){scpy(out->fog_vary,nm,32);out->e.fog_name=out->fog_vary;out->e.fog_comp=(uint8_t)f_fog_k;out->e.fog_mask=f_fog_mask;}}}
  }
 }
 f.error=0;
 out->e.hash=gl2_hash(source);out->e.source=source;out->e.nsampler=(deps&0xf0u)?1:0;out->e.nplan=(uint8_t)gpu;out->e.approx=(uint8_t)(f_lossy||strip_used||out->e.uv_approx);out->e.has_alpha|=(uint8_t)(gpu!=0);
 for(unsigned stage=0;stage<(unsigned)gpu;++stage)out->e.alpha[stage]=f_alpha_word(alpha[stage]);
 for(unsigned i=0;i<f.ns;++i){f_symbol *s=&f.symbols[i];if(s->storage==2&&out->e.nsampler&&s->value.index==(uint8_t)(f_tex_sampler<0?0:f_tex_sampler)){scpy(out->samplers[0],s->name,32);out->e.sampler_names[0]=out->samplers[0];}
  else if(s->storage==1&&((s->slot==1&&(deps&15u))||(s->slot==3&&out->e.nsampler))){unsigned n=out->e.nvary;if(n>=3){f_fail("too many fragment varyings");return -1;}scpy(out->varyings[n],s->name,32);out->e.vary_names[n]=out->varyings[n];out->e.vary_slots[n]=s->slot;out->e.vary_widths[n]=s->value.w;++out->e.nvary;if(s->slot==1)out->e.needs_colour=1;}
 }
 scpy(log,gpu?"compiled:native combiner plan; state may require fallback":"compiled:software-only; no native plan (slow)",64);return 0;
}
