/* Minimal EGL subset over gl2_nano + the standard GL bridge. */
#include "gl2prefix.h"
#include "../gl2api.h"
#include "gl2prefix_undef.h"
#include "../gl2nano.h"
#include "gl2std.h"
#include "gl2_egl.h"
#include "EGL/egl.h"
static const struct nanoos_api *g_api;
static gl2_ctx c;static gl2_prog_obj progs[GL2_MAX_PROGRAMS];static gl2_nano b;static uint32_t segment[4096];
static uint32_t *const target=(uint32_t *)0x09f00000u;
static uint32_t width=64,height=64,opened,current,fault,window_surface,egl_error=EGL_SUCCESS;
/* Wait only for LCD ownership; rendering goes straight to the panel backbuffer.
 * No CPU colour conversion or full-screen copy is required. */
static int wait_lcd(void){
 if(!g_api||!g_api->lcd_ready||!g_api->lcd_back||!g_api->lcd_service||!g_api->lcd_present)return -1;
 uint32_t start=g_api->usec?g_api->usec():0;
 for(unsigned i=0;!g_api->lcd_ready();++i){
  g_api->lcd_service();
  if((g_api->usec&&g_api->usec()-start>=200000u*GL2_WAIT_SCALE)||i>=1000000u*GL2_WAIT_SCALE)return -1;
 }
 return 0;
}
/* Frame queuing: render window frames into three offscreen ARGB8888 buffers placed
 * after the texture arena, present the previous (retired) buffer by display DMA while
 * the next frame is being submitted. Falls back to the synchronous back-buffer path
 * when the kernel reports no queue capacity (or GL2_EGL_NO_QUEUE is defined). */
#define EQ_N 3u
#define EQ_STRIDE 0x80000u
#define EQ_BYTES (240u*432u*4u)
static uint32_t qi;static int queued;
/* GL2_EGL_FBO_SYNC: render FBO frames synchronously with the GPU idle (the pre-v28 behaviour). */
#ifdef GL2_EGL_FBO_SYNC
#define FBO_QUEUED 0
#else
#define FBO_QUEUED (queued&&window_surface)
#endif
static uint32_t *held;   /* window frame retired by a queued FBO end, not presented yet */
static uint32_t *qbuf(unsigned i){uintptr_t base=(0x08a00000u+GL2_TEXTURE_ARENA_BYTES+EQ_STRIDE-1u)&~(uintptr_t)(EQ_STRIDE-1u);return (uint32_t*)(base+(uintptr_t)i*EQ_STRIDE);}
static int queue_supported(void){
#ifdef GL2_EGL_NO_QUEUE
 return 0;
#else
 if(!g_api||NGAME_API_TABLE(g_api)<23||!g_api->mbx_queue_capacity||!g_api->mbx_render_async||!g_api->lcd_present_buffer||!g_api->lcd_buffer_busy||!g_api->lcd_service||!g_api->usec)return 0;
 if(!g_api->mbx_queue_capacity())return 0;
 return (uintptr_t)qbuf(EQ_N-1u)+EQ_BYTES<=0x09f00000u;
#endif
}
static int wait_source(uint32_t *buf){
 uint32_t start=g_api->usec();
 while(g_api->lcd_buffer_busy(buf,EQ_BYTES)){g_api->lcd_service();if(g_api->usec()-start>=200000u*GL2_WAIT_SCALE)return -1;}
 return 0;
}
static const uint32_t *last_presented;
const uint32_t *gl2_egl_last_frame(void){return last_presented?last_presented:(window_surface&&!queued&&g_api?g_api->lcd_back():0);}
static int present_q(uint32_t *buf){
 uint32_t start=g_api->usec();
 for(;;){
  int rc=gl2_nano_present_buffer(&b,buf);
  if(rc==GL2_NANO_OK){last_presented=buf;return 0;}
  if(rc!=GL2_NANO_BUSY)return rc;
  g_api->lcd_service();if(g_api->usec()-start>=200000u*GL2_WAIT_SCALE)return -1;
 }
}
static uint32_t stat_frames,stat_software,stat_fault;
static EGLDisplay display=(EGLDisplay)1;static EGLConfig config=(EGLConfig)1;static EGLContext context=(EGLContext)1;static EGLSurface surface=(EGLSurface)1;
/* The display push is a kernel state machine that only advances inside
 * lcd_service(). A queued frame hands the previous buffer to the display at
 * swap; without service calls during the next frame's submission the push
 * (about one 60 Hz period) is stepped only by the following swap, which then
 * blocks on it. Step it from draw calls instead, at most every 2 ms: each call
 * may poll up to ~200 us in the kernel, so this bounds the cost to a few
 * percent while the push overlaps rendering. */
#ifndef GL2_EGL_SERVICE_US
#define GL2_EGL_SERVICE_US 2000u   /* display push stepping interval during drawing (4000 starved the push: 413 ms stall, run533) */
#endif
static uint32_t svc_last;
static void service_display(void){
 uint32_t now=g_api->usec();
 if(now-svc_last<GL2_EGL_SERVICE_US)return;
 svc_last=now;g_api->lcd_service();
}
static uint32_t recover_last;
/* A fault used to be permanent: every later draw failed and the screen froze while the app
 * kept running. Recover at the next frame instead (at most every 100 ms). */
static int recover(void){
 if(!b.fault)return 0;
 uint32_t now=g_api->usec();if(now-recover_last<100000u)return -1;recover_last=now;
 if(gl2_nano_recover(&b))return -1;
 fault=0;qi=0;last_presented=0;held=0;return 0;
}
uint32_t gl2_egl_recoveries(void){return b.recoveries;}
static void ensure_frame(void){
 if(b.fault&&recover())return;
 if(b.frame_open){if(queued&&window_surface)service_display();return;}
 int rc;
 if(c.fbo_active){uint32_t ft=0,fw=0,fh=0;
  /* An FBO frame needs the GPU idle (gl2_nano_begin_fbo): retire a queued window frame first.
   * Without this, render-to-texture mid-frame (shadow maps, environment maps) after a queued
   * eglSwapBuffers failed begin_fbo, faulted, and stalled in the 100 ms recovery path. */
  rc=0;
  if(!gl2_fbo_target(&c,&ft,&fw,&fh)){
   /* The FBO scene is built while a queued window frame still renders (end_frame queues it
    * too). Without the queue the GPU must be idle first. */
   if(b.pending_valid&&!(FBO_QUEUED)){uint32_t *done=0;rc=gl2_nano_finish(&b,&done);if(!rc&&done&&queued&&window_surface)rc=present_q(done);}
   if(!rc){uint32_t t0=g_api->usec();rc=gl2_nano_begin_fbo(&b,target,fw,0);gl2_nano_prof[10]+=g_api->usec()-t0;++gl2_nano_prof[11];}
  }}
 else if(window_surface&&queued){
  uint32_t *buf=qbuf(qi);uint32_t start=g_api->usec();
  rc=wait_source(buf);
  while(!rc){rc=gl2_nano_begin_target(&b,buf,240,432,240,0);if(rc!=GL2_NANO_BUSY)break;g_api->lcd_service();if(g_api->usec()-start>=200000u*GL2_WAIT_SCALE){rc=-1;break;}}
 }
 else if(window_surface){rc=wait_lcd();if(!rc)rc=gl2_nano_begin(&b,g_api->lcd_back(),0);}
 else rc=gl2_nano_begin_target(&b,target,width,height,width,0);
 if(rc){fault=1;b.fault=1;}
}
/* End the open frame at a target change. An FBO frame is queued like a window frame when
 * the queue exists: its render overlaps the next frame's submission and its texture is
 * filled when the render is collected (gl2_nano_end_fbo_queued). */
static int end_frame(void){
 if(!c.fbo_active)return gl2_nano_end(&b);
 if(FBO_QUEUED){
  uint32_t *ret=0;int rc=gl2_nano_end_fbo_queued(&b,&ret);
  /* The retired window frame is presented at the next swap, as without an FBO pass: the
   * display is still pushing the frame before it, and presenting here waited ~10 ms for it. */
  if(rc!=GL2_NANO_UNSUPPORTED){if(!rc&&ret){if(held)rc=present_q(held);held=ret;}return rc;}
 }
 return gl2_nano_end_fbo(&b);
}
static void changed_target(void){
 if(!b.frame_open)return;
 if(end_frame()){fault=1;b.fault=1;}
}
static int finish(void){
 if(!b.frame_open)return 0;
 return end_frame();
}
void gl2_egl_set_platform(const struct nanoos_api *api){g_api=api;}
/* Landscape (see gl2_egl.h): applies to window surfaces created afterwards. */
static uint32_t orientation;
int gl2_egl_set_orientation(uint32_t mode){if(mode>2u||(window_surface&&current))return -1;orientation=mode;return 0;}
uint32_t gl2_egl_orientation(void){return window_surface?orientation:0u;}
void gl2_egl_stats(uint32_t *f,uint32_t *s,uint32_t *x){if(f)*f=opened?b.frames:stat_frames;if(s)*s=opened?b.software_frames:stat_software;if(x)*x=opened?fault:stat_fault;}
static int open_context(void){
 if(opened)return 0;
 if(!g_api)return -1;
 int rc=gl2_nano_open(&b,g_api,&c,progs,segment,4096);if(rc)return rc;
 rc=gl2_nano_textures(&b,(void*)0x08a00000,GL2_TEXTURE_ARENA_BYTES);if(rc){gl2_nano_close(&b);return rc;}
 gl2std_set_context(&c);gl2std_set_frame_hooks(ensure_frame,changed_target);opened=1;return 0;
}
EGLDisplay eglGetDisplay(EGLNativeDisplayType d){(void)d;return display;}
EGLBoolean eglInitialize(EGLDisplay d,EGLint *major,EGLint *minor){
 if(d!=display)return EGL_FALSE;
 if(!g_api)return EGL_FALSE;
 if(major)*major=1;
 if(minor)*minor=4;
 return EGL_TRUE;
}
EGLBoolean eglTerminate(EGLDisplay d){
 if(d!=display)return EGL_FALSE;
 if(opened){finish();if(queued){uint32_t *last=0;if(!gl2_nano_finish(&b,&last)&&last)present_q(last);if(b.retired_target)present_q(b.retired_target);}stat_frames=b.frames;stat_software=b.software_frames;stat_fault=fault;gl2_nano_close(&b);opened=0;}
 gl2std_set_context(0);current=0;return EGL_TRUE;
}
EGLBoolean eglChooseConfig(EGLDisplay d,const EGLint *a,EGLConfig *configs,EGLint size,EGLint *n){
 (void)a;if(d!=display){egl_error=EGL_BAD_DISPLAY;return EGL_FALSE;}if(configs&&size>0)configs[0]=config;if(n)*n=1;return EGL_TRUE;
}
EGLBoolean eglGetConfigAttrib(EGLDisplay d,EGLConfig cfg,EGLint a,EGLint *v){
 if(d!=display||cfg!=config){egl_error=EGL_BAD_CONFIG;return EGL_FALSE;}
 switch(a){case EGL_RED_SIZE:case EGL_GREEN_SIZE:case EGL_BLUE_SIZE:case EGL_ALPHA_SIZE:*v=8;break;case EGL_DEPTH_SIZE:*v=16;break;case EGL_STENCIL_SIZE:*v=0;break;case EGL_SURFACE_TYPE:*v=EGL_PBUFFER_BIT|EGL_WINDOW_BIT;break;case EGL_RENDERABLE_TYPE:*v=EGL_OPENGL_ES2_BIT;break;default:egl_error=EGL_BAD_ATTRIBUTE;return EGL_FALSE;}return EGL_TRUE;
}
EGLContext eglCreateContext(EGLDisplay d,EGLConfig cfg,EGLContext share,const EGLint *a){
 (void)share;(void)a;if(d!=display||cfg!=config){egl_error=EGL_BAD_CONFIG;return EGL_NO_CONTEXT;}return context;
}
EGLSurface eglCreateWindowSurface(EGLDisplay d,EGLConfig cfg,EGLNativeWindowType win,const EGLint *a){
 if(d!=display||cfg!=config){egl_error=EGL_BAD_CONFIG;return EGL_NO_SURFACE;}
 if(win){egl_error=EGL_BAD_NATIVE_WINDOW;return EGL_NO_SURFACE;}
 if(a&&*a!=EGL_NONE){egl_error=EGL_BAD_ATTRIBUTE;return EGL_NO_SURFACE;}
 if(current||(opened&&b.frame_open)){egl_error=EGL_BAD_ACCESS;return EGL_NO_SURFACE;}
 if(wait_lcd()){egl_error=EGL_NOT_INITIALIZED;return EGL_NO_SURFACE;}
 width=orientation?432u:240u;height=orientation?240u:432u;window_surface=1;queued=queue_supported();qi=0;return surface;
}
EGLSurface eglCreatePbufferSurface(EGLDisplay d,EGLConfig cfg,const EGLint *a){
 if(d!=display||cfg!=config){egl_error=EGL_BAD_CONFIG;return EGL_NO_SURFACE;}
 if(current||(opened&&b.frame_open)){egl_error=EGL_BAD_ACCESS;return EGL_NO_SURFACE;}
 uint32_t w=64,h=64;
 for(const EGLint *p=a;p&&*p!=EGL_NONE;p+=2){if(p[0]==EGL_WIDTH)w=(uint32_t)p[1];else if(p[0]==EGL_HEIGHT)h=(uint32_t)p[1];else{egl_error=EGL_BAD_ATTRIBUTE;return EGL_NO_SURFACE;}}
 if(w<16||h<16||w>64||h>64||(w&15)||(h&15)){egl_error=EGL_BAD_ATTRIBUTE;return EGL_NO_SURFACE;}
 width=w;height=h;window_surface=0;queued=0;return surface;
}
EGLBoolean eglDestroySurface(EGLDisplay d,EGLSurface s){if(d!=display||s!=surface)return EGL_FALSE;return EGL_TRUE;}
EGLBoolean eglDestroyContext(EGLDisplay d,EGLContext x){if(d!=display||x!=context)return EGL_FALSE;return EGL_TRUE;}
EGLBoolean eglMakeCurrent(EGLDisplay d,EGLSurface draw,EGLSurface read,EGLContext x){
 if(d!=display){egl_error=EGL_BAD_DISPLAY;return EGL_FALSE;}
 if(x==EGL_NO_CONTEXT){current=0;gl2std_set_context(0);return EGL_TRUE;}
 if(draw!=surface||read!=surface||x!=context){egl_error=EGL_BAD_MATCH;return EGL_FALSE;}
 if(open_context()){egl_error=EGL_NOT_INITIALIZED;return EGL_FALSE;}
 {uint32_t o=window_surface?orientation:0u;if(c.landscape!=o)gl2_set_landscape(&c,o);}   /* the viewport starts at the surface size */
 gl2std_set_context(&c);gl2std_set_frame_hooks(ensure_frame,changed_target);current=1;return EGL_TRUE;
}
EGLBoolean eglSwapBuffers(EGLDisplay d,EGLSurface s){
 if(d!=display||s!=surface||!current){egl_error=EGL_BAD_SURFACE;return EGL_FALSE;}
 if(c.fbo_active){egl_error=EGL_BAD_MATCH;return EGL_FALSE;}
 if(queued&&window_surface){
  uint32_t *ret=0;int rc=0;
  if(b.frame_open){
   rc=gl2_nano_end_queued(&b,&ret);
   if(rc==GL2_NANO_UNSUPPORTED||rc==GL2_NANO_TARGET){ /* e.g. software frame: end synchronously */
    rc=gl2_nano_end(&b);ret=b.retired_target;
    if(!rc){if(ret)rc=present_q(ret);if(!rc)rc=present_q(b.target);ret=0;qi=(qi+1u)%EQ_N;}
   }else if(!rc)qi=(qi+1u)%EQ_N;
  }else if(b.completed){
   if(b.retired_target)rc=present_q(b.retired_target);
   if(!rc)rc=present_q(b.target);
   qi=(qi+1u)%EQ_N;
  }
  if(!rc&&held){uint32_t t0=g_api->usec();rc=present_q(held);gl2_nano_prof[8]+=g_api->usec()-t0;++gl2_nano_prof[9];}
  held=0;
  if(!rc&&ret)rc=present_q(ret);
  if(rc){fault=1;egl_error=EGL_BAD_ACCESS;return EGL_FALSE;}
  return EGL_TRUE;
 }
 int had_frame=b.frame_open||b.completed;
 if(finish()||(window_surface&&had_frame&&gl2_nano_present(&b))){fault=1;egl_error=EGL_BAD_ACCESS;return EGL_FALSE;}
 return EGL_TRUE;
}
EGLint eglGetError(void){EGLint e=egl_error;egl_error=EGL_SUCCESS;return e;}
const char *eglQueryString(EGLDisplay d,EGLint n){
 if(d!=display)return 0;
 if(n==EGL_VENDOR)return "Nano7 GLES2";
 if(n==EGL_VERSION)return "1.4";
 if(n==EGL_EXTENSIONS)return "";
 return 0;
}
EGLBoolean eglQuerySurface(EGLDisplay d,EGLSurface s,EGLint a,EGLint *v){
 if(d!=display||s!=surface)return EGL_FALSE;
 if(a==EGL_WIDTH)*v=(EGLint)width;
 else if(a==EGL_HEIGHT)*v=(EGLint)height;
 else{if(v)*v=0;}
 return EGL_TRUE;
}
