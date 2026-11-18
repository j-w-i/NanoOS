/* Minimal independent GLES2 + EGL example built purely through NanoOS SDK. */
#include "nano_app.h"
#include "EGL/egl.h"
#include "GLES2/gl2.h"
#if defined(HELLO_VIEWPORT_CLIP) || defined(HELLO_FOGPROBE)
#define HELLO_BENCH 1
#endif
#ifdef HELLO_FOGPROBE
#include "gl2rt.h"
#endif
#ifdef HELLO_BENCH
#include "report.h"
static char report[4096];
static unsigned rused;
static void rtext(const char *s) { while (*s && rused + 1 < sizeof report) report[rused++] = *s++; }
static void rword(uint32_t x) {
    if (rused + 10 >= sizeof report) return;
    static const char h[] = "0123456789abcdef";
    for (unsigned i = 0; i < 8; ++i) report[rused++] = h[(x >> (28 - i * 4)) & 15u];
    report[rused++] = ' ';
}
#endif

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, 0);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    return ok ? sh : 0;
}

int nano_example_main(const struct nanoos_api *api) {
    if (nano_app_init(api) != 0) return 1;

    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(dpy, 0, 0)) return 2;

    static const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };
    EGLConfig cfg = 0;
    EGLint ncfg = 0;
    if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &ncfg) || ncfg < 1) return 3;

    EGLSurface sfc = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)0, 0);
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, 0);
    if (sfc == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT) return 4;
    if (!eglMakeCurrent(dpy, sfc, sfc, ctx)) return 5;


#ifdef HELLO_FOGPROBE
    {
        /* MBX vertex fog probe: ISP bit 24 on, fog factor written to VGP output 2 (varying vFog),
         * red quad over blue. Per case: fog factor, extra scene register (+0x640 candidate for the
         * fog colour), centre pixel read back. */
        rtext("NANOOS GL2 GAME build " __DATE__ " " __TIME__ "\nFOGPROBE api ");
        rword(NGAME_API_TABLE(api)); rtext("\n");
        if (NGAME_API_TABLE(api) < 26) { rtext("needs v26\nHELLO FAIL\n"); goto fog_out; }
        gl2_live_or = 4u;          /* keep output 2 (fog) at link */

        static const char *pfs = "precision mediump float;\nvarying vec4 vColor;\nvoid main(){ gl_FragColor = vColor; }\n";
        static const float q[24] = { -1,-1,0,1, 1,-1,0,1, -1,1,0,1,  1,-1,0,1, 1,1,0,1, -1,1,0,1 };
        static const float qc[24] = { 1,0,0,1, 1,0,0,1, 1,0,0,1, 1,0,0,1, 1,0,0,1, 1,0,0,1 };
        /* Fog colour register sweep: full fog (output 2 lane x = 0, ISP bits 24:23, control 0x8) is
         * black; each candidate MBX core register gets 0xff00ff00 for two frames, then its old value
         * back. Any non-black centre pixel, or a GPU fault, is logged. The report is re-saved every 32
         * registers so a hard hang still shows how far the sweep got. */
        static const char *ovs = "attribute vec4 aPos;\nattribute vec4 aCol;\nuniform vec4 uF;\nvarying vec4 vColor;\nvarying vec4 vFog;\nvoid main(){ vColor = aCol; vFog = uF; gl_Position = aPos; }\n";
        gl2_isp_or = 3u << 23; gl2_ctl_or = 0x8u;
        GLuint pv = compile_shader(GL_VERTEX_SHADER, ovs), pf = compile_shader(GL_FRAGMENT_SHADER, pfs);
        GLuint pp = glCreateProgram(); glAttachShader(pp, pv); glAttachShader(pp, pf); glLinkProgram(pp);
        glUseProgram(pp); GLint lp = glGetAttribLocation(pp, "aPos"), lc = glGetAttribLocation(pp, "aCol"), lf = glGetUniformLocation(pp, "uF");
        for (unsigned a = 0; a < 5; ++a) glDisableVertexAttribArray(a);
        glVertexAttribPointer((GLuint)lc, 4, GL_FLOAT, GL_FALSE, 0, qc); glEnableVertexAttribArray((GLuint)lc);
        glVertexAttribPointer((GLuint)lp, 4, GL_FLOAT, GL_FALSE, 0, q); glEnableVertexAttribArray((GLuint)lp);
        static const float full[4] = { 0, 0, 0, 0 };
        glUniform4fv(lf, 1, full);
        uint32_t tested = 0, hits = 0;
        static const uint32_t offs[] = { 0x644, 0x64c, 0x674 };
        static const uint32_t vals[] = { 0xff00ff00u, 0x0000ff00u, 0x00ff00ffu, 0x3f800000u, 0xffffffffu };
        for (unsigned o = 0; o < 3; ++o) for (unsigned v = 0; v < 5; ++v) {
            int rc = api->mbx_set_scene_reg(0, offs[o], vals[v]);
            uint8_t b[4] = {0,0,0,0}; uint32_t f0 = 0, f1 = 0, sw = 0, fr = 0; nano_app_gfx_stats(&fr, &sw, &f0);
            for (unsigned f = 0; f < 2; ++f) {
                glClearColor(0.0f, 0.0f, 1.0f, 1.0f); glClear(GL_COLOR_BUFFER_BIT);
                glDrawArrays(GL_TRIANGLES, 0, 6);
                if (f == 1u) glReadPixels(120, 216, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, b);
                eglSwapBuffers(dpy, sfc);
            }
            api->mbx_set_scene_reg(0, 0, 0);
            nano_app_gfx_stats(&fr, &sw, &f1); ++tested;
            uint32_t px = (uint32_t)b[0] << 16 | (uint32_t)b[1] << 8 | b[2];
            if (px || f1 != f0) ++hits;
            rtext("R "); rword(offs[o]); rword(vals[v]); rtext("rc "); rword((uint32_t)rc); rtext("px "); rword(px); rtext("fault "); rword(f1 - f0); rtext("\n");
        }
        rtext("SWEEP tested "); rword(tested); rtext("hits "); rword(hits); rtext("\n");
        gl2_isp_or = 0; gl2_ctl_or = 0;
        api->mbx_set_scene_reg(0, 0, 0);
        uint32_t fr = 0, sw = 0, fault = 0; nano_app_gfx_stats(&fr, &sw, &fault);
        rtext("FOGPROBE frames/sw/fault "); rword(fr); rword(sw); rword(fault); rtext("\n");
        rtext(fault == 0u ? "HELLO PASS\n" : "HELLO FAIL\n");
    fog_out:
        report[rused] = 0;
        rpt_mirror_write(report, rused, rpt_sum(report, rused));
        nano_app_dcache_clean();
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        return 0;
    }
#endif
#ifdef HELLO_VIEWPORT_CLIP
    {
        /* Viewport / scissor clipping check (GL clips draws to the viewport and scissor; the SDK
         * uses the VGP clip planes 5.y / 5.z). A quad larger than the viewport is drawn; three
         * pixels per case are read back: R = drawn, B = clear colour. */
        static const char *pvs = "attribute vec4 aPos;\nattribute vec4 aCol;\nvarying vec4 vColor;\nvoid main(){ vColor = aCol; gl_Position = aPos; }\n";
        static const char *pfs = "precision mediump float;\nvarying vec4 vColor;\nvoid main(){ gl_FragColor = vColor; }\n";
        GLuint pv = compile_shader(GL_VERTEX_SHADER, pvs), pf = compile_shader(GL_FRAGMENT_SHADER, pfs);
        GLuint pp = glCreateProgram(); glAttachShader(pp, pv); glAttachShader(pp, pf); glLinkProgram(pp);
        GLint plinked = 0; glGetProgramiv(pp, GL_LINK_STATUS, &plinked);
        static const float q[24] = { -1.6f,-1.6f,0,1, 1.6f,-1.6f,0,1, -1.6f,1.6f,0,1,  1.6f,-1.6f,0,1, 1.6f,1.6f,0,1, -1.6f,1.6f,0,1 };
        static const float qc[24] = { 1,0,0,1, 1,0,0,1, 1,0,0,1, 1,0,0,1, 1,0,0,1, 1,0,0,1 };
        glUseProgram(pp); GLint lp = glGetAttribLocation(pp, "aPos"), lc = glGetAttribLocation(pp, "aCol");
        for (unsigned a = 0; a < 5; ++a) glDisableVertexAttribArray(a);
        glVertexAttribPointer((GLuint)lc, 4, GL_FLOAT, GL_FALSE, 0, qc); glEnableVertexAttribArray((GLuint)lc);
        glVertexAttribPointer((GLuint)lp, 4, GL_FLOAT, GL_FALSE, 0, q); glEnableVertexAttribArray((GLuint)lp);
        /* case: viewport x, y, w, h; scissor (w = 0: off); three sample points; expected */
        static const struct { int vp[4], sc[4], pt[3][2]; const char *want; } cs[] = {
            { {0,150,240,132}, {0,0,0,0},     {{120,300},{120,216},{120,130}}, "BRB" },
            { {60,0,120,432},  {0,0,0,0},     {{30,216},{120,216},{210,216}},  "BRB" },
            { {0,0,240,432},   {0,0,0,0},     {{120,420},{120,216},{120,10}},  "RRR" },
            { {0,0,240,432},   {0,150,240,132}, {{120,300},{120,216},{120,130}}, "BRB" },
            { {0,150,240,132}, {0,0,0,0},     {{120,283},{120,281},{120,150}}, "BRR" },
        };
        rtext("NANOOS GL2 GAME build " __DATE__ " " __TIME__ "\nVCLIP linked "); rword((uint32_t)plinked); rtext("\n");
        unsigned ok = 0, n = sizeof cs / sizeof cs[0];
        for (unsigned i = 0; i < n; ++i) {
            char got[4] = {0,0,0,0}; uint32_t e = 0;
            for (unsigned f = 0; f < 3; ++f) {
                glDisable(GL_SCISSOR_TEST); glViewport(0, 0, 240, 432); glClearColor(0.0f, 0.0f, 1.0f, 1.0f); glClear(GL_COLOR_BUFFER_BIT);
                glViewport(cs[i].vp[0], cs[i].vp[1], cs[i].vp[2], cs[i].vp[3]);
                if (cs[i].sc[2]) { glScissor(cs[i].sc[0], cs[i].sc[1], cs[i].sc[2], cs[i].sc[3]); glEnable(GL_SCISSOR_TEST); }
                glDrawArrays(GL_TRIANGLES, 0, 6);
                if (f == 2u) {
                    for (unsigned k = 0; k < 3; ++k) { uint8_t b[4] = {0,0,0,0}; glReadPixels(cs[i].pt[k][0], cs[i].pt[k][1], 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, b);
                        uint32_t px = (uint32_t)b[0] << 16 | (uint32_t)b[1] << 8 | b[2]; got[k] = px == 0xff0000u ? 'R' : px == 0x0000ffu ? 'B' : '?'; }
                    e = (uint32_t)glGetError();
                }
                eglSwapBuffers(dpy, sfc);
            }
            glDisable(GL_SCISSOR_TEST);
            int pass = got[0] == cs[i].want[0] && got[1] == cs[i].want[1] && got[2] == cs[i].want[2] && !e;
            ok += (unsigned)pass;
            rtext("V "); rtext(got); rtext(" want "); rtext(cs[i].want); rtext(pass ? " ok\n" : " BAD\n");
        }
        uint32_t fr = 0, sw = 0, fault = 0; nano_app_gfx_stats(&fr, &sw, &fault);
        rtext("VCLIP frames/sw/fault "); rword(fr); rword(sw); rword(fault); rtext("\n");
        rtext(ok == n && fault == 0u ? "HELLO PASS\n" : "HELLO FAIL\n");
        report[rused] = 0;
        rpt_mirror_write(report, rused, rpt_sum(report, rused));
        nano_app_dcache_clean();
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        return 0;
    }
#endif
    static const char *vs_src =
        "attribute vec4 aPos;\n"
        "attribute vec4 aCol;\n"
        "uniform vec2 uOffset;\n"
        "varying vec4 vColor;\n"
        "void main() {\n"
        "    vColor = aCol;\n"
        "    gl_Position = vec4(aPos.xy + uOffset, 0.0, 1.0);\n"
        "}\n";
    static const char *fs_src =
        "precision mediump float;\n"
        "varying vec4 vColor;\n"
        "void main() {\n"
        "    gl_FragColor = vColor;\n"
        "}\n";

    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) return 6;

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) return 7;
    glUseProgram(prog);

    GLint a_pos = glGetAttribLocation(prog, "aPos");
    GLint a_col = glGetAttribLocation(prog, "aCol");
    GLint u_offset = glGetUniformLocation(prog, "uOffset");

    static const float verts[12] = {
         0.0f,  0.5f, 0.0f, 1.0f,
        -0.5f, -0.5f, 0.0f, 1.0f,
         0.5f, -0.5f, 0.0f, 1.0f
    };
    static const float cols[12] = {
        1.0f, 0.2f, 0.2f, 1.0f,
        0.2f, 1.0f, 0.2f, 1.0f,
        0.2f, 0.4f, 1.0f, 1.0f
    };

    glViewport(0, 0, (GLsizei)NANO_SCREEN_W, (GLsizei)NANO_SCREEN_H);
    glVertexAttribPointer((GLuint)a_pos, 4, GL_FLOAT, GL_FALSE, 0, verts);
    glEnableVertexAttribArray((GLuint)a_pos);
    glVertexAttribPointer((GLuint)a_col, 4, GL_FLOAT, GL_FALSE, 0, cols);
    glEnableVertexAttribArray((GLuint)a_col);

    float ox = 0.0f, oy = 0.0f;
    uint32_t center_px = 0, glerr = 0, e0 = (uint32_t)glGetError(), e1 = 0, e2 = 0;
    for (unsigned frame = 0; ; ++frame) {
        nano_input_t in;
        nano_app_poll_input(&in);
        if (in.home_pressed) break;
        if (in.down) {
            ox = ((float)in.x / (float)NANO_SCREEN_W) * 2.0f - 1.0f;
            oy = 1.0f - ((float)in.y / (float)NANO_SCREEN_H) * 2.0f;
        }
#ifdef HELLO_BENCH
        if (frame >= 30u) break;
#else
        (void)frame;
#endif
        glUniform2f(u_offset, ox, oy);
        glClearColor(0.05f, 0.08f, 0.14f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 3);
#ifdef HELLO_BENCH
        if (frame == 0u) e1 = (uint32_t)glGetError();
        if (frame == 29u) {
            uint8_t px[4] = {0, 0, 0, 0};
            glReadPixels(120, 216, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            e2 = (uint32_t)glGetError();
            center_px = ((uint32_t)px[3] << 24) | ((uint32_t)px[0] << 16) | ((uint32_t)px[1] << 8) | px[2];
        }
#endif
        eglSwapBuffers(dpy, sfc);
    }

    glerr = (uint32_t)glGetError();
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ctx);
    eglDestroySurface(dpy, sfc);
    eglTerminate(dpy);
#ifdef HELLO_BENCH
    {
        uint32_t frames = 0, sw = 0, fault = 0;
        nano_app_gfx_stats(&frames, &sw, &fault);
        rtext("NANOOS GL2 GAME build " __DATE__ " " __TIME__ "\nHELLO1 status/frames/software/fault/glerr/px ");
        rword(0u); rword(frames); rword(sw); rword(fault); rword(glerr); rword(center_px);
        rtext("\nHELLO2 e0/e1/e2 "); rword(e0); rword(e1); rword(e2);
        rtext("\n");
        rtext((frames == 30u && sw == 0u && fault == 0u && glerr == 0u && (center_px & 0xffffffu) != 0u) ? "HELLO PASS\n" : "HELLO FAIL\n");
        report[rused] = 0;
        rpt_mirror_write(report, rused, rpt_sum(report, rused));
        nano_app_dcache_clean();
    }
#else
    (void)center_px;
    (void)glerr;
    (void)e0;
    (void)e1;
    (void)e2;
#endif
    return 0;
}
