#!/bin/sh
# ============================================================================
# test_pipeline.sh - Test pipeline video a canale singolo (ZCU-DM-X1-R3)
#
# Uso: test_pipeline.sh <step>
#   graph     1. topologia e coerenza formati
#   capture   2. cattura 90 frame + misura fps (funziona in entrambi i modi)
#   frame     3. salva un frame in /tmp per ispezione visiva
#   kms       4. [modo display] scanout diretto sul piano video DP
#   wayland   5. [modo display] rendering sotto Weston (utente WUSER)
#   cpu       6. [modo display] pipeline in background + carico CPU
#   debug     7. diagnosi STREAMON/EPIPE
#   coherency 8. [modo npu] benchmark lettura CPU dei buffer V4L2
#                 -> rileva dma-coherent mancante (regressione nota)
#   npu-check 9. inventario: device DX-M1, plugin dx, modelli, postprocess lib
#   npu-bench 10. [no camera] benchmark NPU isolata (run_model)
#   npu-fps   11. [modo npu] catena camera->NPU headless + fps
#   npu-view  12. [modo npu] catena camera->NPU -> Weston (utente WUSER)
#
# Prima di 4/5/6 eseguire: setup_pipeline.sh display
# Prima di 8/11/12 eseguire: setup_pipeline.sh npu
#
# Variabili d'ambiente:
#   MODEL=YoloV5S_PPU      modello (senza .dxnn). Se esiste la dir dei config
#                          omonima li usa, altrimenti va di proprieta' inline
#                          con function-name=$MODEL (es. YOLOV5Pose_PPU,
#                          SCRFD500M_PPU).
#   MODELS=/home/ked/models          dir dei .dxnn
#   CFGROOT=/usr/share/gstdxstream/configs
#   PPLIB=/usr/share/gstdxstream/lib/libpostprocess_ppu.so
#   WUSER=ked                        utente proprietario di Weston
# ============================================================================
set -u
STEP=${1:-help}
WUSER=${WUSER:-ked}

# --- NPU / dx_stream -------------------------------------------------------
MODEL=${MODEL:-YoloV5S_PPU}
MODELS=${MODELS:-/home/ked/models}
CFGROOT=${CFGROOT:-/usr/share/gstdxstream/configs}
PPLIB=${PPLIB:-/usr/share/gstdxstream/lib/libpostprocess_ppu.so}
MODEL_FILE="$MODELS/$MODEL.dxnn"

case "$STEP" in
    1) STEP=graph ;;  2) STEP=capture ;;  3) STEP=frame ;;
    4) STEP=kms ;;    5) STEP=wayland ;;  6) STEP=cpu ;;  7) STEP=debug ;;
    8) STEP=coherency ;; 9) STEP=npu-check ;; 10) STEP=npu-bench ;;
    11) STEP=npu-fps ;;  12) STEP=npu-view ;;
esac

MDEV=""
for m in /dev/media*; do
    media-ctl -d "$m" -p 2>/dev/null | grep -q vcap-imx219 && { MDEV=$m; break; }
done
[ -n "$MDEV" ] || { echo "media device non trovato"; exit 1; }
TOPO=$(media-ctl -d "$MDEV" -p)
VDEV=$(echo "$TOPO" | awk '/device node name/ { node=$NF }
    /<- "80100000.vpss"/ { print node; exit }')

cur_fmt() { v4l2-ctl -d "$VDEV" --get-fmt-video | awk -F"'" '/Pixel Format/ {print $2}'; }
cur_size() { v4l2-ctl -d "$VDEV" --get-fmt-video | awk -F': ' '/Width\/Height/ {print $2}' | tr -d ' '; }
# mappa fourcc V4L2 -> formato caps GStreamer
gst_fmt() {
    case "$(cur_fmt)" in
        YUYV) echo YUY2 ;;
        UYVY) echo UYVY ;;
        NV16|NM16) echo NV16 ;;
        *) echo YUY2 ;;
    esac
}

need_display_mode() {
    case "$(cur_fmt)" in
        NV16|NM16|YUYV|UYVY) : ;;
        *) echo "Il nodo e' in modo NPU ($(cur_fmt)): eseguire prima 'setup_pipeline.sh display'"; exit 1 ;;
    esac
}

need_npu_mode() {
    case "$(cur_fmt)" in
        RGB3|BGR3) : ;;
        *) echo "Il nodo e' in modo display ($(cur_fmt)): eseguire prima 'setup_pipeline.sh npu'"; exit 1 ;;
    esac
}

# Sorgente V4L2 normalizzata a RGB: dxpreprocess accetta solo { RGB, I420, NV12 },
# BGR non negozia. Con RGB3 e' zero conversioni; con BGR3 serve uno swap R<->B
# (economico: e' solo un rimescolamento di byte sulla risoluzione del modello).
# NB: RGB3 e' reale solo se il bitstream ha CONFIG.HAS_RGB8; il driver non
# valida contro l'IP (v. tabella dei nomi invertiti nel dtsi).
npu_src() {
    _w=$(cur_size | cut -dx -f1); _h=$(cur_size | cut -dx -f2)
    case "$(cur_fmt)" in
        RGB3) echo "v4l2src device=$VDEV io-mode=mmap ! video/x-raw,format=RGB,width=$_w,height=$_h,framerate=30/1" ;;
        BGR3) echo "v4l2src device=$VDEV io-mode=mmap ! video/x-raw,format=BGR,width=$_w,height=$_h,framerate=30/1 ! videoconvert ! video/x-raw,format=RGB" ;;
    esac
}

# Catena dx_stream. Se esiste la dir dei config del modello li usa, altrimenti
# ripiega sulle proprieta' inline con function-name=$MODEL (e' il caso di
# YOLOV5Pose_PPU / SCRFD500M_PPU, che nel repo non hanno config file).
# ATTENZIONE: usare i config di un modello con un altro modello fa abortire
# dxpostprocess (std::vector out of range) appena rileva qualcosa.
dx_chain() {
    if [ -f "$CFGROOT/$MODEL/preprocess_config.json" ]; then
        echo "queue \
          ! dxpreprocess config-file-path=$CFGROOT/$MODEL/preprocess_config.json ! queue \
          ! dxinfer config-file-path=$CFGROOT/$MODEL/inference_config.json model-path=$MODEL_FILE ! queue \
          ! dxpostprocess config-file-path=$CFGROOT/$MODEL/postprocess_config.json ! queue \
          ! dxosd"
    else
        echo "queue \
          ! dxpreprocess preprocess-id=1 resize-width=640 resize-height=640 ! queue max-size-buffers=1 \
          ! dxinfer preprocess-id=1 inference-id=1 model-path=$MODEL_FILE ! queue max-size-buffers=1 \
          ! dxpostprocess inference-id=1 library-file-path=$PPLIB function-name=$MODEL ! queue max-size-buffers=1 \
          ! dxosd"
    fi
}

npu_preflight() {
    [ -e /dev/dxrt0 ] || { echo "ERRORE: /dev/dxrt0 assente (NPU non enumerata: lspci | grep -i 1ff4)"; exit 1; }
    [ -f "$MODEL_FILE" ] || { echo "ERRORE: modello non trovato: $MODEL_FILE"
        echo "  I .dxnn non sono nel repo dx_stream: scaricarli da sdk.deepx.ai."
        echo "  Disponibili qui: $(ls $MODELS 2>/dev/null | tr '\n' ' ')"; exit 1; }
    gst-inspect-1.0 dxpreprocess >/dev/null 2>&1 || {
        echo "ERRORE: elementi dx_stream non caricati. Provare: rm -rf ~/.cache/gstreamer-1.0"
        echo "  e verificare: gst-inspect-1.0 dxpreprocess"; exit 1; }
}

# secondi (centesimi) da /proc/uptime: 'date +%N' non e' garantito su busybox
now() { cut -d' ' -f1 /proc/uptime; }

case "$STEP" in
# -----------------------------------------------------------------------------
graph)
    media-ctl -d "$MDEV" -p
    echo
    echo "video node: $VDEV - formato corrente: $(cur_fmt) $(cur_size)"
    echo "Verificare: field:none su ogni pad, fmt/risoluzione uguali su ogni link."
    ;;
# -----------------------------------------------------------------------------
capture)
    echo "== Cattura 90 frame da $VDEV ($(cur_fmt) $(cur_size)) =="
    v4l2-ctl -d "$VDEV" --stream-mmap=6 --stream-count=90 --stream-to=/dev/null
    echo "Atteso: ~30 fps. Se fermo: dmesg | grep -iE 'imx219|csi|frmbuf'"
    ;;
# -----------------------------------------------------------------------------
frame)
    OUT=/tmp/frame_$(cur_fmt).raw
    echo "== Frame (il 5o) da $VDEV -> $OUT =="
    v4l2-ctl -d "$VDEV" --stream-mmap --stream-count=5 --stream-skip=4 --stream-to=$OUT
    ls -la $OUT
    case "$(cur_fmt)" in
        NV16|NM16) echo "  ffmpeg -f rawvideo -pix_fmt nv16 -s $(cur_size | tr / x) -i $OUT -frames:v 1 -update 1 -y out.png" ;;
        YUYV) echo "  ffmpeg -f rawvideo -pix_fmt yuyv422 -s $(cur_size | tr / x) -i $OUT -frames:v 1 -update 1 -y out.png" ;;
        UYVY) echo "  ffmpeg -f rawvideo -pix_fmt uyvy422 -s $(cur_size | tr / x) -i $OUT -frames:v 1 -update 1 -y out.png" ;;
        RGB3) echo "  ffmpeg -f rawvideo -pix_fmt rgb24 -s $(cur_size | tr / x) -i $OUT -frames:v 1 -update 1 -y out.png"
              echo "  (colori invertiti R<->B? usare -pix_fmt bgr24 e replicare lo"
              echo "   swap nel preprocessing dx_rt del DX-M1)" ;;
        BGR3) echo "  ffmpeg -f rawvideo -pix_fmt bgr24 -s $(cur_size | tr / x) -i $OUT -frames:v 1 -update 1 -y out.png" ;;
    esac
    ;;
# -----------------------------------------------------------------------------
kms)
    need_display_mode
    # kmssink deve poter diventare DRM master: nessun compositor attivo.
    for u in weston weston@root weston@$WUSER wayland-compositor; do
        systemctl stop $u 2>/dev/null
    done
    systemctl --machine=$WUSER@ --user stop weston 2>/dev/null
    pkill -x weston 2>/dev/null
    for i in 1 2 3 4 5; do pgrep -x weston >/dev/null || break; sleep 1; done
    if pgrep -x weston >/dev/null; then
        echo "ERRORE: weston ancora attivo (pid $(pgrep -x weston | tr '\n' ' ')):"
        echo "  detiene il DRM master -> drmModeSetPlane: Permission denied."
        echo "  Fermarlo manualmente e rilanciare."
        exit 1
    fi
    echo 0 > /sys/class/vtconsole/vtcon1/bind 2>/dev/null
    # nome del driver DRM del DP: "zynqmp-dpsub" sui kernel mainline
    # recenti, "xlnx" sull'albero Xilinx (es. 2023.2). Rilevo quale c'e'.
    KMSDRV=""
    for d in zynqmp-dpsub xlnx; do
        modetest -M $d -p >/dev/null 2>&1 && { KMSDRV=$d; break; }
    done
    [ -n "$KMSDRV" ] || { echo "driver DRM del DP non trovato (ne' zynqmp-dpsub ne' xlnx)"; exit 1; }
    # gli ID dei piani dipendono dal kernel: rilevo piano video (NV16)
    # e piano grafica (AR24, per il tip sull'alpha)
    VPLANE=$(modetest -M $KMSDRV -p 2>/dev/null | awk '
        /^[0-9]+/ { id=$1 } /NV16/ { print id; exit }')
    GPLANE=$(modetest -M $KMSDRV -p 2>/dev/null | awk '
        /^[0-9]+/ { id=$1 } /AR24/ { print id; exit }')
    [ -n "$VPLANE" ] || { echo "piano video NV16 non trovato (modetest -M $KMSDRV -p)"; exit 1; }
    echo "driver: $KMSDRV  piano video: $VPLANE  piano grafica: ${GPLANE:-?}"
    echo "Se lo schermo resta coperto dalla grafica: modetest -M $KMSDRV -w ${GPLANE:-<id>}:alpha:0"
    gst-launch-1.0 v4l2src device=$VDEV io-mode=dmabuf \
      ! video/x-raw,format=$(gst_fmt),width=1920,height=1080,framerate=30/1 \
      ! kmssink driver-name=$KMSDRV plane-id=$VPLANE sync=false
    ;;
# -----------------------------------------------------------------------------
wayland)
    need_display_mode
    WUID=$(id -u "$WUSER" 2>/dev/null) || { echo "utente $WUSER inesistente"; exit 1; }
    XRD=/run/user/$WUID
    [ -d "$XRD" ] || { echo "$XRD assente: Weston non in esecuzione."
        echo "  Avviarlo con: systemctl restart weston   (crea la runtime dir)"
        echo "  oppure a mano: mkdir -p $XRD && chown $WUSER:$WUSER $XRD && chmod 700 $XRD"
        echo "                 su - $WUSER -c 'XDG_RUNTIME_DIR=$XRD weston --drm-device=card1'"
        exit 1; }
    WDISP=$(ls "$XRD" 2>/dev/null | grep '^wayland-[0-9]*$' | head -1)
    [ -n "$WDISP" ] || { echo "nessun socket wayland-* in $XRD"; exit 1; }
    id -nG "$WUSER" | grep -qw video || echo "ATTENZIONE: $WUSER non nel gruppo video"
    echo "NB: waylandsink puo' negoziare solo i formati che il compositor offre"
    echo "    via dmabuf (con renderer pixman: nessuno -> 'not-negotiated')."
    echo "    Elenco formati del compositor:"
    echo "      su - $WUSER -c 'XDG_RUNTIME_DIR=$XRD wayland-info' | grep -iA40 dmabuf"
    GSTCMD="XDG_RUNTIME_DIR=$XRD WAYLAND_DISPLAY=$WDISP \
        gst-launch-1.0 v4l2src device=$VDEV io-mode=dmabuf \
        ! video/x-raw,format=$(gst_fmt),width=1920,height=1080,framerate=30/1 \
        ! waylandsink sync=false"
    su - "$WUSER" -c "$GSTCMD" &
    GSTPID=$!
    sleep 5
    echo "Zero-copy: surface su un PLANE, non sul renderer:"
    echo "  su - $WUSER -c 'XDG_RUNTIME_DIR=$XRD weston-debug scene-graph'"
    wait $GSTPID
    ;;
# -----------------------------------------------------------------------------
cpu)
    need_display_mode
    echo "NB: test di carico CPU con fakesink: NESSUNA uscita video prevista."
    echo "    Attendere 15 s per il report di top."
    gst-launch-1.0 v4l2src device=$VDEV io-mode=dmabuf \
      ! video/x-raw,format=$(gst_fmt),width=1920,height=1080,framerate=30/1 \
      ! fakesink sync=false &
    GSTPID=$!
    sleep 15
    top -b -n 1 | head -15
    kill $GSTPID 2>/dev/null
    echo "Atteso: gst-launch a pochi %."
    ;;
# -----------------------------------------------------------------------------
debug)
    mount -t debugfs none /sys/kernel/debug 2>/dev/null
    DDC=/sys/kernel/debug/dynamic_debug/control
    if [ -w "$DDC" ]; then
        echo 'file v4l2-subdev.c +p' > $DDC
        echo 'file media-entity.c +p' > $DDC
        echo 'file xilinx-dma.c +p' > $DDC 2>/dev/null
    else
        echo "(dynamic debug non disponibile: CONFIG_DYNAMIC_DEBUG assente)"
    fi
    dmesg -C 2>/dev/null
    v4l2-ctl -d "$VDEV" --get-fmt-video
    v4l2-ctl -d "$VDEV" --stream-mmap --stream-count=1 --stream-to=/dev/null 2>&1
    dmesg | tail -30
    ;;
# -----------------------------------------------------------------------------
coherency)
    need_npu_mode
    # I buffer V4L2 li alloca vb2-dma-contig usando il device 'vcap-imx219'
    # (xilinx-dma.c: dma->queue.dev = dma->xdev->dev). Senza 'dma-coherent' su
    # QUEL nodo del DT, Linux li considera non coerenti e li mappa UNCACHED: la
    # CPU li legge a ~4 MB/s. Il frmbuf e' cablato su S_AXI_HPC0_FPD (porta
    # coerente CCI-400), quindi la coerenza HW c'e' e dma-coherent e' lecito.
    # Questo test misura proprio quella lettura.
    DTC=/sys/firmware/devicetree/base/amba_pl@0/vcap-imx219/dma-coherent
    if [ -e "$DTC" ]; then echo "device tree: dma-coherent PRESENTE su vcap-imx219"
    else echo "device tree: dma-coherent ASSENTE su vcap-imx219  <-- causa nota di lentezza"; fi
    case "$(cur_fmt)" in RGB3) GF=RGB ;; BGR3) GF=BGR ;; esac
    echo "== 100 frame $(cur_fmt) $(cur_size) letti dalla CPU (videoconvert -> BGRx) =="
    T0=$(now)
    gst-launch-1.0 v4l2src device=$VDEV io-mode=mmap num-buffers=100 \
      ! video/x-raw,format=$GF,width=$(cur_size | cut -dx -f1),height=$(cur_size | cut -dx -f2) \
      ! videoconvert ! video/x-raw,format=BGRx \
      ! fakesink sync=false >/dev/null 2>&1
    T1=$(now)
    echo "$T0 $T1" | awk '{ d=$2-$1; if (d<=0) d=0.001;
        printf "tempo: %.2f s   fps: %.1f\n", d, 100/d }'
    echo
    echo "Riferimenti misurati su questa board:"
    echo "  ~3,4 s  (29 fps) -> OK, buffer cacheable: si e' limitati dalla camera"
    echo "  ~28,8 s (3,5 fps) -> REGRESSIONE: buffer uncached, manca dma-coherent"
    echo "  (100 frame a 30 fps non possono scendere sotto 3,33 s)"
    ;;
# -----------------------------------------------------------------------------
npu-check)
    echo "== DX-M1 =="
    ls -l /dev/dxrt* 2>/dev/null || echo "  ASSENTE: nessun /dev/dxrt*"
    lspci 2>/dev/null | grep -i "1ff4\|deepx" || echo "  (lspci: nessun device DeepX)"
    for b in $(lspci 2>/dev/null | grep -i "1ff4\|deepx" | cut -d' ' -f1); do
        lspci -vv -s "$b" 2>/dev/null | grep -E "LnkCap:|LnkSta:" | sed 's/^/  /'
    done
    echo "  (atteso LnkSta: Speed 5GT/s, Width x2)"
    command -v dxrt-cli >/dev/null && dxrt-cli -s 2>&1 | sed 's/^/  /'
    echo
    echo "== elementi dx_stream =="
    for e in dxpreprocess dxinfer dxpostprocess dxosd dxtracker dxmsgbroker; do
        if gst-inspect-1.0 "$e" >/dev/null 2>&1; then echo "  OK    $e"
        else echo "  MANCA $e"; fi
    done
    echo
    echo "== modelli in $MODELS =="
    ls -la "$MODELS"/*.dxnn 2>/dev/null | sed 's/^/  /' || echo "  nessun .dxnn (scaricarli da sdk.deepx.ai)"
    echo
    echo "== config in $CFGROOT =="
    ls "$CFGROOT" 2>/dev/null | sed 's/^/  /' || echo "  ASSENTI"
    echo
    echo "== librerie di postprocess =="
    ls -l "$PPLIB" 2>/dev/null | sed 's/^/  /' || echo "  MANCA $PPLIB"
    ls $(dirname "$PPLIB")/libpostprocess_*.so 2>/dev/null | sed 's/^/  /'
    echo
    echo "modello selezionato: MODEL=$MODEL -> $MODEL_FILE"
    if [ -f "$CFGROOT/$MODEL/preprocess_config.json" ]; then
        echo "  modo config-file ($CFGROOT/$MODEL)"
    else
        echo "  modo inline: function-name=$MODEL, library=$PPLIB"
    fi
    ;;
# -----------------------------------------------------------------------------
npu-bench)
    # Nessuna camera: misura il tetto della NPU isolata. Qualunque calo nei test
    # con camera e' quindi dovuto al trasporto/preprocess, non all'acceleratore.
    npu_preflight
    command -v run_model >/dev/null || { echo "run_model non presente nell'immagine"; exit 1; }
    echo "== benchmark NPU isolata: $MODEL_FILE =="
    echo "   (in un'altra shell: dxtop, per utilizzo e temperatura)"
    run_model -m "$MODEL_FILE" -b
    ;;
# -----------------------------------------------------------------------------
npu-fps)
    need_npu_mode
    npu_preflight
    echo "== camera -> NPU headless: $(cur_fmt) $(cur_size), MODEL=$MODEL =="
    [ "$(cur_fmt)" = BGR3 ] && echo "   (BGR3: videoconvert BGR->RGB, dxpreprocess non accetta BGR)"
    echo "   Ctrl-C per fermare. In un'altra shell: dxtop / top"
    eval "gst-launch-1.0 -v $(npu_src) ! $(dx_chain) \
      ! fpsdisplaysink video-sink=fakesink text-overlay=false sync=false" 2>&1 \
      | grep --line-buffered -E "last-message|ERROR|WARN"
    ;;
# -----------------------------------------------------------------------------
npu-view)
    need_npu_mode
    npu_preflight
    WUID=$(id -u "$WUSER" 2>/dev/null) || { echo "utente $WUSER inesistente"; exit 1; }
    XRD=/run/user/$WUID
    [ -d "$XRD" ] || { echo "$XRD assente: Weston non in esecuzione (systemctl status weston)"; exit 1; }
    WDISP=$(ls "$XRD" 2>/dev/null | grep '^wayland-[0-9]*$' | head -1)
    [ -n "$WDISP" ] || { echo "nessun socket wayland-* in $XRD"; exit 1; }
    id -nG "$WUSER" | grep -qw video || echo "ATTENZIONE: $WUSER non nel gruppo video"
    echo "== camera -> NPU -> Weston: $(cur_fmt) $(cur_size), MODEL=$MODEL =="
    echo "   Se non compare nulla: il piano grafico DP puo' coprire il video"
    echo "   (plane Primary con alpha=255). Vedi step 4 per gli id dei piani."
    # waylandsink via wl_shm vuole 32 bpp: RGB packed non negozia.
    GSTCMD="export XDG_RUNTIME_DIR=$XRD; export WAYLAND_DISPLAY=$WDISP;
        gst-launch-1.0 $(npu_src) ! $(dx_chain) \
        ! queue ! videoconvert ! video/x-raw,format=BGRx ! waylandsink"
    su - "$WUSER" -c "$GSTCMD"
    ;;
# -----------------------------------------------------------------------------
*)
    sed -n '3,30p' "$0"
    ;;
esac
