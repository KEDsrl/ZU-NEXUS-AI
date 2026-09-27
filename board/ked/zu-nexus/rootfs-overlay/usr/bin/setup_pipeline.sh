#!/bin/sh
# ============================================================================
# setup_pipeline.sh - Init pipeline video IMX219, canale singolo (R3)
#
#   IMX219 -> CSI2RX -> Demosaic -> CSC(WB) -> Gamma -> VPSS -> /dev/video0
#
# La VPSS (scaler+CSC) e' programmabile a runtime: due modi esclusivi.
#
# Uso:
#   setup_pipeline.sh display          YUV422 1920x1080 (NV16) -> piano video DP
#   setup_pipeline.sh npu              RGB 640x640 (RGB3)      -> DX-M1
#   setup_pipeline.sh <modo> --no-controls    solo formati, non tocca WB/gamma
#
# NB: cambiare modo A STREAMING FERMO. Il set di formato sulla CSC resetta i
# controlli colore: WB e gamma vengono riapplicati ad ogni invocazione.
# ============================================================================
set -u

MODE=${1:-display}
case "$MODE" in display|npu) ;; *) echo "uso: $0 display|npu [--no-controls]"; exit 1;; esac

# ------------------------- parametri modificabili --------------------------
WIDTH=1920 ;  HEIGHT=1080            # risoluzione sensore/ISP (comune ai modi)
NPU_W=640  ;  NPU_H=640              # risoluzione modo npu
# NB: 640x640 da un sensore 16:9 significa stretch verticale (immagine
# deformata) fatto dallo scaler VPSS. Alternativa migliore per l'accuratezza:
# NPU_W=640 NPU_H=360 -> stesso aspect ratio del sensore, e dxpreprocess con
# keep_ratio:true calcola ratio=1.0 quindi NON ridimensiona: aggiunge solo le
# bande di letterbox fino a 640x640 (una memcpy invece di un resize).
RGB_FMT=RBG888_1X24
RAW_FMT=SRGGB10_1X10

# Taratura sul campo (lab, luce interna). Scala 0..100, 50 = unitario.
# NB: tenere allineati questi valori ai default di Ctrls in
# package/dxpose-qt/src/isp.h (l'app Qt configura la stessa catena).
# Il verde e' 50 = unitario: la CSC fa bilanciamento puro, niente boost
# globale; la luminosita' viene dal sensore (AGAIN al massimo, exposure 784,
# DGAIN 660 ~2,6x).
WB_RED=60 ; WB_GREEN=50 ; WB_BLUE=86
CSC_BRIGHTNESS=43 ; CSC_CONTRAST=49
# GAMMA = esponente LUT x10: per dati sensore lineari verso display serve
# l'esponente di CODIFICA (~0.45-0.6), NON 2.2: 22 scurisce tutto.
GAMMA=7
# default sensore: il driver imx219 parte con analogue_gain=0 (immagine
# nera in interni). Range: exposure 4..1759, analogue_gain 0..232,
# digital_gain 256..4095. Tarare sul campo e aggiornare qui.
SENSOR_EXPOSURE=784 ; SENSOR_AGAIN=232 ; SENSOR_DGAIN=660

E_CSI=80020000.csi2rx
E_DEM=80030000.demosaic
E_CSC=80300000.csc
E_GAM=80080000.gamma
E_VPSS=80100000.vpss
# ----------------------------------------------------------------------------

log() { echo "[pipeline] $*"; }
die() { echo "[pipeline] ERRORE: $*" >&2; exit 1; }

MDEV=""
for m in /dev/media*; do
    [ -e "$m" ] || continue
    media-ctl -d "$m" -p 2>/dev/null | grep -q "vcap-imx219" && { MDEV=$m; break; }
done
[ -n "$MDEV" ] || die "media device vcap-imx219 non trovato"
TOPO=$(media-ctl -d "$MDEV" -p)

SENSOR=$(echo "$TOPO" | sed -n 's/.*entity [0-9]*: \(imx219 [0-9-]*\).*/\1/p' | head -1)
[ -n "$SENSOR" ] || die "sensore imx219 non trovato"

subdev_of() { echo "$TOPO" | awk -v ent="$1" '
    $0 ~ "entity .*" ent { found=1 }
    found && /device node name/ { print $NF; exit }'; }
SD_SENSOR=$(subdev_of "$SENSOR"); SD_CSC=$(subdev_of "$E_CSC"); SD_GAMMA=$(subdev_of "$E_GAM")

VDEV=$(echo "$TOPO" | awk -v src="$E_VPSS" '
    /device node name/ { node=$NF }
    $0 ~ "<- \"" src "\"" { print node; exit }')
[ -n "$VDEV" ] || die "video node non risolto"
log "modo=$MODE media=$MDEV sensore=\"$SENSOR\" video=$VDEV csc=$SD_CSC gamma=$SD_GAMMA"

V() { media-ctl -d "$MDEV" -V "$1" || die "media-ctl -V '$1'"; }

# --- tratto comune (identico nei due modi) -----------------------------------
V "\"$SENSOR\":0   [fmt:$RAW_FMT/${WIDTH}x${HEIGHT} field:none]"
V "\"$E_CSI\":0    [fmt:$RAW_FMT/${WIDTH}x${HEIGHT} field:none]"
V "\"$E_CSI\":1    [fmt:$RAW_FMT/${WIDTH}x${HEIGHT} field:none]"
V "\"$E_DEM\":0    [fmt:$RAW_FMT/${WIDTH}x${HEIGHT} field:none]"
V "\"$E_DEM\":1    [fmt:$RGB_FMT/${WIDTH}x${HEIGHT} field:none]"
V "\"$E_CSC\":0    [fmt:$RGB_FMT/${WIDTH}x${HEIGHT} field:none]"
V "\"$E_CSC\":1    [fmt:$RGB_FMT/${WIDTH}x${HEIGHT} field:none]"
V "\"$E_GAM\":0    [fmt:$RGB_FMT/${WIDTH}x${HEIGHT} field:none]"
V "\"$E_GAM\":1    [fmt:$RGB_FMT/${WIDTH}x${HEIGHT} field:none]"
V "\"$E_VPSS\":0   [fmt:$RGB_FMT/${WIDTH}x${HEIGHT} field:none]"

# --- uscita VPSS + formato nodo: dipendono dal modo ---------------------------
pick_fmt() {
    node=$1; shift
    avail=$(v4l2-ctl -d "$node" --list-formats 2>/dev/null)
    for f in "$@"; do echo "$avail" | grep -q "'$f'" && { echo "$f"; return 0; }; done
    return 1
}

if [ "$MODE" = display ]; then
    V "\"$E_VPSS\":1 [fmt:UYVY8_1X16/${WIDTH}x${HEIGHT} field:none]"
    # YUYV per primo: packed single-plane, il percorso dmabuf robusto con
    # kmssink. La variante NM16 (NV16 non contiguo) preferita da v4l2src
    # non e' gestita correttamente da kmssink (GST 1.24 + driver xlnx).
    FOURCC=$(pick_fmt "$VDEV" YUYV UYVY NV16) || die "nessun fourcc YUV sul nodo"
    v4l2-ctl -d "$VDEV" --set-fmt-video=width=$WIDTH,height=$HEIGHT,pixelformat=$FOURCC \
        || die "set-fmt $FOURCC"
    OUTDESC="$FOURCC ${WIDTH}x${HEIGHT}"
else
    V "\"$E_VPSS\":1 [fmt:$RGB_FMT/${NPU_W}x${NPU_H} field:none]"
    # RGB3 per primo: e' l'unico formato RGB accettato da dxpreprocess (le sue
    # caps sono { RGB, I420, NV12 }, BGR non negozia). BGR3 resta come ripiego.
    #
    # ATTENZIONE ai nomi (v. tabella nel dtsi): il driver frmbuf inverte i nomi
    # DTS rispetto ai fourcc V4L2 ->  "bgr888"=HAS_RGB8 -> RGB3
    #                                 "rgb888"=HAS_BGR8 -> BGR3
    # e non valida contro l'IP: se il bitstream caricato NON ha HAS_RGB8, RGB3
    # viene comunque enumerato ma la cattura produce dati NON VALIDI (nessun
    # errore, immagine plausibile solo all'apparenza). Questo ordine presuppone
    # il bitstream con HAS_RGB8: dtb e .bit vanno sempre aggiornati insieme.
    FOURCC=$(pick_fmt "$VDEV" RGB3 BGR3) || die "nessun fourcc RGB sul nodo"
    v4l2-ctl -d "$VDEV" --set-fmt-video=width=$NPU_W,height=$NPU_H,pixelformat=$FOURCC \
        || die "set-fmt $FOURCC"
    OUTDESC="$FOURCC ${NPU_W}x${NPU_H}"
fi

# --- controlli ISP (dopo i formati: il set fmt sulla CSC li resetta) ----------
if [ "${2:-}" != "--no-controls" ]; then
    v4l2-ctl -d "$SD_CSC" --set-ctrl=csc_red_gain=$WB_RED,csc_green_gain=$WB_GREEN,csc_blue_gain=$WB_BLUE
    v4l2-ctl -d "$SD_CSC" --set-ctrl=csc_brightness=$CSC_BRIGHTNESS,csc_contrast=$CSC_CONTRAST
    for c in $(v4l2-ctl -d "$SD_GAMMA" -l 2>/dev/null | awk '/gamma/ {print $1}'); do
        v4l2-ctl -d "$SD_GAMMA" --set-ctrl=$c=$GAMMA >/dev/null 2>&1
    done
    [ -n "$SENSOR_EXPOSURE" ] && v4l2-ctl -d "$SD_SENSOR" --set-ctrl=exposure=$SENSOR_EXPOSURE
    [ -n "$SENSOR_AGAIN" ]    && v4l2-ctl -d "$SD_SENSOR" --set-ctrl=analogue_gain=$SENSOR_AGAIN
    [ -n "$SENSOR_DGAIN" ]    && v4l2-ctl -d "$SD_SENSOR" --set-ctrl=digital_gain=$SENSOR_DGAIN
    log "WB R=$WB_RED G=$WB_GREEN B=$WB_BLUE, gamma=$GAMMA applicati"
fi

log "OK - modo $MODE: $VDEV ($OUTDESC)"
