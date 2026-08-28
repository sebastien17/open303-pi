#!/usr/bin/env bash
# Installe l'interface web de pilotage (canal MIDI, sortie audio) sur le Pi.
# A executer APRES systemd/install-service.sh (le service open303 doit deja
# exister, cette interface ne fait que le reconfigurer/redemarrer).
#
# Usage: sudo web/install-web.sh
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WEB_DIR="$ROOT_DIR/web"
INSTALL_DIR=/usr/local/lib/open303-web

if [[ $EUID -ne 0 ]]; then
  echo "Lancez ce script avec sudo." >&2
  exit 1
fi

if ! python3 -c "import flask" 2>/dev/null; then
  echo "Module Python 'flask' introuvable. Installez-le d'abord :" >&2
  echo "  sudo apt install -y python3-flask" >&2
  exit 1
fi

echo "== Utilisateur dedie =="
if ! id open303-web &>/dev/null; then
  useradd --system --no-create-home --shell /usr/sbin/nologin open303-web
fi

echo "== Copie de l'application =="
install -d -m 755 "$INSTALL_DIR"
install -m 644 "$WEB_DIR/app.py" "$INSTALL_DIR/app.py"
install -d -m 755 "$INSTALL_DIR/templates"
install -m 644 "$WEB_DIR/templates/index.html" "$INSTALL_DIR/templates/index.html"

echo "== Script d'application (sudo scope) =="
install -m 755 -o root -g root "$WEB_DIR/open303-web-apply.sh" /usr/local/sbin/open303-web-apply.sh

echo "== Regle sudoers (validee avant installation) =="
TMP_SUDOERS="$(mktemp)"
cp "$WEB_DIR/open303-web.sudoers" "$TMP_SUDOERS"
if ! visudo -cf "$TMP_SUDOERS" >/dev/null; then
  echo "Fichier sudoers invalide, installation annulee." >&2
  rm -f "$TMP_SUDOERS"
  exit 1
fi
install -m 440 -o root -g root "$TMP_SUDOERS" /etc/sudoers.d/open303-web
rm -f "$TMP_SUDOERS"

echo "== Service systemd =="
install -m 644 -D "$ROOT_DIR/systemd/open303-web.service" /etc/systemd/system/open303-web.service

echo "== Activation + demarrage =="
systemctl daemon-reload
systemctl enable --now open303-web.service

IP="$(hostname -I 2>/dev/null | awk '{print $1}')"
echo
echo "Termine. Interface accessible sur : http://${IP:-<ip-du-pi>}:8303"
echo "Verifiez avec: sudo systemctl status open303-web.service"
echo "Logs en direct:         sudo journalctl -u open303-web.service -f"
echo
echo "Rappel : aucune authentification. Reservez cette interface a un reseau"
echo "local de confiance, ne l'exposez pas directement sur Internet."
