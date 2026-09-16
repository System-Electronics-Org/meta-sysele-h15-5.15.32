#!/bin/sh
# Uso: ./stream.sh [30|60] [secondi|inf]
# Percorsi: JSON in $SYSELE_JSON_DIR (default /home/root), log in $STREAM_LOG.
FPS=${1:-30}; DUR=${2:-10}
DIR=${SYSELE_JSON_DIR:-/home/root}
LOG=${STREAM_LOG:-/tmp/stream.log}
case "$FPS" in
  30) J=$DIR/t02_frontend_dsi.json ;;
  60) J=$DIR/t02_frontend_dsi_60.json ;;
  *)  echo "primo argomento: 30 oppure 60"; exit 1 ;;
esac
[ -f "$J" ] || { echo "manca $J, lancia setup.sh"; exit 1; }
echo "fps=$FPS durata=$DUR   (log in $LOG)"
if [ "$DUR" = "inf" ]; then T=""; else T="timeout $DUR"; fi
$T gst-launch-1.0 -v hailofrontendbinsrc config-file-path=$J name=fe \
  fe.src_0 ! queue leaky=downstream max-size-buffers=5 ! videoconvert n-threads=4 ! \
  video/x-raw,format=BGR ! \
  fpsdisplaysink video-sink="kmssink driver-name=hailo-drm force-modesetting=true can-scale=false" \
  text-overlay=false sync=false > $LOG 2>&1
echo "--- risultato ---"
grep -o "rendered: [0-9]*, dropped: [0-9]*, current: [0-9.]*, average: [0-9.]*" $LOG | tail -1
echo "errori nel log: $(grep -ci "error\|SIGSEGV" $LOG)"
echo "ultima attesa lane: $(dmesg | grep -oE "Lanes ready after [0-9]+ us|Timed Out: DSI-DPhy[^(]*" | tail -1)"
