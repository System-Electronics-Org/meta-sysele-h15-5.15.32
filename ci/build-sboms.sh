#!/bin/bash
# Dai documenti SPDX di Yocto ricava le due distinte di consegna, con i nomi che
# dicono di che tipo sono.
#
#   <immagine>-build.spdx.json      cio' che e' servito a costruire, dipendenze
#                                   di build comprese (tipo Build della tassonomia CISA)
#   <immagine>-deployed.spdx.json   solo cio' che e' installato sulla scheda, secondo
#                                   l'elenco esatto del buildhistory (tipo Deployed)
#
# Fallisce se Yocto non ha prodotto l'archivio: una build che chiede la SBOM e non la
# ottiene deve dirlo, non uscire verde a mani vuote.
set -euo pipefail

B=meta-hailo-soc/build
IMG=core-image-hailo-dev-astrial-h15
OUT="$B/tmp/deploy/images/astrial-h15"
HERE="$(cd "$(dirname "$0")" && pwd)"

ARCHIVE=$(ls -1 "$OUT/$IMG"-*.spdx.tar.zst 2>/dev/null | head -1 || true)
if [ -z "$ARCHIVE" ]; then
  echo "ERRORE: nessun archivio SPDX in $OUT."
  echo "La build ha chiesto la SBOM e non l'ha prodotta: controlla che SYSELE_SBOM"
  echo "sia arrivato a bitbake (kas scarta le variabili non dichiarate nella sua"
  echo "sezione env:) e che create-spdx risulti in INHERIT."
  exit 1
fi
echo "archivio: $(basename "$ARCHIVE")"

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
tar --zstd -xf "$ARCHIVE" -C "$WORK"

INSTALLED=$(find "$B/buildhistory/images" -name installed-package-names.txt \
            -path "*$IMG*" 2>/dev/null | head -1 || true)
[ -n "$INSTALLED" ] && echo "elenco installato: $(wc -l < "$INSTALLED") pacchetti" \
                    || echo "ATTENZIONE: buildhistory senza elenco installato, ripiego sui nomi"

python3 "$HERE/make_sbom.py" "$WORK" -t build --lean \
        -n "$IMG" -o "$OUT/$IMG-build.spdx.json"
python3 "$HERE/make_sbom.py" "$WORK" -t deployed --lean \
        ${INSTALLED:+--installed "$INSTALLED"} \
        -n "$IMG" -o "$OUT/$IMG-deployed.spdx.json"

ls -l "$OUT/$IMG"-{build,deployed}.spdx.json
