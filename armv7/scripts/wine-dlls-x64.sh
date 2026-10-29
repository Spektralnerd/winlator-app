#!/usr/bin/env bash
# Einzelne DLLs des originalen Winlator-Wine (brunodev85/wine-10.10-custom, WINE_COMMIT) mit
# Korrekturen neu bauen, für das 64-Bit-Wine der Varianten arm64 und armv7hybrid (WoW64: jede DLL
# als x86_64- und als i386-PE):
#   dbghelp.dll  patches-x64/wine/0001: MiniDumpWriteDump(GetCurrentProcess(), ...) stürzt nicht
#                mehr ab (Absturzbehandlung von Unreal Engine 3 & Co.)
#   shell32.dll  patches/wine/0003: Ziel „C:“ (Laufwerkswurzel) in SHFileOperation
#
#   wine-dlls-x64.sh <ausgabeordner>
#
# Ergebnis: <ausgabeordner>/opt/wine/lib/wine/{x86_64,i386}-windows/<dll>.dll (Baum ab Root-FS,
# wie in rootfs_patches.tzst). Benötigt gcc, flex, bison, gettext (Übersetzungen in den Ressourcen)
# und MinGW für i686 und x86_64.
# (Absichtlich nicht build-*.sh: gehört nicht zum 32-Bit-Unterbau und soll dessen Schlüssel im CI
# nicht verändern.)
set -euo pipefail
. "$(dirname "$0")/env.sh"

[ $# -eq 1 ] || { echo "Aufruf: $0 <ausgabeordner>"; exit 2; }
OUT="$(mkdir -p "$1" && realpath "$1")"
SRC="$SRC_DIR/wine-x64"
BUILD="$BUILD_DIR/wine-x64"
DLLS="dbghelp shell32"
ARCHS="x86_64 i386"

log "Quellen holen ($WINE_REPO $WINE_COMMIT)"
git_fetch "$WINE_REPO" "$WINE_COMMIT" "$SRC"
git -C "$SRC" checkout -q --force . && git -C "$SRC" clean -qfdx
for p in "$ARMV7_DIR"/patches-x64/wine/*.patch "$ARMV7_DIR"/patches/wine/0003-shell32-drive-root-target.patch; do
    [ -f "$p" ] || continue
    log "Patch $(basename "$p")"
    git -C "$SRC" apply "$p"
done

log "Konfigurieren (gebaut werden nur die PE-DLLs)"
rm -rf "$BUILD" && mkdir -p "$BUILD" && cd "$BUILD"
"$SRC/configure" --enable-archs=i386,x86_64 --disable-tests \
    --without-x --without-freetype --without-gnutls --without-gstreamer --without-wayland \
    --without-alsa --without-pulse --without-oss --without-coreaudio --without-capi --without-cups \
    --without-dbus --without-fontconfig --without-gphoto --without-sane --without-v4l2 \
    --without-netapi --without-pcsclite --without-opencl --without-pcap --without-sdl --without-udev \
    --without-usb --without-ffmpeg --without-vulkan --without-opengl --without-krb5 \
    --without-gssapi --without-inotify --without-unwind \
    CFLAGS="-O2 -g0" CROSSCFLAGS="-O2 -g0" > configure.log 2>&1 \
    || { tail -60 configure.log; exit 1; }
grep -E '^configure: ' configure.log | tail -20 || true

TARGETS=""
for d in $DLLS; do for a in $ARCHS; do TARGETS="$TARGETS dlls/$d/$a-windows/$d.dll"; done; done
log "Bauen ($JOBS Jobs):$TARGETS"
make -j"$JOBS" $TARGETS > make.log 2>&1 || { tail -80 make.log; exit 1; }

rm -rf "$OUT/opt"
for d in $DLLS; do
    for a in $ARCHS; do
        f="dlls/$d/$a-windows/$d.dll"
        case $a in x86_64) fmt=pei-x86-64; strip=x86_64-w64-mingw32-strip ;; i386) fmt=pei-i386; strip=i686-w64-mingw32-strip ;; esac
        x86_64-w64-mingw32-objdump -f "$f" | grep -q "file format $fmt" || { echo "$f: falsches Format"; exit 1; }
        # eingebaute Wine-DLL (Kennung im DOS-Kopf), sonst lädt Wine sie nicht als builtin
        grep -qa "Wine builtin DLL" "$f" || { echo "$f: keine eingebaute Wine-DLL"; exit 1; }
        install -D -m 644 "$f" "$OUT/opt/wine/lib/wine/$a-windows/$d.dll"
        $strip --strip-unneeded "$OUT/opt/wine/lib/wine/$a-windows/$d.dll"
    done
done
find "$OUT/opt" -type f -exec ls -la {} +
