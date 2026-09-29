# ZU-NEXUS-AI — System architecture

Detailed description of the implemented system: the PL image chain, the memory
path, the NPU branch, the software stack and the build/boot flow. For build
instructions see [../README.md](../README.md); for the host packages see
[HOST-SETUP.md](HOST-SETUP.md).

---

## 1. Overview

The system is made of **three processing chains that run in parallel** and only
exchange data through the PS DDR. Keeping them separate is what makes the
design fast: pixels are produced, scaled and colour-converted in the FPGA, the
CPU never rewrites a full frame, and the NPU is fed over PCIe.

```
PL (FPGA)   IMX219 → CSI-2 RX → Demosaic → CSC(WB) → Gamma LUT → VPSS(scaler+CSC) → frmbuf WR
                                                                                        │ AXI HPC0 (coherent)
PS DDR      ═════════════════════ shared CMA buffers (cacheable) ══════════════════════
              │ mmap (no copy)
A53 (SW)    v4l2src → [videoconvert] → dxpreprocess → dxinfer → dxpostprocess → dxosd → appsink → Qt/Weston
                                                          │ PCIe DMA  ↑ results
PCIe                                                   DX-M1 NPU (3 cores, LPDDR5)
```

| Chain | Runs on | Cost on the A53 |
|---|---|---|
| Image chain | PL | none (DMA only) |
| Inference | DX-M1 | PCIe transfers only |
| Glue + display | A53 (GStreamer/Qt) | pre/post-processing and blit |

---

## 2. Hardware

### 2.1 Board

| Item | Value |
|---|---|
| SoC | Zynq UltraScale+ MPSoC, quad Cortex-A53 |
| Sensor | Sony **IMX219**, MIPI CSI-2, 2 lanes, RAW10, 1920×1080 (cropped from 3280×2464) |
| Sensor control | I2C, address `2-0010` |
| Accelerator | **DeepX DX-M1**, M.2 (mdot2), PCIe, 3 NPU cores, LPDDR5 3.92 GiB |
| Display | DisplayPort through the PS DPSUB |
| Power monitors | 2× **INA231** on PS I2C1: `0x40` system 5 V, `0x41` M.2 3V3 rail, 20 mΩ shunts |

### 2.2 PL blocks (Vivado block design, `vivado/ZCU-DM-X1-R3.tcl`)

| IP | Address | Configuration |
|---|---|---|
| `mipi_csi2_rx_subsyst_0` | 0x8002_0000 | 2 lanes, RAW10 |
| `v_demosaic_0` | 0x8003_0000 | RAW10 → `RBG888_1X24` |
| `v_proc_ss_2` (`80300000.csc`) | 0x8030_0000 | topology 3, **CSC-only**: white balance, brightness, contrast |
| `v_gamma_lut_0` | 0x8008_0000 | per-channel gamma |
| `v_proc_ss_0` (`80100000.vpss`) | 0x8010_0000 | topology 0, **Scaler-only** with `C_ENABLE_CSC`, polyphase: runtime resize |
| `v_frmbuf_wr_0` | 0x8004_0000 | writes frames to DDR, `MAX 1920×1080`, 1 ppc, 10-bit datapath |
| `axi_gpio_0` | 0x8006_0000 | resets of the video IPs |

The frame buffer writer master goes through `axi_smc` to **`S_AXI_HPC0_FPD`**,
the cache-coherent port served by the CCI-400. This choice is what allows the
buffers to be cacheable (see §4).

### 2.3 Frame buffer formats

The formats the IP can write are fixed at synthesis time by the
`CONFIG.HAS_*` parameters, and must be mirrored in `xlnx,vid-formats` in the
device tree. The driver only does a string comparison and **never validates
against the hardware**, so a mismatch produces corrupted frames with no error.

| DTS name | TCL parameter | V4L2 fourcc |
|---|---|---|
| `rgb888` | `HAS_BGR8` | BGR3 |
| `bgr888` | `HAS_RGB8` | RGB3 |
| `xrgb8888` | `HAS_BGRX8` | XBGR32 |
| `xbgr8888` | `HAS_RGBX8` | BGRX32 |
| `xbgr2101010` | `HAS_RGBX10` | XBGR30 |
| `uyvy` / `yuyv` | `HAS_UYVY8` / `HAS_YUYV8` | UYVY / YUYV |
| `vuy888` / `xvuy8888` | `HAS_YUV8` / `HAS_YUVX8` | VUY24 / XVUY32 |
| `nv16` / `nv12` | `HAS_Y_UV8` / `HAS_Y_UV8_420` | NV16 / NV12 |
| `xv20` / `xv15` | `HAS_Y_UV10` / `HAS_Y_UV10_420` | XV20 / XV15 |

Note the **swapped names** for the two 24-bit RGB formats: this is how the
Xilinx driver maps them, not a typo.

---

## 3. Media pipeline (V4L2)

The chain is exposed through the Media Controller API as
`/dev/media0`, with one capture node `/dev/video0` (`Video Capture
Multiplanar`) and one subdev per IP:

```
imx219 2-0010      SRGGB10_1X10 1920×1080   (crop 688,700 from 3280×2464)
80020000.csi2rx    SRGGB10_1X10 → SRGGB10_1X10
80030000.demosaic  SRGGB10_1X10 → RBG888_1X24
80300000.csc       RBG888_1X24  → RBG888_1X24     white balance
80080000.gamma     RBG888_1X24  → RBG888_1X24
80100000.vpss      RBG888_1X24 1920×1080 → RBG888_1X24 <out_w>×<out_h>   resize
vcap-imx219        /dev/video0, fourcc set with VIDIOC_S_FMT
```

Two consumers configure this chain, in the same way:

* `setup_pipeline.sh display|npu` — shell script, used by
  `video-pipeline.service` and by `test_pipeline.sh`;
* **dxpose-qt** — does it natively with ioctls (`isp.cpp`), no external process.

Entities are looked up by **suffix** (`.csi2rx`, `.demosaic`, `.csc`, `.gamma`,
`.vpss`), not by address, so a memory remap in Vivado does not break userspace.

### 3.1 ISP controls

| Control | Subdev | Driver CID | Range |
|---|---|---|---|
| White balance R/G/B | `.csc` | `V4L2_CID_XILINX_CSC_{RED,GREEN,BLUE}_GAIN` | 0…100, 50 = unity |
| Brightness, contrast | `.csc` | `V4L2_CID_XILINX_CSC_{BRIGHTNESS,CONTRAST}` | 0…100 |
| Gamma R/G/B | `.gamma` | `V4L2_CID_XILINX_GAMMA_CORR_*` | 1…40 (×10, 10 = 1.0) |
| Exposure | `imx219` | `V4L2_CID_EXPOSURE` | 4…1759 |
| Analogue / digital gain | `imx219` | `V4L2_CID_{ANALOGUE,DIGITAL}_GAIN` | 0…232 / 256…4095 |

A `VIDIOC_SUBDEV_S_FMT` on the CSC resets the colour controls to their
defaults, so controls must always be applied **after** the formats.

The CSC driver stores the gains as `stored = 2·value + 20` (value 50 → 120 =
unity) and scales the matrix coefficients proportionally. The auto white
balance in the application uses exactly this relation to convert a requested
multiplier back into a control value.

---

## 4. Memory path and cache coherency

`vb2-dma-contig` allocates the capture buffers from CMA using the **composite
device**, not the frame buffer node: `xilinx-dma.c` does
`dma->queue.dev = dma->xdev->dev`. Therefore `dma-coherent` must be declared on
the **`vcap-imx219`** node (it is also declared on the frame buffer node, which
is correct but not sufficient).

With the property in place the buffers are cacheable and coherency is
guaranteed by the hardware, because the writer is on `S_AXI_HPC0_FPD`. Measured
on this board, same test (`videoconvert` reading capture buffers):

| Buffers | 100 frames | fps |
|---|---|---|
| uncached (no `dma-coherent`) | 28.77 s | 3.5 |
| cacheable (`dma-coherent`) | 3.42 s | 29.3 |

A factor of 8.4, and the second figure is the camera frame rate, not a CPU
limit. CMA is sized with `cma=256M` on the kernel command line.

---

## 5. NPU branch (DX-M1)

### 5.1 Stack

| Layer | Package | Notes |
|---|---|---|
| PCIe driver | `dx-npu-driver` | `dxrt_driver`, `dx_dma`; device `/dev/dxrt0`, mode 0666 |
| Runtime | `dx-rt` | `libdxrt`, `dxrt-cli`, `run_model`, `dxtop`, daemon `dxrtd` |
| Firmware | `dx-fw` | `/lib/firmware/deepx/<chip>/<version>/<form>/fw.bin` |
| GStreamer | `dx-stream` | `dxpreprocess`, `dxinfer`, `dxpostprocess`, `dxosd`, `dxtracker`, `dxmsgbroker` |

Post-processing libraries (`libpostprocess_*.so`) are separate meson projects
inside the dx-stream repository; the Buildroot package cross-compiles them and
installs them in `/usr/share/gstdxstream/lib`, together with the model configs
in `/usr/share/gstdxstream/configs`.

### 5.2 Data path

`dxpreprocess` builds the input tensor (letterbox to 640×640) in a host buffer;
`dxinfer` hands it to the runtime, which DMAs it over PCIe into the module
LPDDR5, runs the model on the NPU cores and reads the outputs back.
`dxpostprocess` decodes them (NMS included) and `dxosd` draws on the frame.
The frame itself never travels over PCIe: only tensors and results do.

Element input caps, which constrain the whole design:

* `dxpreprocess`: `{ RGB, I420, NV12 }` — **not BGR**;
* `dxosd`: draws on the frame it receives, so the format is the same across the
  chain;
* `waylandsink` (wl_shm): needs 32 bpp, hence the final `videoconvert` to BGRx.

### 5.3 Timings

`dxinfer` reads `InferenceEngine::GetLatency()` on every buffer but upstream
does not expose it. The patch
`package/dx-stream/0001-dxinfer-expose-last-inference-latency-and-NPU-time.patch`
adds two read-only properties, in microseconds:

* `last-npu-time-us` — pure NPU compute time (`GetNpuInferenceTime()`);
* `last-latency-us` — full inference latency, PCIe transfers included.

The application polls them twice a second and shows a moving average in the
synoptic. The gap between the two values is the cost of the PCIe path.

### 5.4 Firmware

dx_rt 3.3.2 refuses modules with firmware older than 2.5.2 by throwing a C++
exception from inside `dxinfer`, which aborts the process. The application
therefore checks the version **before** building the pipeline
(`fwupdate.cpp`), compares it with the bundled firmware and, if the module is
older, flashes it with `dxrt-cli` while showing a notice on screen. The result
is recorded in `/var/lib/dxpose-qt/dxm1-fw-update.json`, so a reboot does not
trigger a second flash: the new firmware only becomes active after a full power
cycle, and until then the module still reports the old version.

---

## 6. Software stack

| Component | Version | Notes |
|---|---|---|
| Buildroot | 2025.02.x | `BR2_EXTERNAL` = this tree |
| Toolchain | Buildroot glibc, C++ | aarch64 |
| Kernel | Xilinx `linux-xlnx`, `xilinx-v2023.2` (6.1) | defconfig `xilinx` + `linux.fragment` |
| U-Boot | Xilinx `u-boot-xlnx`, `xilinx-v2023.2` | `xilinx_zynqmp_virt` + `uboot.fragment` |
| ATF | Buildroot `arm-trusted-firmware` | `bl31.elf`, `RESET_TO_BL31=1` |
| Init | systemd | |
| Compositor | Weston (DRM backend) | user `ked`, uid 1001 |
| Toolkit | Qt6 (Widgets, Network) | QPA `wayland` |
| Multimedia | GStreamer 1.24 | base, good (v4l2), bad (kms, waylandsink, y4m) |

### 6.1 Kernel configuration additions (`linux.fragment`)

Video (CSI-2, demosaic, gamma, VPSS scaler and CSC, frame buffer DMA), PCIe
(`PCIE_XILINX_NWL`), DisplayPort, and the INA231 power monitors
(`SENSORS_INA2XX`, `HWMON`, `I2C_CADENCE`).

### 6.2 systemd units

| Unit | Role |
|---|---|
| `weston.service` | compositor as `ked`, `--drm-device=card1` (card0 is render-only) |
| `video-pipeline.service` | `setup_pipeline.sh display` at boot, waits for `vcap-imx219` |
| `dxpose-qt.service` | the application; waits for the wayland socket, detects `WAYLAND_DISPLAY`, `StateDirectory=dxpose-qt`, reads `/etc/default/dxpose-qt` |

`dxpose-qt.service` is `PartOf=weston.service`, so it follows the compositor.

---

## 7. The dxpose-qt application

Sources in `package/dxpose-qt/src`:

| File | Content |
|---|---|
| `main.cpp` | UI, GStreamer controller, video widget, side panel, startup flow |
| `isp.cpp/.h` | media topology and ISP controls through ioctls; gray-world AWB |
| `npuinfo.cpp/.h` | parses `dxrt-cli -s` (versions, voltages, clocks, temperatures) |
| `fwupdate.cpp/.h` | firmware comparison, flashing, persistent state |
| `pipelineview.cpp/.h` | vector synoptic of the three chains |

### 7.1 Threads

The UI thread never touches GStreamer or the NPU. A worker thread owns the
pipeline, the bus poll (50 ms), the watchdog (750 ms) and the timing poll
(500 ms). Frames reach the UI as `QImage` copies through a queued connection.
Tearing down a pipeline is done on a detached thread, because
`gst_element_set_state(NULL)` can block while the streaming thread is inside an
NPU call.

### 7.2 Layout

The video (1280×720) and the synoptic (1280×200) form a single block; the
synoptic is anchored 8 px above the bottom edge and the video is centred in the
space above. The right panel takes the remaining width (640 px on a 1920×1080
screen, minimum 400). The synoptic is drawn in a logical 1280×200 coordinate
system and scaled uniformly, so it stays sharp at any size.

### 7.3 Auto white balance

Gray-world on the last displayed frame: means of R, G and B over a subsampled
grid, ignoring clipped (≥250) and near-black (≤8) pixels. Green is the fixed
reference, only red and blue gains move, so the correction does not change
overall brightness. Each step applies 25 % of the requested correction, because
the loop is closed on a frame the CSC has already corrected; a full correction
would oscillate. Disabled while the file fallback is active.

### 7.4 Source fallback

The application switches to the fallback video when the camera is not detected
at startup, when the camera pipeline errors out (the source element is named
`camsrc` so its errors can be told apart from NPU ones), when it fails to reach
PLAYING, or when frames stop arriving (4 s, or 20 s with no first frame). NPU
errors are treated differently: switching source would not help, so the process
exits and systemd restarts it. **Retry camera** re-runs detection and ISP setup
and rebuilds the pipeline.

---

## 8. Boot flow

```
BOOT.BIN ─ fsbl.elf (a53-0)         board/ked/zu-nexus/boot/
         ├ pmufw.elf (pmu)
         ├ ZCU-DM-X1-R3.bit (PL)    ← bitstream loaded by the FSBL
         ├ bl31.elf (el-3, ATF)     ← from Buildroot images/
         └ u-boot (el-2)
U-Boot → extlinux.conf → Image + system.dtb
         cmdline: root=/dev/mmcblk0p2 rw rootwait cma=256M ...
systemd → weston.service → dxpose-qt.service
```

`BOOT.BIN` is assembled by `board/ked/zu-nexus/mk-bootbin.sh` (invoked by
`post-image.sh`) using the `bootgen` binary shipped in
`board/ked/zu-nexus/boot/`, so Vitis is not needed for a plain rebuild.

SD card layout (`genimage.cfg`): FAT32 boot partition of 128 MB
(`BOOT.BIN`, `Image`, `system.dtb`, `extlinux/extlinux.conf`) and an ext4
rootfs of 1024 MB.

---

## 9. Build system notes

* **Device tree copy** — `external.mk` copies the `.dts/.dtsi` into the kernel
  and U-Boot trees from `LINUX_PRE_BUILD_HOOKS` / `UBOOT_PRE_BUILD_HOOKS`, not
  from the post-patch hooks. Patch hooks only run once, when sources are first
  extracted, so `linux-rebuild` used to silently rebuild the *old* device tree.
* **Patches** — `librdkafka` (honour `OPENSSL_NO_ENGINE`, otherwise
  `libgstdxstream.so` fails to load with `undefined symbol: ENGINE_init`) and
  `dx-stream` (expose the dxinfer timings). Kernel patches live in
  `board/ked/zu-nexus/patches/linux`.
* **dx-stream** — needs `INSTALL_STAGING`, because the post-processing
  libraries resolve `dependency('gstdxstream')` through pkg-config.
* **dxpose-qt** — `SITE_METHOD=local`: sources are re-synced on every build, so
  `dxpose-qt-rebuild` is enough after editing the code. Fallback video and logos
  are installed by post-install hooks from `media/`.
* **post-build** — checks that `/etc/localtime` exists, otherwise the board
  would silently run in UTC.

---

## 10. Measured figures

| Measurement | Value |
|---|---|
| Capture + full NPU chain, 640×640, headless | 30.15 fps, 0 dropped |
| `videoconvert` on capture buffers, cacheable | 29.3 fps (camera-bound) |
| `videoconvert` on capture buffers, uncached | 3.5 fps |
| `videoconvert` on regular memory | ~90 fps |
| CMA | 256 MB total, ~245 MB free |
| PCIe link | `LnkCap` 8 GT/s x4, `LnkSta` **2.5 GT/s x2** (x2 is by design, Gen1 is a downgrade to investigate) |

---

## 11. Open points

* PCIe link trains at Gen1 instead of Gen2: check PERST# timing, reference clock
  and signal integrity on the M.2 connector.
* With a bitstream built with `HAS_RGB8`, capture in RGB3 (`--rgb`) removes the
  last CPU conversion from the path.
* Display straight to the DP video plane (`kmssink` on plane 39) is possible —
  the plane accepts RG24 — but the DPSUB requires the layer to be exactly
  1920×1080 and the graphics plane (41) must have its alpha lowered, otherwise
  it covers the video.
