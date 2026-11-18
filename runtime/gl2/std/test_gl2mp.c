/* Multi-pass fragment shaders: each case is drawn as several blended passes and compared per pixel
 * with the same expression evaluated in C from the textures as the rasteriser samples them. */
#include "GLES2/gl2.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
int gl2mp_host_init(int mode);uint32_t *gl2mp_host_target(void);int gl2mp_passes(unsigned program);int gl2mp_passes_add(unsigned program);unsigned gl2mp_err_site(void);
static const char *VS="attribute vec4 aPosition;attribute vec2 aTexCoord;varying vec2 vUV;void main(){gl_Position=aPosition;vUV=aTexCoord;}";
static GLuint sh(GLenum t,const char *s){GLuint o=glCreateShader(t);glShaderSource(o,1,&s,0);glCompileShader(o);GLint ok=0;glGetShaderiv(o,GL_COMPILE_STATUS,&ok);if(!ok){char log[128];GLsizei n=0;glGetShaderInfoLog(o,sizeof log,&n,log);printf("compile failed: %s\n  %s\n",log,s);}return ok?o:0;}
/* a normal that swings from left to right across the quad: per-vertex lighting is flat along x, per-pixel is not */
static const char *VSN="attribute vec4 aPosition;attribute vec2 aTexCoord;attribute vec3 aNormal;varying vec2 vUV;varying vec3 vN;void main(){gl_Position=aPosition;vUV=aTexCoord;vN=aNormal;}";
static const char *cur_vs;static float swing=0.35f;   /* the normal turns about 41 degrees across the quad */
static GLuint prog(const char *fs){GLuint v=sh(GL_VERTEX_SHADER,cur_vs?cur_vs:VS),f=sh(GL_FRAGMENT_SHADER,fs);if(!v||!f)return 0;GLuint p=glCreateProgram();glAttachShader(p,v);glAttachShader(p,f);glLinkProgram(p);GLint ok=0;glGetProgramiv(p,GL_LINK_STATUS,&ok);if(!ok){char log[128];GLsizei n=0;glGetProgramInfoLog(p,sizeof log,&n,log);printf("link failed: %s\n  %s\n",log,fs);return 0;}glDeleteShader(v);glDeleteShader(f);return p;}
static float px[3][256][4];   /* 0: texture A, 1: texture B, 2: result */
static void grab(int slot){uint32_t *t=gl2mp_host_target();for(int i=0;i<256;++i){px[slot][i][0]=(t[i]>>16&255)/255.f;px[slot][i][1]=(t[i]>>8&255)/255.f;px[slot][i][2]=(t[i]&255)/255.f;px[slot][i][3]=(t[i]>>24)/255.f;}}
static void draw(GLuint p){
 static const GLfloat pos[16]={-1,-1,0,1, 1,-1,0,1, -1,1,0,1, 1,1,0,1},uv[8]={0,0, 1,0, 0,1, 1,1};
 glUseProgram(p);
 GLint a=glGetAttribLocation(p,"aPosition"),t=glGetAttribLocation(p,"aTexCoord");
 glVertexAttribPointer((GLuint)a,4,GL_FLOAT,GL_FALSE,0,pos);glEnableVertexAttribArray((GLuint)a);
 if(t>=0){glVertexAttribPointer((GLuint)t,2,GL_FLOAT,GL_FALSE,0,uv);glEnableVertexAttribArray((GLuint)t);}
 static GLfloat nrm[12];for(int q=0;q<4;++q){nrm[3*q]=(q&1)?swing:-swing;nrm[3*q+1]=0;nrm[3*q+2]=0.94f;}
 GLint nl=glGetAttribLocation(p,"aNormal");
 if(nl>=0){glVertexAttribPointer((GLuint)nl,3,GL_FLOAT,GL_FALSE,0,nrm);glEnableVertexAttribArray((GLuint)nl);}
 GLint l;
 if((l=glGetUniformLocation(p,"sA"))>=0)glUniform1i(l,0);
 if((l=glGetUniformLocation(p,"sB"))>=0)glUniform1i(l,1);
 if((l=glGetUniformLocation(p,"uTint"))>=0)glUniform4f(l,0.5f,0.75f,0.25f,1);
 if((l=glGetUniformLocation(p,"uAmb"))>=0)glUniform4f(l,0.125f,0.25f,0.0625f,1);
 if((l=glGetUniformLocation(p,"uMix"))>=0)glUniform1f(l,0.25f);
 if((l=glGetUniformLocation(p,"uAlpha"))>=0)glUniform1f(l,0.5f);
 if((l=glGetUniformLocation(p,"uL"))>=0)glUniform3f(l,0.6f,0,0.8f);
 if((l=glGetUniformLocation(p,"uLd"))>=0)glUniform3f(l,0.6f,0.8f,0);
 if((l=glGetUniformLocation(p,"uL2"))>=0)glUniform3f(l,1,1,0);
 if((l=glGetUniformLocation(p,"uN"))>=0)glUniform3f(l,0,0,2.f);
 glDrawArrays(GL_TRIANGLE_STRIP,0,4);
 {unsigned site=gl2mp_err_site();GLenum e=glGetError();if(e!=GL_NO_ERROR){printf("GL error 0x%x at gl2api.c line %u\n",e,site);assert(0);}}
}
typedef void (*ref_fn)(int i,float *out);
static float tol=3.5f;
static float sat(float v){return v<0?0:v>1?1:v;}
static const float TINT[3]={0.5f,0.75f,0.25f},AMB[3]={0.125f,0.25f,0.0625f};
static void r_mix(int i,float *o){for(int k=0;k<3;++k)o[k]=px[0][i][k]*0.75f+px[1][i][k]*0.25f;o[3]=1;}
static void r_prod(int i,float *o){for(int k=0;k<3;++k)o[k]=sat(px[0][i][k]*px[1][i][k]*TINT[k]+AMB[k]*px[0][i][k]);o[3]=1;}
static void r_blur(int i,float *o){int x=i%16;if(x<1||x>14){o[0]=-1;return;}for(int k=0;k<3;++k)o[k]=0.5f*(px[0][i-1][k]+px[0][i+1][k]);o[3]=1;}
static void r_sub(int i,float *o){for(int k=0;k<3;++k)o[k]=sat(1-px[0][i][k]*TINT[k]);o[3]=1;}
static void r_lit(int i,float *o){for(int k=0;k<3;++k)o[k]=sat(px[0][i][k]*0.8f+AMB[k]);o[3]=1;}
static void r_bright(int i,float *o){for(int k=0;k<3;++k)o[k]=sat(px[0][i][k]*TINT[k]*2+px[1][i][k]*px[1][i][3]);o[3]=1;}
static float ndl(int i){float t=((float)(i%16)+0.5f)/16.f,nx=-swing+2*swing*t,nz=0.94f;float d=(0.6f*nx+0.8f*nz)/sqrtf(nx*nx+nz*nz);return d<0?0:d;}
static void r_ppl(int i,float *o){for(int k=0;k<3;++k)o[k]=TINT[k]*ndl(i);o[3]=1;}
static void r_ppltex(int i,float *o){for(int k=0;k<3;++k)o[k]=px[0][i][k]*ndl(i);o[3]=1;}
static void r_spec(int i,float *o){float s=powf(ndl(i),8.f);for(int k=0;k<3;++k)o[k]=sat(px[0][i][k]*0.5f+s);o[3]=1;}
/* normal map = texture A, diffuse = texture B, light colour = TINT, ambient = AMB */
static float nd(int i,const float *l){float d=0;for(int k=0;k<3;++k)d+=(2*px[0][i][k]-1)*l[k];return sat(d);}
static const float LD[3]={0.6f,0.8f,0},L2[3]={0.70710678f,0.70710678f,0};
static void r_nm(int i,float *o){float d=nd(i,LD);for(int k=0;k<3;++k)o[k]=sat(px[1][i][k]*d*TINT[k]);o[3]=1;}
static void r_nmamb(int i,float *o){float d=nd(i,LD);for(int k=0;k<3;++k)o[k]=sat(px[1][i][k]*(AMB[k]+d*TINT[k]));o[3]=1;}
static void r_nmnorm(int i,float *o){float d=nd(i,L2);for(int k=0;k<3;++k)o[k]=sat(px[1][i][k]*d);o[3]=1;}
static void r_nmacc(int i,float *o){static const float bg[3]={0.2f,0.1f,0.05f};float d=nd(i,LD);for(int k=0;k<3;++k)o[k]=sat(bg[k]+px[1][i][k]*d*TINT[k]);o[3]=-1;}
static float pw(float x,int k){float r=1;while(k--)r*=x;return r;}
static void r_sp8(int i,float *o){float s=pw(nd(i,LD),8);for(int k=0;k<3;++k)o[k]=sat(px[1][i][k]*s);o[3]=1;}
static void r_sp6(int i,float *o){float s=pw(nd(i,LD),6);for(int k=0;k<3;++k)o[k]=sat(s*TINT[k]);o[3]=1;}
static void r_d3(int i,float *o){static const float bg[3]={0.2f,0.1f,0.05f};float d=nd(i,LD),s=pw(nd(i,L2),8);for(int k=0;k<3;++k)o[k]=sat(bg[k]+px[1][i][k]*d*TINT[k]+s*AMB[k]*4);o[3]=-1;}
static void r_d3amb(int i,float *o){static const float bg[3]={0.2f,0.1f,0.05f};float d=nd(i,LD);for(int k=0;k<3;++k)o[k]=sat(bg[k]+px[1][i][k]*(AMB[k]+d*TINT[k]));o[3]=-1;}
static void r_blend(int i,float *o){for(int k=0;k<3;++k)o[k]=0.5f*(px[0][i][k]*0.75f+px[1][i][k]*0.25f)+0.5f*(k==0?0.2f:k==1?0.4f:0.6f);o[3]=-1;}
static int check(const char *name,GLuint p,ref_fn ref,int min_passes,int blend){
 if(!p){printf("FAIL %s: no program\n",name);return 1;}
 int n=gl2mp_passes(p);
 if(blend==2){glClearColor(0.2f,0.1f,0.05f,1);glEnable(GL_BLEND);glBlendFunc(GL_ONE,GL_ONE);}
 else if(blend){glClearColor(0.2f,0.4f,0.6f,1);glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);}else glClearColor(0,0,0,1);
 glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);draw(p);glDisable(GL_BLEND);grab(2);
 float worst=0;
 for(int i=0;i<256;++i){float e[4];ref(i,e);if(e[0]<0)continue;for(int k=0;k<(e[3]<0?3:4);++k){float d=fabsf(e[k]-px[2][i][k]);if(d>worst)worst=d;}}
 int bad=worst>tol/255||n<min_passes;
 printf("%s %-28s passes %d  worst error %.1f/255\n",bad?"FAIL":"ok  ",name,n,worst*255);
 return bad;
}
int main(void){
 assert(gl2mp_host_init(2)==0);
 glViewport(0,0,16,16);glEnable(GL_DEPTH_TEST);glDepthFunc(GL_LESS);
 uint8_t a[16*16*4],b[16*16*4];
 for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x){unsigned i=(y*16+x)*4;a[i]=(uint8_t)(x*17);a[i+1]=(uint8_t)(y*17);a[i+2]=0x40;a[i+3]=255;b[i]=(uint8_t)(255-x*17);b[i+1]=0x80;b[i+2]=(uint8_t)(y*17);b[i+3]=(uint8_t)(x*17);}
 GLuint tex[2];glGenTextures(2,tex);
 for(int u=0;u<2;++u){glActiveTexture(GL_TEXTURE0+(GLenum)u);glBindTexture(GL_TEXTURE_2D,tex[u]);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,16,16,0,GL_RGBA,GL_UNSIGNED_BYTE,u?b:a);}
 /* the textures as the rasteriser samples them */
 GLuint pa=prog("precision mediump float;uniform sampler2D sA;varying vec2 vUV;void main(){gl_FragColor=texture2D(sA,vUV);}");
 GLuint pb=prog("precision mediump float;uniform sampler2D sB;varying vec2 vUV;void main(){gl_FragColor=texture2D(sB,vUV);}");
 assert(pa&&pb);
 glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);draw(pa);grab(0);
 glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);draw(pb);grab(1);
 glDeleteProgram(pa);glDeleteProgram(pb);glUseProgram(0);
 assert(px[0][5][0]!=px[0][6][0]&&px[1][5][0]!=px[0][5][0]);
 int bad=0;
#define H "precision mediump float;uniform sampler2D sA;uniform sampler2D sB;uniform vec4 uTint;uniform vec4 uAmb;uniform float uMix;uniform float uAlpha;uniform vec3 uL;uniform vec3 uN;varying vec2 vUV;"
 static const struct{const char *name,*fs;ref_fn ref;int passes,blend;}cases[]={
  {"crossfade two textures",H"void main(){gl_FragColor=vec4(mix(texture2D(sA,vUV).rgb,texture2D(sB,vUV).rgb,uMix),1.0);}",r_mix,2,0},
  {"texture x texture + ambient",H"void main(){vec3 t=texture2D(sA,vUV).rgb;gl_FragColor=vec4(t*texture2D(sB,vUV).rgb*uTint.rgb+uAmb.rgb*t,1.0);}",r_prod,3,0},
  {"two offset taps",H"void main(){vec2 d=vec2(0.0625,0.0);gl_FragColor=vec4((texture2D(sA,vUV+d).rgb+texture2D(sA,vUV-d).rgb)*0.5,1.0);}",r_blur,2,0},
  {"one minus product",H"void main(){gl_FragColor=vec4(1.0-texture2D(sA,vUV).rgb*uTint.rgb,1.0);}",r_sub,4,0},
  {"normalize/dot/max lighting",H"void main(){float l=max(dot(normalize(uN),uL),0.0);gl_FragColor=vec4(texture2D(sA,vUV).rgb*l+uAmb.rgb,1.0);}",r_lit,2,0},
  {"overbright + alpha-weighted",H"void main(){vec4 q=texture2D(sB,vUV);gl_FragColor=vec4(texture2D(sA,vUV).rgb*uTint.rgb*2.0+q.rgb*q.a,1.0);}",r_bright,2,0},
  {"crossfade, alpha blended",H"void main(){gl_FragColor=vec4(mix(texture2D(sA,vUV).rgb,texture2D(sB,vUV).rgb,uMix),uAlpha);}",r_blend,2,1},
 };
 for(unsigned i=0;i<sizeof cases/sizeof *cases;++i){GLuint p=prog(cases[i].fs);bad+=check(cases[i].name,p,cases[i].ref,cases[i].passes,cases[i].blend);if(p){glUseProgram(0);glDeleteProgram(p);}}
 /* per-pixel functions of a normal, through a lookup table the driver bakes (tolerance: a 32x32 table) */
 cur_vs=VSN;tol=6.5f;
#define HN "precision mediump float;uniform sampler2D sA;uniform vec4 uTint;uniform vec3 uL;varying vec2 vUV;varying vec3 vN;"
 static const struct{const char *name,*fs;ref_fn ref;int passes;}lit[]={
  {"per-pixel diffuse",HN"void main(){float d=max(dot(normalize(vN),uL),0.0);gl_FragColor=vec4(uTint.rgb*d,1.0);}",r_ppl,1},
  {"per-pixel diffuse x texture",HN"void main(){float d=max(dot(normalize(vN),uL),0.0);gl_FragColor=vec4(texture2D(sA,vUV).rgb*d,1.0);}",r_ppltex,2},
  {"texture + per-pixel specular",HN"void main(){float s=pow(max(dot(normalize(vN),uL),0.0),8.0);gl_FragColor=vec4(texture2D(sA,vUV).rgb*0.5+vec3(s),1.0);}",r_spec,2},
 };
 for(unsigned i=0;i<sizeof lit/sizeof *lit;++i){GLuint p=prog(lit[i].fs);bad+=check(lit[i].name,p,lit[i].ref,lit[i].passes,0);if(p){glUseProgram(0);glDeleteProgram(p);}}
 /* how the error grows with the turn of the normal across one triangle (information, not a gate) */
 for(int q=0;q<3;++q){static const float sw[3]={0.17f,0.6f,1.2f};swing=sw[q];tol=255;GLuint p=prog(lit[0].fs);char nm[48];snprintf(nm,sizeof nm,"  diffuse, normal turn %d deg",(int)(2*atanf(swing/0.94f)*57.3f));check(nm,p,lit[0].ref,1,0);if(p){glUseProgram(0);glDeleteProgram(p);}}
 swing=0.35f;
 /* normal map (texture A) x diffuse (texture B): the combiner's DOT3 in its own pass */
 cur_vs=VS;tol=4.5f;
#define HM "precision mediump float;uniform sampler2D sA;uniform sampler2D sB;uniform vec4 uTint;uniform vec4 uAmb;uniform vec3 uLd;uniform vec3 uL2;varying vec2 vUV;"
 static const struct{const char *name,*fs;ref_fn ref;int passes,blend,acc;}nm[]={
  {"normal map x diffuse x light",HM"void main(){vec3 n=texture2D(sA,vUV).rgb*2.0-1.0;float d=clamp(dot(n,uLd),0.0,1.0);gl_FragColor=vec4(texture2D(sB,vUV).rgb*d*uTint.rgb,1.0);}",r_nm,2,0,0},
  {"  ... plus ambient",HM"void main(){vec3 n=texture2D(sA,vUV).rgb*2.0-1.0;float d=max(dot(n,uLd),0.0);vec3 b=texture2D(sB,vUV).rgb;gl_FragColor=vec4(b*(uAmb.rgb+d*uTint.rgb),1.0);}",r_nmamb,3,0,0},
  {"  ... normalize(light)",HM"void main(){vec3 n=texture2D(sA,vUV).rgb*2.0-1.0;float d=clamp(dot(n,normalize(uL2)),0.0,1.0);gl_FragColor=vec4(texture2D(sB,vUV).rgb*d,1.0);}",r_nmnorm,2,0,0},
  {"specular pow(d, 8) x diffuse",HM"void main(){vec3 n=texture2D(sA,vUV).rgb*2.0-1.0;float s=pow(clamp(dot(n,uLd),0.0,1.0),8.0);gl_FragColor=vec4(texture2D(sB,vUV).rgb*s,1.0);}",r_sp8,5,0,0},
  {"specular pow(d, 6) x colour",HM"void main(){vec3 n=texture2D(sA,vUV).rgb*2.0-1.0;float s=pow(max(dot(n,uLd),0.0),6.0);gl_FragColor=vec4(uTint.rgb*s,1.0);}",r_sp6,5,0,0},
  {"diffuse + specular, additive",HM"void main(){vec3 n=texture2D(sA,vUV).rgb*2.0-1.0;float d=clamp(dot(n,uLd),0.0,1.0);float s=pow(clamp(dot(n,normalize(uL2)),0.0,1.0),8.0);\n gl_FragColor=vec4(texture2D(sB,vUV).rgb*d*uTint.rgb+uAmb.rgb*4.0*s,1.0);}",r_d3,0,2,10},
  {"ambient + diffuse, additive",HM"void main(){vec3 n=texture2D(sA,vUV).rgb*2.0-1.0;float d=clamp(dot(n,uLd),0.0,1.0);vec3 b=texture2D(sB,vUV).rgb;gl_FragColor=vec4(b*(uAmb.rgb+d*uTint.rgb),1.0);}",r_d3amb,3,2,5},
  {"  ... additive (ONE/ONE)",HM"void main(){vec3 n=texture2D(sA,vUV).rgb*2.0-1.0;float d=clamp(dot(n,uLd),0.0,1.0);gl_FragColor=vec4(texture2D(sB,vUV).rgb*d*uTint.rgb,1.0);}",r_nmacc,2,2,4},
 };
 for(unsigned i=0;i<sizeof nm/sizeof *nm;++i){GLuint p=prog(nm[i].fs);bad+=check(nm[i].name,p,nm[i].ref,nm[i].passes,nm[i].blend);
  if(p&&nm[i].acc&&gl2mp_passes_add(p)!=nm[i].acc){printf("FAIL   additive passes %d, want %d\n",gl2mp_passes_add(p),nm[i].acc);++bad;}
  if(p){glUseProgram(0);glDeleteProgram(p);}}
 if(bad){printf("FAIL multi-pass fragment shaders: %d case(s)\n",bad);return 1;}
 puts("PASS multi-pass fragment shaders");return 0;
}
