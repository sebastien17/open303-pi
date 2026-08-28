#!/usr/bin/env python3
"""Petite interface web de pilotage pour open303-pi.

Ne touche jamais au binaire audio directement : elle se contente de
reecrire /etc/default/open303 (canal MIDI, peripherique de sortie) et de
redemarrer open303.service via un script sudo scope (voir apply_config.sh
et systemd/open303-web.sudoers). Aucun etat partage avec le thread audio
temps reel.
"""

import re
import subprocess

from flask import Flask, abort, redirect, render_template, request, url_for

DEFAULT_FILE = "/etc/default/open303"
BINARY = "/usr/local/bin/open303_pi_host"
APPLY_SCRIPT = "/usr/local/sbin/open303-web-apply.sh"

# Caracteres attendus dans un nom de peripherique ALSA/RtAudio (ex: "KT USB
# Audio (USB Audio)"). Pas d'authentification sur cette interface : on
# n'ecrit jamais tel quel dans /etc/default/open303 (via sudo) une valeur qui
# ne matche pas ce format, meme si subprocess.run() sans shell=True exclut
# deja l'injection de commande classique.
AUDIO_DEVICE_RE = re.compile(r"^[A-Za-z0-9 _\-().:]{0,100}$")

app = Flask(__name__)


def list_audio_devices():
    """Interroge le binaire lui-meme (--list-devices) pour la liste exacte
    des peripheriques qu'il verrait au demarrage -- evite toute divergence
    avec une enumeration ALSA faite cote Python."""
    try:
        out = subprocess.run(
            [BINARY, "--list-devices"], capture_output=True, text=True, timeout=5
        ).stdout
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return []
    devices = []
    for line in out.splitlines():
        m = re.match(r"\s*\[(\d+)\]\s+(.*)", line)
        if m:
            devices.append(m.group(2).strip())
    return devices


def read_current_config():
    channel = None  # None = tous les canaux
    audio_device = ""
    try:
        with open(DEFAULT_FILE) as f:
            content = f.read()
    except FileNotFoundError:
        content = ""
    m = re.search(r"EXTRA_ARGS=(.*)", content)
    extra_args = m.group(1).strip() if m else ""
    m = re.search(r"--channel\s+(\d+)", extra_args)
    if m:
        channel = int(m.group(1))
    m = re.search(r'--audio-device\s+"([^"]*)"', extra_args)
    if m:
        audio_device = m.group(1)
    return channel, audio_device


@app.route("/")
def index():
    channel, audio_device = read_current_config()
    return render_template(
        "index.html",
        channel=channel,
        audio_device=audio_device,
        devices=list_audio_devices(),
    )


@app.route("/apply", methods=["POST"])
def apply():
    channel_raw = request.form.get("channel", "all")
    audio_device = request.form.get("audio_device", "").strip()

    if not AUDIO_DEVICE_RE.match(audio_device):
        abort(400, "Nom de peripherique invalide.")

    extra_args = ""
    if channel_raw != "all":
        try:
            channel = int(channel_raw)
        except ValueError:
            abort(400, "Canal MIDI invalide.")
        if not 0 <= channel <= 15:
            abort(400, "Canal MIDI invalide (0-15 attendu).")
        extra_args += f"--channel {channel} "
    if audio_device:
        extra_args += f'--audio-device "{audio_device}" '

    subprocess.run(
        ["sudo", "-n", APPLY_SCRIPT, extra_args.strip()],
        check=True,
    )
    return redirect(url_for("index"))


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=8303)
