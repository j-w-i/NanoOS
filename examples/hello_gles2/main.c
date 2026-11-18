/* Minimal independent GLES2 + EGL example built purely through NanoOS SDK. */
#include "nano_app.h"
#include "EGL/egl.h"
#include "GLES2/gl2.h"
#if defined(HELLO_VIEWPORT_CLIP) || defined(HELLO_FOGPROBE) || defined(HELLO_SHADOWPROBE) || defined(HELLO_MPPROBE) || defined(HELLO_DOT3PROBE)
#define HELLO_BENCH 1
#endif
#if defined(HELLO_FOGPROBE) || defined(HELLO_DOT3PROBE)
#include "gl2rt.h"
#endif
#ifndef SPN
#define SPN 64
#endif
#ifdef HELLO_BENCH
#include "report.h"
static char report[12288];   /* the report mirror holds 16 KiB */
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
        /* D: s as in A; out = clamp(4(s + A - 0.5)): x16 slope and a darkness floor 4A - 2 in one pass (A in .w) */
        static const char *fsD = "precision lowp float;\nuniform sampler2D sShadow;\nvarying vec4 vColor;\nvarying vec2 vTexCoord;\nvoid main(){ vec4 t = texture2D(sShadow, vTexCoord);\n vec3 s = clamp(((t.xyz + (1.0 - vColor.xyz)) - 0.5) * 4.0, 0.0, 1.0);\n gl_FragColor = vec4(clamp(((s + vColor.www) - 0.5) * 4.0, 0.0, 1.0), 1.0); }\n";
        static const char *cvs = "attribute vec4 aPos;\nattribute vec4 aCol;\nvarying vec4 vColor;\nvoid main(){ vColor = aCol; gl_Position = aPos; }\n";
        static const char *cfs = "precision lowp float;\nvarying vec4 vColor;\nvoid main(){ gl_FragColor = vColor; }\n";
        static const float q[24] = { -1,-1,0,1, 1,-1,0,1, -1,1,0,1,  1,-1,0,1, 1,1,0,1, -1,1,0,1 };
        static const float quv[12] = { 0,0, 1,0, 0,1,  1,0, 1,1, 0,1 };
        static const float qramp[24] = { 0,0,0,1, 1,1,1,1, 0,0,0,1,  1,1,1,1, 1,1,1,1, 0,0,0,1 };
        rtext("NANOOS GL2 GAME build " __DATE__ " " __TIME__ "\nSHADOWPROBE\n");
        GLuint progs[4]; const char *fss[4] = { fsA, fsB, fsC, fsD }; GLint lpos[4], luv[4], lP[4], lS[4];
        GLuint rv = compile_shader(GL_VERTEX_SHADER, rvs);
        for (unsigned i = 0; i < 4; ++i) {
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
            { 3, 0, 0, 0.75f, 0.625f }, { 3, 0, 0, 0.95f, 0.625f }, { 3, 0, 1, 0.95f, 0.625f }, { 3, 1, 1, 0.95f, 0.75f }, { 3, 0, 0, 0.85f, 0.5f },
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
                float o = pi == 0 ? s * cs[c].k + (1.0f - cs[c].k) : pi == 1 ? 4.0f * (s - 0.5f) : pi == 3 ? 4.0f * (s + cs[c].k - 0.5f) : s; o = o < 0 ? 0 : o > 1 ? 1 : o;
                if (cs[c].blend) o *= 0.8f;
                int want = (int)(o * 255.0f + 0.5f), got = px[k][0], d = got - want; if (d < 0) d = -d;
                if (d > (pi == 1 || pi == 3 ? 40 : 14)) ++bad;
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
#ifdef HELLO_DOT3PROBE
    {
        /* DOT3 on the MBX combiner (layer word 0 bit 13, as RetailOS's GL_DOT3_RGB emits). A 64x64
         * texture of 16 known colours (4x4 blocks) on a full-screen quad, a constant vertex colour P,
         * the combiner words forced through gl2_l0_or / gl2_w2_set. Every config reads the 16 block
         * centres; expected values are fitted offline against config 0 (the texture as sampled). */
        static const char *dvs = "attribute vec4 aPos;\nattribute vec2 aUV;\nuniform vec4 uP;\nvarying vec4 vColor;\nvarying vec2 vTexCoord;\nvoid main(){ vColor = uP; vTexCoord = aUV; gl_Position = aPos; }\n";
        static const char *dfs1 = "precision lowp float;\nuniform sampler2D sT;\nvarying vec4 vColor;\nvarying vec2 vTexCoord;\nvoid main(){ gl_FragColor = texture2D(sT, vTexCoord) * vColor; }\n";
        /* any shader with a two-stage plan, for the second layer (its combiner words are replaced) */
        static const char *dfs2 = "precision lowp float;\nuniform sampler2D sT;\nvarying vec4 vColor;\nvarying vec2 vTexCoord;\nvoid main(){ vec4 t = texture2D(sT, vTexCoord);\n vec3 s = clamp(((t.xyz + (1.0 - vColor.xyz)) - 0.5) * 4.0, 0.0, 1.0);\n gl_FragColor = vec4(s * vColor.www + (1.0 - vColor.www), 1.0); }\n";
        static const float q[24] = { -1,-1,0,1, 1,-1,0,1, -1,1,0,1,  1,-1,0,1, 1,1,0,1, -1,1,0,1 };
        static const float quv[12] = { 0,0, 1,0, 0,1,  1,0, 1,1, 0,1 };
        static const uint8_t cols[16][3] = {
            {255,128,128},{0,128,128},{128,255,128},{128,0,128},{128,128,255},{128,128,0},{255,255,255},{0,0,0},
            {192,64,128},{64,192,200},{220,40,90},{30,160,240},{128,128,128},{255,0,128},{100,200,50},{180,180,20} };
        /* combiner words: colour half h << 16 | alpha half. h fields: X1 15:14 (0 K, 1 C, 2 T, 3 one),
         * Y1 12:11 (0 P, 1 C, 2 T, 3 K), X2 10:9, Y2 7:6, drop product 2 bit 3. */
        enum { W_TP = 0x86084610u,      /* T * P (RetailOS MODULATE); with bit 13: DOT3(T, P) */
               W_P = 0xC6086610u,       /* one * P: the vertex colour as the combiner sees it */
               W_RGBA = 0x86484610u,    /* T, P with colour Y2 = 1: DOT3_RGBA */
               W_TT = 0x96084610u,      /* X1 T, Y1 T */
               W_TC = 0x8E084610u,      /* X1 T, Y1 C */
               W_CP = 0x46086610u,      /* stage 2: C * P */
               W_CT = 0x56084610u };    /* stage 2: C * T */
        /* dot: bit 0 DOT3 on layer 0, bit 1 on layer 1 */
        static const struct { uint8_t two, dot; uint32_t w0, w1; float p[4]; } cfg[] = {
            { 0, 0, W_TP, 0, { 1, 1, 1, 1 } },              /*  0 texture as sampled */
            { 0, 0, W_P, 0, { 0.25f, 0.5f, 0.75f, 0.5f } },  /*  1 P as sampled */
            { 0, 1, W_TP, 0, { 1, 0.5f, 0.5f, 1 } },        /*  2 DOT3: 2 T.r - 1 */
            { 0, 1, W_TP, 0, { 0.5f, 1, 0.5f, 1 } },        /*  3 2 T.g - 1 */
            { 0, 1, W_TP, 0, { 0.5f, 0.5f, 1, 1 } },        /*  4 2 T.b - 1 */
            { 0, 1, W_TP, 0, { 0.5f, 0.5f, 0.5f, 1 } },     /*  5 0 */
            { 0, 1, W_TP, 0, { 0, 0.5f, 0.5f, 1 } },        /*  6 1 - 2 T.r */
            { 0, 1, W_TP, 0, { 1, 1, 1, 1 } },              /*  7 2 (T.r + T.g + T.b) - 3 */
            { 0, 1, W_TP, 0, { 0.75f, 0.25f, 0.5f, 0.6f } },/*  8 general */
            { 0, 1, W_RGBA, 0, { 1, 0.5f, 0.5f, 1 } },      /*  9 DOT3_RGBA: alpha too */
            { 0, 1, W_TT, 0, { 1, 1, 1, 1 } },              /* 10 4 sum (T - .5)^2 */
            { 0, 1, W_TC, 0, { 1, 0.5f, 0.5f, 1 } },        /* 11 Y1 = C (reads 0 in unit 0 without DOT3) */
            { 0, 0, W_TC, 0, { 1, 0.5f, 0.5f, 1 } },        /* 12 the same without DOT3: T * C */
            { 1, 1, W_TP, W_CP, { 1, 0.5f, 0.5f, 1 } },     /* 13 two stages: DOT3(T, P), then C * P */
            { 1, 1, W_TP, W_CT, { 1, 0.5f, 0.5f, 1 } },     /* 14 DOT3(T, P), then C * T */
            { 1, 0, W_TP, W_CT, { 1, 1, 1, 1 } },           /* 15 no DOT3: T * P, then C * T = T^2 */
            { 0, 1, 0x86094610u, 0, { 1, 0.5f, 0.5f, 1 } }, /* 16 DOT3 with result scale x4 (h bit 0) */
            { 0, 1, 0x8608C610u, 0, { 1, 0.5f, 0.5f, 1 } }, /* 17 DOT3 with result scale x2 (w2 bit 15) */
            { 1, 2, W_P, 0x8E084610u, { 1, 0.5f, 0.5f, 1 } },/* 18 stage 1 = P, stage 2 = DOT3(T, C) */
            { 1, 2, W_P, 0x8E094610u, { 1, 0.5f, 0.5f, 1 } },/* 19 the same at x4 */
            { 0, 1, 0x86094610u, 0, { 0.75f, 0.25f, 0.5f, 0.6f } }, /* 20 x4, general P */
        };
        enum { NCFG = sizeof cfg / sizeof cfg[0] };
        rtext("NANOOS GL2 GAME build " __DATE__ " " __TIME__ "\nDOT3PROBE\n");
        static uint8_t tex[64 * 64 * 4];
        for (unsigned y = 0; y < 64; ++y) for (unsigned x = 0; x < 64; ++x) {
            unsigned b = (y / 16u) * 4u + x / 16u; uint8_t *p = tex + 4 * (y * 64 + x);
            p[0] = cols[b][0]; p[1] = cols[b][1]; p[2] = cols[b][2]; p[3] = (uint8_t)(b * 16u + 8u);
        }
        GLuint t = 0; glGenTextures(1, &t); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, t);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        GLuint v = compile_shader(GL_VERTEX_SHADER, dvs), f1 = compile_shader(GL_FRAGMENT_SHADER, dfs1), f2 = compile_shader(GL_FRAGMENT_SHADER, dfs2);
        GLuint pr[2]; GLint lk[2] = { 0, 0 };
        for (unsigned i = 0; i < 2; ++i) { pr[i] = glCreateProgram(); glAttachShader(pr[i], v); glAttachShader(pr[i], i ? f2 : f1); glLinkProgram(pr[i]); glGetProgramiv(pr[i], GL_LINK_STATUS, &lk[i]); }
        rtext("setup tex/link1/link2/err "); rword(t); rword((uint32_t)lk[0]); rword((uint32_t)lk[1]); rword(glGetError()); rtext("\n");
        glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND);
        for (unsigned c = 0; c < NCFG; ++c) {
            GLuint p = pr[cfg[c].two]; uint8_t px[16][4];
            for (unsigned fr = 0; fr < 2; ++fr) {
                glViewport(0, 0, 240, 432); glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
                glUseProgram(p); for (unsigned a = 0; a < 5; ++a) glDisableVertexAttribArray(a);
                GLint lp = glGetAttribLocation(p, "aPos"), lu = glGetAttribLocation(p, "aUV");
                glVertexAttribPointer((GLuint)lp, 4, GL_FLOAT, GL_FALSE, 0, q); glEnableVertexAttribArray((GLuint)lp);
                glVertexAttribPointer((GLuint)lu, 2, GL_FLOAT, GL_FALSE, 0, quv); glEnableVertexAttribArray((GLuint)lu);
                glUniform1i(glGetUniformLocation(p, "sT"), 0);
                glUniform4f(glGetUniformLocation(p, "uP"), cfg[c].p[0], cfg[c].p[1], cfg[c].p[2], cfg[c].p[3]);
                gl2_l0_or[0] = (cfg[c].dot & 1u) ? 1u << 13 : 0; gl2_l0_or[1] = (cfg[c].dot & 2u) ? 1u << 13 : 0;
                gl2_w2_set[0] = cfg[c].w0; gl2_w2_set[1] = cfg[c].two ? cfg[c].w1 : 0;
                glDisable(GL_BLEND);   /* drops the cached header: the knobs change it */
                glDrawArrays(GL_TRIANGLES, 0, 6);
                if (fr == 1u) for (unsigned b = 0; b < 16; ++b)
                    glReadPixels((GLint)((b % 4u) * 60u + 30u), (GLint)((b / 4u) * 108u + 54u), 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px[b]);
                eglSwapBuffers(dpy, sfc);
            }
            gl2_l0_or[0] = gl2_l0_or[1] = 0; gl2_w2_set[0] = gl2_w2_set[1] = 0; glDisable(GL_BLEND);
            rtext("D "); rword(c); rword(glGetError());
            for (unsigned b = 0; b < 16; ++b) rword((uint32_t)px[b][3] << 24 | (uint32_t)px[b][0] << 16 | (uint32_t)px[b][1] << 8 | px[b][2]);
            rtext("\n");
        }
        /* the shader compiler's own DOT3 lowering, from GLSL (two stages: the dot, then the shader's
         * colour from it and its alpha) */
        {
#define GH "precision lowp float;\nuniform sampler2D sT;\nvarying vec4 vColor;\nvarying vec2 vTexCoord;\n"
            static const struct { const char *fs; float p[4]; } gc[] = {
                { GH "void main(){ vec4 t = texture2D(sT, vTexCoord);\n float d = dot(t.rgb - vec3(0.5), vColor.rgb - vec3(0.5)) * 4.0;\n gl_FragColor = vec4(vec3(d), t.a); }\n", { 1, 0.5f, 0.5f, 1 } },
                { GH "void main(){ vec4 t = texture2D(sT, vTexCoord);\n float d = dot(t.rgb - vec3(0.5), vColor.rgb - vec3(0.5)) * 4.0;\n gl_FragColor = vec4(vec3(d), t.a); }\n", { 0.75f, 0.25f, 0.5f, 1 } },
                { GH "void main(){ vec4 t = texture2D(sT, vTexCoord);\n vec3 n = t.rgb * 2.0 - 1.0; vec3 l = vColor.rgb * 2.0 - 1.0;\n float d = clamp(dot(n, l), 0.0, 1.0);\n gl_FragColor = vec4(d * t.rgb, 1.0); }\n", { 1, 0.75f, 0.5f, 1 } },
                { GH "void main(){ vec4 t = texture2D(sT, vTexCoord);\n vec3 n = t.rgb * 2.0 - 1.0; vec3 l = vColor.rgb * 2.0 - 1.0;\n float d = clamp(dot(n, l), 0.0, 1.0);\n gl_FragColor = vec4(vec3(d) * vColor.a, vColor.a); }\n", { 1, 0.5f, 0.5f, 0.5f } },
            };
            extern int gl2std_program_passes(unsigned program);
            for (unsigned c = 0; c < sizeof gc / sizeof gc[0]; ++c) {
                GLuint gf = compile_shader(GL_FRAGMENT_SHADER, gc[c].fs), gpr = glCreateProgram(); GLint glk = 0;
                glAttachShader(gpr, v); if (gf) glAttachShader(gpr, gf); glLinkProgram(gpr); glGetProgramiv(gpr, GL_LINK_STATUS, &glk);
                uint8_t px[16][4]; uint32_t sw0 = 0, x0 = 0, y0 = 0; nano_app_gfx_stats(&x0, &sw0, &y0);
                for (unsigned fr = 0; fr < 2; ++fr) {
                    glViewport(0, 0, 240, 432); glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
                    glUseProgram(gpr); for (unsigned a = 0; a < 5; ++a) glDisableVertexAttribArray(a);
                    GLint lp = glGetAttribLocation(gpr, "aPos"), lu = glGetAttribLocation(gpr, "aUV");
                    glVertexAttribPointer((GLuint)lp, 4, GL_FLOAT, GL_FALSE, 0, q); glEnableVertexAttribArray((GLuint)lp);
                    glVertexAttribPointer((GLuint)lu, 2, GL_FLOAT, GL_FALSE, 0, quv); glEnableVertexAttribArray((GLuint)lu);
                    glUniform1i(glGetUniformLocation(gpr, "sT"), 0);
                    glUniform4f(glGetUniformLocation(gpr, "uP"), gc[c].p[0], gc[c].p[1], gc[c].p[2], gc[c].p[3]);
                    glDrawArrays(GL_TRIANGLES, 0, 6);
                    if (fr == 1u) for (unsigned b = 0; b < 16; ++b)
                        glReadPixels((GLint)((b % 4u) * 60u + 30u), (GLint)((b / 4u) * 108u + 54u), 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px[b]);
                    eglSwapBuffers(dpy, sfc);
                }
                uint32_t sw1 = 0; nano_app_gfx_stats(&x0, &sw1, &y0);
                rtext("G "); rword(c); rword((uint32_t)glk); rword((uint32_t)gl2std_program_passes(gpr)); rword(glGetError()); rword(sw1 - sw0);
                for (unsigned b = 0; b < 16; ++b) rword((uint32_t)px[b][3] << 24 | (uint32_t)px[b][0] << 16 | (uint32_t)px[b][1] << 8 | px[b][2]);
                rtext("\n");
                glUseProgram(0); glDeleteProgram(gpr); if (gf) glDeleteShader(gf);
            }
        }
        /* normal map (unit 0, the 16-colour texture) x diffuse (unit 1) through the multi-pass path */
        {
            static const uint8_t cols2[16][3] = {
                {200,200,200},{255,128,64},{64,255,128},{128,64,255},{255,255,0},{0,255,255},{255,0,255},{160,120,80},
                {80,160,120},{120,80,160},{255,255,255},{30,30,30},{220,180,140},{140,220,180},{180,140,220},{90,90,200} };
            static uint8_t tex2[64 * 64 * 4];
            for (unsigned y = 0; y < 64; ++y) for (unsigned x = 0; x < 64; ++x) {
                unsigned b = (y / 16u) * 4u + x / 16u; uint8_t *p = tex2 + 4 * (y * 64 + x);
                p[0] = cols2[b][0]; p[1] = cols2[b][1]; p[2] = cols2[b][2]; p[3] = 255;
            }
            GLuint t2 = 0; glGenTextures(1, &t2); glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, t2);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex2);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glActiveTexture(GL_TEXTURE0);
#define NH "precision mediump float;\nuniform sampler2D sN;\nuniform sampler2D sD;\nuniform vec4 uTint;\nuniform vec4 uAmb;\nuniform vec3 uLd;\nuniform vec3 uL2;\nvarying vec4 vColor;\nvarying vec2 vTexCoord;\n"
            static const struct { const char *fs; uint8_t add; } nc[] = {
                { "precision lowp float;\nuniform sampler2D sT;\nvarying vec4 vColor;\nvarying vec2 vTexCoord;\nvoid main(){ gl_FragColor = texture2D(sT, vTexCoord) * vColor; }\n", 0 },   /* reference: unit 1 */
                { NH "void main(){ vec3 n = texture2D(sN, vTexCoord).rgb * 2.0 - 1.0; float d = clamp(dot(n, uLd), 0.0, 1.0);\n gl_FragColor = vec4(texture2D(sD, vTexCoord).rgb * d * uTint.rgb, 1.0); }\n", 0 },
                { NH "void main(){ vec3 n = texture2D(sN, vTexCoord).rgb * 2.0 - 1.0; float d = max(dot(n, uLd), 0.0); vec3 b = texture2D(sD, vTexCoord).rgb;\n gl_FragColor = vec4(b * (uAmb.rgb + d * uTint.rgb), 1.0); }\n", 0 },
                { NH "void main(){ vec3 n = texture2D(sN, vTexCoord).rgb * 2.0 - 1.0; float d = clamp(dot(n, normalize(uL2)), 0.0, 1.0);\n gl_FragColor = vec4(texture2D(sD, vTexCoord).rgb * d, 1.0); }\n", 0 },
                { NH "void main(){ vec3 n = texture2D(sN, vTexCoord).rgb * 2.0 - 1.0; float d = clamp(dot(n, uLd), 0.0, 1.0);\n gl_FragColor = vec4(texture2D(sD, vTexCoord).rgb * d * uTint.rgb, 1.0); }\n", 1 },
                /* specular: pow of a dot product (framebuffer squaring, or d^2 chains in destination alpha) */
                { NH "void main(){ vec3 n = texture2D(sN, vTexCoord).rgb * 2.0 - 1.0; float s = pow(clamp(dot(n, uLd), 0.0, 1.0), 8.0);\n gl_FragColor = vec4(texture2D(sD, vTexCoord).rgb * s, 1.0); }\n", 0 },
                { NH "void main(){ vec3 n = texture2D(sN, vTexCoord).rgb * 2.0 - 1.0; float s = pow(max(dot(n, uLd), 0.0), 6.0);\n gl_FragColor = vec4(uTint.rgb * s, 1.0); }\n", 0 },
                { NH "void main(){ vec3 n = texture2D(sN, vTexCoord).rgb * 2.0 - 1.0; float d = clamp(dot(n, uLd), 0.0, 1.0);\n float s = pow(clamp(dot(n, normalize(uL2)), 0.0, 1.0), 8.0);\n gl_FragColor = vec4(texture2D(sD, vTexCoord).rgb * d * uTint.rgb + uAmb.rgb * 4.0 * s, 1.0); }\n", 1 },
                { NH "void main(){ vec3 n = texture2D(sN, vTexCoord).rgb * 2.0 - 1.0; float d = clamp(dot(n, uLd), 0.0, 1.0); vec3 b = texture2D(sD, vTexCoord).rgb;\n gl_FragColor = vec4(b * (uAmb.rgb + d * uTint.rgb), 1.0); }\n", 1 },
            };
            extern int gl2std_program_passes_additive(unsigned program); extern int gl2std_program_passes(unsigned program);
            for (unsigned c = 0; c < sizeof nc / sizeof nc[0]; ++c) {
                GLuint nf = compile_shader(GL_FRAGMENT_SHADER, nc[c].fs), npr = glCreateProgram(); GLint nlk = 0;
                glAttachShader(npr, v); if (nf) glAttachShader(npr, nf); glLinkProgram(npr); glGetProgramiv(npr, GL_LINK_STATUS, &nlk);
                uint8_t px[16][4]; uint32_t sw0 = 0, x0 = 0, y0 = 0; nano_app_gfx_stats(&x0, &sw0, &y0);
                for (unsigned b = 0; b < 16; ++b) px[b][0] = px[b][1] = px[b][2] = px[b][3] = 0;
                if (nlk) for (unsigned fr = 0; fr < 2; ++fr) {
                    glViewport(0, 0, 240, 432); glDisable(GL_BLEND);
                    if (nc[c].add) glClearColor(0.2f, 0.1f, 0.05f, 1); else glClearColor(0, 0, 0, 1);
                    glClear(GL_COLOR_BUFFER_BIT);
                    glUseProgram(npr); for (unsigned a = 0; a < 5; ++a) glDisableVertexAttribArray(a);
                    GLint lp = glGetAttribLocation(npr, "aPos"), lu = glGetAttribLocation(npr, "aUV"), l;
                    glVertexAttribPointer((GLuint)lp, 4, GL_FLOAT, GL_FALSE, 0, q); glEnableVertexAttribArray((GLuint)lp);
                    glVertexAttribPointer((GLuint)lu, 2, GL_FLOAT, GL_FALSE, 0, quv); glEnableVertexAttribArray((GLuint)lu);
                    if ((l = glGetUniformLocation(npr, "sT")) >= 0) glUniform1i(l, 1);
                    if ((l = glGetUniformLocation(npr, "sN")) >= 0) glUniform1i(l, 0);
                    if ((l = glGetUniformLocation(npr, "sD")) >= 0) glUniform1i(l, 1);
                    if ((l = glGetUniformLocation(npr, "uP")) >= 0) glUniform4f(l, 1, 1, 1, 1);
                    if ((l = glGetUniformLocation(npr, "uTint")) >= 0) glUniform4f(l, 1.0f, 0.8f, 0.6f, 1);
                    if ((l = glGetUniformLocation(npr, "uAmb")) >= 0) glUniform4f(l, 0.2f, 0.2f, 0.25f, 1);
                    if ((l = glGetUniformLocation(npr, "uLd")) >= 0) glUniform3f(l, 0.6f, 0.8f, 0);
                    if ((l = glGetUniformLocation(npr, "uL2")) >= 0) glUniform3f(l, 1, 0, 1);
                    if (nc[c].add) { glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE); }
                    glDrawArrays(GL_TRIANGLES, 0, 6);
                    glDisable(GL_BLEND);
                    if (fr == 1u) for (unsigned b = 0; b < 16; ++b)
                        glReadPixels((GLint)((b % 4u) * 60u + 30u), (GLint)((b / 4u) * 108u + 54u), 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px[b]);
                    eglSwapBuffers(dpy, sfc);
                }
                uint32_t sw1 = 0; nano_app_gfx_stats(&x0, &sw1, &y0);
                rtext("N "); rword(c); rword((uint32_t)nlk); rword((uint32_t)gl2std_program_passes(npr)); rword((uint32_t)gl2std_program_passes_additive(npr)); rword(glGetError()); rword(sw1 - sw0);
                for (unsigned b = 0; b < 16; ++b) rword((uint32_t)px[b][3] << 24 | (uint32_t)px[b][0] << 16 | (uint32_t)px[b][1] << 8 | px[b][2]);
                rtext("\n");
                glUseProgram(0); glDeleteProgram(npr); if (nf) glDeleteShader(nf);
            }
            /* DOT3 calibration sweeps: hw-reports/run-dark2 (transfer) and run-dark3 (vertex colour conversion) */
            /* vertex colour as the combiner sees it after the driver's correction (GL2_COLOUR_COMP): k / 64 in red,
             * k / 64 + 1/512 in green, 1 - k / 64 in blue; QC counts bytes that are not round(255 v) */
            {
                unsigned miss = 0, worst = 0;
                for (unsigned k = 0; k <= 64; ++k) {
                    uint8_t px[4] = { 0, 0, 0, 0 };
                    float vk = (float)k / 64.0f, vg = vk + 1.0f / 512.0f, vv[3]; if (vg > 1) vg = 1;
                    vv[0] = vk; vv[1] = vg; vv[2] = 1.0f - vk;
                    for (unsigned fr = 0; fr < 2; ++fr) {
                        glViewport(0, 0, 240, 432); glDisable(GL_BLEND); glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
                        glUseProgram(pr[0]); for (unsigned a2 = 0; a2 < 5; ++a2) glDisableVertexAttribArray(a2);
                        GLint lp = glGetAttribLocation(pr[0], "aPos"), lu = glGetAttribLocation(pr[0], "aUV");
                        glVertexAttribPointer((GLuint)lp, 4, GL_FLOAT, GL_FALSE, 0, q); glEnableVertexAttribArray((GLuint)lp);
                        glVertexAttribPointer((GLuint)lu, 2, GL_FLOAT, GL_FALSE, 0, quv); glEnableVertexAttribArray((GLuint)lu);
                        glUniform1i(glGetUniformLocation(pr[0], "sT"), 0);
                        glUniform4f(glGetUniformLocation(pr[0], "uP"), vv[0], vv[1], vv[2], 1);
                        gl2_w2_set[0] = 0xC6086610u; glDisable(GL_BLEND);
                        glDrawArrays(GL_TRIANGLES, 0, 6);
                        if (fr == 1u) glReadPixels(30, 54, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
                        eglSwapBuffers(dpy, sfc);
                        gl2_w2_set[0] = 0; glDisable(GL_BLEND);
                    }
                    for (unsigned ch = 0; ch < 3; ++ch) {
                        unsigned want = (unsigned)(vv[ch] * 255.0f + 0.5f), got = px[ch], d = want > got ? want - got : got - want;
                        if (d) ++miss;
                        if (d > worst) worst = d;
                    }
                    if (!(k % 8u)) { rtext("Q "); rword(k); rword((uint32_t)px[0] << 16 | (uint32_t)px[1] << 8 | px[2]); rtext("\n"); }
                }
                rtext("QC misses/worst "); rword(miss); rword(worst); rtext("\n");
            }
        }
        uint32_t fr = 0, sw = 0, fault = 0; nano_app_gfx_stats(&fr, &sw, &fault);
        rtext("DOT3PROBE frames/sw/fault "); rword(fr); rword(sw); rword(fault); rtext("\n");
        rtext(sw == 0u && fault == 0u ? "HELLO PASS\n" : "HELLO FAIL\n");
        report[rused] = 0;
        rpt_mirror_write(report, rused, rpt_sum(report, rused));
        nano_app_dcache_clean();
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        return 0;
    }
#endif
#ifdef HELLO_MPPROBE
    {
        /* Multi-pass fragment shaders on the device (gl2fmp.inc; build with -DGL2_MP_MODE=2
         * -DGL2_MAX_PROGRAMS=24). Each case is a fragment shader the combiner cannot run in one pass,
         * drawn on a full-screen quad with depth testing on and compared at five pixels with the same
         * expression evaluated from the two textures as the GPU itself samples them (read back from
         * plain single-texture frames first). */
        extern int gl2std_program_passes(unsigned program); extern unsigned gl2std_error_site(void); extern const uint32_t *gl2std_lut_stats(void);
        static const char *mvs = "attribute vec4 aPos;\nattribute vec2 aUV;\nvarying vec2 vUV;\nvoid main(){ vUV = aUV; gl_Position = aPos; }\n";
#define MPH "precision mediump float;\nuniform sampler2D sA;\nuniform sampler2D sB;\nuniform vec4 uTint;\nuniform vec4 uAmb;\nuniform float uMix;\nuniform float uAlpha;\nuniform vec3 uL;\nuniform vec3 uN;\nvarying vec2 vUV;\n"
#define MPHN "precision mediump float;\nuniform sampler2D sA;\nuniform vec4 uTint;\nuniform vec3 uL;\nvarying vec2 vUV;\nvarying vec3 vN;\n"
        static const char *mfs[] = {
            "precision mediump float;\nuniform sampler2D sA;\nvarying vec2 vUV;\nvoid main(){ gl_FragColor = texture2D(sA, vUV); }\n",
            "precision mediump float;\nuniform sampler2D sB;\nvarying vec2 vUV;\nvoid main(){ gl_FragColor = texture2D(sB, vUV); }\n",
            MPH "void main(){ gl_FragColor = vec4(mix(texture2D(sA, vUV).rgb, texture2D(sB, vUV).rgb, uMix), 1.0); }\n",
            MPH "void main(){ vec3 t = texture2D(sA, vUV).rgb; gl_FragColor = vec4(t * texture2D(sB, vUV).rgb * uTint.rgb + uAmb.rgb * t, 1.0); }\n",
            MPH "void main(){ vec2 d = vec2(0.0625, 0.0); gl_FragColor = vec4((texture2D(sA, vUV + d).rgb + texture2D(sA, vUV - d).rgb) * 0.5, 1.0); }\n",
            MPH "void main(){ gl_FragColor = vec4(1.0 - texture2D(sA, vUV).rgb * uTint.rgb, 1.0); }\n",
            MPH "void main(){ float l = max(dot(normalize(uN), uL), 0.0); gl_FragColor = vec4(texture2D(sA, vUV).rgb * l + uAmb.rgb, 1.0); }\n",
            MPH "void main(){ vec4 q = texture2D(sB, vUV); gl_FragColor = vec4(texture2D(sA, vUV).rgb * uTint.rgb * 2.0 + q.rgb * q.a, 1.0); }\n",
            MPH "void main(){ gl_FragColor = vec4(mix(texture2D(sA, vUV).rgb, texture2D(sB, vUV).rgb, uMix), uAlpha); }\n",
            /* per-pixel functions of a normal that swings across the quad (lookup table baked by the driver) */
            MPHN "void main(){ float d = max(dot(normalize(vN), uL), 0.0); gl_FragColor = vec4(uTint.rgb * d, 1.0); }\n",
            MPHN "void main(){ float d = max(dot(normalize(vN), uL), 0.0); gl_FragColor = vec4(texture2D(sA, vUV).rgb * d, 1.0); }\n",
            MPHN "void main(){ float s = pow(max(dot(normalize(vN), uL), 0.0), 8.0); gl_FragColor = vec4(texture2D(sA, vUV).rgb * 0.5 + vec3(s), 1.0); }\n",
        };
        static const char *mvn = "attribute vec4 aPos;\nattribute vec2 aUV;\nattribute vec3 aNrm;\nvarying vec2 vUV;\nvarying vec3 vN;\nvoid main(){ vUV = aUV; vN = aNrm; gl_Position = aPos; }\n";
        static const float qn[18] = { -0.35f,0,0.94f, 0.35f,0,0.94f, -0.35f,0,0.94f,  0.35f,0,0.94f, 0.35f,0,0.94f, -0.35f,0,0.94f };
        enum { MPN = sizeof mfs / sizeof mfs[0] };
        static const float q[24] = { -1,-1,0,1, 1,-1,0,1, -1,1,0,1,  1,-1,0,1, 1,1,0,1, -1,1,0,1 };
        static const float quv[12] = { 0,0, 1,0, 0,1,  1,0, 1,1, 0,1 };
        static const float tint[3] = { 0.5f, 0.75f, 0.25f }, amb[3] = { 0.125f, 0.25f, 0.0625f }, bg[3] = { 0.2f, 0.4f, 0.6f };
        static const int sx[5] = { 40, 80, 120, 160, 200 }, sy[5] = { 90, 350, 216, 130, 300 };
        rtext("NANOOS GL2 GAME build " __DATE__ " " __TIME__ "\nMPPROBE\n");
        static uint8_t ta[SPN * SPN * 4], tb[SPN * SPN * 4];
        for (unsigned y = 0; y < SPN; ++y) for (unsigned x = 0; x < SPN; ++x) {
            uint8_t rx = (uint8_t)((x * 255u) / (SPN - 1)), ry = (uint8_t)((y * 255u) / (SPN - 1)); uint8_t *p = ta + 4 * (y * SPN + x), *r = tb + 4 * (y * SPN + x);
            p[0] = (uint8_t)((x * x * 255u) / ((SPN - 1) * (SPN - 1))); p[1] = ry; p[2] = 0x40; p[3] = 255; r[0] = (uint8_t)(255 - rx); r[1] = 0x80; r[2] = ry; r[3] = rx;
        }
        GLuint tex[2]; glGenTextures(2, tex);
        for (unsigned i = 0; i < 2; ++i) {
            glActiveTexture(GL_TEXTURE0 + i); glBindTexture(GL_TEXTURE_2D, tex[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SPN, SPN, 0, GL_RGBA, GL_UNSIGNED_BYTE, i ? tb : ta);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        rtext("textures err "); rword(glGetError()); rtext("\n");
        GLuint mv = compile_shader(GL_VERTEX_SHADER, mvs), mn = compile_shader(GL_VERTEX_SHADER, mvn);
        uint8_t ra[5][3][4], rb[5][4]; unsigned nbad = 0;   /* A at x-15, x, x+15; B at x */
        glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS);
        for (unsigned c = 0; c < MPN; ++c) {
            GLuint f = compile_shader(GL_FRAGMENT_SHADER, mfs[c]); GLint lk = 0; uint8_t px[5][4]; int blend = c == 8u, lit = c >= 9u;
            GLuint p = glCreateProgram(); glAttachShader(p, lit ? mn : mv); if (f) glAttachShader(p, f); glLinkProgram(p); glGetProgramiv(p, GL_LINK_STATUS, &lk);
            if (f) glDeleteShader(f);
            int passes = gl2std_program_passes(p);
            for (unsigned k = 0; k < 5; ++k) for (unsigned b = 0; b < 4; ++b) px[k][b] = 0;
            if (lk) for (unsigned fr = 0; fr < 2; ++fr) {
                glViewport(0, 0, 240, 432); glDisable(GL_BLEND);
                if (blend) glClearColor(bg[0], bg[1], bg[2], 1.0f); else glClearColor(0, 0, 0, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                glUseProgram(p); for (unsigned a = 0; a < 5; ++a) glDisableVertexAttribArray(a);
                GLint lp = glGetAttribLocation(p, "aPos"), lu = glGetAttribLocation(p, "aUV"), l;
                glVertexAttribPointer((GLuint)lp, 4, GL_FLOAT, GL_FALSE, 0, q); glEnableVertexAttribArray((GLuint)lp);
                if (lu >= 0) { glVertexAttribPointer((GLuint)lu, 2, GL_FLOAT, GL_FALSE, 0, quv); glEnableVertexAttribArray((GLuint)lu); }
                { GLint ln = glGetAttribLocation(p, "aNrm"); if (ln >= 0) { glVertexAttribPointer((GLuint)ln, 3, GL_FLOAT, GL_FALSE, 0, qn); glEnableVertexAttribArray((GLuint)ln); } }
                if ((l = glGetUniformLocation(p, "sA")) >= 0) glUniform1i(l, 0);
                if ((l = glGetUniformLocation(p, "sB")) >= 0) glUniform1i(l, 1);
                if ((l = glGetUniformLocation(p, "uTint")) >= 0) glUniform4f(l, tint[0], tint[1], tint[2], 1);
                if ((l = glGetUniformLocation(p, "uAmb")) >= 0) glUniform4f(l, amb[0], amb[1], amb[2], 1);
                if ((l = glGetUniformLocation(p, "uMix")) >= 0) glUniform1f(l, 0.25f);
                if ((l = glGetUniformLocation(p, "uAlpha")) >= 0) glUniform1f(l, 0.5f);
                if ((l = glGetUniformLocation(p, "uL")) >= 0) glUniform3f(l, 0.6f, 0, 0.8f);
                if ((l = glGetUniformLocation(p, "uN")) >= 0) glUniform3f(l, 0, 0, 2.0f);
                if (blend) { glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); }
                glDrawArrays(GL_TRIANGLES, 0, 6);
                glDisable(GL_BLEND);
                if (fr == 1u) for (unsigned k = 0; k < 5; ++k) {
                    if (c == 0u) for (int o = 0; o < 3; ++o) glReadPixels(sx[k] + 15 * (o - 1), sy[k], 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, ra[k][o]);
                    else if (c == 1u) glReadPixels(sx[k], sy[k], 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rb[k]);
                    else glReadPixels(sx[k], sy[k], 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px[k]);
                }
                eglSwapBuffers(dpy, sfc);
            }
            uint32_t worst = 0, gsite = gl2std_error_site(), ge = glGetError();
            rtext("C "); rword(c); rtext("fs "); rword(f != 0); rtext("linked "); rword((uint32_t)lk); rtext("passes "); rword((uint32_t)passes); rtext("err "); rword(ge); rword(gsite); rtext("\n");
            if (c < 2u) {
                for (unsigned k = 0; k < 5; ++k) { const uint8_t *v = c ? rb[k] : ra[k][1]; rtext("  ref "); rword((uint32_t)v[3] << 24 | (uint32_t)v[0] << 16 | (uint32_t)v[1] << 8 | v[2]); }
                rtext("\n");
                if (!lk || ge) ++nbad;
                continue;
            }
            for (unsigned k = 0; k < 5; ++k) {
                uint32_t want = 0;
                for (unsigned ch = 0; ch < 3; ++ch) {
                    float a = ra[k][1][ch] / 255.0f, am = ra[k][0][ch] / 255.0f, ap = ra[k][2][ch] / 255.0f, b = rb[k][ch] / 255.0f, ba = rb[k][3] / 255.0f, e = 0;
                    float nx = -0.35f + 0.7f * (((float)sx[k] + 0.5f) / 240.0f), nl = nx * nx + 0.94f * 0.94f, rs = 1.0f;
                    for (unsigned it = 0; it < 24; ++it) rs = 0.5f * (rs + nl / rs);   /* sqrt */
                    float dl = (0.6f * nx + 0.8f * 0.94f) / rs, d8 = dl * dl; d8 *= d8; d8 *= d8;
                    switch (c) {
                    case 9: e = tint[ch] * dl; break;
                    case 10: e = a * dl; break;
                    case 11: e = a * 0.5f + d8; break;
                    case 2: e = a * 0.75f + b * 0.25f; break;
                    case 3: e = a * b * tint[ch] + amb[ch] * a; break;
                    case 4: e = 0.5f * (am + ap); break;
                    case 5: e = 1.0f - a * tint[ch]; break;
                    case 6: e = a * 0.8f + amb[ch]; break;
                    case 7: e = a * tint[ch] * 2.0f + b * ba; break;
                    default: e = 0.5f * (a * 0.75f + b * 0.25f) + 0.5f * bg[ch]; break;
                    }
                    if (e < 0) e = 0;
                    if (e > 1) e = 1;
                    uint32_t w = (uint32_t)(e * 255.0f + 0.5f), g = px[k][ch], d = w > g ? w - g : g - w;
                    if (d > worst) worst = d;
                    want |= w << (16 - 8 * ch);
                }
                rtext("  px "); rword((uint32_t)px[k][0] << 16 | (uint32_t)px[k][1] << 8 | px[k][2]); rtext("want "); rword(want);
            }
            { uint32_t lim = lit ? 12u : 8u; int minp = c == 9u ? 1 : 2;
              rtext("\n  worst "); rword(worst); rtext(worst <= lim && lk && !ge && passes >= minp ? "ok\n" : "BAD\n");
              if (worst > lim || !lk || ge || passes < minp) ++nbad; }
            if (c == 10u && lk) {
                /* cost of the lookup table: 60 frames with the light moved every frame (table repainted) and 60 with it still */
                GLint ll = glGetUniformLocation(p, "uL"); uint32_t tm[2] = { 0, 0 };
                for (unsigned mode = 0; mode < 2; ++mode) for (unsigned fr = 0; fr < 60; ++fr) {
                    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); glUseProgram(p);
                    if (!mode) glUniform3f(ll, 0.6f - 0.004f * (float)fr, 0, 0.8f); else glUniform3f(ll, 0.6f, 0, 0.8f);
                    uint32_t t0 = nano_app_usec(); glDrawArrays(GL_TRIANGLES, 0, 6); tm[mode] += nano_app_usec() - t0;
                    eglSwapBuffers(dpy, sfc);
                }
                rtext("  LUT draw us/frame: light moving "); rword(tm[0] / 60u); rtext("light still "); rword(tm[1] / 60u); rtext("err "); rword(glGetError()); rtext("\n");
            }
            glUseProgram(0); glDeleteProgram(p);
        }
        { const uint32_t *ls = gl2std_lut_stats(); rtext("LUT reused/repainted/noslot/lasterr "); for (unsigned i = 0; i < 4; ++i) rword(ls[i]); rtext("\n"); }
        uint32_t fr = 0, sw = 0, fault = 0; nano_app_gfx_stats(&fr, &sw, &fault);
        rtext("MPPROBE frames/sw/fault "); rword(fr); rword(sw); rword(fault); rtext("cases bad "); rword(nbad); rtext("\n");
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
