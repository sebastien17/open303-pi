#!/usr/bin/env bash
# Verifications d'environnement pour le dev WSL natif (build-wsl/, IntelliSense).
# La config VS Code elle-meme (.vscode/*.json) est committee dans le depot ;
# ce script ne la genere plus (evite d'ecraser des edits manuels).
#
# A lancer depuis WSL, a la racine du projet :
#   bash check-wsl-env.sh

set -euo pipefail

if [ ! -f CMakeLists.txt ] || [ ! -d docker ]; then
  echo "Erreur: lancez ce script depuis la racine de open303-pi." >&2
  exit 1
fi

# ---------------------------------------------------------------------------
# 1. Bits executables (perdus des qu'un outil cote Windows reecrit le fichier)
# ---------------------------------------------------------------------------
chmod +x docker/build-cross.sh systemd/install-service.sh 2>/dev/null || true

# ---------------------------------------------------------------------------
# 2. Dependances de developpement cote WSL
# ---------------------------------------------------------------------------
missing=""
for m in rtaudio rtmidi alsa; do
  pkg-config --exists "$m" 2>/dev/null || missing="$missing $m"
done
if ! command -v cmake >/dev/null 2>&1; then missing="$missing cmake"; fi

if [ -n "$missing" ]; then
  echo
  echo "!! Dependances manquantes cote WSL :$missing"
  echo "   Sans elles, la compilation native et IntelliSense ne marcheront pas."
  echo "   Installez-les puis relancez ce script :"
  echo
  echo "     sudo apt update && sudo apt install -y build-essential cmake git pkg-config \\"
  echo "         librtaudio-dev librtmidi-dev libasound2-dev gdb-multiarch"
  echo
else
  echo "OK: toutes les dependances WSL sont presentes."
fi
