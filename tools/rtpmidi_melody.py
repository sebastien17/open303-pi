#!/usr/bin/env python3
"""Envoie une petite melodie en RTP-MIDI (Wi-Fi) vers le Pi.

Reprend le handshake AppleMIDI de tools/rtpmidi_test.py, mais joue
plusieurs notes espacees pour permettre une ecoute confortable.
"""
import random
import socket
import struct
import sys
import time

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.64"
CONTROL_PORT = 5004
DATA_PORT = CONTROL_PORT + 1
SESSION_NAME = b"PyMIDI"

SSRC = random.randint(0, 0xFFFFFFFF)
TOKEN = random.randint(0, 0xFFFFFFFF)

seq = 0
ts = 0


def invite(sock, port):
    packet = struct.pack(">HH I I", 0xFFFF, 0x494E, 2, TOKEN)
    packet += struct.pack(">I", SSRC) + SESSION_NAME + b"\x00"
    sock.sendto(packet, (HOST, port))
    sock.settimeout(3)
    data, _ = sock.recvfrom(1024)
    name = data[16:].split(b"\x00")[0].decode(errors="replace")
    print(f"  port {port}: accepte par '{name}'")


def send_midi(sock, midi_bytes):
    global seq, ts
    header = struct.pack(">BBHII", 0x80, 0xE1, seq, ts, SSRC)
    sock.sendto(header + bytes([len(midi_bytes) & 0x0F]) + midi_bytes, (HOST, DATA_PORT))
    seq += 1
    ts += 500


def main():
    ctrl = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    data = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    print(f"Connexion RTP-MIDI a {HOST}:{CONTROL_PORT}")
    invite(ctrl, CONTROL_PORT)
    try:
        invite(data, DATA_PORT)
    except Exception:
        pass  # ack du port data souvent absent, sans consequence

    print("Session ouverte -- attente 4 s (le Pi doit detecter le nouveau port)")
    time.sleep(4)

    # Reglages : son 303 bien typé
    for cc_num, val in ((70, 0), (74, 45), (71, 105), (1, 100), (73, 70), (7, 110),
                        (25, 0), (26, 55), (27, 0)):
        send_midi(data, bytes([0xB0, cc_num, val]))
        time.sleep(0.05)

    melody = [36, 48, 39, 36, 43, 36, 46, 36]
    print(f"Envoi de {len(melody)} notes en Wi-Fi...")
    for i, n in enumerate(melody, 1):
        vel = 110 if i % 4 == 1 else 80          # accent sur le 1er temps
        send_midi(data, bytes([0x90, n, vel]))
        time.sleep(0.32)
        send_midi(data, bytes([0x80, n, 0]))
        time.sleep(0.06)
    print("Termine.")


if __name__ == "__main__":
    main()
