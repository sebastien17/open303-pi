#!/usr/bin/env bash
# A executer SUR LE PI (pas en cross-compilation), apres avoir compile le
# binaire (voir README, sections 4 ou 4bis).
#
# Usage: sudo systemd/install-service.sh [chemin/vers/open303_pi_host]
set -euo pipefail

BIN_SRC="${1:-build/open303_pi_host}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ $EUID -ne 0 ]]; then
  echo "Lancez ce script avec sudo." >&2
  exit 1
fi

if [[ ! -x "$BIN_SRC" ]]; then
  echo "Binaire introuvable: $BIN_SRC (compilez d'abord, cf README)." >&2
  exit 1
fi

echo "== Utilisateur dedie =="
if ! id open303 &>/dev/null; then
  useradd --system --no-create-home --shell /usr/sbin/nologin -G audio open303
else
  usermod -aG audio open303
fi

echo "== Copie du binaire =="
install -m 755 "$BIN_SRC" /usr/local/bin/open303_pi_host

echo "== Fichiers de config =="
install -m 644 -D "$ROOT_DIR/systemd/open303.service" /etc/systemd/system/open303.service
install -m 644 -D "$ROOT_DIR/systemd/open303-restart.service" /etc/systemd/system/open303-restart.service
install -m 644 -D "$ROOT_DIR/systemd/99-open303-midi.rules" /etc/udev/rules.d/99-open303-midi.rules

if [[ ! -f /etc/default/open303 ]]; then
  install -m 644 -D "$ROOT_DIR/systemd/open303.default" /etc/default/open303
else
  echo "/etc/default/open303 existe deja, on ne l'ecrase pas."
fi

echo "== Rechargement systemd + udev =="
systemctl daemon-reload
udevadm control --reload-rules

echo "== Activation + demarrage =="
systemctl enable --now open303.service

echo
echo "Termine. Verifiez avec: sudo systemctl status open303.service"
echo "Logs en direct:         sudo journalctl -u open303.service -f"
