#!/usr/bin/env python3
"""Prüft ein zusammengestelltes armhf-Root-FS auf fehlende Bibliotheken, ohne es auszuführen.

  * armhf-Programme/Bibliotheken: jede NEEDED-Bibliothek muss im Root-FS liegen
  * i386-Teile von Wine (Box86): jede NEEDED-Bibliothek und jede Bibliothek, die Wine zur
    Laufzeit lädt (SONAME_* aus config.h), muss entweder von Box86 auf eine armhf-Bibliothek
    abgebildet werden ("wrapped", die armhf-Fassung muss dann im Root-FS liegen) oder als
    i386-Bibliothek in usr/lib/i386-linux-gnu liegen
  * keine Dateien für fremde Architekturen (x86-64, aarch64)

Aufruf: check-rootfs.py <rootfs> <box86-wrapped.txt> <wine-sonames.txt> [<treiber-baum> ...]
Treiber-Bäume (z. B. die einzelnen graphics_driver-Pakete) werden gegen das Root-FS geprüft.
Rückgabe 1 bei fehlenden Pflicht-Bibliotheken.
"""
import os
import re
import subprocess
import sys

EM_386, EM_ARM, EM_X86_64, EM_AARCH64 = 3, 40, 62, 183
# Wine-eigene Unix-Bibliotheken (liegen nebeneinander in i386-unix)
WINE_INTERNAL = re.compile(r"^(ntdll|win32u|[a-z0-9_]+)\.so$")
# Bibliotheken, die Wine nur für Zusatzfunktionen lädt; fehlen sie, fällt nur diese Funktion weg
OPTIONAL = {"libOpenCL.so.1", "libcups.so.2", "libcapi20.so.3", "libnetapi.so", "libsane.so.1",
            "libgphoto2.so.6", "libgphoto2_port.so.12", "libpcsclite.so.1", "libodbc.so.2",
            "libv4l2.so.0", "libSDL2-2.0.so.0", "libudev.so.1", "libusb-1.0.so.0", "libpcap.so.0.8",
            "libxkbcommon.so.0", "libwayland-client.so.0", "libunwind.so.8", "libosmesa.so.8",
            "libXss.so.1", "libXxf86vm.so.1",
            # EGL: Wine nimmt dann GLX (Winlators X-Server, Mesa über gallium-xlib); ODBC: nur Datenbanken
            "libEGL.so.1", "libodbc.so", "libnetapi.so"}


def elf_machine(path):
    try:
        with open(path, "rb") as f:
            head = f.read(20)
    except OSError:
        return None
    if len(head) < 20 or head[:4] != b"\x7fELF":
        return None
    return int.from_bytes(head[18:20], "little")


def needed(path):
    """NEEDED-Einträge; bei RUNPATH $ORIGIN zählen Dateien im selben Ordner als vorhanden"""
    out = subprocess.run(["readelf", "-d", path], capture_output=True, text=True).stdout
    libs = re.findall(r"\(NEEDED\)\s+Shared library: \[([^\]]+)\]", out)
    if re.search(r"\((RUNPATH|RPATH)\)\s+Library r(un)?path: \[[^\]]*\$ORIGIN", out):
        here = set(os.listdir(os.path.dirname(path)))
        libs = [l for l in libs if l not in here]
    return libs


def scan(tree):
    """ELF-Dateien eines Baums nach Architektur"""
    found = {}
    for top, dirs, files in os.walk(tree):
        for name in files:
            path = os.path.join(top, name)
            if os.path.islink(path) and not os.path.exists(path):
                continue
            m = elf_machine(path)
            if m is not None:
                found.setdefault(m, []).append(path)
    return found


def main():
    root, wrapped_file, sonames_file = sys.argv[1:4]
    extra_trees = sys.argv[4:]
    wrapped = set(open(wrapped_file).read().split())
    wine_sonames = set(open(sonames_file).read().split()) if os.path.exists(sonames_file) else set()

    armhf_libs, i386_libs = set(), set()
    found = scan(root)
    armhf_elfs, i386_elfs = found.pop(EM_ARM, []), found.pop(EM_386, [])
    foreign = [(os.path.relpath(p, root), m) for m, paths in found.items() for p in paths]
    for top, dirs, files in os.walk(os.path.join(root, "usr/lib")):
        rel = os.path.relpath(top, os.path.join(root, "usr/lib"))
        for name in files:
            if rel == ".":
                armhf_libs.add(name)
            elif rel.startswith("i386-linux-gnu"):
                i386_libs.add(name)
    # Bibliotheken in Unterordnern (pulseaudio usw.) findet ld.so nur über RUNPATH; mitzählen,
    # aber gesondert melden, wenn sie dort gesucht werden müssten
    armhf_libs |= {"libGL.so.1", "libGL.so.1.7.0"}  # kommen mit dem Grafiktreiber

    problems, warnings = [], []
    for rel, m in foreign:
        problems.append(f"fremde Architektur (e_machine {m}): {rel}")

    for path in armhf_elfs:
        rel = os.path.relpath(path, root)
        for lib in needed(path):
            if lib not in armhf_libs:
                (warnings if lib in OPTIONAL else problems).append(f"armhf {rel}: {lib} fehlt")

    for tree in extra_trees:
        found = scan(tree)
        own = {n for _, _, files in os.walk(os.path.join(tree, "usr/lib")) for n in files}
        for m, paths in found.items():
            for path in paths:
                rel = os.path.join(os.path.basename(tree.rstrip("/")), os.path.relpath(path, tree))
                if m != EM_ARM:
                    problems.append(f"fremde Architektur (e_machine {m}): {rel}")
                    continue
                for lib in needed(path):
                    if lib not in armhf_libs and lib not in own:
                        problems.append(f"armhf {rel}: {lib} fehlt im Root-FS")

    wine_dir = os.path.join(root, "opt/wine/lib/wine/i386-unix")
    for path in i386_elfs:
        rel = os.path.relpath(path, root)
        is_wine = path.startswith(wine_dir) or rel.startswith("opt/wine/bin/")
        # Box86 bringt einige i386-Bibliotheken für andere Programme mit; deren Lücken sind nur Hinweise
        report = problems if is_wine else warnings
        for lib in needed(path):
            if is_wine and WINE_INTERNAL.match(lib) and os.path.exists(os.path.join(wine_dir, lib)):
                continue
            if lib in i386_libs:
                continue
            if lib in wrapped:
                if lib.startswith(("libc.so", "libm.so", "libdl.so", "libpthread.so", "librt.so",
                                   "ld-linux.so", "libresolv.so", "libutil.so")):
                    continue
                if lib not in armhf_libs:
                    (warnings if lib in OPTIONAL else report).append(
                        f"i386 {rel}: {lib} von Box86 abgebildet, armhf-Fassung fehlt")
                continue
            (warnings if lib in OPTIONAL else report).append(f"i386 {rel}: {lib} weder abgebildet noch vorhanden")

    for lib in sorted(wine_sonames):
        if lib in i386_libs:
            continue
        if lib in wrapped and lib in armhf_libs:
            continue
        what = "armhf-Fassung fehlt" if lib in wrapped else "nicht von Box86 abgebildet"
        (warnings if lib in OPTIONAL else problems).append(f"Wine lädt {lib}: {what}")

    print(f"armhf-ELF: {len(armhf_elfs)}, i386-ELF: {len(i386_elfs)}, "
          f"armhf-Bibliotheken: {len(armhf_libs)}, i386-Bibliotheken: {len(i386_libs)}")
    for w in sorted(set(warnings)):
        print("Hinweis:", w)
    for p in sorted(set(problems)):
        print("FEHLER:", p)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
