# NanoOS Architecture, Runtime & Application SDK

## 1. System Architecture & Ownership

NanoOS is a bare-metal single-address-space kernel for the iPod Nano 7 (`S5L8740` ARM Cortex-A5 + PowerVR MBX Lite GPU) that boots via a RetailOS chainloader (`NanoOS.hbapp`) and executes standalone `NGM1` (`game.bin`) applications.

```
+-------------------------------------------------------------------+
| Application Repo (e.g. shmup-nano7, examples/hello_gles2)         |
|   Uses ONLY: nano_app.h, nano_fs.h, nano_heap.h, EGL/egl.h,       |
|              GLES2/gl2.h (no raw physical addresses or drivers)   |
+-------------------------------------------------------------------+
| NanoOS Application SDK (NanoOS/sdk/)                              |
|   - nano_app.c  : entry, input/touch mapping, NGAME_HOME exit     |
|   - nano_fs.c   : HFS+ volume mount & POSIX-like file I/O         |
|   - nano_heap.c : O(1) coalescing multi-region heap + libc shims  |
|   - app.mk / app.ld / start.S / mkgame.py                         |
+-------------------------------------------------------------------+
| Canonical Runtime Libraries (NanoOS/runtime/)                     |
|   - runtime/gl2/     : GLES2/EGL bridge, VGP compiler, gl2tex     |
|   - runtime/storage/ : SFTL, disk cache, partition, HFS+, mount   |
+-------------------------------------------------------------------+
| NanoOS Kernel (NanoOS/src/) — Hardware Ownership                  |
|   - nand.c (FMC/PPN), mbx.c (TA/ISP/TSP), lcd.c (scanout),        |
|     touch.c, buttons.c, mmu.c (memmap.h), game.c (NGAME ABI v23)  |
+-------------------------------------------------------------------+
```

### Hardware Ownership Boundaries
- **Kernel (`NanoOS/src/`):** Exclusively owns MMU/cache configuration (`memmap.h`), NAND FMC/PPN transport (`nand.c`), MBX GPU command submission/IRQs (`mbx.c`), LCD framebuffer DMA & vsync (`lcd.c`), touchscreen (`touch.c`), and physical buttons (`buttons.c`).
- **System Services (`gameabi.h`):** `NGAME_ABI = 1`, services table `v23`. Applications receive `const struct nanoos_api *api` at entry and never program hardware registers directly.
- **App-linked Runtime (`NanoOS/runtime/`, `NanoOS/sdk/`):** Statically linked into each `game.bin` (`MM_GAME = 0x0b000000`) so `gl2` command-buffer building, VGP shader compilation, and HFS+/SFTL read caching run in the application's CPU context without kernel bloat or driver duplication.

## 2. 64 MiB DRAM Memory Map (`src/memmap.h`)

| Region | Address Range | Attributes | Owner |
|---|---|---|---|
| `MM_KERNEL` | `0x08000000..0x08200000` | Cached | NanoOS kernel `.text/.data/.bss`, MMU tables, stack |
| `MM_LCD` | `0x08200000..0x08300000` | Uncached | Double-buffered `240x432` ARGB8888 LCD scanout |
| `MM_DMA` | `0x08500000..0x08800000` | Uncached | Kernel `ppn_dma`, MBX parameter/PB tables |
| `MM_GAME_GPU` | `0x08a00000..0x0a000000` | Uncached | SDK-managed GPU texture arena (`egl_nano.c` / `gl2nano.c`) |
| `MM_GAME_HEAP` | `0x0a000000..0x0b000000` | Cached | `ns_game` (`0x0a400000`, ~1.5 MiB) + SDK CPU heap (`nano_heap.c`) |
| `MM_GAME` | `0x0b000000..0x0ba00000` | Cached | `game.bin` image (`text/data/bss`) + tail heap |
| `MM_GAME_HEAP2` | `0x0ba20000..0x0be40000` | Cached | Secondary SDK CPU heap region (clear of report mirrors) |

## 3. GLES 2.0 Reimplementation, Shader Compiler & Texture Formats

- **GLES 2.0 & EGL Implementation (`NanoOS/runtime/gl2/`):**
  - `gl2api.c` / `gl2api.h` + `gl2soft.inc`: GLES 2.0 state machine, buffer/texture/shader/program objects, and single-pass TSP combiner lowering (`C`, `P`, `T`, `T*C`, `T*P`) with software fallback only for unsupported fragment shaders.
  - `gl2rt.c` / `gl2rt.h`: Native PowerVR MBX Lite TA/TSP/ISP command-list builder.
  - `gl2vsc.c` / `gl2vsc.h`: GLSL ES 1.00 vertex shader compiler emitting VGP microcode.
  - `gl2tex.c`: Mipmap & texture upload/swizzle (`ARGB8888`, `RGB565`, `ARGB4444`, `ARGB1555`, `AL88`, and `PVRTC4`).
  - `gl2nano.c` / `gl2nano.h`: Hardware MBX Lite submission, sync, and vsync swap backend.
  - `std/gl2std.c`, `std/egl_nano.c`, `std/GLES2/gl2.h`, `std/EGL/egl.h`: Standard `gl*` and `egl*` C API wrappers and headers used by applications.
- **PVRTC 4bpp Hardware Verification (`texpvrtc`, `run398`/`run399`):**
  - Physical Nano 7 silicon testing revealed that MBX TSP format `0x20000` decodes as **2bpp** (8×4 blocks), whereas `0x38000` (`fmt = 0x38`) decodes **4bpp PVRTC** (4×4 blocks) for both opaque (`RGB554`/`RGB555`) and alpha (`ARGB3443`/`ARGB3444`) blocks.
  - Block order is PowerVR Morton twiddle (`Y` in even bits `0x5555`, `X` in odd bits `0xAAAA`).
  - `glCompressedTexImage2D` (`0x8C00` RGB, `0x8C02` RGBA) supports full mip chains down to `1x1` (`max(w*h/2, 32)` bytes per level).
- **Supported Native Texture Formats:** `ARGB8888` (`0x19`), `RGB565` (`0x05`), `ARGB4444` (`0x09`), `ARGB1555` (`0x01`), `AL88` (`0x11`), and `PVRTC4` (`0x38`). `glTexImage2D` supports mip levels up to `GL2_MAX_TEXTURE_SIZE`.

## 4. Storage, NAND & Filesystem Optimisations

- **NAND PPN Driver (`src/nand.c`):** Removed redundant 4,096-word uncached `ppn_dma` pre-fill and replaced single-word copies with `burst_copy_4096()` (8-word unrolled transfers).
- **SFTL Mount (`runtime/storage/mount.c`):** Non-recovery `survey()` reads only the first usable bank of `page 0` per block (~2,084 vs ~8,336 reads), and `erased()` checks the 16-word spare record before scanning the 16 KiB page buffer.
- **Disk Cache (`runtime/storage/disk.c`):** 16-page (256 KiB) LRU sector cache + 32-bit `fast_copy()`.
- **HFS+ Catalog Lookup (`runtime/storage/hfs.c`):** Index-node binary search from `cat_tree.root` (`OPEN_COLD`: `299.26 ms -> 28.81 ms`, with cache `13.01 ms`; `OPEN_WARM`: `1.15 ms`).

## 5. Application SDK Reference (`NanoOS/sdk/`)

Include `<nano_app.h>`, `<nano_fs.h>`, `<nano_heap.h>`, `<EGL/egl.h>`, `<GLES2/gl2.h>`:

- **Lifecycle & Input (`nano_app.h`):**
  - `nano_app_init(api)` — binds `NGAME` API, initializes EGL platform & input state.
  - `nano_app_set_orientation(NANO_ORIENT_LANDSCAPE)` (home button on the right) or `NANO_ORIENT_LANDSCAPE_LEFT` — call before `eglCreateWindowSurface`. The window surface becomes 432x240 and the default framebuffer (viewport, scissor, `glReadPixels`) is landscape; the GL2 runtime rotates `gl_Position` onto the portrait panel inside the GPU header (x/y swap on the output-0 writes + one negated viewport scale), so there is **no per-vertex cost**. FBOs are not rotated. `nano_app_screen_w()/_h()` give the logical size. Not supported: CPU-rasterised (software) window frames in landscape (they report `GL2_NANO_UNSUPPORTED`). Lower level: `gl2_egl_set_orientation()` in `gl2_egl.h`.
  - `nano_app_poll_input(&in)` — polls buttons (`home_pressed` via `NGAME_HOME`) and maps the raw touch panel to display coordinates for the current orientation (portrait `(0..239, 0..431)`, landscape `(0..431, 0..239)`, top-left origin) with `pressed`/`moved`/`released` edges and `prev_x`/`prev_y` for drag deltas.
  - `nano_app_usec()`, `nano_app_dcache_clean()`, `nano_app_gfx_stats()`.
- **Filesystem (`nano_fs.h`):**
  - `nano_fs_mount()`, `nano_fs_open(path)`, `nano_fs_read()`, `nano_fs_pread()`, `nano_fs_seek()`, `nano_fs_tell()`, `nano_fs_close()`, `nano_fs_read_all()`.
- **Memory (`nano_heap.h` + `sdk/include/libc/`):**
  - `malloc`, `calloc`, `realloc`, `free` with O(1) doubly-linked coalescing and next-fit rover (~22.4 MiB usable across `MM_GAME_HEAP`, `MM_GAME` tail, and `MM_GAME_HEAP2`), plus `nano_heap_stats()`.

## 6. Build & Deployment Workflow

```sh
# 1. Build NanoOS kernel
make -C NanoOS

# 2. Build an application (default: examples/hello_gles2)
make -C NanoOS app
make -C NanoOS app APP_DIR=/path/to/external-app

# 3. Build RetailOS chainloader (NanoOS.hbapp)
make -C NanoOS chainloader NANOAPPS=/path/to/nano7kit/nanoapps
```

## 7. Measured Hardware Benchmarks (Nano 7)

- **Mount time (`US[0]`):** `19.36 s` (`run401`) → `7.86 s` (`run402`, 1-bank `survey()`) → **`4.97 s`** (`run403`, + `nand.c` burst/DMA fix).
- **SHMUP `act1.scene` load (`US[3]`):** `5.85 s` (`run400`, PNGs) → **`0.85 s`** (`run403`, offline `.ntx` PVRTC 4bpp + native formats).
- **SHMUP `act1.scene` sustained frame rate (`run403`):** `1,505` GPU frames, `software=0`, `fault=0`, `glerr=0`, median `16.83 ms` (`~59.4 FPS`).

## 7a. Performance playbook (READ BEFORE OPTIMISING; measured on hardware)

1. **The reference result needs no overclocking.** `archive/gl2rt/gamebench` (release `run227`) renders 40,000 triangles
   at **18.11 ms/frame (~55 FPS) at stock clocks**. It gets there through: (a) **queued frames** (`GAME_QUEUE`,
   `GAME_QUEUE_DISPLAY`: three offscreen 240x432 buffers, `gl2_nano_end_queued()` + `gl2_nano_present_buffer()`, so CPU
   submission of frame N+1 overlaps ISP rendering and display DMA of frame N; the synchronous path of the same scene is
   52.6 ms), (b) **hardware back-face culling** (`glEnable(GL_CULL_FACE)` is honoured in the packet header, see `gl2api.c`),
   (c) **triangle strips** (about 1 vertex per triangle) and (d) a **trivial vertex program**. See
   `archive/gl2rt/QUEUED_BACKEND.md`.
2. **Frame queuing is built into the EGL window path** (`runtime/gl2/std/egl_nano.c`): when the kernel reports
   `mbx_queue_capacity()` and API table >= 23, `eglSwapBuffers` renders into three buffers placed after the texture
   arena, queues the frame and presents the retired one by display DMA. Define `GL2_EGL_NO_QUEUE` to get the old
   synchronous back-buffer path. **`glReadPixels` (and any FBO switch) ends the open frame synchronously**, so a bench that
   reads pixels every frame never exercises the queue: runs 488-495 measured that, not queuing. The display push only
   advances inside `lcd_service()`; `ensure_frame()` now steps it from draw calls (at most every 2 ms), otherwise the next
   swap blocked ~13.6 ms on it (run495 swap 13.6 ms/frame -> run496 0.5 ms/frame).
3. **Vertex-feed cost scales with the compiled vertex-program length, about 16 ns per VGP instruction per vertex.**
   OpenLara room/entity programs are 135/146 instructions; position-only versions cost the same vertex data but the FIFO
   submit time for 67.6k vertices fell from 213 ms to 71 ms (3.15 -> 1.05 microseconds per vertex, run487 vs run486).
   Earlier claims that submit cost is "independent of vertex-shader length" were wrong (the test only trimmed ~20%).
   Cheapest wins are therefore: fewer vertices (strips, culling, merged draws) and fewer VGP instructions
   (`gl2vsc.c` copy-propagation/MAD fusion already shave 165 -> 135). Cache per-draw uniform-only work on the CPU.
4. **Do not spend time on the GPU clock field.** CLKCON `+0x1c[29:16]` accepts PLL-B/1 (about 266 MHz, readback
   `0x2000....`) with no change in any timing (run489 = run488). Slowing it to PLL-B/8 (`0x2007....`) did not slow the
   frame either; that run failed NAND mount (-8), cause unknown, so treat the field as unrelated to the VGP rate. The
   CPU MAX profile (PLL-A 532 vs stock 342, run487 vs run479) cut game update 10.8 -> 6.9 ms but left render time
   unchanged (79.9 -> 78.9 ms): render is bound by GPU vertex consumption, not the CPU.
5. **Backend changes that measurably helped OpenLara (image bit-identical, runs 492-495):** `cross()` compiled as a
   vector expression (room VS 135 -> 115 instructions, submit time -12%, 78.5 -> 74.5 ms/frame), and unread uniform rows
   are no longer sent in every draw header (`gl2_program_scan_constants`; entity VS declares 77 rows, reads ~15):
   74.5 -> 71.4 ms/frame. `vsc_eq`-style emulator checks (old vs new compile, 2000 random vertices) must stay at 0 mismatches.
6. **Superseded by 8.** Where 71 ms of render went (per frame, run495; whole frame was ~95 ms incl. update and swap): rooms ~26 ms (7.4k vertices, 5 draws; FIFO submit 2.6 us/vertex),
   entities ~38 ms (5.5k vertices in 95 draws: submit ~16 ms, gather ~14 ms, app CPU ~14 ms, ~400 us per draw),
   everything else ~7 ms. Per-draw overhead for small meshes is as important as vertex count: merging or culling entity
   draws is the largest remaining lever, then fewer/cheaper VS instructions.
7. Useful phase probes (`hw-reports/run487.txt` layout): rooms ~40% and entities+sky ~60% of the opaque phase;
   opaque ~90% of the frame; ISP wait only ~4 ms per frame. Cull rooms by portal rectangle on the CPU: the backend
   ignores partial scissors only because `GL2_SCISSOR_IGNORE_DRAWS` is set for OpenLara.
8. **Submission is GPU-bound; keep the FIFO fed (run479 FIFO profile, runs 496-498).** Mean FIFO occupancy during
   OpenLara submit is 119 of 123 words: the CPU waits for the GPU. Any CPU work between packets idles the GPU once the
   ~25 us FIFO backlog drains. Converted triangle lists are therefore converted and submitted in `GL2_SUBMIT_CHUNK`
   (default 12) vertex sub-packets instead of 256 (header cache resends only the two primitive words): frame
   77.5 -> 68.5 ms, image bit-identical. Bench numbers (`LARA_BENCH`): frame 0 does all readbacks and is untimed;
   frames 1-5 are timed. Per frame (run498): update 6.8 ms, render 60.9 ms (rooms 21.6, entities 35.4, transparent 3.1),
   swap 0.5 ms; FIFO submit 27.9 ms for 13.5k vertices (indexed lists are expanded: 3 vertices per triangle). GL calls
   outside draws cost ~0.3 ms; ~14 ms of the entity phase is OpenLara CPU (animation/joints), not the backend.
   Next levers: fewer vertices per triangle (strips from static index buffers), then app-side entity CPU work.
11. **Correction (2026-11-17): the 60 FPS result rendered OpenLara wrongly.** An ANGLE GLES 2.0 reference
   (`openlara-nano7/tools/host_ref`) showed smeared textures, no lighting, black entities and missing UI that
   the 30x27 grid check could not see. Six generic runtime bugs were fixed (`docs/PERFORMANCE.md` section 6);
   correct rendering costs 31.4 ms/frame (32 FPS) at stock clocks (run522). Always diff against the reference.
10. **OpenLara reaches 60 FPS at stock clocks (run514/517: 16.7 ms/frame, image bit-identical to run487)** after two more
   generic steps: vertex inputs the pruned GPU code no longer reads are not sent (room vertex 15 -> 7 words, run513
   25.2 ms), and static list draws stay lists (one cached packet per draw) unless real strips are cheaper: each packet
   costs ~4 us, so per-quad strip packets lost once vertices got cheap (run514). Full guide: `docs/PERFORMANCE.md`.
9. **OpenLara reaches 35 FPS at stock clocks (run512: 28.4 ms/frame, image bit-identical to run487).** Steps, each measured:
   - **Clocks (run499):** stock clocks submit faster than CPU MAX (FIFO submit 20.1 vs 27.9 ms): MAX lowers the bus
     dividers (133/66/133 vs stock 171/85/171 MHz) and vertex throughput follows the bus, not the GPU field.
   - **Per-strip packets (`gl2strip.inc`, run500, 68.5 -> 63.1 ms):** static `GL_TRIANGLES` element draws (opaque, depth
     written) are converted once to strips that keep each triangle's winding; each strip is its own primitive packet
     (no joining vertices). OpenLara gives every face its own vertices, so a quad costs 4 vertex runs instead of 6.
   - **GPU-only vertex-program pruning (`vsc_prune`, `native_outputs`, run504, -> 53.7 ms):** the native TSP reads only
     the varyings of its combiner plan (slots 1/3, plus 4 with two stages); position and clip (0, 5) always. Everything
     else (per-vertex lighting in slots 2/4/6/7) is removed from a GPU copy; the software path keeps the full code. Room VS
     115 -> 29 instructions. **Output 4 must still be written** (zero-filled): leaving it unwritten hung the device (run501).
   - **Converted-vertex cache (`GL2_VCACHE_WORDS`, opt-in, OpenLara 2 MB, runs 505/511):** native vertex words of static
     draws are cached in strip order and submitted straight from memory.
   - **SDK math (run508):** `sdk/src/nano_heap.c` was built soft-float and `sqrtf` ran 24 software-double Newton steps;
     OpenLara animation spent 13.5 ms/frame there. It is now built with VFP and uses `vsqrt` (joints 13.5 -> 0.6 ms,
     update 5.4 -> 0.6 ms, level load 6.2 -> 4.5 s).
   Per frame now (stock): update 0.6 ms, render 26.8 ms (FIFO submit ~17.5 ms, GPU/bus-bound), swap 0.4 ms. CPU MAX is
   no longer needed. Remaining lever: overlapping CPU work with FIFO feeding (needs an interrupt- or DMA-fed FIFO).

## 8. Key Bugs Resolved

1. **PVRTC 4bpp format descriptor (`0x20000` vs `0x38000`):** Hardware probe `run398` caught that `0x20000` is 2bpp on MBX Lite; `0x38000` is 4bpp for both RGB and RGBA.
2. **Mip level > 6 rejection:** `image_format()` in `gl2tex.c` capped `level > 6` (64×64 max), rejecting 256×256 mip levels `7..8`; updated to `(1u << level) <= GL2_MAX_TEXTURE_SIZE`.
3. **MD5 entity token overwrite (`entities.c`):** `ENT_LoadEntity()` overwrote `LE_getCurrentToken()` before `ENT_Put()`, causing `cityBlue.obj.md5mesh` to be opened 48× instead of once.
4. **Shader/program bounds (`gl2api.c`):** `GL2_MAX_PROGRAMS`/`GL2_MAX_SHADERS` had hardcoded bounds in `shp()`/`glIsShader()`/`glUseProgram()`.
5. **Touch drag (`commScale == 0`):** SHMUP required `SRC_OnResizeScreen(240, 432)` before `IO_EVENT_MOVED` coordinates scaled properly.
6. **`glViewport` origin ignored:** x/y were dropped, so a letterboxed game (SHMUP: 240x360 at y = 36) drew from the top edge. The header viewport transform (`gl2rt.c`) and the CPU rasterizer (`gl2soft.inc`) now map NDC into the viewport rectangle, portrait and both landscape rotations.
7. **No viewport / scissor clipping:** the TA only clips to the render target, so geometry outside a smaller viewport drew into the bars (SHMUP's baked visibility culling showed there). Hardware probe (`hw-reports/run-clipprobe-1..3`, `run-vclip-1`): the TA clips on VGP output `5.y` always (the far plane `w - z`) and on `5.z` when control-word bit 20 is set; `5.w` is never used, and stage bit 5 turns the `5.z` plane off for SDK code. Every vertex program now computes `5.y` / `5.z` as `position . plane` from two literal rows the runtime rewrites per viewport / scissor (`gl2vsc.c emit_clip`, `gl2api.c sync_prog_target`): one edge inside the target goes on `5.z`, a second on `5.y` (that draw then has no far-plane clip; the depth test still rejects most of what lies beyond). A third / fourth edge stays unclipped on the GPU. Cost: one VGP instruction per vertex. `examples/hello_gles2` `-DHELLO_VIEWPORT_CLIP` checks it on hardware. NanoEmu does not model these planes yet.
8. **Fragment shaders the GPU cannot run compiled anyway:** they linked "software-only" and GPU frames dropped every draw. `glCompileShader` now fails them on GPU contexts (`GL2_SOFTWARE_FS=1` accepts them), so apps take their own fallback (SHMUP's `kUberFsMbx`).
9. **Texture uploads refused mid-frame:** any upload while a frame had draws queued failed, so SHMUP's level textures never loaded. Textures no queued draw samples can now be written; storage from the arena is never shared while referenced (`gl2tex.c tex_write_blocked`).

## 9. Verified vs Experimental / Limitations

- **Verified on physical Nano 7:** Kernel + `NGAME` ABI `v23`, EGL window surface + vsync presentation, VGP vertex compilation, single-pass TSP combiners (`C/P/T/T*C/T*P`), `ARGB8888`/`RGB565`/`ARGB4444`/`PVRTC4` (`0x38000`) + mipmaps, SFTL/HFS+ read-only mount & streaming, touch + `NGAME_HOME` exit.
- **Limitations:** Multi-pass arbitrary fragment shaders fall back to software or are rejected (`kUberVsMbx`/`kUberFsMbx` lowers per-vertex lighting); `NanoOS` app SDK filesystem is read-only by default (persistent saves require preallocated 32 KiB save files via `sdk/src/storage/save.c`); `test_gl2api.py` requires the parent `gles2shim`/`glsl2vgp` models.

## 10. Regression Testing

```sh
# Host unit & sanitizer tests (inside NanoOS)
python3 NanoOS/runtime/gl2/test_gl2pvrtc.py
python3 NanoOS/runtime/gl2/std/test_gl2stdapp.py
python3 NanoOS/runtime/storage/test_hfs.py
python3 NanoOS/runtime/storage/test_storage.py

# Physical Nano 7 automated test (with run227 NanoOS.hbapp & Disk Mode)
python3 bundle_native.py --fixture texpvrtc --out hwbridge/build/payload.bin
python3 hwbridge/run_native.py --out hw-reports/runNN.txt
```
