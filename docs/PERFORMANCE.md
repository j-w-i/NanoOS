# NanoOS GLES2 performance guide (iPod nano 7, PowerVR MBX Lite)

Measured on hardware while taking OpenLara (Tomb Raider 1, `../openlara-nano7`) from ~95 ms/frame
to 16.7 ms/frame at stock clocks (runs 495-517, November 2026). **That 60 FPS image was wrong**: it matched
the old NanoOS output, not real GLES2 (see section 6). With the rendering fixed against an ANGLE GLES 2.0
reference, OpenLara runs at **31.1 ms/frame (32 FPS) at stock clocks** (run523, with every correctness fix below; run522 was 31.4 ms).
Everything below is in the shared runtime/SDK, so every app linked against the current SDK gets it.
The reference for "what fast looks like" is still `archive/gl2rt/gamebench` (run227, 40k triangles, 55 FPS).

## 1. How the hardware spends a frame

- **Submission is GPU/bus-bound.** The CPU writes vertex words into the 123-word TA FIFO
  (`0x3b800000`); during OpenLara draws the FIFO averaged 119/123 full. Time in `mbx_submit*` is time
  the GPU (VGP + TA) and the bus need to swallow the words.
- **Cost model (OpenLara, run513/514):** ~0.06 us per FIFO word, plus the vertex program's run time
  per vertex, plus **~4 us per primitive packet** (each packet needs its own doorbell: the TA reads
  every word up to the doorbell as vertex data, so doorbells cannot be batched across draws).
- **Vertex throughput follows the bus clock, not the GPU field.** The CPU MAX clock profile lowers
  the bus dividers (133/66/133 MHz vs stock 171/85/171) and made submission ~40% slower (run499).
  CLKCON `+0x1c` GPU field changes did nothing. Use stock clocks.
- **The CPU waits while the GPU drains, and the GPU idles while the CPU works.** The FIFO only holds
  ~25-50 us of work. Overlapping them needs an interrupt- or DMA-fed FIFO (not available yet).

## 2. What the runtime now does for you (all generic)

| Technique | Where | Effect on OpenLara |
|---|---|---|
| Frames queue in 3 offscreen buffers; the display push is stepped from draw calls (`service_display`) | `runtime/gl2/std/egl_nano.c` | swap 13.6 -> 0.5 ms |
| Converted draws are fed in `GL2_SUBMIT_CHUNK` (12) vertex sub-packets so CPU conversion overlaps the FIFO backlog | `gl2api.c` fused path | 77.5 -> 68.5 ms |
| **GPU-only vertex-program pruning**: the native code keeps only what the TSP reads for the linked combiner plan (position, clip, plan varyings); dropped TSP outputs 1-4 get a single zero write (output 4 must be written: run501 hung) | `gl2vsc.c vsc_prune`, `gl2api.c native_outputs/prune_native` | room VS 115 -> 29 instructions |
| **Input slimming**: vertex inputs the pruned code no longer reads are not sent | `gl2api.c link_build` | room vertex 15 -> 7 words |
| Static list draws: cached triangle strips (per-strip packets, only when cheaper than the list at `GL2_PRIM_COST_VERTS`=5) | `gl2strip.inc` | used for real strips; lone quads stay lists |
| **Converted-vertex cache** for static buffers: native words are built once and submitted straight from memory, one packet per draw (opt-in `GL2_VCACHE_WORDS`) | `gl2api.c` | ~10 ms/frame of short->float conversion gone |
| Header cache compares exact words; uniform setters no longer invalidate it; **delta headers** send only changed constant rows when the program is unchanged (`GL2_DELTA_HEADERS`) | `gl2api.c` | fewer FIFO words; ~116 deltas/frame |
| **SDK libm on the FPU**: `sqrt`/`sqrtf` use `vsqrt`, the file is built with VFP (`sdk/app.mk`) | `sdk/src/nano_heap.c` | animation 13.5 -> 0.6 ms/frame; level load 6.2 -> 4.5 s |
| Unread uniform rows are not sent; `cross()` compiles to vector ops | `gl2rt.c`, `gl2vsc.c` | earlier runs 492-495 |

The software (CPU) fragment path still runs the full, unpruned vertex program with the full input layout.

## 3. Build knobs (define in your app's `APP_CPPFLAGS`)

| Define | Default | Meaning |
|---|---|---|
| `GL2_VCACHE_WORDS` | 0 (off) | words of converted-vertex cache (OpenLara: `524288u` = 2 MB BSS) |
| `GL2_STRIP_POOL_WORDS` | 65536 | strip/list cache pool (uint16); 0 disables strip/vertex caching |
| `GL2_PRIM_COST_VERTS` | 5 | packet cost in vertex runs when choosing strips vs list |
| `GL2_SUBMIT_CHUNK` / `GL2_STRIP_CHUNK` | 12 / 26 | sub-packet size while converting |
| `GL2_VS_PRUNE` | 1 | GPU-only vertex program pruning and input slimming |
| `GL2_DELTA_HEADERS` | 1 | delta header submission |
| `GL2_EGL_NO_QUEUE` | undefined | force the synchronous swap path |

Static caches key on buffer generation, so `glBufferData`/`glBufferSubData` invalidate them.
Only `GL_STATIC_DRAW` buffers are cached.

## 4. Techniques for any app (in order of payoff)

1. **Measure the whole frame, not the render call.** `glReadPixels` ends the frame synchronously
   (bypasses queuing); keep readbacks out of timed frames.
2. **Profile your CPU code for libm and soft-float.** Anything compiled with `-mfloat-abi=soft` calls
   libgcc for every float op. Apps should use `-mfpu=vfpv3-d16 -mfloat-abi=softfp` (ABI-compatible
   with the SDK) for game code.
3. **Fewer packets beat fewer vertices** once vertices are cheap: one draw = one packet. Merge draws
   with identical state; avoid per-quad/per-sprite packets (~4 us each).
4. **Fewer words per vertex.** Only send attributes the shader really uses on the GPU path; prefer
   packed `GL_UNSIGNED_BYTE` colours.
5. **Short vertex programs.** Per-vertex work the fixed-function TSP cannot use (e.g. per-vertex
   lighting fed to an unsupported fragment path) is pruned automatically; computing lighting into
   the colour varying (`vDiffuse`, slot 1) is what actually reaches the screen.
6. **Static geometry in `GL_STATIC_DRAW` buffers with `glDrawElements`** so the vertex cache applies.
7. **Stay at stock clocks**; only raise the CPU if you are CPU-bound and can afford slower submission.

## 5. Open levers

- Overlap CPU work with FIFO feeding (IRQ- or DMA-fed FIFO; experimental FIFO DMA in `src/mbx_dma_fifo.inc`
  was slower in its synchronous form).
- Strip restart within one packet (unknown in the TA) would make strips pay off again.
- OpenLara: per-vertex lighting never reaches the TSP (pre-existing); folding it into `vDiffuse` would
  restore lighting at small cost.

## 6. Correctness: always compare against a real GLES 2.0 reference

`openlara-nano7/tools/host_ref` renders the same build through ANGLE (GLES 2.0 + EGL) on macOS;
`compare.py` diffs it against a NanoOS frame (device or NanoEmu). A 30x27 brightness grid that matches
an older NanoOS run is **not** a correctness check: it hid every bug below. OpenLara frame 4 went from
mean abs diff 52.8 (94% of pixels off) to 9.6 (11%) after these generic runtime fixes:

| Bug (all pre-existing) | Fix |
|---|---|
| UV0 sent as 2 words while the shader read `.zw` (inputs read 0/1): every texture smeared | `uv0_input`: send input 3 at the width the GPU code reads (`gl2api.c link_build`, `GL2_UV0W`) |
| Fragment `uv /= v.zw` silently dropped (kept the vertex-side multiply) | fragment compiler records `uv_divide`; GPU variant appends `uv.xy /= max(uv.zw, eps)` per vertex. A raw 1/0 there hung the device (run518) |
| All non-colour varyings aliased to the colour input in `gl2fsc.inc`: `color *= vLight` vanished (unlit scene, lamp-only entities black) | extra varyings get their own inputs; the colour product is recorded as a fold and the GPU variant appends `vDiffuse.xyz *= vLight.xyz` (Gouraud). `mix()` fog factors are dropped before folding |
| Rectangular textures: mip levels uploaded/generated with level-0 twiddle | `upload()`/`glGenerateMipmap` twiddle by each level's own size |
| Fixed 2 MB texture slots: a 1024x1024 RGBA16 atlas's mips spilled into the next texture | arena allocated by full mip-chain size (`tex_reserve`) |
| Deleted-but-attached shaders pinned slots: programs after the 16th (sky, GUI) failed to link | `glCreateShader` reclaims deleted shader slots of linked programs |

GPU variant: at link the vertex shader is recompiled with those statements appended to `main()` (the
compiler allocates registers), checked to keep the uniform/literal layout, then pruned. The software
fragment path keeps the original compile.

Later fixes (2026-11-17), all in the SDK/runtime:

| Bug | Fix |
|---|---|
| C++ static constructors never ran (`COLOR_WHITE` was 0: carved doors black) | `sdk/src/app.ld` keeps `.init_array`, `start.S` runs it before `nano_example_main`; `__cxa_atexit`/`__dso_handle` stubs in `nano_cpp.cpp` |
| Draw-time attribute mask used VGP slots instead of GL locations (`glBindAttribLocation` apps) | `draw_stream_i` uses `attr_bind` and raises an error instead of dropping draws |
| `GL2_MAX_TEXTURES` 8 exhausted: later textures (font glyphs: health bar) undefined | apps can raise it (`GL2_MAX_TEXTURES`, `GL2_TEXTURE_ARENA_BYTES`); slots are arena-allocated by size |
| Vertex `if` without `else`, or with `op=` / swizzled assignments to locals in a branch, failed to compile (OpenLara underwater programs) | `gl2vsc.c parse_if_chain`: implicit empty else for locals, branch assignments reuse `stmt()`. Both sides are computed and selected (`a + (b - a) * cond`) |
| A second, unused `sampler2D` declaration failed the fragment compile (OpenLara filters) | samplers are counted when sampled; the bound name is the one `texture2D` uses |
| Fragment `texture2D(s, uv + offset)` (blur taps) and `textureCube` (crystals, Midas gold) unsupported | approximated, flagged in `gl2_fs_entry.uv_approx`: offset taps -> the UV0 sample (a normalised blur becomes unblurred), `textureCube` -> white (the material colour shows) |
| Vertex `sin`/`cos`/`floor`/`fract` missing (OpenLara underwater caustics) | no FRC/FLR on the VGP: IEEE round-to-nearest trick `(x + 1.5*2^23) - 1.5*2^23`, odd Taylor series to y^11 on the reduced angle (clamped to [-pi, pi]); max error 5e-4 in a VGP model. **Not yet checked on hardware** |
| Fragment user functions (`vec4 blur() {...}`), `const` locals, `v[k]` on a local vector | functions are inlined at each call (`f_call_user` in `gl2fsc.inc`, depth 4, parameters shadow outer names) |
| Underwater fragment programs had no native plan (CPU only, so GPU frames rejected their draws) | the mix stripper drops only a *uniform-only* fog side (it used to drop any side with uniforms, e.g. the colour side), also strips a constant fog colour `mix(K, colour, fog)`, takes `t = 1` for other varying mixes (whole entity tinted, not only below the water plane), and folds per-lane constant tints into the vertex colour (`fold_const`) |

Check an app's shaders on the host before running on the device: `python3 NanoOS/runtime/gl2/shader_check.py *.glsl`
reports compile errors, native vs CPU-only fragment plans, approximations and VGP program length.
OpenLara: all LOW-detail programs get native plans; `FILTER_UPSCALE`, `FILTER_DOWNSAMPLE` (loops) and
`FILTER_ANAGLYPH` (two samplers) still fail and are unused at LOW detail / without stereo.

**Faults are recoverable** (2026-11-17): a GPU completion timeout, display wait timeout or failed
frame begin used to set a sticky fault, after which every draw failed (`GL_INVALID_OPERATION`) and the
screen froze while the game kept running (OpenLara demo, flight log: frame time 16 -> 5 ms, glerr 0x502
every frame from ~frame 2400). `gl2_nano_recover` halts and re-initialises the GPU and drops queued
frame state, keeping textures, programs and buffers; EGL calls it at the next frame (at most every
100 ms) and `gl2_egl_recoveries()` counts it. Landscape window frames no longer refuse to begin when
the bound program is CPU-only (they start a GPU frame and reject only those draws).

A program that fails to link is worse than an approximation: GLES keeps the previous program
bound after the failed `glUseProgram`, so the draws render with the wrong shader.

Landscape mode (`nano_app_set_orientation`, see ARCHITECTURE_AND_SDK.md section 5) costs nothing
per vertex: output-0 writes get x/y swapped in the header copy of the code and one viewport scale
is negated (a rotation, so winding and culling are unchanged). Checked against a 432x240 ANGLE
reference (`HOST_LANDSCAPE=1`): frame 4 mean abs diff 6.2, 0.6% of pixels off (portrait: 5.7, 0.6%).

Speed: the lit room program is ~89 VGP instructions, 36 of them lane-assembly MOVs; folding those in
`gl2vsc.c` is the next lever. Packed inputs (`GL2_PACK_INPUTS`: SHORT4 position, ubyte4 light, codes from
the stock driver) are bit-identical and gave 32.6 -> 31.4 ms.

## 7. Movement benchmark (run525, landscape, stock clocks)

`openlara-nano7` built with `-DLARA_BENCH -DLARA_FIXED_DT -DLARA_LANDSCAPE=1 -DLARA_WALK` replays
`src/walk_script.h` (600 frames at a fixed 33 ms game step: run down the start corridor, cross the big
room, the next room, turn round, run back; rooms 90 -> 13 -> 15 -> 13, wolves attack) and times every
frame swap-to-swap. `tools/host_ref` replays the same path with `HOST_WALK=1` (ANGLE frames every 30th).

| avg | p50 | p90 | p99 | max | GL errors | recoveries |
|---|---|---|---|---|---|---|
| 30.5 ms | 31.1 ms | 35.5 ms | 67.1 ms | 69.7 ms | 0 | 0 |

Frames: 23 under 16.7 ms, 89 under 25, 337 under 33.4, 121 under 50, 30 at 50 ms or more.
Frame time tracks draw calls, not triangles: 25 draws 16 ms, 110 draws 31 ms, 288 draws (turning to
face the whole big room) 61 ms, about 0.17 ms per draw. Per-draw cost (packet doorbells, headers,
state) is the lever for 60 FPS.

## 8. Push toward 60 FPS (runs 526-534, landscape walk, stock clocks)

Profiling the walk (`WPROF` in the LARA_WALK report) showed the frame was **vertex-program bound**, not
per-draw bound: the CPU spent 20 of 29 ms waiting for the TA FIFO, and the cost fit ~20 ns per VGP
instruction per vertex with almost no fixed per-vertex cost. Every change below is generic runtime code.

| Change | Where | Walk avg | p99 |
|---|---|---|---|
| baseline (run525) | | 30.5 ms | 67.1 ms |
| **Copy propagation + retargeting** on the GPU code (`vsc_optimize`): reads of copies use the source, single-use results are computed straight into the copy's destination, dead copies pruned. Lit room 89 -> 67 instructions, entity 99 -> 79 | `gl2vsc.c` | 25.7 ms (run527) | 56.8 ms |
| Strips joined into one primitive with degenerate triangles (`GL2_STRIP_JOIN`) | `gl2strip.inc` | no change: OpenLara rooms are quad soup (no shared edges) | |
| **Zero-uniform specialisation**: at link the uniform rows whose zeroing removes the most code become candidates (`GL2_SPEC_ROWS` 6); at draw the zero lanes of their current values select a cached folded variant (`vsc_fold_zero`, `GL2_SPEC_SLOTS` 8). Unused OpenLara lights (colour 0) and the identity room rotation fold away: lit room 67 -> 19-29 instructions | `gl2api.c spec_*`, `gl2vsc.c` | 19.9 ms (run529), 19.4 ms with 8 slots (run530) | 47.5 ms |
| Constant-record layout cached per program (`clay_*`), header code words not rewritten when unchanged (`gl2_hdr_memo_*`) | `gl2rt.c` | 18.3 ms (run534) | 39.3 ms |

run534: avg 18.3 ms (55 FPS), median 17.3 ms, 254 of 600 frames under 16.7 ms, 314 under 25 ms, worst
40.4 ms (turning to face the whole big room, 288 draws). The image is unchanged against ANGLE
(emulator frame 4: 5.7 portrait, 6.2 landscape).

Exactness: `python3 NanoOS/runtime/gl2/vs_opt_check.py vs fs ...` runs the unoptimised, optimised and
every specialised variant of each program in a VGP model on random inputs; all OpenLara programs match
bit for bit (trials where the base program overflows to inf are skipped: 0*inf).

A GL2_EGL_SERVICE_US of 4 ms (display push stepped half as often) cut 0.6 ms per frame but produced a
413 ms stall in run533; it stays at 2 ms.

Where the remaining average frame goes (run532): CPU waiting on the FIFO 7.7 ms (rooms 3.3, entities 4.4),
OpenLara's own render CPU 4.2 ms, runtime per-draw CPU 2.4 ms (header build 0.9, header compare 0.5),
display servicing 1.4 ms, update 1.2 ms, swap 1.4 ms. **These run one after the other**: the next lever is
overlapping CPU work with GPU vertex processing (a frame-sized command buffer fed to the TA FIFO by DMA or
an interrupt while the CPU continues), which would bring the average under 16.7 ms and roughly halve the
heavy views.

## 9. CPU/GPU overlap attempts (runs 535-538)

Goal: let the GPU keep consuming vertex words while the CPU runs the application, instead of the
CPU spinning on the full 123-word TA FIFO and the GPU idling during application work.

**Flow-controlled DMA (kernel `MBX_FIFO_DMA 2`, API v24 `mbx_submit_parts_async`)**: the header goes
through the CPU, the vertex span is streamed by DMA engine 2 channel 0 in memory-to-peripheral mode,
the call returns and the doorbell is rung at the next FIFO access (`fa_sync`). The channel
configuration follows RetailOS's driver (config = flow << 11 | dest request << 6 | src request << 1 |
0x8001; the GPU is engine 2 request 1). Result: **the GPU never raised its DMA request** with any of
six configurations (runs 535/536: count untouched, no DMA error, FIFO empty, TC never set). RetailOS
must enable the request somewhere not yet found. The timeout/overflow abort, scene failure and
recovery path worked (one recovered fault, then CPU fallback). Off by default (`MBX_FIFO_DMA 0`).

**Software queue (`MBX_FIFO_QUEUE`)**: async draws are queued in the kernel and fed without waiting
whenever the application calls into GL (`mbx_fifo_pump` from every `gl2std` entry). Correct on the
device, but **no gain** (run537: walk 18.3 ms either way): a pump can only top the FIFO up, the GPU
drains it in ~25 us, and the application's CPU stretches between GL calls are far longer. Its 140 KB
of kernel BSS also broke the NanoEmu boot, so it is compiled out (`MBX_FIFO_QUEUE 0`) and the v24 calls
fall back to synchronous submission (`GL2_NANO_ASYNC_FIFO 0` in the runtime).

**Is there a DMA enable to find? No.** A read-only dump of the running RetailOS (`hwbridge/dump_ram.py`,
0x08000000-0x08A00000) confirms the firmware's DMA tables: request 30 = engine 2 request 1 at
peripheral address 0x3b000000 (table at 0x0890c170, channel start 0x080b1cb4, config = flow |
dest req << 6 | src req << 1 | 0x8001). But the earlier stock-path trace
(`hw-reports/native-submit-trace.md`) shows **RetailOS feeds the 3D FIFO with CPU stores** (credit
from +0x12c occupancy, 123 - occupancy), run155 already saw the hardware-paced request time out, and
run158 showed 0x3b000000 is not a 3D FIFO alias. The 0x3b800000 literals in the firmware are the
float 1/256, not FIFO pointers. Software-paced DMA works but only in credit-sized pieces and was
slower than CPU stores (runs 156-163).

What would work: something that refills the FIFO while application code runs: an interrupt-driven feeder (a timer every ~20 us topping
the FIFO up from the queue; NanoOS currently runs with CPU interrupts masked). Expected gain from the
profile: up to ~6 ms of the 18.3 ms average frame.

## 10. FIQ-fed FIFO with DMA copying (runs 539-546)

The kernel now feeds the TA FIFO from a queue while the application runs (all generic, kernel API v24):
- `mbx_submit_parts_async` queues cached draws (header words copied, vertex span referenced);
  every synchronous FIFO use and the EOL drain the queue first, so packet order and one doorbell per
  packet hold. `GL2_NANO_ASYNC_FIFO 1` uses it for converted-vertex-cache spans.
- **FIQ feeder** (`MBX_FIFO_FIQ`): timer channel B (3 MHz, VIC IRQ 8, set up as RetailOS's scheduler
  tick) is routed to the CPU's FIQ line only (VIC INTSELECT bit 8), so the IRQ sources RetailOS left
  enabled stay masked. A small vector table (`src/fiq.s`, installed through VBAR while a game runs;
  other exceptions go to the previous vectors and are recorded in `nano_exc_info`) calls
  `nano_fiq_handler`, which tops the FIFO up every 10 us unless main code is in a FIFO section.
  First boot faulted because ARM helpers lacked `.type %function` (Thumb `bl` into ARM code).
- **DMA copying** (`MBX_FIFO_DMA 3`): each top-up hands the FIFO's free room to DMA engine 2 channel 0
  (memory to memory, source AHB master 2), so the CPU only programs the transfer.

Measurement that changed the plan: sampling FIFO occupancy every tick showed the FIFO **empty 78% of the
time** (run540): the frame is CPU-bound, so feeding alone could not help; moving the copying off the CPU did.

| Run | Change | Walk avg | median | frames < 16.7 ms | p99 |
|---|---|---|---|---|---|
| 534/538 | before | 18.3 ms | 17.3 | 254 / 600 | 39 ms |
| 539 | FIQ feeder, CPU copies | 18.5 ms | 17.7 | 254 | 40 ms |
| 541 | + DMA copying, 20 us | 17.3 ms | 16.2 | 407 | 35 ms |
| 543 | 10 us period | **17.1 ms** | **15.7** | **440** | 36 ms |
| 544 | 5 us period | 17.7 ms | 16.5 | 395 | 38 ms |

**Texture filtering:** OpenLara's port forced LOW (nearest) in `Game::init`; the Nano build now lets the
launcher choose (`LARA_FILTER`, playable default 1 = bilinear). Bilinear costs nothing (run546: 17.1 ms).
Trilinear (TSP filter 0xce) faulted on every frame on hardware (run545, 300 recoveries), so the runtime
samples the nearest mip level instead (`GL2_TRILINEAR 0`). Shadows above LOW (shadow maps) and per-pixel
specular are not possible on the MBX combiner; OpenLara's LOW blob shadows already render.

## 11. Distance fog (2026-11-18, landscape walk, stock clocks)

OpenLara's LOW shaders fade to the level's fog colour with `mix(uFogParams.xyz, color, f)`, `f` a per-vertex
`1/exp(distance)`. The fragment stripper used to drop that `mix` (no fog at all). It now folds `f` per vertex into
the colour (`gl2_fs_entry.fog_*`, `GL2_FOLD_FOG`): rooms read it from `vDiffuse.w`, entities from `vNormal.w`.
Exact for black fog (TR1 levels 1, 2, 3B, 10A); coloured fog (sandy/green/red/mist, at most 0.25) fades to black.
Emulator frame 4 against ANGLE: mean diff 6.4 -> 2.9, pixels off 1.0% -> 0.3%.

Cost was +11 VGP instructions per entity vertex (+1.4 ms). Three exact compiler rewrites in `gl2vsc.c` cut it to +8:
value-range tracking (`>= 0`, `<= 1` through constants, `length`, `v.v`, `exp2`, products), `1/exp2(y)` -> one EX2 of
`-y`, and `max(x,0)`/`min(x,1)` dropped when the range is known (so `clamp(1/exp(len), 0, 1)` costs one EX2).
`vs_opt_check.py` reports max diff 0 on every OpenLara program.

| Run | avg | median |
|---|---|---|
| run-fog-nofog (`GL2_FOLD_FOG=0`) | 17.6 ms | 16.4 ms |
| run-fog-fog (first fold) | 19.0 ms | 18.5 ms |
| run-fog-fog2 (with the rewrites) | **18.3 ms** | 17.4 ms |

The VGP has a fog output (slot 2); if the TSP applies it with a fog colour, coloured fog would become exact and
the colour multiply free. Not explored yet.

## 12. OpenLara detail tiers above LOW (investigation, 2026-11-18)

`Game::init` loaded the saved settings over the launcher's choice; Nano builds now take lighting/shadows/water
from `nano7_detail[]` (`-DLARA_LIGHTING/-DLARA_SHADOWS/-DLARA_WATER`, default 0 = LOW; `tools/host_ref` reads
`HOST_LIGHTING/HOST_SHADOWS/HOST_WATER`). ANGLE frame 4 (landscape, bilinear) against LOW, and what the NanoOS GL2
compilers make of each tier's new shaders:

| Tier | Adds | Frame 4 vs LOW | NanoOS today |
|---|---|---|---|
| Lighting HIGH | `OPT_AMBIENT`: per-entity ambient cube (vertex shader) + `AmbientCache` | 0.4 (Lara tinted/darkened by her surroundings) | vertex code compiles (137-151 instr). The cache renders the room 6x per new sector into 64x64 FBOs (CPU-rasterised on NanoOS), downsamples with `FILTER_DOWNSAMPLE` (loops: fragment compile fails) to 1x1 and reads back: needs fragment loops, <16 px FBO readback, and a GPU FBO path to avoid per-sector stalls |
| Shadows MEDIUM | `OPT_SHADOW`: shadow map, per-pixel depth compare | 1.4 | impossible as written on the MBX combiner (vertex output slots and fragment varyings run out; no per-pixel compare). A projected shadow texture (two-stage T0*T1 with per-vertex projected UVs) would be an engine-side rewrite |
| Shadows HIGH | + `OPT_CONTACT` contact AO (per-pixel loop over 15 contacts) | 2.2 | same, plus per-pixel loop |
| Water MEDIUM | `OPT_UNDERWATER_FOG` (per-pixel underwater fog + specular) + `WaterCache` surface effects | 0 here (no water in view) | fog part foldable per vertex; specular per pixel not possible; water surface simulate/caustics shaders fail (multi-sampler, sampler arithmetic) |
| Water HIGH | + `OPT_CAUSTICS` (caustics texture) | 0 here | caustics texture comes from the water simulation (fails) |

### 12a. Lighting HIGH on the Nano (2026-11-18)

The ambient cube per map sector (`AmbientCache`) is now filled one of three ways (`nano7AmbientMode`, `cache.h`):
- 0: OpenLara's own path. Needs `FILTER_DOWNSAMPLE` (fragment loops, offset taps, division: not compiled by NanoOS).
- 1: the six 64x64 environment views rendered on the GPU (render-to-texture), read back after each face, and
  downsampled on the CPU with FILTER_DOWNSAMPLE's exact arithmetic. ANGLE: identical to OpenLara's shader path.
  On the iPod each new sector costs 6 synchronous GPU scenes: walk frames of 0.8-5 s (run-amb-li2). Not usable.
- 2 (default): estimated on the CPU from the room's faces (texture average x baked vertex light, solid angle per
  cube-map pixel, luminance weighting as the downsample). No GPU work. Against mode 1 along the walk (147 sectors):
  mean abs error 0.016 on values averaging 0.116, scale 0.95 fitted; rendered walk frames differ by <= 0.1.

The cube is applied per vertex in the GPU vertex program (`OPT_AMBIENT`, `calcAmbient`). iPod walk (run-amb-low2
vs run-amb-est, same tree): 19.3 -> 19.5 ms average, no faults or GL errors. Playable builds default to
lighting HIGH (`LARA_LIGHTING`), the bench keeps LOW.
