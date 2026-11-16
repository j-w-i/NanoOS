# NanoOS v0.0.4 beta

Freestanding ARM kernel for the iPod nano 7G, linked at `0x08000000`, with
MMU and D-cache enabled (flat 1:1 map; LCD, GPU test buffers, DMA arena and
all MMIO stay uncached/strongly-ordered). The 432x240 landscape UI rotates
into the native 240x432 LCD scanout at `0x08200000`. Requires the ARM
bare-metal toolchain, Python 3/Pillow, and the sibling NanoApps SDK.

## Drivers and DevMode

Hardware driver implementations cover LCD, touch, buttons, audio, NAND,
USB, Bluetooth HID, and MBX GPU, all verified on the real device:

- **Touch** works on hardware, including after the MMU/D-cache enable, via
  SPI chip-select pacing (setup/hold/high delays) and a query-to-fetch gap
  on the Grape touch controller. The DevMode Drivers page shows live touch
  counters (queries, ATN-low polls, reports, timeouts) for diagnosis.
- **Bluetooth** pairs with a real DualSense gamepad; the pad drives the
  ViceNano scene on the device.
- **Audio** probes the codec and feeds a PCM tone at 44.1 kHz.
- **USB** reads the OTG core ID.
- **NAND** (Apple PPN via the FMC at `0x38a00000`) reads pages on
  hardware. After the RetailOS handoff the FMC register space is
  unpowered, so the driver runs a staged recovery (controller reset, FMC
  clock enable, PSC power-domain wake, candidate clock gates), then
  performs the RetailOS read sequence (0x0A command, 4-byte address,
  SET FEATURES power state) with a terminating power-mode poll and a
  full D-cache clean so reports survive the reboot. Refuses flash
  writes.
- **MBX GPU** renders scenes on hardware; see below.

Drivers are polled with bounded waits. These are development interfaces,
not a complete peripheral stack; see [DRIVER_PORT.md](DRIVER_PORT.md).

Every boot starts with a STOCK/SAFE/MAX clock selector. Nominal CPU/GPU
rates are 342/133, 342/171, and 342/266 MHz respectively. Selection lasts
for this boot only. The profile names do not establish hardware stability.

Volume down/up selects; Home (or both volume buttons together) confirms.
NanoEmu also maps Left/Right and Enter to these controls. The menu contains
StockOS, ViceNano, PS1Emu, Settings, and DevMode. Home returns from a
scene; StockOS requests a reset to RetailOS. DevMode shows clocks, memory,
FPS, and diagnostics; Vol+ opens Drivers, then the MBX GPU test; Vol-
returns to stats.

## ViceNano M8 on hardware

The integrated build links ViceNano M8 into the full NanoOS kernel. On the
real iPod the MBX renders the M8 scenes (clear, triangle, textured quad,
and the VC street-corner world preview) at about 70 fps with the MMU and
D-cache on; a hardware report (`R80.txt`) is written to the iPod disk after
each run. The scene is controllable with the DualSense gamepad or touch.

```sh
python3 nanoos/installer/build_payload.py
python3 nanoos/installer/patch_disk_emu.py --mode all
python3 nanoos/installer/list_device_files.py
```

## Game loader (reVC stage 2)

NanoOS also loads standalone game binaries (NGM1 images) through the
NanoOSLoader RetailOS app: the loader scatters 256 KB chunks through the
RetailOS heap into the game window at `0x08800000`-`0x0c000000`, then the
kernel hands over a versioned services table (clock, cache maintenance,
input, MBX driver). The first real game is a carved reVC (Vice City)
build; see the ViceNano repository. The stage-1 stub and old loaders stay
compatible.

`make -C nanoos` builds the standalone kernel, whose ViceNano scene is a
placeholder. `nanoos/run-in-nanoemu.sh STOCK` (also SAFE/MAX) runs its
emulator regression; it does not test the integrated installer bundle.
NanoEmu needs a built `emulator/build/qemu-system-arm`, snap6 dumps, disk
and resource images. Rebuild emulator changes with `emulator/build.sh`.

See [INSTALL_IPOD.md](INSTALL_IPOD.md) for integrated bundle testing,
device file transfer, and rollback. Emulator validation is continuous;
hardware validation covers the drivers listed above, including NAND page
reads. Clock stability beyond stock profiles remains under active bring-up.

NanoOS by Tenspav. Requires the community untethered jailbreak to install.

## Consolidated GLES 2.0, NAND/Storage & Application SDK

See [docs/ARCHITECTURE_AND_SDK.md](docs/ARCHITECTURE_AND_SDK.md) for full architecture, memory map, and SDK details.

**Performance work: read [docs/PERFORMANCE.md](docs/PERFORMANCE.md) (techniques, runtime features, build knobs; OpenLara 60 FPS at stock clocks) and [section 7a of the architecture doc](docs/ARCHITECTURE_AND_SDK.md#7a-performance-playbook-read-before-optimising-measured-on-hardware) first.** The 40k-triangle 55 FPS reference (`run227`) uses stock clocks, queued frames, hardware culling, strips and a trivial vertex shader; vertex-program length drives vertex-feed cost; the GPU clock field does not.

- **GLES 2.0 + EGL Reimplementation ([`runtime/gl2/`](runtime/gl2/)):**
  - `gl2api.c` (GLES 2.0 state/API + TSP combiner lowering), `gl2rt.c` (MBX Lite TA/TSP/ISP packet builder), `gl2vsc.c` (GLSL vertex shader -> VGP microcode compiler), `gl2tex.c` (mipmaps & texture formats including PVRTC 4bpp `0x38`), `gl2nano.c` (hardware submit/vsync), and standard `gl*`/`egl*` headers & wrappers under `runtime/gl2/std/` (`GLES2/gl2.h`, `EGL/egl.h`, `gl2std.c`, `egl_nano.c`).
- **NAND PPN Driver & Storage Stack ([`src/nand.c`](src/nand.c), [`runtime/storage/`](runtime/storage/)):**
  - Burst-DMA PPN NAND page reads, 1-bank SFTL `survey()` mount, 16-page (256 KiB) LRU disk sector cache (`disk.c`), and HFS+ B-tree binary-search catalog lookup (`hfs.c`), plus preallocated save-file helpers (`sdk/src/storage/`).
- **Application SDK & Example ([`sdk/`](sdk/), [`examples/hello_gles2/`](examples/hello_gles2/)):**
  - Build standalone `NGM1` apps via `make -C NanoOS app APP_DIR=/path/to/app` using `sdk/app.mk`, `nano_app.h`, `nano_fs.h`, `nano_heap.h`, and freestanding `sdk/include/libc/`.

### Measured Hardware Performance (iPod Nano 7)

- **SFTL/HFS+ Mount (`US[0]`):** `19.36 s` → `4.95–4.97 s` (1-bank `survey()` + `nand.c` burst/DMA).
- **SHMUP `act1.scene` Asset Load (`US[3]`):** `5.85 s` (PNG) → `0.85 s` (offline `.ntx` PVRTC 4bpp + native formats) with `13.01 ms` cached cold file open (`1.15 ms` warm).
- **SHMUP `act1.scene` Sustained Rendering (1,505 frames):** `software=0`, `fault=0`, `glerr=0`, median `16.83–16.99 ms/frame` (`~59 FPS`).
- **`examples/hello_gles2` (`HELLO_BENCH`):** `30` hardware frames, `software=0`, `fault=0`.
