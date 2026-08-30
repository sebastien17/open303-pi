#!/usr/bin/env python3
"""Petite interface web de pilotage pour open303-pi.

Ne touche jamais au binaire audio directement : elle se contente de
reecrire /etc/default/open303 (canal MIDI, peripherique de sortie) et de
redemarrer open303.service via un script sudo scope (voir apply_config.sh
et systemd/open303-web.sudoers). Aucun etat partage avec le thread audio
temps reel.
"""

import io
import math
import re
import subprocess

import qrcode
from flask import Flask, abort, redirect, render_template, request, send_file, url_for

DEFAULT_FILE = "/etc/default/open303"
BINARY = "/usr/local/bin/open303_pi_host"
APPLY_SCRIPT = "/usr/local/sbin/open303-web-apply.sh"
# Ecrit par open303_pi_host --meter-file, dans /run (tmpfs = RAM). Volontairement
# un simple fichier : le lire coute une poignee de microsecondes, alors qu'un
# `journalctl` lance 2 fois par seconde pour la meme information volerait du CPU
# au thread audio temps reel sur un Pi 3B+.
METER_FILE = "/run/open303/level"

# Caracteres attendus dans un nom de peripherique ALSA/RtAudio/RtMidi (ex:
# "KT USB Audio (USB Audio)", "Elektron Digitakt II:...24:0"). Pas
# d'authentification sur cette interface : on n'ecrit jamais tel quel dans
# /etc/default/open303 (via sudo) une valeur qui ne matche pas ce format,
# meme si subprocess.run() sans shell=True exclut deja l'injection de
# commande classique.
DEVICE_NAME_RE = re.compile(r"^[A-Za-z0-9 _\-().:]{0,100}$")

app = Flask(__name__)


def current_output_device():
    """Peripheriques de sortie deja ouverts en exclusif par open303.service
    (le device actif) n'apparaissent PAS dans `--list-devices` -- RtAudio ne
    peut pas sonder un device qu'un autre process a deja ouvert. Constate en
    pratique : la carte USB active disparait de l'enumeration tant que le
    service tourne. On retrouve donc le device reellement utilise via la
    derniere ligne "Sortie audio: ..." des logs, pour l'ajouter a la liste."""
    try:
        out = subprocess.run(
            ["journalctl", "-u", "open303.service", "-n", "200", "--no-pager"],
            capture_output=True, text=True, timeout=5,
        ).stdout
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return ""
    match = None
    for line in out.splitlines():
        m = re.search(r"Sortie audio:\s*(.+)", line)
        if m:
            match = m.group(1).strip()
    return match or ""


def list_audio_devices():
    """Interroge le binaire lui-meme (--list-devices) pour la liste exacte
    des peripheriques qu'il verrait au demarrage -- evite toute divergence
    avec une enumeration ALSA faite cote Python. Complete avec le device
    actuellement actif (cf current_output_device()) s'il manque a l'appel."""
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

    active = current_output_device()
    if active and active not in devices:
        devices.insert(0, active)
    return devices


def detected_channels():
    """Canaux MIDI sur lesquels des notes sont reellement arrivees recemment,
    numerotes 1-16 comme sur le materiel.

    Evite le tatonnement le plus courant : on ne sait pas forcement sur quel
    canal emet son sequenceur, et un filtre --channel mal choisi donne un
    silence total sans le moindre indice. Lu depuis les logs (une seule fois
    par affichage de page, pas en boucle)."""
    try:
        out = subprocess.run(
            ["journalctl", "-u", "open303.service", "-n", "400", "--no-pager"],
            capture_output=True, text=True, timeout=5,
        ).stdout
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return []
    found = set()
    for line in out.splitlines():
        m = re.search(r"Note On\s+ch=(\d+)", line)
        if m:
            found.add(int(m.group(1)) + 1)  # 0-15 en interne -> 1-16 affiche
    return sorted(found)


def current_midi_port():
    """Dernier port MIDI reellement ouvert par open303.service, lu dans ses
    logs (ligne "MIDI ouvert sur: ..."). Contrairement a l'audio, RtMidi ne
    bloque pas l'enumeration d'un port deja utilise par un autre processus
    (les sequenceurs ALSA supportent plusieurs abonnes) -- pas besoin de
    fusionner ce resultat dans list_midi_ports(), juste de l'afficher."""
    try:
        out = subprocess.run(
            ["journalctl", "-u", "open303.service", "-n", "200", "--no-pager"],
            capture_output=True, text=True, timeout=5,
        ).stdout
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return ""
    match = None
    for line in out.splitlines():
        m = re.search(r"MIDI ouvert sur:\s*(.+)", line)
        if m:
            match = m.group(1).strip()
    return match or ""


def format_status(config_label, active_label):
    """Une seule ligne d'affichage : la config si elle force un choix
    explicite, sinon "auto" avec le choix reellement actif entre
    parentheses ("auto" seul serait peu informatif -- ca ne dit pas lequel
    des peripheriques disponibles a ete retenu)."""
    if config_label:
        return config_label
    return f"auto ({active_label})" if active_label else "auto"


def midi_port_label(name):
    """Meme regle que dans list_midi_ports() : c'est soit "Midi Through"
    (Wi-Fi via rtpmidid ou boucle locale), soit forcement un controleur USB
    reel dans ce projet. Utilise aussi bien pour les options du menu que
    pour l'affichage du port configure/actif dans le bloc STATUS."""
    if not name:
        return ""
    return "WiFi (rtpmidid)" if "Midi Through" in name else f"USB: {name}"


def list_midi_ports():
    """Interroge le binaire (--list-midi-ports). Exclut les ports internes
    de rtpmidid ("Network Export", "Announcements") : verifie empiriquement
    qu'ils ne relaient PAS le MIDI local/reseau vers un abonne tiers (ce sont
    des ports de service, pas une source generique) -- les proposer dans le
    menu ne ferait que produire un choix qui semble valide mais ne recoit
    jamais rien. "Midi Through" reste propose : c'est le point d'entree reel
    aussi bien pour un controleur USB que pour le MIDI recu via rtpmidid
    (cf commentaires dans pickMidiPort(), src/main.cpp).

    Retourne des dicts {value, label} plutot que de simples chaines : les
    noms ALSA bruts ("Midi Through:...", "Elektron Digitakt II:...24:0") ne
    disent rien d'explicite a l'utilisateur sur USB vs Wi-Fi. On ne peut pas
    distinguer plus finement au niveau ALSA -- tout port qui n'est ni
    "Midi Through" ni interne a rtpmidid est, dans ce projet, forcement un
    controleur MIDI classe USB reel -- donc le label se resume a ces deux
    cas. Seul le label affiche est enjolive : la valeur soumise au
    formulaire (matchee cote binaire par sous-chaine) reste le nom ALSA
    exact."""
    try:
        out = subprocess.run(
            [BINARY, "--list-midi-ports"], capture_output=True, text=True, timeout=5
        ).stdout
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return []
    ports = []
    for line in out.splitlines():
        m = re.match(r"\s*\[(\d+)\]\s+(.*)", line)
        if not m or "rtpmidid" in m.group(2):
            continue
        name = m.group(2).strip()
        ports.append({"value": name, "label": midi_port_label(name)})
    return ports


def read_current_config():
    channel = None  # None = tous les canaux
    audio_device = ""
    midi_port = ""
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
    m = re.search(r'--midi-port\s+"([^"]*)"', extra_args)
    if m:
        midi_port = m.group(1)
    return channel, audio_device, midi_port


@app.route("/")
def index():
    channel, audio_device, midi_port = read_current_config()
    return render_template(
        "index.html",
        channel=channel,
        audio_device=audio_device,
        audio_status=format_status(audio_device, current_output_device()),
        devices=list_audio_devices(),
        midi_port=midi_port,
        midi_status=format_status(midi_port_label(midi_port), midi_port_label(current_midi_port())),
        midi_ports=list_midi_ports(),
        detected=detected_channels(),
    )


@app.route("/api/meter")
def api_meter():
    """Niveau crete de la sortie audio, relu depuis METER_FILE.

    Volontairement minimal (une lecture de ~10 octets en RAM, pas de
    sous-processus) : cet endpoint est interroge en boucle par la page, et
    tourne sur le meme Pi que le thread audio temps reel."""
    try:
        with open(METER_FILE) as f:
            peak = float(f.read().strip())
    except (FileNotFoundError, ValueError, OSError):
        return {"available": False, "peak": 0.0, "dbfs": None}
    dbfs = 20.0 * math.log10(peak) if peak > 0.0 else None
    return {"available": True, "peak": peak, "dbfs": dbfs}


@app.route("/qr.png")
def qr_png():
    """QR code pointant vers l'URL reellement utilisee pour acceder a la
    page (request.url_root reprend le Host: envoye par le client, donc
    l'IP/port effectivement joignables sur le reseau local) -- genere a la
    volee, pas de cache, l'IP du Pi pouvant changer entre deux boots.

    Couleurs alignees sur le theme terminal de la page (vert vif sur fond
    quasi-noir) plutot que le noir/blanc par defaut : contraste toujours
    largement suffisant pour un scan telephone, la luminance entre les deux
    couleurs restant tres marquee."""
    qr = qrcode.QRCode(border=2)
    qr.add_data(request.url_root)
    qr.make(fit=True)
    img = qr.make_image(fill_color="#aaffbb", back_color="#0b0f0a")
    buf = io.BytesIO()
    img.save(buf, format="PNG")
    buf.seek(0)
    return send_file(buf, mimetype="image/png")


@app.route("/apply", methods=["POST"])
def apply():
    channel_raw = request.form.get("channel", "all")
    audio_device = request.form.get("audio_device", "").strip()
    midi_port = request.form.get("midi_port", "").strip()

    if not DEVICE_NAME_RE.match(audio_device):
        abort(400, "Invalid device name.")
    if not DEVICE_NAME_RE.match(midi_port):
        abort(400, "Invalid MIDI port name.")

    extra_args = ""
    if channel_raw != "all":
        try:
            channel = int(channel_raw)
        except ValueError:
            abort(400, "Invalid MIDI channel.")
        if not 0 <= channel <= 15:
            abort(400, "Invalid MIDI channel (expected 0-15).")
        extra_args += f"--channel {channel} "
    if audio_device:
        extra_args += f'--audio-device "{audio_device}" '
    if midi_port:
        extra_args += f'--midi-port "{midi_port}" '

    subprocess.run(
        ["sudo", "-n", APPLY_SCRIPT, extra_args.strip()],
        check=True,
    )
    return redirect(url_for("index"))


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=8303)
