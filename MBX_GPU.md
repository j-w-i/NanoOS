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
