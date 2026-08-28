#!/usr/bin/env python3
"""Minimal AppleMIDI / RTP-MIDI client, stdlib only.

Simulates sending a MIDI note over the network to a rtpmidid session (e.g.
open303-pi's Pi) without needing a real controller, a DAW, or the Windows
rtpMIDI GUI. Useful for testing the WiFi MIDI path in isolation.

Protocol: RFC 6295 (RTP MIDI) + the AppleMIDI session-establishment
extension (invitation handshake on the control port and control+1, the
"data" port).

Usage:
    python3 rtpmidi_test.py <host> [control_port] [pause_seconds]

    host           IP/hostname of the rtpmidid target (e.g. the Pi)
    control_port   default 5004 (rtpmidid's [rtpmidi_announce] port)
    pause_seconds  optional pause between session setup and sending the
                   note -- gives you time to check `--list-midi-ports` on
                   the target and point a consumer at the freshly created
                   port before it's needed (rtpmidid creates a NEW ALSA
                   port per connection and does not always clean up old
                   ones -- see recap-open303-pi.md, point 33).

Known caveats (see recap-open303-pi.md for the full story):
- The data-port invitation's acknowledgement sometimes never arrives even
  though the server accepted the session correctly (confirmed via its own
  logs) -- this script tolerates that and proceeds anyway.
- Every run creates a brand new session/port on the server; there is no
  reuse of a previous session from a prior run.
"""
import random
import socket
import struct
import sys
import time

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.64"
CONTROL_PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 5004
DATA_PORT = CONTROL_PORT + 1
SESSION_NAME = b"PyMIDI"

SSRC = random.randint(0, 0xFFFFFFFF)
TOKEN = random.randint(0, 0xFFFFFFFF)


def invite(sock, port):
    """AppleMIDI invitation handshake on one port (control or data)."""
    packet = struct.pack(">HH I I", 0xFFFF, 0x494E, 2, TOKEN)  # 0xFFFF 'IN' version=2 token
    packet += struct.pack(">I", SSRC) + SESSION_NAME + b"\x00"
    sock.sendto(packet, (HOST, port))
    sock.settimeout(3)
    data, _ = sock.recvfrom(1024)
    signature, command = struct.unpack(">HH", data[:4])
    if signature != 0xFFFF or command != 0x4F4B:  # 'OK'
        raise RuntimeError(f"Invitation refused on port {port}: {data!r}")
    _, _, server_ssrc = struct.unpack(">III", data[4:16])
    name = data[16:].split(b"\x00")[0].decode(errors="replace")
    print(f"  port {port}: accepted by '{name}' (ssrc={server_ssrc:08x})")
    return server_ssrc


def send_midi(sock, seq, timestamp, midi_bytes):
    header = struct.pack(">BBHII", 0x80, 0xE1, seq, timestamp, SSRC)
    cmd = bytes([len(midi_bytes) & 0x0F]) + midi_bytes
    sock.sendto(header + cmd, (HOST, DATA_PORT))


def main():
    print(f"Connecting to {HOST}:{CONTROL_PORT} (data port {DATA_PORT}) as '{SESSION_NAME.decode()}'")
    ctrl_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    data_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    invite(ctrl_sock, CONTROL_PORT)
    try:
        invite(data_sock, DATA_PORT)
    except (RuntimeError, socket.timeout, TimeoutError) as e:
        print(f"  (data port ack not received: {e} -- proceeding anyway, "
              f"server may have accepted it silently)")
    print("Session established (or assumed so).")

    pause = float(sys.argv[3]) if len(sys.argv) > 3 else 0.0
    if pause > 0:
        print(f"Session open -- pausing {pause:.0f}s before sending "
              f"(check/point a consumer at this fresh port now)...")
        time.sleep(pause)

    seq = 0
    ts = 0
    note, vel = 48, 80  # C3, moderate velocity

    print(f"Sending Note On  note={note} vel={vel}")
    send_midi(data_sock, seq, ts, bytes([0x90, note, vel]))
    seq += 1
    ts += 1000
    time.sleep(2.0)

    print(f"Sending Note Off note={note}")
    send_midi(data_sock, seq, ts, bytes([0x80, note, 0]))
    print("Done.")


if __name__ == "__main__":
    main()
