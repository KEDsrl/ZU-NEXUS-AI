#!/bin/sh
# ============================================================================
# tune_isp.sh - Taratura interattiva ISP/sensore A STREAMING ATTIVO
#
# Avviare prima la pipeline (test_pipeline.sh 4 o 5), poi questo script in
# un'altra shell. Tasti minuscolo = diminuisce, MAIUSCOLO = aumenta:
#
#   r/R  WB rosso        b/B  WB blu         n/N  WB verde
#   l/L  brightness      k/K  contrast       g/G  gamma (RGB insieme)
#   e/E  esposizione     a/A  gain analogico d/D  gain digitale
#   p    stampa il blocco variabili per setup_pipeline.sh
#   q    esci (stampa il riepilogo finale)
# ============================================================================
set -u

# --- discovery subdev (stessa logica di setup_pipeline.sh) ------------------
MDEV=""
for m in /dev/media*; do
    media-ctl -d "$m" -p 2>/dev/null | grep -q vcap-imx219 && { MDEV=$m; break; }
done
[ -n "$MDEV" ] || { echo "media device non trovato"; exit 1; }
TOPO=$(media-ctl -d "$MDEV" -p)
subdev_of() { echo "$TOPO" | awk -v ent="$1" '
    $0 ~ "entity .*" ent { found=1 }
    found && /device node name/ { print $NF; exit }'; }
SD_SENSOR=$(subdev_of "imx219")
SD_CSC=$(subdev_of "80300000.csc")
SD_GAMMA=$(subdev_of "80080000.gamma")

# NB: alcuni v4l2-ctl stampano su stdout il warning VIDIOC_SUBDEV_S_CLIENT_CAP
# prima del valore: prendiamo solo l'ULTIMA riga (quella del controllo).
getc() { v4l2-ctl -d "$1" --get-ctrl="$2" 2>/dev/null | awk -F': ' 'END {gsub(/ /,"",$2); print $2}'; }
num()  { case "${1:-}" in ''|*[!0-9]*) echo "$2" ;; *) echo "$1" ;; esac; }
setc() { v4l2-ctl -d "$1" --set-ctrl="$2=$3" 2>/dev/null; }
clamp() { v=$1; [ "$v" -lt "$2" ] && v=$2; [ "$v" -gt "$3" ] && v=$3; echo $v; }

# nome del controllo gamma (dipende dal driver)
GR=$(v4l2-ctl -d "$SD_GAMMA" -l 2>/dev/null | awk '/red.*gamma|gamma.*red/ {print $1; exit}')
GG=$(v4l2-ctl -d "$SD_GAMMA" -l 2>/dev/null | awk '/green.*gamma|gamma.*green/ {print $1; exit}')
GB=$(v4l2-ctl -d "$SD_GAMMA" -l 2>/dev/null | awk '/blue.*gamma|gamma.*blue/ {print $1; exit}')

# stato iniziale letto dai driver
WB_R=$(num "$(getc "$SD_CSC" csc_red_gain)" 50)
WB_G=$(num "$(getc "$SD_CSC" csc_green_gain)" 50)
WB_B=$(num "$(getc "$SD_CSC" csc_blue_gain)" 50)
BRI=$(num "$(getc "$SD_CSC" csc_brightness)" 50)
CON=$(num "$(getc "$SD_CSC" csc_contrast)" 50)
GAM=$(num "$(getc "$SD_GAMMA" "$GR")" 10)
EXP=$(num "$(getc "$SD_SENSOR" exposure)" 1600)
AG=$(num "$(getc "$SD_SENSOR" analogue_gain)" 150)
DG=$(num "$(getc "$SD_SENSOR" digital_gain)" 256)

show() {
    printf '\r\033[KWB r/g/b=%s/%s/%s  bri=%s con=%s  gamma=%s  exp=%s ag=%s dg=%s > ' \
        "$WB_R" "$WB_G" "$WB_B" "$BRI" "$CON" "$GAM" "$EXP" "$AG" "$DG"
}
dump() {
    echo ""
    echo "# --- valori tarati: incollare in setup_pipeline.sh ---"
    echo "WB_RED=$WB_R ; WB_GREEN=$WB_G ; WB_BLUE=$WB_B"
    echo "CSC_BRIGHTNESS=$BRI ; CSC_CONTRAST=$CON"
    echo "GAMMA=$GAM"
    echo "SENSOR_EXPOSURE=$EXP ; SENSOR_AGAIN=$AG ; SENSOR_DGAIN=$DG"
}

echo "Taratura live (r/R b/B n/N WB - l/L k/K bri/con - g/G gamma - e/E a/A d/D sensore - p dump - q esci)"
OLDSTTY=$(stty -g)
trap 'stty "$OLDSTTY"; echo' EXIT INT TERM
stty -icanon -echo min 1 time 0

while :; do
    show
    key=$(dd bs=1 count=1 2>/dev/null)
    case "$key" in
        r) WB_R=$(clamp $((WB_R-2)) 0 100); setc $SD_CSC csc_red_gain $WB_R ;;
        R) WB_R=$(clamp $((WB_R+2)) 0 100); setc $SD_CSC csc_red_gain $WB_R ;;
        b) WB_B=$(clamp $((WB_B-2)) 0 100); setc $SD_CSC csc_blue_gain $WB_B ;;
        B) WB_B=$(clamp $((WB_B+2)) 0 100); setc $SD_CSC csc_blue_gain $WB_B ;;
        n) WB_G=$(clamp $((WB_G-2)) 0 100); setc $SD_CSC csc_green_gain $WB_G ;;
        N) WB_G=$(clamp $((WB_G+2)) 0 100); setc $SD_CSC csc_green_gain $WB_G ;;
        l) BRI=$(clamp $((BRI-2)) 0 100);  setc $SD_CSC csc_brightness $BRI ;;
        L) BRI=$(clamp $((BRI+2)) 0 100);  setc $SD_CSC csc_brightness $BRI ;;
        k) CON=$(clamp $((CON-2)) 0 100);  setc $SD_CSC csc_contrast $CON ;;
        K) CON=$(clamp $((CON+2)) 0 100);  setc $SD_CSC csc_contrast $CON ;;
        g) GAM=$(clamp $((GAM-1)) 1 40)
           setc $SD_GAMMA "$GR" $GAM; setc $SD_GAMMA "$GG" $GAM; setc $SD_GAMMA "$GB" $GAM ;;
        G) GAM=$(clamp $((GAM+1)) 1 40)
           setc $SD_GAMMA "$GR" $GAM; setc $SD_GAMMA "$GG" $GAM; setc $SD_GAMMA "$GB" $GAM ;;
        e) EXP=$(clamp $((EXP-100)) 4 1759); setc $SD_SENSOR exposure $EXP ;;
        E) EXP=$(clamp $((EXP+100)) 4 1759); setc $SD_SENSOR exposure $EXP ;;
        a) AG=$(clamp $((AG-10)) 0 232);   setc $SD_SENSOR analogue_gain $AG ;;
        A) AG=$(clamp $((AG+10)) 0 232);   setc $SD_SENSOR analogue_gain $AG ;;
        d) DG=$(clamp $((DG-50)) 256 4095); setc $SD_SENSOR digital_gain $DG ;;
        D) DG=$(clamp $((DG+50)) 256 4095); setc $SD_SENSOR digital_gain $DG ;;
        p) dump ;;
        q) dump; exit 0 ;;
    esac
done
