# ZU-NEXUS-AI — Buildroot BSP for IMX219 + DeepX DX-M1 on ZynqMP

`BR2_EXTERNAL` tree for the **KED ZU-NEXUS-AI** board (Zynq UltraScale+). It
contains the IMX219 image processing chain implemented in PL, the integration of
the **DeepX DX-M1** accelerator (PCIe M.2) and **dxpose-qt**, a Qt6 application
that shows the live video with inference results, ISP controls and telemetry.

## Architecture

Three parallel chains that meet only through system memory:

```
PL (FPGA)   IMX219 → CSI-2 RX → Demosaic → CSC(WB) → Gamma → VPSS(scaler+CSC) → frmbuf WR
                                                                                    │ AXI HPC0 (coherent)
PS DDR      ═══════════════════ shared CMA buffers ════════════════════════════════
              │ mmap
A53 (SW)    v4l2src → [videoconvert] → dxpreprocess → dxinfer → dxpostprocess → dxosd → appsink (Qt)
                                                          │ PCIe DMA ↑
PCIe                                                  DX-M1 NPU
```

| Block | Address | Notes |
|---|---|---|
| `mipi_csi2_rx_subsyst_0` | 0x8002_0000 | 2 lanes, RAW10 |
| `v_demosaic_0` | 0x8003_0000 | outputs `RBG888_1X24` |
| `v_frmbuf_wr_0` | 0x8004_0000 | AXI master on **S_AXI_HPC0_FPD** (coherent, CCI-400) |
| `v_gamma_lut_0` | 0x8008_0000 | |
| `v_proc_ss_0` (`80100000.vpss`) | 0x8010_0000 | **Scaler-only** + CSC: runtime resize |
| `v_proc_ss_2` (`80300000.csc`) | 0x8030_0000 | **CSC-only**: white balance, brightness, contrast |

## Layout

```
board/ked/zu-nexus/     device tree, rootfs overlay, boot scripts, post-build/image
  dts/linux/            zynqmp-ked-zcu-revA.dts + ...-pl.dtsi
  rootfs-overlay/       systemd units, setup_pipeline.sh, test_pipeline.sh, weston.ini
configs/                zu_nexus_ai_defconfig
package/
  dx-npu-driver/        DX-M1 PCIe driver
  dx-rt/                dx_rt runtime + dxrt-cli
  dx-fw/                module firmware (/lib/firmware/deepx)
  dx-stream/            DeepX GStreamer plugins (+ patch: dxinfer exposes timings)
  librdkafka/           dx-stream dependency (+ OPENSSL_NO_ENGINE patch)
  ked-libyuv/           libyuv for dx-stream
  dxpose-qt/            Qt6 application (sources in package/dxpose-qt/src)
media/                  fallback video and panel logos
vivado/                 block design TCL and constraints
external.mk, Config.in, build.sh
```

## Requirements

* Linux host (tested on Ubuntu 22.04) with the usual Buildroot build packages:
  `sudo apt install build-essential git bc bison flex libssl-dev cpio unzip rsync file wget python3`
* **Vivado 2023.2**, only needed to regenerate the bitstream
* About 40 GB of free disk space
* The `.dxnn` models from `sdk.deepx.ai`, to be copied to `/home/ked/models` on the target
* Optional: `media/dron_720p.y4m`, the fallback video (not tracked in git: it is large)

## Building

```bash
git clone <this-repo> ked-external
cd ked-external
./build.sh                 # fetches Buildroot if missing, applies the defconfig and builds
```

The result is `../buildroot/output/images/sdcard.img`.

### Useful commands

```bash
./build.sh defconfig              # re-apply the defconfig (ALWAYS after editing it)
./build.sh menuconfig             # interactive configuration
./build.sh linux-reconfigure      # after editing linux.fragment
./build.sh dxpose-qt-rebuild      # rebuild only the Qt application
./build.sh dx-stream-dirclean     # force dx-stream patches to be re-applied
./build.sh bootbin                # regenerate BOOT.BIN
./build.sh vivado synth           # FPGA synthesis + XSA export
./build.sh xsa <file.xsa>         # regenerate fsbl/pmufw and extract the .bit
```

> **Note:** `./build.sh <target>` does **not** re-apply the defconfig. After
> editing `configs/zu_nexus_ai_defconfig`, run `./build.sh defconfig` first.

### Writing the SD card

```bash
lsblk                                   # identify the device (e.g. /dev/sdb)
sudo dd if=../buildroot/output/images/sdcard.img of=/dev/sdX bs=4M conv=fsync status=progress
sync
```

Two partitions: a FAT32 boot partition (`BOOT.BIN`, `Image`, `system.dtb`,
`extlinux/`) and an ext4 rootfs.

## First boot

`weston.service` and `dxpose-qt.service` start automatically (user `ked`,
uid 1001). The application configures the video chain itself, loads the model on
the DX-M1 and shows the video, the pipeline synoptic and the control panel.

Quick checks:

```bash
ls /sys/firmware/devicetree/base/amba_pl@0/vcap-imx219/dma-coherent   # must exist
grep -i ina /sys/class/hwmon/hwmon*/name                              # two "ina231"
dxrt-cli -s                                                           # DX-M1 status
systemctl status dxpose-qt
test_pipeline.sh npu-check                                            # full inventory
```

## Things worth knowing

### `dma-coherent` (performance)

The frame buffer writer is wired to `S_AXI_HPC0_FPD`, the coherent port through
CCI-400. The device tree declares `dma-coherent` on the **`vcap-imx219`** node,
because that is the device vb2 allocates from (`xilinx-dma.c` does
`dma->queue.dev = dma->xdev->dev`). Without it the buffers are *uncached* and the
CPU reads them at roughly 4 MB/s: the same test measured **3.5 fps** against
**29 fps** once the property was in place.

### Format names are swapped

In `drivers/dma/xilinx/xilinx_frmbuf.c` the device tree names are swapped with
respect to the V4L2 fourcc they expose:

| DTS name | `CONFIG.HAS_*` in the TCL | V4L2 fourcc |
|---|---|---|
| `bgr888` | `HAS_RGB8` | **RGB3** |
| `rgb888` | `HAS_BGR8` | **BGR3** |

The driver does **not** validate against the bitstream: a format that is
declared but not implemented in hardware is still enumerated and produces
invalid data with no error at all. **Always update the `.bit` and the `.dtb`
together.**

With a bitstream that has `HAS_RGB8`, capture can be done in RGB3 and the last
software conversion disappears:

```bash
echo 'DXPOSE_ARGS="--rgb"' > /etc/default/dxpose-qt
systemctl restart dxpose-qt
```

### DX-M1 firmware

dx_rt 3.3.2 requires firmware 2.5.2 or newer. At startup the application
compares the module firmware with the one bundled in `/lib/firmware/deepx` and,
if the module is older, updates it automatically while showing a notice on
screen. The new firmware only becomes active after a **full power cycle**; a
reboot is not enough. Automatic updates can be disabled with `--no-fw-update`.

### File fallback

If the camera is not detected, or its pipeline fails, the application switches to
`/usr/share/dxpose-qt/media/dron_720p.y4m` (installed from `media/dron_720p.y4m`
when present). This needs the `y4m` plugin from gst-plugins-bad, already enabled
in the defconfig. The **Retry camera** button switches back to the camera.

## Target scripts

```bash
setup_pipeline.sh display|npu     # configure the media chain and the ISP controls
tune_isp.sh                       # interactive WB/gamma/brightness/contrast tuning
test_pipeline.sh <step>           # 1..12: topology, capture, fps, kms, wayland, NPU...
```

## dxpose-qt application

Main options (see `dxpose-qt --help`):

| option | default | description |
|---|---|---|
| `--width/--height` | 1280×720 | capture resolution (resize done by the VPSS) |
| `--rgb` | off | capture RGB3 (requires `HAS_RGB8`) |
| `--model` | `YoloV5S_PPU` | initial model |
| `--no-awb` | off | start with auto white balance disabled |
| `--no-fw-update` | off | no automatic firmware update |
| `--logos-dir` | `/usr/share/dxpose-qt/logos` | panel logos |
| `--windowed` | off | windowed instead of fullscreen |

Permanent options go into `/etc/default/dxpose-qt`.

## License

The DeepX packages (`dx-rt`, `dx-stream`, `dx-fw`, `dx-npu-driver`) are covered
by DeepX proprietary licenses. The rest of the BSP is distributed as stated in
the repository.
