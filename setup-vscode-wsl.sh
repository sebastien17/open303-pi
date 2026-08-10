#!/usr/bin/env bash
# Configuration VS Code pour un usage WSL NATIF (extension Remote - WSL),
# avec cross-compilation Docker vers le Raspberry Pi (arm64 / Pi OS 64-bit).
#
# Modele retenu :
#   - edition + IntelliSense + compilation rapide : natif WSL, en x86_64
#     (dossier build-wsl/) -> boucle courte, aucune emulation QEMU
#   - binaire reellement deploye sur le Pi : docker/build-cross.sh (arm64)
#     -> dossier build-cross/
#
# Les en-tetes RtAudio/RtMidi/ALSA sont les memes dans les deux cas, donc
# IntelliSense reste fidele ; seuls les flags d'architecture different
# (le bloc Cortex-A53 du CMakeLists ne s'active pas sur x86, par conception).
#
# A lancer UNE FOIS depuis WSL, a la racine du projet :
#   bash setup-vscode-wsl.sh

set -euo pipefail

if [ ! -f CMakeLists.txt ] || [ ! -d docker ]; then
  echo "Erreur: lancez ce script depuis la racine de open303-pi." >&2
  exit 1
fi

# ---------------------------------------------------------------------------
# 0. Bits executables (perdus des qu'un outil cote Windows reecrit le fichier)
# ---------------------------------------------------------------------------
chmod +x docker/build-cross.sh systemd/install-service.sh 2>/dev/null || true

# ---------------------------------------------------------------------------
# 1. Dependances de developpement cote WSL
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
fi

# ---------------------------------------------------------------------------
# 2. Correctifs amont
# ---------------------------------------------------------------------------
# third_party/Open303 est un vendor drop patche directement dans les fichiers
# (prototypeTable, idle - voir commentaires dans rosic_MipMappedWaveTable.h et
# rosic_Open303.h) : build-wsl et build-cross lisent le meme repertoire, rien
# a refaire ici.

mkdir -p .vscode

# ---------------------------------------------------------------------------
# .vscode/settings.json
# ---------------------------------------------------------------------------
cat > .vscode/settings.json <<'SETTINGS_EOF'
{
  // IntelliSense se base sur compile_commands.json genere par CMake : il voit
  // ainsi exactement les memes flags que le build, dont -include cstring
  // -include climits, sans lesquels le code d'Open303 semble plein d'erreurs.
  "C_Cpp.default.compileCommands": "${workspaceFolder}/build-wsl/compile_commands.json",
  "C_Cpp.default.cppStandard": "c++17",

  // build-wsl/ = compilation native x86 dans WSL (iteration rapide).
  // build-cross/ = binaire arm64 pour le Pi, produit par docker/build-cross.sh.
  "cmake.buildDirectory": "${workspaceFolder}/build-wsl",
  "cmake.configureOnOpen": false,

  "files.associations": {
    "*.h": "cpp"
  },

  "search.exclude": {
    "**/build-wsl": true,
    "**/build-cross": true,
    "**/third_party/Open303/Libraries": true,
    "**/third_party/Open303/Build": true,
    "**/.git": true
  },
  "files.watcherExclude": {
    "**/build-wsl/**": true,
    "**/build-cross/**": true,
    "**/third_party/**": true
  },

  "[cpp]": {
    "editor.tabSize": 2,
    "editor.insertSpaces": true,
    "editor.detectIndentation": false
  },
  "[shellscript]": {
    "editor.tabSize": 2
  },
  "files.trimTrailingWhitespace": true,
  "files.insertFinalNewline": true
}
SETTINGS_EOF

# ---------------------------------------------------------------------------
# .vscode/extensions.json
# ---------------------------------------------------------------------------
cat > .vscode/extensions.json <<'EXTENSIONS_EOF'
{
  "recommendations": [
    "ms-vscode-remote.remote-wsl",
    "ms-vscode.cpptools",
    "ms-vscode.cmake-tools"
  ]
}
EXTENSIONS_EOF

# ---------------------------------------------------------------------------
# .vscode/c_cpp_properties.json
# ---------------------------------------------------------------------------
cat > .vscode/c_cpp_properties.json <<'CCPP_EOF'
{
  "version": 4,
  "configurations": [
    {
      "name": "WSL (x86_64)",
      // compile_commands.json fait foi ; includePath n'est qu'un repli tant que
      // la tache "Configurer (CMake natif)" n'a pas encore ete lancee.
      "compileCommands": "${workspaceFolder}/build-wsl/compile_commands.json",
      "includePath": [
        "${workspaceFolder}/src",
        "${workspaceFolder}/third_party/Open303/Source/DSPCode",
        "/usr/include"
      ],
      "cStandard": "c17",
      "cppStandard": "c++17",
      "intelliSenseMode": "linux-gcc-x64"
    }
  ]
}
CCPP_EOF

# ---------------------------------------------------------------------------
# .vscode/tasks.json
# ---------------------------------------------------------------------------
cat > .vscode/tasks.json <<'TASKS_EOF'
{
  "version": "2.0.0",
  "tasks": [
    {
      "label": "Configurer (CMake natif)",
      "type": "shell",
      "command": "cmake -S . -B build-wsl -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_BUILD_TYPE=Release",
      "detail": "Genere build-wsl/compile_commands.json, la source de verite d'IntelliSense.",
      "problemMatcher": []
    },
    {
      "label": "Compiler (natif WSL, x86)",
      "type": "shell",
      "command": "cmake --build build-wsl -j $(nproc)",
      "detail": "Verification de compilation rapide, sans emulation. Le binaire est x86 : utile pour --sine et pour valider le code, pas pour le Pi.",
      "group": { "kind": "build", "isDefault": true },
      "problemMatcher": ["$gcc"],
      "dependsOn": ["Configurer (CMake natif)"]
    },
    {
      "label": "Cross-compiler pour le Pi (Docker, arm64)",
      "type": "shell",
      "command": "./docker/build-cross.sh",
      "detail": "Build officiel : conteneur Debian trixie arm64 via QEMU. Produit build-cross/open303_pi_host.",
      "group": "build",
      "problemMatcher": ["$gcc"]
    },
    {
      "label": "Deployer sur le Pi (scp)",
      "type": "shell",
      "command": "scp build-cross/open303_pi_host ${input:piHost}:~/",
      "detail": "Transfere le binaire arm64 vers le Pi.",
      "problemMatcher": []
    },
    {
      "label": "Cross-compiler + deployer",
      "dependsOrder": "sequence",
      "dependsOn": [
        "Cross-compiler pour le Pi (Docker, arm64)",
        "Deployer sur le Pi (scp)"
      ],
      "problemMatcher": []
    },
    {
      "label": "Redemarrer le service sur le Pi",
      "type": "shell",
      "command": "ssh ${input:piHost} 'sudo systemctl restart open303.service && systemctl --no-pager status open303.service'",
      "detail": "Apres deploiement, si vous utilisez l'unite systemd de systemd/.",
      "problemMatcher": []
    },
    {
      "label": "Journal du service sur le Pi",
      "type": "shell",
      "command": "ssh ${input:piHost} 'sudo journalctl -u open303.service -f'",
      "detail": "Logs en direct : c'est la qu'apparaissent les xruns et les evenements MIDI perdus.",
      "isBackground": true,
      "problemMatcher": []
    },
    {
      "label": "Nettoyer build-wsl",
      "type": "shell",
      "command": "rm -rf build-wsl",
      "problemMatcher": []
    }
  ],
  "inputs": [
    {
      "id": "piHost",
      "type": "promptString",
      "description": "Cible SSH du Raspberry Pi",
      "default": "pi@raspberrypi.local"
    }
  ]
}
TASKS_EOF

# ---------------------------------------------------------------------------
# .vscode/launch.json
# ---------------------------------------------------------------------------
cat > .vscode/launch.json <<'LAUNCH_EOF'
{
  "version": "0.2.0",
  "configurations": [
    {
      // Debug du binaire x86 natif, dans WSL.
      // WSL n'expose pas de peripherique MIDI USB par defaut : le programme
      // s'arretera sur "Aucun port MIDI trouve" sauf si vous avez attache le
      // controleur avec usbipd-win. Reste utile pour poser des points d'arret
      // sur le parsing d'arguments, l'init du moteur et la file MIDI.
      "name": "WSL natif (x86)",
      "type": "cppdbg",
      "request": "launch",
      "program": "${workspaceFolder}/build-wsl/open303_pi_host",
      "args": ["44100", "256", "--sine"],
      "cwd": "${workspaceFolder}",
      "stopAtEntry": false,
      "MIMode": "gdb",
      "miDebuggerPath": "/usr/bin/gdb",
      "preLaunchTask": "Compiler (natif WSL, x86)",
      "setupCommands": [
        {
          "description": "Affichage lisible des conteneurs STL",
          "text": "-enable-pretty-printing",
          "ignoreFailures": true
        }
      ]
    },
    {
      // Debug du vrai binaire, sur le vrai materiel : la seule config qui
      // permet de voir les xruns et le comportement audio reel.
      //
      // Prerequis cote WSL :  sudo apt install gdb-multiarch
      // Sur le Pi :           sudo apt install gdbserver
      //                       gdbserver :2345 ./open303_pi_host 44100 256
      //
      // Le binaire doit porter ses symboles : passez CMAKE_BUILD_TYPE a
      // RelWithDebInfo dans docker/build-cross.sh avant de cross-compiler.
      "name": "Distant : Raspberry Pi (gdbserver)",
      "type": "cppdbg",
      "request": "launch",
      "program": "${workspaceFolder}/build-cross/open303_pi_host",
      "miDebuggerServerAddress": "raspberrypi.local:2345",
      "miDebuggerPath": "/usr/bin/gdb-multiarch",
      "cwd": "${workspaceFolder}",
      "stopAtEntry": false,
      "MIMode": "gdb",
      "setupCommands": [
        {
          "description": "Affichage lisible des conteneurs STL",
          "text": "-enable-pretty-printing",
          "ignoreFailures": true
        }
      ]
    }
  ]
}
LAUNCH_EOF

echo
echo "Configuration WSL native installee :"
echo "  .vscode/{settings,extensions,c_cpp_properties,tasks,launch}.json"
echo
echo "Si vous aviez installe la config Dev Container, .devcontainer/ est"
echo "toujours la et n'est plus utilisee : supprimez-la si vous ne comptez"
echo "pas y revenir (rm -rf .devcontainer)."
echo
echo "Etape suivante : ouvrez le projet avec l'extension Remote - WSL :"
echo "    code ."
echo "puis Ctrl+Shift+B pour compiler."
