#!/usr/bin/env bash
# rootfs_patches.tzst der 64-Bit-Root-FS (arm64, armv7hybrid) mit den Korrekturen:
#   - wfm.exe (Winlator File Manager, brunodev85/wfm) als x86-64-Programm mit patches/wfm, statt
#     der wfm.exe des Originals; gelinkt direkt gegen die libcdio.dll aus demselben Archiv, die
#     unverändert bleibt
#   - optional ein Dateibaum ab Root-FS, der dazukommt bzw. Gleichnamiges ersetzt (die neu gebauten
#     Wine-DLLs aus wine-dlls-x64.sh: opt/wine/lib/wine/...)
#
#   rootfs-patches-x64.sh <rootfs_patches.tzst> [baum]
#
# Benötigt: git, x86_64-w64-mingw32-gcc/-windres/-objdump (gcc-mingw-w64-x86-64), zstd, tar.
# (Absichtlich weder build-*.sh noch patch-*.sh: das Skript gehört nicht zum 32-Bit-Unterbau und
# soll dessen Schlüssel im CI nicht verändern.)
set -euo pipefail
. "$(dirname "$0")/env.sh"

[ $# -ge 1 ] && [ $# -le 2 ] && [ -f "$1" ] || { echo "Aufruf: $0 <rootfs_patches.tzst> [baum]"; exit 2; }
ARCHIVE="$(realpath "$1")"
TREE=""
if [ $# -eq 2 ]; then
    [ -d "$2" ] || { echo "kein Ordner: $2"; exit 2; }
    TREE="$(realpath "$2")"
fi
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
    "$WINDIR/libcdio.dll" -Wl,--subsystem,windows -Wl,--no-insert-timestamp -static-libgcc

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

OLD_SIZE=$(stat -c %s "$WINDIR/wfm.exe")
install -m 644 "$TMP/wfm.exe" "$WINDIR/wfm.exe"
echo "wfm.exe: $OLD_SIZE -> $(stat -c %s "$TMP/wfm.exe") Byte, sha256 $(sha256sum "$TMP/wfm.exe" | cut -c1-16)"
if [ -n "$TREE" ]; then
    log "Dateien aus $TREE"
    (cd "$TREE" && find . -type f | sort) > "$TMP/tree.lst"
    while read -r f; do echo "  $f ($(stat -c %s "$TREE/$f") Byte)"; done < "$TMP/tree.lst"
    cp -a "$TREE/." "$TMP/patches/"
fi

# Reproduzierbar packen (feste Zeit für alles Neue, eine Kompressionsrunde): gleicher Inhalt gibt
# dieselbe Datei und damit dieselbe rootfs_patches.id, sonst entpackten die Container nach jedem
# Bau alles neu
log "rootfs_patches neu packen (zstd -$LEVEL)"
tar -C "$TMP/patches" --owner=0 --group=0 --numeric-owner --sort=name \
    --mtime=@"${SOURCE_DATE_EPOCH:-1767225600}" --clamp-mtime -cf - . |
    zstd -"$LEVEL" -T1 -q -f -o "$ARCHIVE.neu"
mv -f "$ARCHIVE.neu" "$ARCHIVE"
mkdir -p "$TMP/check" && zstd -dc "$ARCHIVE" | tar -C "$TMP/check" -xf -
cmp "$TMP/check/home/xuser/.wine/drive_c/windows/wfm.exe" "$TMP/wfm.exe"
if [ -n "$TREE" ]; then
    while read -r f; do cmp "$TMP/check/$f" "$TREE/$f"; done < "$TMP/tree.lst"
fi
echo "$(basename "$ARCHIVE"): $(stat -c %s "$ARCHIVE") Byte, $(zstd -dc "$ARCHIVE" | tar -tf - | wc -l) Einträge"
