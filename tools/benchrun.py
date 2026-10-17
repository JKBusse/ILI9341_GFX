#!/usr/bin/env python3
"""Spielt die Benchmark-Firmwares (bench/) nacheinander auf und sammelt die Ergebnisse.

  benchrun.py                       alle Bibliotheken
  benchrun.py bench_ours bench_lovyan
Ergebnis: bench/results.json und eine Tabelle auf der Konsole.
"""
import json
import os
import subprocess
import sys
import time

import serial

PIO = os.path.expanduser("~/.platformio/penv/bin/pio")
PORT = "/dev/cu.usbserial-0001"
ENVS = ["bench_adafruit", "bench_tftespi", "bench_lovyan", "bench_arduinogfx", "bench_ours"]
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def run_env(env):
    up = subprocess.run([PIO, "run", "-e", env, "-t", "upload"], cwd=ROOT, capture_output=True, text=True)
    if up.returncode:
        print(up.stdout[-1500:], up.stderr[-1500:])
        raise SystemExit(f"Upload {env} fehlgeschlagen")
    ser = serial.Serial()
    ser.port, ser.baudrate, ser.timeout = PORT, 921600, 0.5
    ser.dtr = ser.rts = False
    ser.exclusive = True
    ser.open()  # setzt das Board zurück -> Benchmark startet neu
    res = {"tests": {}, "anim": {}, "heap": None}
    deadline = time.time() + 150
    while time.time() < deadline:
        line = ser.readline().decode(errors="replace").strip()
        if not line:
            continue
        parts = line.split()
        if parts[0] == "RES" and len(parts) == 4:
            res["tests"][parts[2]] = int(parts[3])
        elif parts[0] == "ANIM" and len(parts) >= 5:
            res["anim"][parts[2]] = {"fps": float(parts[3]), "info": " ".join(parts[4:])}
        elif parts[0] == "HEAP":
            res["heap"] = {"free": int(parts[2]), "maxblk": int(parts[3])}
        elif parts[0] == "DONE":
            break
        print(f"  {line}", flush=True)
    ser.close()
    return res


def main():
    envs = sys.argv[1:] or ENVS
    path = os.path.join(ROOT, "bench", "results.json")
    results = json.load(open(path)) if os.path.exists(path) else {}
    for env in envs:
        print(f"== {env}", flush=True)
        results[env] = run_env(env)
        json.dump(results, open(path, "w"), indent=1)
    names = [e for e in ENVS if e in results]
    tests = list(results[names[0]]["tests"].keys())
    print("\n" + "Test".ljust(20) + "".join(n.replace("bench_", "")[:11].rjust(12) for n in names))
    for t in tests:
        print(t.ljust(20) + "".join(str(results[n]["tests"].get(t, "-")).rjust(12) for n in names))
    for a in sorted({a for n in names for a in results[n]["anim"]}):
        print((a + " fps").ljust(20) + "".join(
            str(results[n]["anim"].get(a, {}).get("fps", "-")).rjust(12) for n in names))


if __name__ == "__main__":
    main()
