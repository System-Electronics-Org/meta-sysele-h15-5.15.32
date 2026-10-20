#!/bin/bash
# Diagnostico: LICENSE_PATH arriva a bitbake, e il testo di licenza finisce nel documento?
# Forza create_spdx su una ricetta e rilegge il risultato. Solo per il ramo di misura.
set -u
cd "$(dirname "$0")/../../meta-hailo-soc" || exit 0

echo "### LICENSE_PATH come lo espande bitbake"
kas shell astrial-h15-ci.yml -c "bitbake -e video-encoder" 2>/dev/null \
  | grep -E '^LICENSE_PATH=' || echo "  non leggibile"

echo "### forzo create_spdx su video-encoder"
kas shell astrial-h15-ci.yml -c "bitbake -f -c create_spdx video-encoder" 2>&1 | tail -4

echo "### testo di licenza nel documento rigenerato"
python3 - <<'PY'
import json, glob
hits = glob.glob("build/tmp/deploy/spdx/astrial-h15/recipes/recipe-video-encoder.spdx.json")
if not hits:
    print("  documento non trovato")
for f in hits:
    for e in json.load(open(f)).get("hasExtractedLicensingInfos", []):
        t = e.get("extractedText")
        print(f"  {e['licenseId']} -> {len(t)} caratteri" if t else f"  {e['licenseId']} -> NULLO")
        if t:
            first = next((l for l in t.split("\n") if l.strip()), "")
            print(f"    primo rigo: {first.strip()[:70]}")
PY
