#!/usr/bin/env bash
# wfm.exe (Winlator File Manager, brunodev85/wfm) als x86-64-Programm mit den Korrekturen aus
# patches/wfm, für die 64-Bit-Root-FS (arm64, armv7hybrid): ersetzt die wfm.exe des Originals in
# rootfs_patches.tzst. Gelinkt wird direkt gegen die libcdio.dll aus demselben Archiv, die
# unverändert bleibt.
#
#   wfm-x64.sh <rootfs_patches.tzst>
#
# Benötigt: git, x86_64-w64-mingw32-gcc/-windres/-objdump (gcc-mingw-w64-x86-64), zstd, tar.
# (Absichtlich nicht build-*.sh: das Skript gehört nicht zum 32-Bit-Unterbau und soll dessen
# Schlüssel im CI nicht verändern.)
set -euo pipefail
. "$(dirname "$0")/env.sh"

[ $# -eq 1 ] && [ -f "$1" ] || { echo "Aufruf: $0 <rootfs_patches.tzst>"; exit 2; }
ARCHIVE="$(realpath "$1")"
MINGW=x86_64-w64-mingw32
LEVEL="${ZSTD_LEVEL:-19}"
TMP="$BUILD_DIR/wfm-x64"
rm -rf "$TMP" && mkdir -p "$TMP/patches" "$TMP/obj"

log "rootfs_patches entpacken"
zstd -dc "$ARCHIVE" | tar -C "$TMP/patches" -xf -
WINDIR="$TMP/patches/home/xuser/.wine/drive_c/windows"
for f in wfm.exe libcdio.dll; do
    [ -f "$WINDIR/$f" ] || { echo "$f fehlt in $ARCHIVE"; exit 1; }
done
$MINGW-objdump -f "$WINDIR/libcdio.dll" | grep -q 'file format pei-x86-64' \
    || { echo "libcdio.dll im Archiv ist keine x86-64-DLL"; exit 1; }

log "wfm $WFM_COMMIT holen"
WFM="$SRC_DIR/wfm-x64"
git_fetch "$WFM_REPO" "$WFM_COMMIT" "$WFM"
git -C "$WFM" checkout -q --force . && git -C "$WFM" clean -qfdx
for p in "$ARMV7_DIR"/patches/wfm/*.patch; do
    [ -f "$p" ] || continue
    log "Patch $(basename "$p")"
    git -C "$WFM" apply "$p"
done

log "wfm.exe (x86-64) bauen"
for c in "$WFM"/src/*.c; do
    $MINGW-gcc -O2 -std=c99 -DUNICODE -D_UNICODE -DCOBJMACROS -DWINVER=0x0600 -Wall \
        -I"$WFM/include" -I"$WFM/include/libcdio" -c "$c" -o "$TMP/obj/$(basename "${c%.c}").o"
done
$MINGW-windres -I"$WFM/include" -I"$WFM/include/libcdio" -I"$WFM/res" -i "$WFM/res/resource.rc" -o "$TMP/obj/resource.o"
# ld linkt PE-DLLs auch ohne Importbibliothek
$MINGW-gcc -o "$TMP/wfm.exe" "$TMP"/obj/*.o -s -lcomctl32 -lgdi32 -lole32 -luuid \
    "$WINDIR/libcdio.dll" -Wl,--subsystem,windows -static-libgcc

$MINGW-objdump -f "$TMP/wfm.exe" | grep -q 'file format pei-x86-64' || { echo "wfm.exe ist kein x86-64-Programm"; exit 1; }
# nur DLLs, die Wine mitbringt (und libcdio.dll aus dem Archiv) – keine MinGW-Laufzeit-DLLs
IMPORTS="$($MINGW-objdump -p "$TMP/wfm.exe" | sed -n 's/^[[:space:]]*DLL Name: //p' | sort -u)"
echo "Importe: $(echo $IMPORTS)"
for d in $IMPORTS; do
    case "${d,,}" in
        advapi32.dll|comctl32.dll|gdi32.dll|kernel32.dll|msvcrt.dll|ole32.dll|shell32.dll|user32.dll|libcdio.dll) ;;
        *) echo "unerwartete DLL: $d"; exit 1 ;;
    esac
done

log "rootfs_patches neu packen (zstd -$LEVEL)"
OLD_SIZE=$(stat -c %s "$WINDIR/wfm.exe")
install -m 644 "$TMP/wfm.exe" "$WINDIR/wfm.exe"
tar -C "$TMP/patches" --owner=0 --group=0 --numeric-owner --sort=name -cf - . |
    zstd -"$LEVEL" -T0 -q -f -o "$ARCHIVE.neu"
mv -f "$ARCHIVE.neu" "$ARCHIVE"
zstd -dc "$ARCHIVE" | tar -xOf - ./home/xuser/.wine/drive_c/windows/wfm.exe | cmp - "$TMP/wfm.exe"
echo "wfm.exe: $OLD_SIZE -> $(stat -c %s "$TMP/wfm.exe") Byte, sha256 $(sha256sum "$TMP/wfm.exe" | cut -c1-16)"
echo "$(basename "$ARCHIVE"): $(stat -c %s "$ARCHIVE") Byte, $(zstd -dc "$ARCHIVE" | tar -tf - | wc -l) Einträge"
