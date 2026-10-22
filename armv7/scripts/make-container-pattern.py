#!/usr/bin/env python3
"""Macht aus einem frisch mit wineboot erzeugten Win32-Präfix die Container-Vorlage von Winlator.

Wie beim Original (RootFSInstaller.java / container_pattern.tzst):
  * dosdevices: nur c: -> ../drive_c und z: -> Root-FS
  * .update-timestamp = "disable" (Wine baut das Präfix beim Start nicht neu)
  * Dateien direkt in system32, die Byte für Byte den DLLs aus Wines lib-Ordner gleichen,
    werden entfernt und in common_dlls.json aufgeführt; die App kopiert sie beim Anlegen
    eines Containers aus opt/wine/lib/wine/i386-windows zurück.

Aufruf: make-container-pattern.py <WINEPREFIX> <wine-lib-i386-windows> <ROOT_PREFIX> <common_dlls.json>
"""
import filecmp
import json
import os
import shutil
import sys


def main():
    prefix, libdir, root_prefix, out_json = sys.argv[1:5]

    dosdevices = os.path.join(prefix, "dosdevices")
    shutil.rmtree(dosdevices, ignore_errors=True)
    os.makedirs(dosdevices)
    os.symlink("../drive_c", os.path.join(dosdevices, "c:"))
    os.symlink(root_prefix, os.path.join(dosdevices, "z:"))

    with open(os.path.join(prefix, ".update-timestamp"), "w") as f:
        f.write("disable\n")

    drive_c = os.path.join(prefix, "drive_c")
    if not os.path.isdir(os.path.join(drive_c, "users", "xuser")):
        raise SystemExit("drive_c/users/xuser fehlt (USER=xuser beim wineboot?)")
    # Verknüpfungen in den Bauhost (z. B. Benutzerordner -> $HOME/...) durch leere Ordner ersetzen
    for top, dirs, files in os.walk(drive_c):
        for name in dirs + files:
            path = os.path.join(top, name)
            if os.path.islink(path):
                print("ersetze Verknüpfung", os.path.relpath(path, prefix), "->", os.readlink(path))
                os.remove(path)
                os.makedirs(path)

    windows = os.path.join(drive_c, "windows")
    system32 = os.path.join(windows, "system32")
    if os.path.isdir(os.path.join(windows, "syswow64")):
        raise SystemExit("syswow64 vorhanden: das ist kein Win32-Präfix")

    removed = []
    for name in sorted(os.listdir(libdir)):
        src = os.path.join(libdir, name)
        dst = os.path.join(system32, name)
        if os.path.isfile(src) and os.path.isfile(dst) and not os.path.islink(dst) \
                and filecmp.cmp(src, dst, shallow=False):
            os.remove(dst)
            removed.append(name)

    if len(removed) < 300:
        raise SystemExit(f"nur {len(removed)} gemeinsame DLLs gefunden, Präfix unvollständig?")

    with open(out_json, "w") as f:
        json.dump({"system32": removed}, f, separators=(",", ":"))

    kept = sorted(os.listdir(system32))
    print(f"system32: {len(removed)} DLLs ausgelagert, {len(kept)} Einträge bleiben")


if __name__ == "__main__":
    main()
