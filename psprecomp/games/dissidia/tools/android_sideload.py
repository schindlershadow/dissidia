#!/usr/bin/env python3
"""Side-load the game's files into the app of an Android user that adb cannot write to (a secondary user: the shell's storage is always user 0).

  android_sideload.py --device SERIAL [--source-device SERIAL2 | --dir LOCAL_WORKDIR] [--port 9998] [--parts disc0,mods,movie_jp,voice_jp]

The app (running as that user, with no game files yet) connects to 127.0.0.1:<port> on the phone, which `adb reverse` forwards to this script, and
writes what it receives into its own folder. Steps: this script sets up the reverse + the property debug.psprecomp.import_port, listens, and prints
"READY"; then start the app as the target user. Files come from a work folder on this machine (--dir) or are streamed from another connected
device's copy (--source-device, no local disk needed). Python 3 standard library only.
"""
import argparse
import os
import socket
import subprocess
import sys
import tarfile

PACKAGE = "org.psprecomp.game"
SRC_DEVICE_DIR = "/sdcard/Android/data/%s/files/game" % PACKAGE


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--device", required=True, help="the phone to load (adb serial)")
    ap.add_argument("--source-device", help="stream the files from this device's app folder")
    ap.add_argument("--dir", help="or: a work folder on this machine (disc0/, mods/, ...)")
    ap.add_argument("--port", type=int, default=9998)
    ap.add_argument("--parts", default="disc0,mods,movie_jp,voice_jp")
    a = ap.parse_args()
    if not a.source_device and not a.dir:
        sys.exit("give --source-device or --dir")
    parts = a.parts.split(",")
    adb = ["adb", "-s", a.device]
    subprocess.check_call(adb + ["reverse", "tcp:%d" % a.port, "tcp:%d" % a.port])
    subprocess.check_call(adb + ["shell", "setprop", "debug.psprecomp.import_port", str(a.port)])
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", a.port))
    srv.listen(1)
    print("READY: start the app as the target user", flush=True)
    conn, _ = srv.accept()
    sent = 0
    try:
        if a.source_device:
            p = subprocess.Popen(["adb", "-s", a.source_device, "exec-out", "tar c -C %s %s" % (SRC_DEVICE_DIR, " ".join(parts))], stdout=subprocess.PIPE)
            with tarfile.open(fileobj=p.stdout, mode="r|") as t:
                for m in t:
                    if not m.isfile():
                        continue
                    conn.sendall(("F %d %s\n" % (m.size, m.name)).encode())
                    f = t.extractfile(m)
                    while True:
                        b = f.read(1 << 20)
                        if not b:
                            break
                        conn.sendall(b)
                        sent += len(b)
        else:
            for part in parts:
                for root, _, files in os.walk(os.path.join(a.dir, part)):
                    for fn in files:
                        full = os.path.join(root, fn)
                        rel = os.path.relpath(full, a.dir)
                        conn.sendall(("F %d %s\n" % (os.path.getsize(full), rel)).encode())
                        with open(full, "rb") as f:
                            while True:
                                b = f.read(1 << 20)
                                if not b:
                                    break
                                conn.sendall(b)
                                sent += len(b)
        conn.sendall(b"E\n")
    finally:
        conn.close()
        subprocess.call(adb + ["shell", "setprop", "debug.psprecomp.import_port", "''"])
        subprocess.call(adb + ["reverse", "--remove", "tcp:%d" % a.port])
    print("sent %d MB" % (sent >> 20), flush=True)


if __name__ == "__main__":
    main()
