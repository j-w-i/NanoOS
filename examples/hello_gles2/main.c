/* Minimal independent GLES2 + EGL example built purely through NanoOS SDK. */
#include "nano_app.h"
#include "EGL/egl.h"
#include "GLES2/gl2.h"
#if defined(HELLO_VIEWPORT_CLIP) || defined(HELLO_FOGPROBE) || defined(HELLO_SHADOWPROBE)
#define HELLO_BENCH 1
#endif
#ifdef HELLO_FOGPROBE
#include "gl2rt.h"
#endif
#ifndef SPN
#define SPN 64
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
#ifdef SP_LAND
    nano_app_set_orientation(NANO_ORIENT_LANDSCAPE);
#endif

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
#ifdef HELLO_SHADOWPROBE
    {
        /* Shadow-map depth compare on the MBX combiner. A 64x64 RGBA texture holds a depth ramp
         * D(u) = u (rendered through an FBO, or uploaded: case column "src"). A full-screen receiver
         * quad over a grey 0.8 scene samples it with u = 0..1 across x and compares against a
         * receiver depth R in the vertex colour:
         *   A: s = clamp(4(T + 1 - R - 0.5)); out = s*K + (1-K)   (two stages, K = 1-darkness in .w)
         *   B: s = clamp(4(T + 1 - R - 0.5)); out = clamp(4(s - 0.5))  (two stages, x16 slope)
         *   C: out = clamp(4(T + 1 - R - 0.5))                     (one stage, bias + x4)
         * blended onto the scene with DST_COLOR x src (blend column) or written directly.
         * Five pixels per case; expected values from the same formulas in float. */
        static const char *rvs = "attribute vec4 aPos;\nattribute vec2 aUV;\nuniform vec4 uP;\nvarying vec4 vColor;\nvarying vec2 vTexCoord;\nvoid main(){ vColor = uP; vTexCoord = aUV; gl_Position = aPos; }\n";
        static const char *fsA = "precision lowp float;\nuniform sampler2D sShadow;\nvarying vec4 vColor;\nvarying vec2 vTexCoord;\nvoid main(){ vec4 t = texture2D(sShadow, vTexCoord);\n vec3 s = clamp(((t.xyz + (1.0 - vColor.xyz)) - 0.5) * 4.0, 0.0, 1.0);\n gl_FragColor = vec4(s * vColor.www + (1.0 - vColor.www), 1.0); }\n";
        static const char *fsB = "precision lowp float;\nuniform sampler2D sShadow;\nvarying vec4 vColor;\nvarying vec2 vTexCoord;\nvoid main(){ vec4 t = texture2D(sShadow, vTexCoord);\n vec3 s = clamp(((t.xyz + (1.0 - vColor.xyz)) - 0.5) * 4.0, 0.0, 1.0);\n gl_FragColor = vec4(clamp((s - 0.5) * 4.0, 0.0, 1.0), 1.0); }\n";
        static const char *fsC = "precision lowp float;\nuniform sampler2D sShadow;\nvarying vec4 vColor;\nvarying vec2 vTexCoord;\nvoid main(){ vec4 t = texture2D(sShadow, vTexCoord);\n gl_FragColor = vec4(clamp(((t.xyz + (1.0 - vColor.xyz)) - 0.5) * 4.0, 0.0, 1.0), 1.0); }\n";
        static const char *cvs = "attribute vec4 aPos;\nattribute vec4 aCol;\nvarying vec4 vColor;\nvoid main(){ vColor = aCol; gl_Position = aPos; }\n";
        static const char *cfs = "precision lowp float;\nvarying vec4 vColor;\nvoid main(){ gl_FragColor = vColor; }\n";
        static const float q[24] = { -1,-1,0,1, 1,-1,0,1, -1,1,0,1,  1,-1,0,1, 1,1,0,1, -1,1,0,1 };
        static const float quv[12] = { 0,0, 1,0, 0,1,  1,0, 1,1, 0,1 };
        static const float qramp[24] = { 0,0,0,1, 1,1,1,1, 0,0,0,1,  1,1,1,1, 1,1,1,1, 0,0,0,1 };
        rtext("NANOOS GL2 GAME build " __DATE__ " " __TIME__ "\nSHADOWPROBE\n");
        GLuint progs[3]; const char *fss[3] = { fsA, fsB, fsC }; GLint lpos[3], luv[3], lP[3], lS[3];
        GLuint rv = compile_shader(GL_VERTEX_SHADER, rvs);
        for (unsigned i = 0; i < 3; ++i) {
            GLuint f = compile_shader(GL_FRAGMENT_SHADER, fss[i]); GLint lk = 0;
            progs[i] = glCreateProgram(); glAttachShader(progs[i], rv); if (f) glAttachShader(progs[i], f); glLinkProgram(progs[i]);
            glGetProgramiv(progs[i], GL_LINK_STATUS, &lk);
            lpos[i] = glGetAttribLocation(progs[i], "aPos"); luv[i] = glGetAttribLocation(progs[i], "aUV");
            lP[i] = glGetUniformLocation(progs[i], "uP"); lS[i] = glGetUniformLocation(progs[i], "sShadow");
            rtext("prog "); rword(i); rtext("fs "); rword(f != 0); rtext("linked "); rword((uint32_t)lk); rtext("\n");
        }
        GLuint cv = compile_shader(GL_VERTEX_SHADER, cvs), cf = compile_shader(GL_FRAGMENT_SHADER, cfs);
        GLuint cp = glCreateProgram(); glAttachShader(cp, cv); glAttachShader(cp, cf); glLinkProgram(cp);
        GLint cpos = glGetAttribLocation(cp, "aPos"), ccol = glGetAttribLocation(cp, "aCol");
        /* texture 0: ramp uploaded; texture 1: ramp rendered through an FBO */
        GLuint tex[2]; glGenTextures(2, tex);
        static uint8_t ramp[SPN * SPN * 4];
        for (unsigned y = 0; y < SPN; ++y) for (unsigned x = 0; x < SPN; ++x) { uint8_t v = (uint8_t)((x * 255u + (SPN-1)/2) / (SPN-1)); uint8_t *p = ramp + 4 * (y * SPN + x); p[0] = p[1] = p[2] = v; p[3] = 255; }
        for (unsigned i = 0; i < 2; ++i) {
            glBindTexture(GL_TEXTURE_2D, tex[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SPN, SPN, 0, GL_RGBA, GL_UNSIGNED_BYTE, i ? 0 : ramp); rtext("teximage "); rword(i); rword(glGetError()); rtext("\n");
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        GLuint fbo = 0; glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex[1], 0);
        uint32_t fbst = (uint32_t)glCheckFramebufferStatus(GL_FRAMEBUFFER); rtext("fbo setup err "); rword(glGetError()); rtext("\n");
        glViewport(0, 0, SPN, SPN); glClearColor(1, 1, 1, 1); glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(cp); for (unsigned a = 0; a < 5; ++a) glDisableVertexAttribArray(a);
        glVertexAttribPointer((GLuint)cpos, 4, GL_FLOAT, GL_FALSE, 0, q); glEnableVertexAttribArray((GLuint)cpos);
        glVertexAttribPointer((GLuint)ccol, 4, GL_FLOAT, GL_FALSE, 0, qramp); glEnableVertexAttribArray((GLuint)ccol);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        uint8_t fb0[4] = {0}, fb1[4] = {0}, fb2[4] = {0};
        uint32_t fe0 = glGetError(); glReadPixels(SPN/8, SPN/2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, fb0); glReadPixels(SPN/2, SPN/2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, fb1); glReadPixels(SPN*7/8, SPN/2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, fb2); rtext("fbo draw err "); rword(fe0); rword(glGetError()); rtext("\n");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        rtext("FBO status "); rword(fbst); rtext("ramp u.125/.5/.875 "); rword(fb0[0]); rword(fb1[0]); rword(fb2[0]); rtext("\n");
        /* cases: program, texture, blend, R, K */
        static const struct { unsigned prog, src, blend; float r, k; } cs[] = {
            { 2, 0, 0, 0.75f, 1.0f }, { 2, 0, 0, 0.50f, 1.0f }, { 2, 0, 1, 0.75f, 1.0f },
            { 0, 0, 1, 0.75f, 0.5f }, { 0, 0, 1, 0.60f, 0.3f }, { 0, 0, 0, 0.75f, 0.5f },
            { 1, 0, 1, 0.75f, 1.0f }, { 1, 0, 0, 0.75f, 1.0f },
            { 2, 1, 0, 0.75f, 1.0f }, { 0, 1, 1, 0.75f, 0.5f }, { 1, 1, 1, 0.75f, 1.0f },
        };
        static const float us[5] = { 0.1f, 0.3f, 0.5f, 0.7f, 0.9f };
        unsigned nbad = 0, ncase = sizeof cs / sizeof cs[0];
        for (unsigned c = 0; c < ncase; ++c) {
            unsigned pi = cs[c].prog; uint8_t px[5][4];
            for (unsigned f = 0; f < 2; ++f) {
                glViewport(0, 0, 240, 432); glDisable(GL_BLEND);
                glClearColor(0.8f, 0.8f, 0.8f, 1.0f); glClear(GL_COLOR_BUFFER_BIT);
                glUseProgram(progs[pi]); for (unsigned a = 0; a < 5; ++a) glDisableVertexAttribArray(a);
                glVertexAttribPointer((GLuint)lpos[pi], 4, GL_FLOAT, GL_FALSE, 0, q); glEnableVertexAttribArray((GLuint)lpos[pi]);
                glVertexAttribPointer((GLuint)luv[pi], 2, GL_FLOAT, GL_FALSE, 0, quv); glEnableVertexAttribArray((GLuint)luv[pi]);
                glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex[cs[c].src]); glUniform1i(lS[pi], 0);
                glUniform4f(lP[pi], cs[c].r, cs[c].r, cs[c].r, cs[c].k);
                if (cs[c].blend) { glEnable(GL_BLEND); glBlendFunc(GL_DST_COLOR, GL_ZERO); }
                glDrawArrays(GL_TRIANGLES, 0, 6);
                glDisable(GL_BLEND);
                if (f == 1u) for (unsigned k = 0; k < 5; ++k) glReadPixels((GLint)(us[k] * 240.0f), 216, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px[k]);
                eglSwapBuffers(dpy, sfc);
            }
            rtext("C "); rword(c); rword(glGetError());
            unsigned bad = 0;
            for (unsigned k = 0; k < 5; ++k) {
                float t = us[k], s = 4.0f * (t + 1.0f - cs[c].r - 0.5f); s = s < 0 ? 0 : s > 1 ? 1 : s;
                float o = pi == 0 ? s * cs[c].k + (1.0f - cs[c].k) : pi == 1 ? 4.0f * (s - 0.5f) : s; o = o < 0 ? 0 : o > 1 ? 1 : o;
                if (cs[c].blend) o *= 0.8f;
                int want = (int)(o * 255.0f + 0.5f), got = px[k][0], d = got - want; if (d < 0) d = -d;
                if (d > (pi == 1 ? 40 : 14)) ++bad;
                rword((uint32_t)got << 16 | (uint32_t)want);
            }
            rtext(bad ? "BAD\n" : "ok\n"); nbad += bad != 0;
        }
        /* OpenLara's per-frame pattern: queued window frame, then render-to-texture (with a depth
         * renderbuffer, like OpenLara's bindTarget), then the window frame sampling it. */
        {
            GLuint rb = 0; glGenRenderbuffers(1, &rb); glBindRenderbuffer(GL_RENDERBUFFER, rb);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, SPN, SPN); glBindRenderbuffer(GL_RENDERBUFFER, 0);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo); glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb);
            rtext("loop fbo status "); rword((uint32_t)glCheckFramebufferStatus(GL_FRAMEBUFFER)); rword(glGetError()); rtext("\n");
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            extern unsigned gl2std_error_site(void);
            for (unsigned it = 0; it < 6; ++it) {
                uint32_t t0 = nano_app_usec(), e[4], st[4];
                glBindFramebuffer(GL_FRAMEBUFFER, fbo); glViewport(0, 0, SPN, SPN);
                glClearColor(1, 1, 1, 1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                glUseProgram(cp); for (unsigned a = 0; a < 5; ++a) glDisableVertexAttribArray(a);
                glVertexAttribPointer((GLuint)cpos, 4, GL_FLOAT, GL_FALSE, 0, q); glEnableVertexAttribArray((GLuint)cpos);
                glVertexAttribPointer((GLuint)ccol, 4, GL_FLOAT, GL_FALSE, 0, qramp); glEnableVertexAttribArray((GLuint)ccol);
                glDrawArrays(GL_TRIANGLES, 0, 6);
                e[0] = glGetError(); st[0] = gl2std_error_site(); uint32_t t1 = nano_app_usec();
                glBindFramebuffer(GL_FRAMEBUFFER, 0); glViewport(0, 0, 240, 432);
                glClearColor(0.8f, 0.8f, 0.8f, 1.0f); glClear(GL_COLOR_BUFFER_BIT);
                e[1] = glGetError(); st[1] = gl2std_error_site(); uint32_t t2 = nano_app_usec();
                glUseProgram(progs[0]); for (unsigned a = 0; a < 5; ++a) glDisableVertexAttribArray(a);
                glVertexAttribPointer((GLuint)lpos[0], 4, GL_FLOAT, GL_FALSE, 0, q); glEnableVertexAttribArray((GLuint)lpos[0]);
                glVertexAttribPointer((GLuint)luv[0], 2, GL_FLOAT, GL_FALSE, 0, quv); glEnableVertexAttribArray((GLuint)luv[0]);
                glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex[1]); glUniform1i(lS[0], 0);
                glUniform4f(lP[0], 0.75f, 0.75f, 0.75f, 0.5f);
                glEnable(GL_BLEND); glBlendFunc(GL_DST_COLOR, GL_ZERO); glDrawArrays(GL_TRIANGLES, 0, 6); glDisable(GL_BLEND);
                e[2] = glGetError(); st[2] = gl2std_error_site(); uint32_t t3 = nano_app_usec();
                eglSwapBuffers(dpy, sfc);
                e[3] = glGetError(); st[3] = gl2std_error_site(); uint32_t t4 = nano_app_usec();
                uint32_t f0 = 0, f1 = 0, f2 = 0; nano_app_gfx_stats(&f0, &f1, &f2);
                rtext("L "); rword(it); rword(t1 - t0); rword(t2 - t1); rword(t3 - t2); rword(t4 - t3);
                for (unsigned k = 0; k < 4; ++k) { rword(e[k]); rword(st[k]); } rword(f0); rword(f2); rtext("\n");
            }
        }
        uint32_t fr = 0, sw = 0, fault = 0; nano_app_gfx_stats(&fr, &sw, &fault);
        rtext("SHADOWPROBE frames/sw/fault "); rword(fr); rword(sw); rword(fault); rtext("cases bad "); rword(nbad); rtext("\n");
        rtext(nbad == 0u && sw == 0u && fault == 0u ? "HELLO PASS\n" : "HELLO FAIL\n");
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
