#!/usr/bin/env bash
# Cross-compilation pour Raspberry Pi via Docker + emulation QEMU.
#
# Usage:
#   docker/build-cross.sh                  # cible arm64 (Pi OS 64-bit) <- defaut
#   docker/build-cross.sh linux/arm/v7     # cible armhf (Pi OS 32-bit)
#
# Pre-requis (une seule fois sur la machine de dev, pas sur le Pi):
#   sudo apt install docker.io qemu-user-static binfmt-support
#
# L'enregistrement QEMU et le builder buildx sont verifies et retablis
# automatiquement par ce script (voir ensure_qemu / ensure_builder) : ils ne
# survivent pas a un redemarrage de Docker Desktop ni a un "wsl --shutdown".

set -euo pipefail

PLATFORM="${1:-linux/arm64}"
IMG="open303-cross-builder"
BUILDER="open303-multiarch"
PROBE_IMG="debian:trixie-slim"   # deja necessaire au build, donc aucun pull en plus
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Le driver "docker" par defaut ne sait pas construire pour une autre
# plateforme ; il faut un builder "docker-container". On en utilise un dedie,
# reference explicitement par --builder, pour ne pas dependre de celui qui se
# trouve etre actif ni modifier le reglage global de l'utilisateur.
ensure_builder() {
  if ! docker buildx inspect "$BUILDER" >/dev/null 2>&1; then
    echo "-- Builder buildx '$BUILDER' absent : creation"
    docker buildx create --name "$BUILDER" --driver docker-container --bootstrap >/dev/null
  fi
}

# Sans enregistrement binfmt/QEMU, un conteneur ARM echoue des sa premiere
# instruction avec "exec format error" (le noyau ne sait pas executer son
# /bin/sh). On sonde avec l'image de base du build, puis on repare si besoin.
ensure_qemu() {
  if docker run --rm --platform "$PLATFORM" "$PROBE_IMG" true >/dev/null 2>&1; then
    return
  fi
  echo "-- Emulation $PLATFORM indisponible (exec format error) : enregistrement de QEMU"
  docker run --rm --privileged tonistiigi/binfmt --install all >/dev/null
  if ! docker run --rm --platform "$PLATFORM" "$PROBE_IMG" true >/dev/null 2>&1; then
    echo "Erreur: l'emulation $PLATFORM ne fonctionne toujours pas apres" >&2
    echo "  docker run --rm --privileged tonistiigi/binfmt --install all" >&2
    echo "Verifiez que Docker Desktop tourne et que l'integration WSL est activee" >&2
    echo "pour cette distro (Settings -> Resources -> WSL Integration)." >&2
    exit 1
  fi
}

echo "== Verification de l'emulation et du builder =="
ensure_qemu
ensure_builder

echo "== Construction de l'image de build (${PLATFORM}) =="
docker buildx build --builder "$BUILDER" --platform "$PLATFORM" --load \
  -t "$IMG" -f "$ROOT_DIR/docker/Dockerfile.cross" "$ROOT_DIR"

echo "== Compilation dans le conteneur =="
docker run --rm --platform "$PLATFORM" \
  -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
  -v "$ROOT_DIR":/src -w /src "$IMG" \
  bash -c '
    set -e

    # Le conteneur tourne en root : sans precaution, tout ce quil ecrit dans
    # /src (le clone third_party/Open303 et le binaire build-cross/) appartient
    # a root cote hote, et devient non modifiable sans sudo. On restitue donc
    # la propriete a lutilisateur hote en sortie, succes comme echec.
    trap "chown -R $HOST_UID:$HOST_GID /src/third_party /src/build-cross 2>/dev/null || true" EXIT

    # third_party/Open303 est un vendor drop patche a la main (voir plus bas),
    # pas un clone vierge regenere a chaque build : on ne le re-clone donc que
    # sil est absent, et dans ce cas les deux correctifs ci-dessous sont
    # perdus (a reappliquer manuellement, cf commentaires dans les fichiers
    # cibles).
    if [ ! -d third_party/Open303 ]; then
      echo "!! third_party/Open303 absent : clone dun depot vierge, SANS les"
      echo "   correctifs amont (prototypeTable, idle). Voir README." >&2
      git clone https://github.com/RobinSchmidt/Open303 third_party/Open303
    fi

    # --- Correctifs amont, appliques directement dans le vendor drop -------
    # (RobinSchmidt/Open303 nayant pas bouge depuis ~2 ans, on patche le
    # fichier trace dans le depot plutot que de repatcher via sed a chaque
    # build) :
    #  - rosic_MipMappedWaveTable.h: prototypeTable[tableLength+4] (declare
    #    trop court dorigine, depassement de 32 octets signale par GCC en
    #    -Waggressive-loop-optimizations, risque de restructuration incorrecte
    #    de boucle a -O3).
    #  - rosic_Open303.h: idle re-arme en fin de getSample() (sinon la chaine
    #    DSP oversamplee x4 tourne en permanence meme sans note, gros poste de
    #    charge CPU sur le Pi 3B+).

    # Le cache CMake garde en memoire larchitecture ciblee au premier
    # "cmake .." (flags -mfpu/-mfloat-abi 32-bit vs 64-bit). Si on change de
    # plateforme (armv7 <-> arm64) sans nettoyer, la config est reutilisee a
    # tort. On repart donc a zero systematiquement pour eviter ce piege.
    rm -rf build-cross
    mkdir -p build-cross && cd build-cross
    cmake .. -DOPEN303_DIR=/src/third_party/Open303 -DCMAKE_BUILD_TYPE=Release
    make -j"$(nproc)"
  '

echo
echo "Binaire pret: build-cross/open303_pi_host"
echo "A transferer sur le Pi, par exemple:"
echo "  scp build-cross/open303_pi_host pi@raspberrypi.local:~/"
