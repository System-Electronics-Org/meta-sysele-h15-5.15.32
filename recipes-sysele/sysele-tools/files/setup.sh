#!/bin/sh
# Ricrea i JSON della pipeline dopo un flash. Uso: sh setup.sh
S=${SYSELE_VISION_CONFIG:-/home/root/apps/webserver/resources/configs/vision_config1.json}
OUT=${SYSELE_JSON_DIR:-/home/root}
[ -f "$S" ] || { echo "manca $S"; exit 1; }
python3 - "$S" "$OUT" <<'PY'
import json, sys
c = json.load(open(sys.argv[1]))
c["input_video"]["sensor_id"] = "SENSOR_1"
c["application_input_streams"]["resolutions"] = [{"width": 1280, "height": 800, "framerate": 30,
    "pool_max_buffers": 10, "scaling_mode": "LETTERBOX_MIDDLE"}]
out = sys.argv[2]
json.dump(c, open(out + "/t02_frontend_dsi.json", "w"), indent=4)
s = json.dumps(c, indent=4).replace('"framerate": 30', '"framerate": 60')
open(out + "/t02_frontend_dsi_60.json", "w").write(s)
print("json ricreati")
PY
chmod +x /home/root/tests/*.sh 2>/dev/null || true
