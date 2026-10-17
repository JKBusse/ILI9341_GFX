#!/usr/bin/env python3
"""Steuert die Demo über die serielle Schnittstelle und liest die Messwerte.

Beispiele:
  tftctl.py                       # 5 s mitlesen
  tftctl.py s0 b -t 8             # Plasma festhalten, Benchmark, 8 s lesen
  tftctl.py f80 s0 -t 6           # SPI auf 80 MHz, Plasma
  tftctl.py s1 @3 v -t 3          # Synthwave, 3 s laufen lassen, Display zurücklesen
  tftctl.py s4 @3 --shot bild.png # Screenshot als PNG
  tftctl.py --listen shots/       # verbunden bleiben: Enter oder BOOT-Taste = Screenshot
"""
import argparse
import os
import re
import select
import struct
import sys
import time
import zlib

import serial

STAT_RE = re.compile(r"(\w+)=([\w.]+)")


def write_png(path, w, h, rgb):
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def take_screenshot(ser, path):
    ser.reset_input_buffer()
    ser.write(b"P\n")
    ser.timeout = 0.2
    buf = b""
    deadline = time.time() + 20
    # Kopfzeile "SHOT w h\n" in den Rohdaten suchen (davor können Reste von
    # Statuszeilen stehen), danach genau w*h*2 Bytes lesen
    while time.time() < deadline:
        buf += ser.read(4096)
        i = buf.find(b"SHOT ")
        if i >= 0 and b"\n" in buf[i:]:
            j = buf.index(b"\n", i)
            _, w, h = buf[i:j].decode().split()
            w, h = int(w), int(h)
            data = buf[j + 1:]
            while len(data) < w * h * 2 and time.time() < deadline:
                data += ser.read(w * h * 2 - len(data))
            break
    else:
        raise SystemExit("keine Antwort auf Screenshot-Befehl")
    data = data[:w * h * 2]
    if len(data) != w * h * 2:
        raise SystemExit(f"unvollständig: {len(data)} von {w * h * 2} Bytes")
    save_shot(path, w, h, data)


def save_shot(path, w, h, data):
    rgb = bytearray()
    for i in range(0, len(data), 2):
        c = (data[i] << 8) | data[i + 1]
        r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
        rgb += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))
    write_png(path, w, h, bytes(rgb))
    print(f"Screenshot {w}x{h} gespeichert: {path}", flush=True)


def listen(ser, outdir):
    """Verbunden bleiben: Enter löst einen Screenshot aus; Screenshots, die das
    Board selbst schickt (BOOT-Taste, fb.screenshot() im Code), werden ebenfalls
    gespeichert."""
    os.makedirs(outdir, exist_ok=True)
    ser.write(b"L1\n")  # beim Board anmelden, damit die BOOT-Taste Bilder schickt
    print("Enter = Screenshot, Strg+C = Ende. BOOT-Taste am Board geht auch.", flush=True)
    ser.timeout = 0.05
    buf = b""
    stdin_open = True
    count = 0
    try:
        while True:
            if stdin_open and select.select([sys.stdin], [], [], 0)[0]:
                if sys.stdin.readline():
                    ser.write(b"P\n")
                else:
                    stdin_open = False  # Eingabe beendet (z.B. Pipe), weiter lauschen
            buf += ser.read(4096)
            i = buf.find(b"SHOT ")
            if i >= 0:
                j = buf.find(b"\n", i)
                if j < 0:
                    continue
                _, w, h = buf[i:j].decode().split()
                w, h = int(w), int(h)
                data = buf[j + 1:]
                deadline = time.time() + 10
                while len(data) < w * h * 2 and time.time() < deadline:
                    data += ser.read(w * h * 2 - len(data))
                if len(data) >= w * h * 2:
                    count += 1
                    name = time.strftime("shot_%Y%m%d_%H%M%S") + f"_{count:03d}.png"
                    save_shot(os.path.join(outdir, name), w, h, data[:w * h * 2])
                buf = data[w * h * 2:]
            elif b"\n" in buf:
                buf = buf[buf.rfind(b"\n") + 1:]  # Textzeilen verwerfen, Rest behalten
    except KeyboardInterrupt:
        pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmds", nargs="*", help="Befehle an die Firmware (s<n>, a, f<MHz>, b)")
    ap.add_argument("-p", "--port", default="/dev/cu.usbserial-0001")
    ap.add_argument("-t", "--seconds", type=float, default=5)
    ap.add_argument("--wait", type=float, default=0, help="Sekunden warten bevor Befehle gesendet werden")
    ap.add_argument("--shot", help="nach den Befehlen einen Screenshot als PNG speichern")
    ap.add_argument("--listen", metavar="ORDNER", help="verbunden bleiben und Screenshots speichern")
    args = ap.parse_args()

    ser = serial.Serial()
    ser.port = args.port
    ser.baudrate = 921600
    ser.timeout = 0.1
    ser.dtr = False  # kein Reset beim Öffnen
    ser.rts = False
    # Exklusiv öffnen: macOS erlaubt sonst mehreren Programmen denselben Port,
    # und die Daten werden still zwischen ihnen aufgeteilt
    ser.exclusive = True
    try:
        ser.open()
    except serial.SerialException as e:
        raise SystemExit(f"{args.port} ist belegt (läuft noch ein Monitor oder tftctl --listen?): {e}")

    # Das Öffnen des Ports setzt das Board zurück: warten, bis es läuft
    deadline = time.time() + 8
    while time.time() < deadline:
        if ser.readline().startswith(b"STAT"):
            break
    time.sleep(args.wait)
    ser.reset_input_buffer()

    for c in args.cmds:
        if c.startswith("@"):  # @<s> = lokal warten, z.B. @3
            time.sleep(float(c[1:]))
            continue
        ser.write((c + "\n").encode())
        time.sleep(0.2)

    if args.shot:
        take_screenshot(ser, args.shot)
        return
    if args.listen:
        listen(ser, args.listen)
        return

    stats = []
    end = time.time() + args.seconds
    while time.time() < end:
        line = ser.readline().decode(errors="replace").strip()
        if not line:
            continue
        print(line, flush=True)
        if line.startswith("STAT"):
            stats.append(dict(STAT_RE.findall(line)))

    # Erste Messung nach einem Befehl verwerfen (Übergangssekunde)
    stats = stats[1:] if len(stats) > 1 else stats
    if stats:
        def avg(k):
            v = [float(s[k]) for s in stats if k in s]
            return sum(v) / len(v) if v else 0
        print(f"== Mittel über {len(stats)} s: fps={avg('fps'):.1f} "
              f"render_us={avg('render_us'):.0f} display_us={avg('display_us'):.0f}")


if __name__ == "__main__":
    main()
