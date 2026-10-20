#!/bin/bash
# L'interruttore SBOM e' arrivato fino a bitbake? Stampa cosa vede, non cosa speriamo.
#
# Serve perche' un INHERIT condizionato a una variabile che non arriva non scatta e non
# protesta: la build riesce, semplicemente senza SBOM. E' gia' successo una volta.
set -u
cd "$(dirname "$0")/../../meta-hailo-soc" || exit 0
echo "### shell"
echo "  SYSELE_SBOM=${SYSELE_SBOM:-non impostata}"
echo "### cosa vede bitbake"
kas shell astrial-h15-ci.yml -c "bitbake -e" 2>/dev/null \
  | grep -E '^SYSELE_SBOM=|^SYSELE_BSP_VERSION=|^INHERIT=' | cut -c1-200 \
  | sed 's/^/  /' || echo "  non leggibile"
