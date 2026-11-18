# MBX GPU driver

`src/mbx.c` implements the polled PowerVR MBX interface declared in
`src/mbx.h`; `src/mbx_test.c` supplies the DevMode GPU page. The current
submission path targets NanoEmu's `nano7_mbx.c` / `nano7_mbxr.c` model.

## Registers and initialization

The core register block starts at **`0x3b000000`**. The TA (Tile Accelerator)
FIFO aperture is `0x3b800000`; the segment doorbell is `0x3bc00000`.
Core-relative offsets used below are hexadecimal.

`mbx_init()` reads the signature at `+0xff0` and revision at `+0xf10`,
acknowledges stale events through `+0x134`, and builds eight 4 KB-aligned
page tables (32 KB total, 4 KB pages). These map GPU VA
`0x00000000..0x01ffffff` to physical DRAM `0x08000000..0x09ffffff`.
It writes the physical table roots to `+0x1000..+0x101c`, issues a memory
barrier, enables/invalidates the MMU with `+0x1020 = 0x101`, and waits up to
20 ms for bit 16. Its return value is **only** the signature comparison
against `0xb105f00d`; an MMU wait failure does not change that result.
`MBX_VA(p)` subtracts `0x08000000` from a CPU physical pointer.

## Public submission API

One frame follows this sequence:

1. `mbx_begin()` starts timing, clears events, and writes `1` to `+0x800`
   to begin a TA scene.
2. `mbx_submit(fifo, words)` writes a segment's 32-bit words to the FIFO,
   issues a barrier, then commits it with doorbell `1`. Submit one draw per
   segment. State headers (`0x1000mmmm`) select following state blocks;
   primitive prefixes are `0x7...` list, `0x4...` strip, and `0x5...` fan.
   Unchanged state may be reused by starting at the primitive word.
3. `mbx_render(target, w, h, stride)` sends end-of-list `0xf0000000` and
   waits up to 20 ms for scene status bit 4 at `+0x12c`. It then programs
   target GPU VA (`+0x65c`), stride **in pixels** (`+0x660`), and inclusive
   last x/y in bits 31:16 of `+0x654` / `+0x658`. After a barrier it restarts
   timing and kicks the 3D job with `+0x6ec = 1`.
4. `mbx_wait_done(timeout_us)` waits for **both** job status bits 2 and 6
   (`0x44`). Waits record status, acknowledge events, and return `0` on
   completion or `-1` on timeout. `mbx_render()` returns the TA wait result
   and still kicks the job after a TA timeout; it does not wait for 3D.

The global `mbx` diagnostics record completed scenes/jobs, last scene/job
duration in microseconds, last status, and cumulative timeouts.
`mbx_empty_scene()` performs begin/end and waits for scene completion
without a 3D target job.

## Target and test geometry

Render output is 32-bit **ARGB8888** (`0xAARRGGBB`). The test uses a
432x240 landscape target at physical `0x08300000` (GPU VA `0x00300000`),
stride 432, above the LCD buffers and outside the allocator heap. The CPU
copies each target pixel through `lcd_pixel()` and calls `lcd_present()`;
this is not direct GPU scanout.

The test's vertex format word is `0x502`: IEEE-754 float x/y in pixel
coordinates, implicit z=0, then packed **ABGR** color (`0xAABBGGRR`), three
32-bit words per vertex. Control word `0` bypasses a vertex program.
The ISP state selects opaque ONE/ZERO blending, Gouraud shading, depth
always, and no depth writes. The first segment uses header `0x10000021`
(ISP and vertex-format blocks), list primitive `0x70000000`, and a large
triangle `(0,0), (864,0), (0,480)` to cover the target with dark blue
`0xff401808` (RGB 8,24,64). The second segment reuses state and draws red,
green, and blue vertices at `(moving x,30)`, `(386,210)`, and `(46,210)`.
The apex sweeps x=66..366 over a 600-frame cycle; `mbx_f32()` converts its
small integer x coordinate to float bits without libgcc.

## Expected emulator test flow

From the repository root, run `nanoos/run-in-nanoemu.sh STOCK` (or `SAFE`
/ `MAX`). This builds the standalone kernel/chainloader, creates a disk
patch, and runs `test_nanoemu.py`. Prerequisites are the ARM toolchain,
Python/Pillow, built NanoEmu, snap6 dumps, and disk/resource images.
The wrapper uses a read-only disk image with replacements in the patch;
it does not access an attached iPod. Its optional `--real` flag means
NanoEmu's device-behavior simulation, not physical hardware.

After clock selection, enter DevMode, press Vol+ for Drivers, then Vol+
again for GPU (emulator Right maps to Vol+). Expect `MBX OK`, signature
`B105F00D`, and one completed empty scene on Drivers. The GPU page should
show `OK`, advancing job counts, timings, a dark-blue background, and an
animated Gouraud triangle. The regression checks background RGB at
`(10,120)`, a bright non-background triangle pixel at `(216,180)`, and two
different frames. Screenshots go to `nanoos/build/screens/<profile>/`
(`DevMode_gpu*`); logs go to `nanoos/build/nanoemu-<profile>.log`.
Vol- returns to stats; Home returns to the launcher. These are expected
checks, not a claim that the current build has passed them.

## Known limitations

- The current black-screen investigation is unresolved. Signature and
  completion status alone do not establish correct rendered pixels or LCD
  presentation; compare target contents, GPU screenshots, and status/
  timeout diagnostics. No root cause or hardware success is established here.
- This is an emulator-oriented bring-up path. Real TA parameter buffers and
  tile setup are missing. MMU entry flag bits are unknown and left clear;
  NanoEmu only checks that mapped pages are DRAM.
- FIFO writes have no space check: NanoEmu consumes immediately, whereas
  hardware needs occupancy handling for the `0x7b`-word FIFO. Clock/power
  gates are not initialized here; the code assumes the RetailOS handoff,
  and NanoEmu does not enforce those gates.
- There is no argument/bounds validation, interrupt-driven completion,
  cache maintenance, or timeout recovery. NanoOS currently runs with CPU
  MMU/caches disabled. NanoEmu clears targets to black before rendering;
  the test explicitly draws its colored background.

## Vertex fog (hardware probes run-fogprobe-1..5, 2026-11-18)

Matches RetailOS with `glEnable(GL_FOG)` (NanoEmu session 42), with corrections:
- Enable: ISP word bit 24 **and** VGP control word bit 3 (`0x8`, the extra vertex output). ISP bit 23 is what
  RetailOS also sets; with control bit 3 the draw needs bit 23 or 24 (neither: the primitive is not drawn).
  Bit 23 alone draws unfogged.
- Factor: VGP output 2 **lane x** (not w as the NanoEmu model reads). Lanes y/z/w have no visible effect: no
  offset/specular colour. pixel = f * colour + (1 - f) * fog colour (f = 0.5 on red gave 0x800000).
- Fog colour: black by default. Not any free 3D-setup register: writing 0xff00ff00 to each of the 44
  registers in 0x600..0x6fc that NanoOS does not own (`mbx_set_scene_reg`, kernel API v26) left fully fogged
  pixels black; 0x640 included. RetailOS never programs it (its driver has no glFog*). Location unknown.
- Tools: `examples/hello_gles2 -DHELLO_FOGPROBE`, runtime knobs `gl2_isp_or`, `gl2_ctl_or`, `gl2_live_or`.
- Wider sweep (run-fogsweep-1..4): every MBX core register in 0x000..0xefc that NanoOS does not write
  (0x200-0x5fc, 0x700-0x7fc, 0x900-0xefc: 704; 0x800-0x8fc and 0x000-0x1fc: 171) was set to 0xff00ff00 for two
  fully fogged frames and restored: fogged pixels stayed black. Only side effect: 0x8bc caused one recovered
  GPU fault. The ISP-list registers written 0 every scene (0x644, 0x64c, 0x674) were overridden after the list
  with five values each: still black. Not tested: 0xf00..0xffc (ID/version area) and registers NanoOS writes
  non-zero. The word after the ISP word (0xe0000000) does not carry a fog colour in bits 0..27 (0xe000ff00,
  0xe0ff00ff, 0xefffffff stay black); 0xe0800000 dropped the primitive and left later draws dropped too, so
  avoid bit 23 there. Conclusion: on this MBX Lite the vertex-fog colour is not CPU-programmable through the
  core registers (fixed black, or held outside the register file); coloured fog stays a per-vertex fold.

## VGP operand file 3 is not zero (2026-11-18)

Source operand file 3 (bits 17:16 + 2k = 3) was assumed to read 0.0. It does not.
Specialised OpenLara room programs folded dead light chains to `MOV temp <- file3`,
and the device rendered every room brighter and flatter than NanoEmu and ANGLE
(hw-reports/flight-sb-*: `GL2_SPEC=0`, `GL2_VS_OPT=0` and the constant-zero fold
all matched the reference; output retargeting, CSE and the forward/backward copy passes
did not change it). The fold now copies a constant lane that the variant's key
proves is 0.0 (`VSC_ZERO_OPERAND 0`). File 3 is still used only for writes to
outputs the fragment plan does not read, where the value is irrelevant. The exact value is not
measured: the brightening fits ~1.0, which NanoEmu (`NANO7_MBXR_FILE3`, default 1.0)
and vs_opt_check.c now model, so code that depends on file 3 reading zero fails in both.

## Shadow maps on the fixed combiner (2026-11-18)

No depth textures or per-pixel compare exist, but the combiner's bias (-0.5), complement and x4
scale give one: casters write linear light depth as a colour into an RGBA render target; a receiver
pass computes s = clamp(4(T + 1 - R - 0.5)) with R (receiver depth + offset) in the vertex colour,
then s*K + (1-K) in a second stage, multiply-blended. `hello_gles2 -DHELLO_SHADOWPROBE` checks it on
hardware (11 cases incl. FBO-rendered ramps, all within 3/255; the bias bit's first hardware use).
OpenLara uses it as an opt-in (Options > Detail > Shadows MEDIUM): walk 38.7 ms vs 20.7 ms, because
each map update is a GPU sync point (one-deep queue), plus a 5 ms copy out of uncached memory and a
receiver redraw. Known issue: a render-to-texture store is sometimes refused (GL_INVALID_OPERATION,
gl2tex.c tex_write_blocked) although the map updates each frame.

Sharper compare (2026-11-18, later): out = clamp(4(s + A - 0.5)) as the second stage gives a x16 slope
and a darkness floor of 4A - 2 in the same pass (A in the vertex alpha). Verified on hardware
(`hello_gles2 -DHELLO_SHADOWPROBE` cases 0xb..0xf, hw-reports/run-shD1.txt, HELLO PASS). OpenLara
stores half depth in the map so receiver depth + offset stays below 1; the shadow now starts about 8
units behind a caster and is full 64 units further (was: from about 20 units in front to 235 behind).
Walk benchmark with shadows on: 26.1 ms (hw-reports/run-sx1.txt), unchanged within noise.

## Multi-pass fragment shaders (2026-11-18)

A fragment shader that does not fit one combiner pass is drawn as several GPU passes over the same
geometry (`runtime/gl2/gl2fmp.inc`). The colour is rewritten as a sum of terms
`V * product(texture factors)`:

- `V` is everything that does not depend on a texture sample: varyings, uniforms, constants and any
  builtin (`normalize`, `dot`, `max`, `sqrt`, `step`, ...). It runs per vertex in that pass's vertex
  program and reaches the combiner as the vertex colour. Fragment uniforms work this way too (they
  read 0 in the combiner itself).
- A texture factor is `T`, `T.a`, `1-T` or `1-T.a` at any per-vertex coordinate. The coordinate is
  computed by the pass's vertex program, so `texture2D(s, uv + offset)` taps are real.
- Pass 1 draws with the application's state. Later passes blend: ONE/ONE adds a term,
  DST_COLOR/ZERO multiplies by a second lookup, and a pair of ONE_MINUS_DST_COLOR/ZERO passes
  around an add subtracts. Later passes do not write depth and test LEQUAL where pass 1 tested LESS.
- Each pass is a hidden program in the program pool (size `GL2_MAX_PROGRAMS` for them). Its vertex
  shader is the application's with the pass colour and coordinate appended; uniforms are copied
  from the parent program at each draw.
- With blending on, a multi-pass program draws only when every later pass is an add and alpha is
  per vertex (SRC_ALPHA/ONE_MINUS_SRC_ALPHA, SRC_ALPHA/ONE, ONE/ONE, ONE/ONE_MINUS_SRC_ALPHA);
  otherwise the draw raises GL_INVALID_OPERATION.

Opt-in: `gl2_fragment_passes(ctx, mode)` / `gl2std_fragment_passes(mode)`, or build with
`-DGL2_MP_MODE=<mode>`. The default is 0 (off): OpenLara and SHMUP pick a simpler shader when one
fails to compile, and that must not change under them.
Mode 1 uses passes only for shaders that have no single-pass plan and otherwise fail to compile.
Mode 2 also replaces single-pass plans that approximate the source (dropped builtins, stripped
fog/mix terms, offset taps collapsed to the centre). `gl2std_program_passes(program)` reports the
pass count. Cost: the geometry is submitted once per pass.

Not expressible (the lowering fails and the old behaviour stays): a nonlinear function of a texture
sample (`pow`, `step`, a clamp that can bite inside the expression), a coordinate that depends on
another sample, more than one term that multiplies two different lookups, `if` other than
`if (...) discard;`, uniform arrays, cube maps, `pow`.

Verified on hardware (`hello_gles2 -DHELLO_MPPROBE -DGL2_MP_MODE=2 -DGL2_MAX_PROGRAMS=24
-DGL2_MAX_SHADERS=16`, hw-reports/run-mp2.txt: HELLO PASS, 0 software frames, 0 faults): seven
shaders of 2 to 4 passes, each within 2/255 of the expression evaluated from the textures as the GPU
samples them. This is the first hardware evidence for ONE_MINUS_DST_COLOR as a source factor, for
DST_COLOR/ZERO with a numeric check, and for LEQUAL multi-pass over depth written by pass 1.
Host check: `runtime/gl2/std/test_gl2mp.py` (software rasteriser, per pixel).

## Per-pixel functions of a normal: lookup tables (2026-11-18)

A term of a multi-pass shader whose per-vertex factor depends on one vec3 varying only through its
direction (the shader wrote `normalize(v)`, as lighting does) is evaluated per pixel instead of per
vertex (`gl2fmp.inc` `mp_lut_pick`, `gl2api.c` `mp_lut_texture`):

- The compiler proves "direction only" structurally (degree of homogeneity 0 in the varying) and
  serialises the factor as a small program over the direction and the uniforms it reads.
- The CPU runs that program for every texel of a `GL2_MP_LUT_SIZE` (32) square sphere-map texture
  and repaints it only when one of those uniforms changes. `pow`, `sqrt`, `min`, `max`, `step`,
  `floor`, `fract`, `exp` are available there; `sin` / `cos` are not.
- The pass's vertex program outputs the sphere-map coordinate of the varying; the pass multiplies
  by the table (which holds half the value; the pass doubles, so factors up to 2 survive).
- Tables live in a ring of `GL2_MP_LUT_RING` (8) textures. A table sampled in scene s is rewritten
  from scene s + 2 on (the queue is one deep), so about four tables may change every frame; beyond
  that the newest table stays on screen. The ring shrinks if `GL2_MAX_TEXTURES` is too small.

Limits. The coordinate is interpolated linearly, so the error grows with how far the normal turns
across one triangle: 1.3/255 at 20 degrees, 1.7 at 41, 4.8 at 65, 21.7 at 103 (host test). A factor
that reads two varyings (a per-vertex light vector and a normal) is not a table. A normal read from
a texture (normal mapping) is not covered: that needs a per-pixel dot product between a texture and
a vector, the combiner's DOT3 mode, whose first hardware probe did not match (see FINDINGS).

Verified on hardware (`hello_gles2 -DHELLO_MPPROBE -DGL2_MP_MODE=2 -DGL2_MAX_PROGRAMS=24
-DGL2_MAX_SHADERS=24 -DGL2_MAX_TEXTURES=16`, hw-reports/run-lut5.txt, HELLO PASS): per-pixel
diffuse in one pass and diffuse x texture in two within 3/255, texture + `pow(n.l, 8)` specular
within 7/255. Cost of one draw of the two-pass case: 0.31 ms with the light still, 2.2 ms when the
light moves every frame, so one table repaint is about 1.9 ms.

## DOT3 (2026-11-18): verified on hardware

The combiner's dot-product mode works (`hello_gles2 -DHELLO_DOT3PROBE`, hw-reports/run-dot3-1, -2,
-3, all HELLO PASS with no software frames or faults). The earlier "failed" run354 rendered every
draw in software (its report: 18 software frames), so it said nothing about the GPU.

- Enable: layer word 0 bit 13 of the stage's texture layer, as RetailOS's GL_DOT3_RGB emits.
- Colour = clamp(scale * sum over r,g,b of (X1 - 0.5) * (Y1 - 0.5)), the same value in r, g and b.
  The stage's ordinary result scale applies: x1 gives a quarter of GL's DOT3, x2 half, x4 exactly
  GL's 4 * dot. Colour Y2 field = 1 (RetailOS's GL_DOT3_RGBA) also gives x4.
- X1 / Y1 select the operands as usual (T, P; C reads 0 in stage 1 here too). X2 must be "one" and
  product 2 dropped: X2 = P gave per-channel values that are not a dot product.
- Alpha = clamp(sum) at x1 (x4 with Y2 = 1), whatever the stage's alpha half says.
- Works in either stage: stage 2 can take DOT3(T, C), and stage 2 reads a stage-1 DOT3 result as
  C (DOT3 then C * P and C * T matched exactly).
- Precision: within 1/255 of the float formula over 16 colours and 8 vertex colours.

Driver use (`gl2fsc.inc` `f_lower_dot3`, `GL2_DOT3` default 1): when the ordinary one- and
two-stage lowerings fail, a shader whose colour is a function of `4 * dot(T - 0.5, P - 0.5)` or
`dot(T * 2 - 1, P * 2 - 1)` (P the vertex colour, or T with itself), optionally under
`clamp(x, 0, 1)`, compiles to two stages: stage 1 the dot at x4, stage 2 the shader's colour as a
function of it (C, C * T, C * P.a, ...) and its real alpha. Verified from GLSL on the device
(run-dot3-3) within 3/255: the plain dot with the texture's alpha, `clamp(dot(n, l), 0, 1) * t.rgb`
and `d * vColor.a`. The light vector must arrive in the colour varying (`vColor`, 0..1 encoded);
the normal is the texture. Test knobs: `gl2_l0_or[2]`, `gl2_w2_set[2]` (gl2rt.h).

Not done: a normal map and a separate diffuse texture in one draw (the single-pass compiler takes one
sampler; the multi-pass path does not yet know DOT3 terms), and normalising the per-vertex light
vector per pixel. NanoEmu's model (`nano7_mbxr.c`) now follows these measurements.

## Normal maps with a separate diffuse texture: DOT3 in the multi-pass path (2026-11-18)

A shader such as

    vec3 n = texture2D(sNormal, uv).rgb * 2.0 - 1.0;
    float d = clamp(dot(n, uLight), 0.0, 1.0);          // or max(dot(...), 0.0)
    gl_FragColor = vec4(texture2D(sDiffuse, uv).rgb * (uAmbient.rgb + d * uLightColour.rgb), 1.0);

draws natively in passes (`gl2fmp.inc` `mp_dot_match` / `mp_dot_site`). A sum `a * (T_k - 0.5) * B_k`
over a texture T and per-vertex B (uniforms, varyings, `normalize()` of them) becomes a factor of
its own: its pass is the single-pass DOT3 program with `0.5 + a/4 * B` in the vertex colour. It goes
first; the term's per-vertex factor rides on the next pass (DST_COLOR multiply) or a pass of its
own; other terms add as before. A bare (unclamped) dot is accepted only as the lane's only term.

Additive draws (the application blends GL_ONE / GL_ONE, as per-light accumulation does) cannot
multiply in the framebuffer, so they use a second pass list through destination alpha:
alpha := 1 (ONE/ONE, 0,0,0,1), alpha *= dot (ZERO/SRC_COLOR, 1,1,1,d), colour += diffuse * V *
alpha (DST_ALPHA/ONE, alpha 0), alpha := 1 again (`GL2_MP_ACC_RESTORE`, default 1, so alpha ends
as GL leaves it). Scalar lookups (`texture.a`) can be multiplied into alpha too; at most one
per-channel lookup per term. `gl2std_program_passes_additive()` reports that list's length.

Verified on hardware (`hello_gles2 -DHELLO_DOT3PROBE -DGL2_MP_MODE=1 -DGL2_MAX_PROGRAMS=24
-DGL2_MAX_SHADERS=24`, hw-reports/run-nm1, run-nm2, HELLO PASS, no software frames): normal map x
diffuse x light colour (2 passes), plus ambient (3), with `normalize(light)` (2), and the additive
form over a coloured background (4), all within 6/255 of the float formula (consistently 1-2% dark
from 8-bit steps between passes). First hardware evidence for DST_ALPHA and ZERO/SRC_COLOR blending
and for framebuffer alpha surviving between passes. Host check: `std/test_gl2mp.py`.

Limits. One light per draw (two dot products in a term multiply, which is not a lighting sum).
(Specular was added later the same day, see below.) The
light vector is per vertex and not renormalised per pixel. The vertex colour holds `0.5 + a/4 * B`,
so `|B|` must stay within 2/a (1 for the usual `* 2 - 1` decode). Specular: see the next section. Depth: later passes test LEQUAL where
the first wrote depth with LESS; GL_EQUAL across passes (Doom 3's light passes) is not yet tested.

## Specular: pow(dot, k) (2026-11-18)

`pow(clamp(dot(n, h), 0.0, 1.0), k)` and `pow(max(dot(n, h), 0.0), k)`, k = 1 or an even integer up
to 64, with n from a texture as above, are factors too (`mp_dot_k`). A pass writes d^2 in all four
channels (DOT3, then C * C in the second combiner stage). Then:

- Ordinary draws: the framebuffer is squared in place by blending ZERO / DST_ALPHA (it holds the same
  value in colour and alpha), so pow(d, 2^m) takes m passes; other even k multiply by d^2 again
  (k/2 passes). The chain must be the term's first factor, the shader's alpha must be 1, and an
  alpha := 1 pass ends the list. pow(d, 8) x diffuse: 5 passes.
- Additive draws (ONE / ONE): colour holds the accumulated light, so the chain multiplies d^2 into
  destination alpha k/2 times. Every term gets its own alpha segment, so diffuse + specular in one
  shader works there (Doom 3's interaction shape): diffuse d x diffuse x light colour + pow(d_h, 8) x
  specular colour = 10 passes. Such a shader has no ordinary list: drawn without ONE / ONE blending
  it raises GL_INVALID_OPERATION.
- Passes with identical shaders share one hidden program (a chain repeats its pass).

Verified on hardware (`hello_gles2 -DHELLO_DOT3PROBE -DGL2_MP_MODE=1 -DGL2_MAX_PROGRAMS=32
-DGL2_MAX_SHADERS=24`, hw-reports/run-spec1.txt, HELLO PASS, no software frames, alpha 255 after
every case): pow(d, 8) x diffuse within 8/255, pow(d, 6) x colour within 9/255, additive diffuse +
specular within 10/255, additive ambient + diffuse within 6/255. First hardware use of ZERO /
DST_ALPHA. All results run a little dark: a 2-pass diffuse chain is already about 5% low (97 vs 92
at mid values) although a single DOT3 pass and the plain multiply passes measure exact; pow
magnifies it. Not yet traced.

Cost. Every pass submits the geometry again. An exponent of 16 in an additive draw is 8 passes of
its own.

## DOT3 and vertex colour precision (2026-11-18): why chained passes ran dark

Measured on the device (hw-reports/run-dark1..5, `hello_gles2 -DHELLO_DOT3PROBE`):

- DOT3 = floor(255 * 4 * sum (t - 127.5)/255 * (p - 128)/255) for texel t and vertex colour p
  (worst 2/255 over 176 samples, run-dark2). The texel side matches GL; the vertex colour's zero
  is 128, not 127.5.
- Vertex colour float -> byte (what the combiner sees as P, run-dark3, 206 samples):
  round(255 v) for v <= 0.5, about floor(255.56 v - 0.56) above, so bright colours lose 0.5 to
  1.75 steps (0.8 -> 203, 0.55 -> 139). This affects every vertex colour, not only DOT3.
- Together these made a 2-pass normal-map chain about 4% dark, and pow() magnified it.

The multi-pass path now encodes the light vector for this (gl2fmp.inc `mp_dot_hw`, device builds
only: the host rasteriser is exact) and compensates every pass colour above 0.5. Worst errors
against the float formula, before -> after: normal map x diffuse 5 -> 3, plus ambient 6 -> 4,
pow(d, 8) 8 -> 5, pow(d, 6) 9 -> 5, additive diffuse + specular 10 -> 6 (/255). What remains is a
flat 1-2 step offset from the GPU rounding down in each pass (dot product, combiner multiply,
blend), the same at dark and bright values; no combiner operand can carry a constant that small.

Applications' own vertex colours are corrected too (`gl2api.c gpu_variant`, `GL2_COLOUR_COMP`,
default 1, device only): the GPU copy of every vertex shader that writes a vec4 colour varying gets
`c += step(0.501, c) * (c * -0.002191 + 0.004148)`, 3 VGP instructions per vertex (one literal row,
so the MAD forms survive the one-constant-register rule). If the extra literal would change a
program's uniform / literal layout it is dropped for that program, keeping its other
GPU-side folds. Measured (hw-reports/run-col1): all 195 probed colour bytes now equal round(255 v)
(were up to 1.75 steps low). OpenLara: the fixed-step walk frame 150 is unchanged apart from mean
brightness +0.1 (no pixel moved more than 8, run-col2); walk benchmark with shadows off 20.86 ms
average vs 20.26 ms with `-DGL2_COLOUR_COMP=0` (run-col-perf1 / 0), about 0.6 ms (3%).
The single-pass DOT3 from GLSL still sees the colour operand's 128 zero (half a step).
NanoEmu's DOT3 uses the 128 zero; its vertex colour conversion still rounds.
