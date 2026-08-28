#!/usr/bin/env bash
# Applique un nouvel EXTRA_ARGS a /etc/default/open303 et redemarre le
# service. Concu pour etre appele UNIQUEMENT via la regle sudoers dediee
# installee par install-web.sh (utilisateur open303-web, sans mot de passe,
# scope a ce seul script) -- jamais lance directement par un utilisateur
# non privilegie sans cette regle.
#
# Usage: open303-web-apply.sh ['--channel N'] ['--audio-device "NOM"']
# (un seul argument, deja assemble par l'appelant ; peut etre vide)
set -euo pipefail

EXTRA_ARGS="${1:-}"

# Revalidation defensive : meme si web/app.py filtre deja les entrees avant
# d'arriver ici, ce script tourne en root via sudo -- on ne fait pas
# confiance a l'appelant seul. N'accepte que la forme exacte attendue.
if [[ -n "$EXTRA_ARGS" ]]; then
  if ! [[ "$EXTRA_ARGS" =~ ^(--channel\ [0-9]{1,2})?\ ?(--audio-device\ \"[A-Za-z0-9 _.:()-]{0,100}\")?$ ]]; then
    echo "Argument EXTRA_ARGS rejete (format inattendu): $EXTRA_ARGS" >&2
    exit 1
  fi
fi

DEFAULT_FILE=/etc/default/open303

if [[ -f "$DEFAULT_FILE" ]] && grep -q '^EXTRA_ARGS=' "$DEFAULT_FILE"; then
  sed -i "s|^EXTRA_ARGS=.*|EXTRA_ARGS=${EXTRA_ARGS}|" "$DEFAULT_FILE"
else
  echo "EXTRA_ARGS=${EXTRA_ARGS}" >> "$DEFAULT_FILE"
fi

systemctl restart open303.service
