/* Equivalence check for the native vertex-code optimisations (vsc_optimize, zero-uniform
 * specialisation): links each vertex/fragment pair with the optimiser off and on, runs both
 * GPU programs in a VGP model (same semantics as NanoEmu's nano7_mbxr.c) on random inputs and
 * constants, and for every specialisation key also runs the folded variant against the base
 * program with those lanes zeroed. Trials whose base run produces inf/NaN are skipped (0*inf).
 * Built and run by vs_opt_check.py. */
#include "gl2api.c"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
static gl2_ctx C; static gl2_prog_obj progs[GL2_MAX_PROGRAMS]; static uint32_t seg[8192];
static char vsrc[65536],fsrc[65536];
static void rd(const char*p,char*b){FILE*f=fopen(p,"rb");size_t n=fread(b,1,65535,f);b[n]=0;fclose(f);}
static float cst[256][4];static int nonfinite;
static void run(const uint32_t *code,unsigned n,float in[8][4],float out[8][4]){float t[16][4];memset(t,0,sizeof t);memset(out,0,32*4);
 for(unsigned i=0;i<n;++i){unsigned A=code[2*i],B=code[2*i+1],op=A&31,dst=B>>26,mask=(B>>22)&15;float s[3][4],res[4]={0,0,0,0};
  for(int k=0;k<3;++k){unsigned f=(B>>(16+2*k))&3,idx=(B>>(4*k))&15,swz=(A>>(8+8*k))&255;const float *v;static const float zero[4]={1,1,1,1}; /* operand file 3: not 0.0 on hardware (NanoEmu models 1.0) */
   if(f==0)v=in[idx&7];else if(f==1)v=cst[((B>>12)&15)<<4|idx];else if(f==2)v=t[idx];else v=zero;
   for(int q=0;q<4;++q){s[k][q]=v[(swz>>(2*q))&3];if(A&(1u<<(5+k)))s[k][q]=-s[k][q];}}
  switch(op){case 1:for(int q=0;q<4;++q)res[q]=s[0][q];break;case 2:for(int q=0;q<4;++q)res[q]=s[0][q]+s[1][q];break;case 3:for(int q=0;q<4;++q)res[q]=s[0][q]*s[1][q]+s[2][q];break;case 4:for(int q=0;q<4;++q)res[q]=s[0][q]*s[1][q];break;
   case 5:res[0]=res[1]=res[2]=res[3]=1.f/s[0][3];break;case 6:res[0]=res[1]=res[2]=res[3]=1.f/sqrtf(fabsf(s[0][3]));break;case 7:res[0]=res[1]=res[2]=res[3]=s[0][0]*s[1][0]+s[0][1]*s[1][1]+s[0][2]*s[1][2];break;
   case 8:res[0]=res[1]=res[2]=res[3]=s[0][0]*s[1][0]+s[0][1]*s[1][1]+s[0][2]*s[1][2]+s[0][3]*s[1][3];break;case 9:for(int q=0;q<4;++q)res[q]=fminf(s[0][q],s[1][q]);break;case 10:for(int q=0;q<4;++q)res[q]=fmaxf(s[0][q],s[1][q]);break;
   case 12:for(int q=0;q<4;++q)res[q]=s[0][q]>=s[1][q];break;case 19:res[0]=res[1]=res[2]=res[3]=exp2f(s[0][3]);break;default:printf("op %u?\n",op);}
  float *d=dst>=16?t[dst-16]:out[dst];for(int q=0;q<4;++q)if(mask>>q&1){d[q]=res[q];if(!isfinite(res[q]))nonfinite=1;}}}
static unsigned link1(int opt,uint32_t *code){
 gl2_vs_opt=opt;memset(&C,0,sizeof C);memset(progs,0,sizeof progs);gl2_init(&C,progs,seg,8192,0,0);
 uint32_t vs=gl2x_glCreateShader(&C,GL_VERTEX_SHADER),fs=gl2x_glCreateShader(&C,GL_FRAGMENT_SHADER);
 gl2x_glShaderSource(&C,vs,vsrc);gl2x_glShaderSource(&C,fs,fsrc);gl2x_glCompileShader(&C,vs);gl2x_glCompileShader(&C,fs);
 uint32_t p=gl2x_glCreateProgram(&C);gl2x_glAttachShader(&C,p,vs);gl2x_glAttachShader(&C,p,fs);
 static const char*an[]={"aCoord","aNormal","aTexCoord","aColor","aLight"};for(int i=0;i<5;++i)gl2x_glBindAttribLocation(&C,p,i,an[i]);
 gl2x_glLinkProgram(&C,p);gl2_prog_obj*P=&C.pr[p];if(!P->ok)return 0;
 for(unsigned i=0;i<2*P->prog.nstage;++i)code[i]=P->prog.code[i];
 for(unsigned i=0;i<P->gpu_nlit;++i)for(int k=0;k<4;++k)cst[P->gpu_lit_idx[i]][k]=P->gpu_lit_val[i][k];
 return P->prog.nstage;}

static gl2_prog_obj *linkP(void){
 gl2_vs_opt=1;memset(&C,0,sizeof C);memset(progs,0,sizeof progs);gl2_init(&C,progs,seg,8192,0,0);
 uint32_t vs=gl2x_glCreateShader(&C,GL_VERTEX_SHADER),fs=gl2x_glCreateShader(&C,GL_FRAGMENT_SHADER);
 gl2x_glShaderSource(&C,vs,vsrc);gl2x_glShaderSource(&C,fs,fsrc);gl2x_glCompileShader(&C,vs);gl2x_glCompileShader(&C,fs);
 uint32_t p=gl2x_glCreateProgram(&C);gl2x_glAttachShader(&C,p,vs);gl2x_glAttachShader(&C,p,fs);
 static const char*an[]={"aCoord","aNormal","aTexCoord","aColor","aLight"};for(int i=0;i<5;++i)gl2x_glBindAttribLocation(&C,p,i,an[i]);
 gl2x_glLinkProgram(&C,p);return C.pr[p].ok?&C.pr[p]:0;}
static int foldtest(void){
 gl2_prog_obj*P=linkP();if(!P)return 0;int bad=0;
 static float lit[256][4];memset(cst,0,sizeof cst);for(unsigned i=0;i<P->gpu_nlit;++i)for(int k=0;k<4;++k)cst[P->gpu_lit_idx[i]][k]=P->gpu_lit_val[i][k];memcpy(lit,cst,sizeof cst);
 printf("   candidates %u:",P->nspec);for(unsigned i=0;i<P->nspec;++i)printf(" c%u",P->spec_row[i]);printf("\n");
 unsigned keys[16];unsigned nk=0;for(unsigned i=0;i<P->nspec;++i){keys[nk++]=15u<<(4*i);keys[nk++]=7u<<(4*i);}
 unsigned all=0,allxyz=0;for(unsigned i=0;i<P->nspec;++i){all|=15u<<(4*i);allxyz|=7u<<(4*i);}if(P->nspec){keys[nk++]=all;keys[nk++]=allxyz;}
 static uint32_t var[512][2];static uint8_t zr[256];
 for(unsigned q=0;q<nk;++q){memset(zr,0,sizeof zr);for(unsigned i=0;i<P->nspec;++i)zr[P->spec_row[i]]=(keys[q]>>(4*i))&15;
  unsigned nv=spec_build(P,zr,var);double worst=0;
  for(int trial=0;trial<1500;++trial){float in[8][4],oa[8][4],ob[8][4];srand(trial*13+5);
   for(int r=0;r<8;++r)for(int k=0;k<4;++k)in[r][k]=(float)(rand()%2000-1000)/37.f;
   for(int r=0;r<256;++r)for(int k=0;k<4;++k)cst[r][k]=lit[r][k]!=0?lit[r][k]:((zr[r]>>k)&1)?0.f:(float)(rand()%2000-1000)/91.f;
   nonfinite=0;run(&P->gpu_code[0][0],P->base_nstage,in,oa);if(nonfinite)continue;run(&var[0][0],nv,in,ob);
   for(int o=0;o<8;++o)for(int k=0;k<4;++k){double d=fabs(oa[o][k]-ob[o][k]);if(isnan(oa[o][k])&&isnan(ob[o][k]))continue;if(isnan(oa[o][k])!=isnan(ob[o][k]))d=1e9;if(d>worst){worst=d;if(getenv("WHY"))printf("     trial %d out%d.%d base %g var %g\n",trial,o,k,oa[o][k],ob[o][k]);}}}
  printf("   key %04x: %u -> %u instr  max diff %g%s\n",keys[q],P->base_nstage,nv,worst,worst>1e-3?"  MISMATCH":"");if(worst>1e-3)bad=1;}
 return bad;}
static const char *opn(unsigned o){switch(o){case 1:return "MOV";case 2:return "ADD";case 3:return "MAD";case 4:return "MUL";case 5:return "RCP";case 6:return "RSQ";case 7:return "DP3";case 8:return "DP4";case 9:return "MIN";case 10:return "MAX";case 12:return "SGE";case 19:return "EX2";}return "?";}
static void dis(const uint32_t *code,unsigned n){for(unsigned i=0;i<n;++i){unsigned A=code[2*i],B=code[2*i+1],op=A&31,dst=B>>26,m=(B>>22)&15;printf("%3u %s d%-2u.%c%c%c%c",i,opn(op),dst,m&1?'x':'_',m&2?'y':'_',m&4?'z':'_',m&8?'w':'_');
   for(int k=0;k<3;++k){unsigned f=(B>>(16+2*k))&3,idx=(B>>(4*k))&15,sw=(A>>(8+8*k))&255;printf("  %c%u.%c%c%c%c%s",f==0?'i':f==1?'c':f==2?'t':'0',f==1?(((B>>12)&15)<<4|idx):idx,"xyzw"[sw&3],"xyzw"[sw>>2&3],"xyzw"[sw>>4&3],"xyzw"[sw>>6&3],A&(1u<<(5+k))?"-":"");}printf("\n");}}
int main(int argc,char**argv){

 static uint32_t a[512],b[512];int bad=0;
 for(int x=1;x+1<argc;x+=2){rd(argv[x],vsrc);rd(argv[x+1],fsrc);
  unsigned na=link1(0,a),nb=link1(1,b);unsigned movs=0,mova=0;for(unsigned i=0;i<nb;++i)movs+=(b[2*i]&31)==1;for(unsigned i=0;i<na;++i)mova+=(a[2*i]&31)==1;
  double worst=0;
  for(int trial=0;trial<2000;++trial){float in[8][4],oa[8][4],ob[8][4];
   for(int r=0;r<256;++r)for(int k=0;k<4;++k){ /* keep literal rows: only randomise rows above 23? randomise all non-literal rows */ }
   srand(trial*7+1);for(int r=0;r<8;++r)for(int k=0;k<4;++k)in[r][k]=(float)(rand()%2000-1000)/37.f;
   static float lit[256][4];if(trial==0)memcpy(lit,cst,sizeof cst);
   for(int r=0;r<256;++r)for(int k=0;k<4;++k)cst[r][k]=lit[r][k]!=0?lit[r][k]:(float)(rand()%2000-1000)/91.f;
   run(a,na,in,oa);run(b,nb,in,ob);
   for(int o=0;o<8;++o)for(int k=0;k<4;++k){double d=fabs(oa[o][k]-ob[o][k]);if(isnan(oa[o][k])!=isnan(ob[o][k]))d=1e9;if(!(isnan(oa[o][k])&&isnan(ob[o][k]))&&d>worst)worst=d;}}
  const char*bn=strrchr(argv[x],'/');printf("%s  %u -> %u instr  (MOVs %u -> %u)  max diff %g %s\n",bn?bn+1:argv[x],na,nb,mova,movs,worst,worst>0?"MISMATCH":"");if(worst>0)bad=1;bad|=foldtest();}
 return bad;}
