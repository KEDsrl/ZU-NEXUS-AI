# ZU-NEXUS-AI — BSP Buildroot per IMX219 + DeepX DX-M1 su ZynqMP

`BR2_EXTERNAL` per la board **KED ZU-NEXUS-AI** (Zynq UltraScale+). Contiene la
catena video in PL per il sensore **IMX219**, l'integrazione dell'acceleratore
**DeepX DX-M1** (PCIe M.2) e l'applicazione Qt6 **dxpose-qt** che mostra il video
con l'inferenza e i controlli dell'ISP.

## Architettura

Tre catene parallele che si incontrano solo nella DDR:

```
PL (FPGA)   IMX219 → CSI-2 RX → Demosaic → CSC(WB) → Gamma → VPSS(scaler+CSC) → frmbuf WR
                                                                                    │ AXI HPC0 (coerente)
PS DDR      ═══════════════════ buffer CMA condivisi ═══════════════════════════════
              │ mmap
A53 (SW)    v4l2src → [videoconvert] → dxpreprocess → dxinfer → dxpostprocess → dxosd → appsink (Qt)
                                                          │ PCIe DMA ↑
PCIe                                                  DX-M1 NPU
```

| Blocco | Indirizzo | Note |
|---|---|---|
| `mipi_csi2_rx_subsyst_0` | 0x8002_0000 | 2 lane, RAW10 |
| `v_demosaic_0` | 0x8003_0000 | uscita `RBG888_1X24` |
| `v_frmbuf_wr_0` | 0x8004_0000 | master AXI su **S_AXI_HPC0_FPD** (coerente, CCI-400) |
| `v_gamma_lut_0` | 0x8008_0000 | |
| `v_proc_ss_0` (`80100000.vpss`) | 0x8010_0000 | **Scaler-only** + CSC: resize a runtime |
| `v_proc_ss_2` (`80300000.csc`) | 0x8030_0000 | **CSC-only**: white balance, brightness, contrast |

## Contenuto

```
board/ked/zu-nexus/     device tree, overlay del rootfs, script di boot, post-build/image
  dts/linux/            zynqmp-ked-zcu-revA.dts + ...-pl.dtsi
  rootfs-overlay/       service systemd, setup_pipeline.sh, test_pipeline.sh, weston.ini
configs/                zu_nexus_ai_defconfig
package/
  dx-npu-driver/        driver PCIe del DX-M1
  dx-rt/                runtime dx_rt + dxrt-cli
  dx-fw/                firmware del modulo (/lib/firmware/deepx)
  dx-stream/            plugin GStreamer DeepX (+ patch: latenza esposta da dxinfer)
  librdkafka/           dipendenza di dx-stream (+ patch OPENSSL_NO_ENGINE)
  ked-libyuv/           libyuv per dx-stream
  dxpose-qt/            applicazione Qt6 (sorgenti in package/dxpose-qt/src)
media/                  video di fallback e loghi del pannello
vivado/                 TCL del block design e vincoli
external.mk, Config.in, build.sh
```

## Prerequisiti

* Host Linux (testato su Ubuntu 22.04) con i pacchetti di build di Buildroot:
  `sudo apt install build-essential git bc bison flex libssl-dev cpio unzip rsync file wget python3`
* **Vivado 2023.2** (solo per rigenerare il bitstream)
* Circa 40 GB di spazio libero
* I modelli `.dxnn` da `sdk.deepx.ai`, da copiare in `/home/ked/models` sul target
* Facoltativo: `media/dron_720p.y4m`, video di fallback (non versionato: è grande)

## Compilazione

```bash
git clone <questo-repo> ked-external
cd ked-external
./build.sh                 # scarica Buildroot se assente, applica il defconfig e compila
```

Il risultato è `../buildroot/output/images/sdcard.img`.

### Comandi utili

```bash
./build.sh defconfig              # riapplica il defconfig (SEMPRE dopo averlo modificato)
./build.sh menuconfig             # configurazione interattiva
./build.sh linux-reconfigure      # dopo una modifica a linux.fragment
./build.sh dxpose-qt-rebuild      # ricompila solo l'app Qt
./build.sh dx-stream-dirclean     # forza la riapplicazione delle patch di dx-stream
./build.sh bootbin                # rigenera BOOT.BIN
./build.sh vivado synth           # sintesi FPGA + export XSA
./build.sh xsa <file.xsa>         # rigenera fsbl/pmufw ed estrae il .bit
```

> **Attenzione:** `./build.sh <target>` **non** riapplica il defconfig. Dopo aver
> modificato `configs/zu_nexus_ai_defconfig` eseguire prima `./build.sh defconfig`.

### Scrittura della SD

```bash
lsblk                                   # individuare il device (es. /dev/sdb)
sudo dd if=../buildroot/output/images/sdcard.img of=/dev/sdX bs=4M conv=fsync status=progress
sync
```

Due partizioni: FAT32 di boot (`BOOT.BIN`, `Image`, `system.dtb`, `extlinux/`) ed
ext4 di rootfs.

## Primo avvio

All'accensione partono `weston.service` e `dxpose-qt.service` (utente `ked`,
uid 1001). L'app configura da sola la catena video, carica il modello sul DX-M1 e
mostra video, sinottico della pipeline e pannello dei controlli.

Verifiche rapide:

```bash
ls /sys/firmware/devicetree/base/amba_pl@0/vcap-imx219/dma-coherent   # deve esistere
grep -i ina /sys/class/hwmon/hwmon*/name                              # due "ina231"
dxrt-cli -s                                                           # stato del DX-M1
systemctl status dxpose-qt
test_pipeline.sh npu-check                                            # inventario completo
```

## Note importanti

### `dma-coherent` (prestazioni)

Il `frmbuf` è cablato su `S_AXI_HPC0_FPD`, porta coerente via CCI-400. Il device
tree dichiara `dma-coherent` sul nodo **`vcap-imx219`** (è quello che alloca i
buffer: `xilinx-dma.c` fa `dma->queue.dev = dma->xdev->dev`). Senza quella riga i
buffer sono *uncached* e la CPU li legge a ~4 MB/s: misurato **3,5 fps** contro
**29 fps** sullo stesso test.

### Nomi dei formati invertiti

In `drivers/dma/xilinx/xilinx_frmbuf.c` i nomi del device tree sono invertiti
rispetto ai fourcc V4L2:

| nome DTS | `CONFIG.HAS_*` nel TCL | fourcc V4L2 |
|---|---|---|
| `bgr888` | `HAS_RGB8` | **RGB3** |
| `rgb888` | `HAS_BGR8` | **BGR3** |

Il driver **non** verifica il bitstream: un formato dichiarato ma non
implementato viene enumerato lo stesso e produce dati non validi, senza errori.
**`.bit` e `.dtb` vanno sempre aggiornati insieme.**

Con un bitstream che ha `HAS_RGB8` si può catturare in RGB3 ed eliminare
l'ultima conversione software:

```bash
echo 'DXPOSE_ARGS="--rgb"' > /etc/default/dxpose-qt
systemctl restart dxpose-qt
```

### Firmware del DX-M1

dx_rt 3.3.2 richiede firmware ≥ 2.5.2. All'avvio l'app confronta il firmware del
modulo con quello incluso in `/lib/firmware/deepx` e, se è più vecchio, lo
aggiorna da sola mostrando un avviso a schermo. Il nuovo firmware diventa attivo
solo dopo un **power cycle completo** (un reboot non basta). L'aggiornamento
automatico si disattiva con `--no-fw-update`.

### Fallback su file

Se la camera non viene rilevata, o la sua pipeline va in errore, l'app passa a
`/usr/share/dxpose-qt/media/dron_720p.y4m` (installato da `media/dron_720p.y4m`,
se presente). Serve il plugin `y4m` di gst-plugins-bad, già nel defconfig. Dal
fallback si torna alla camera con il pulsante **Retry camera**.

## Script sul target

```bash
setup_pipeline.sh display|npu     # configura la catena media e i controlli ISP
tune_isp.sh                       # regolazione interattiva di WB/gamma/brightness/contrast
test_pipeline.sh <step>           # 1..12: topologia, cattura, fps, kms, wayland, NPU...
```

## Applicazione dxpose-qt

Opzioni principali (vedi `dxpose-qt --help`):

| opzione | default | descrizione |
|---|---|---|
| `--width/--height` | 1280×720 | risoluzione di cattura (resize nel VPSS) |
| `--rgb` | off | cattura RGB3 (richiede `HAS_RGB8`) |
| `--model` | `YoloV5S_PPU` | modello iniziale |
| `--no-awb` | off | avvio senza auto white balance |
| `--no-fw-update` | off | niente aggiornamento firmware automatico |
| `--logos-dir` | `/usr/share/dxpose-qt/logos` | loghi del pannello |
| `--windowed` | off | finestra invece che schermo intero |

Le opzioni a regime si impostano in `/etc/default/dxpose-qt`.

## Licenza

I pacchetti DeepX (`dx-rt`, `dx-stream`, `dx-fw`, `dx-npu-driver`) sono soggetti
alle licenze proprietarie DeepX. Il resto del BSP è distribuito come indicato nel
repository.
