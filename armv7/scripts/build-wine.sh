#!/usr/bin/env bash
# Wine 10.10 mit den Winlator-Änderungen (brunodev85/wine-10.10-custom) als reines 32-Bit-Wine
# (i386-unix + i386-windows) und daraus die Container-Vorlage (Win32-Präfix).
# Läuft nativ in einem i386-Debian-Container (siehe ci-wine.sh); auf dem Gerät führt Box86
# die i386-Teile aus und bildet die Linux-Bibliotheken auf die armhf-Fassungen im Root-FS ab.
#
# Optionen wie beim originalen Winlator-Wine: ohne SDL/udev/USB (Eingabegeräte laufen über
# Winlator selbst), ohne Drucker, Scanner, Kameras, OpenCL und ffmpeg.
#
# Ergebnis: $STAGE_DIR/wine/opt/wine
#           $STAGE_DIR/pattern/.wine   (Container-Vorlage, kompakt)
#           $OUT_DIR/common_dlls.json
set -euo pipefail
. "$(dirname "$0")/env.sh"

SRC="$SRC_DIR/wine"
BUILD="$BUILD_DIR/wine"
PREFIX="$ROOT_PREFIX/opt/wine"

log "Quellen holen"
git_fetch "$WINE_REPO" "$WINE_COMMIT" "$SRC"

log "Konfigurieren"
rm -rf "$BUILD" && mkdir -p "$BUILD" && cd "$BUILD"
"$SRC/configure" --prefix="$PREFIX" \
    --disable-tests \
    --with-mingw \
    --without-wayland --without-oss --without-coreaudio --without-capi --without-cups \
    --without-gphoto --without-sane --without-v4l2 --without-netapi --without-pcsclite \
    --without-opencl --without-pcap --without-sdl --without-udev --without-usb --without-ffmpeg \
    CFLAGS="-O2 -g0" CROSSCFLAGS="-O2 -g0" > "$BUILD/configure.log" 2>&1 \
    || { tail -60 "$BUILD/configure.log"; exit 1; }
grep -E '^configure: ' "$BUILD/configure.log" | tail -40
grep -q '^#define SONAME_LIBX11 ' include/config.h || { echo "X11 fehlt"; exit 1; }
# Bibliotheken, die Wine zur Laufzeit lädt (für check-rootfs.py)
mkdir -p "$OUT_DIR"
grep -oE '^#define SONAME_[A-Z0-9_]+ "[^"]+"' include/config.h | cut -d'"' -f2 | sort -u > "$OUT_DIR/wine-sonames.txt"
tr '\n' ' ' < "$OUT_DIR/wine-sonames.txt"; echo

log "Bauen ($JOBS Jobs)"
make -j"$JOBS" > "$BUILD/make.log" 2>&1 || { tail -80 "$BUILD/make.log"; exit 1; }

log "Installieren nach $PREFIX"
rm -rf "$PREFIX" && make install-lib > /dev/null
rm -rf "$PREFIX/include" "$PREFIX/share/man" "$PREFIX/share/applications"
find "$PREFIX/lib/wine/i386-unix" -name '*.so' -exec strip --strip-unneeded {} +
strip --strip-unneeded "$PREFIX"/bin/wine* 2>/dev/null || true
"$PREFIX/bin/wine" --version
file "$PREFIX/bin/wine" "$PREFIX/lib/wine/i386-unix/ntdll.so" "$PREFIX/lib/wine/i386-windows/kernel32.dll"
[ ! -d "$PREFIX/lib/wine/x86_64-windows" ] || { echo "64-Bit-Teile im Wine-Baum, falscher Bau"; exit 1; }
mkdir -p "$STAGE_DIR/wine/opt" && rm -rf "$STAGE_DIR/wine/opt/wine" && cp -a "$PREFIX" "$STAGE_DIR/wine/opt/wine"

log "Container-Vorlage (Win32-Präfix) erzeugen"
HOMEDIR="$ROOT_PREFIX/home/xuser"
rm -rf "$HOMEDIR" && mkdir -p "$HOMEDIR"
# das Winlator-Wine legt seinen Server-Ordner unter rootfs/tmp an (wie auf dem Gerät)
mkdir -p "$ROOT_PREFIX/tmp"
export HOME="$HOMEDIR" USER=xuser LOGNAME=xuser WINEPREFIX="$HOMEDIR/.wine" WINEARCH=win32 WINEDEBUG=-all
export WINEDLLOVERRIDES="mscoree,mshtml=" PATH="$PREFIX/bin:$PATH"
xvfb-run -a sh -c 'wine wineboot -i && wineserver -w'
python3 -I "$ARMV7_DIR/scripts/make-container-pattern.py" "$WINEPREFIX" "$PREFIX/lib/wine/i386-windows" \
    "$ROOT_PREFIX" "$OUT_DIR/common_dlls.json"
rm -rf "$STAGE_DIR/pattern" && mkdir -p "$STAGE_DIR/pattern" && cp -a "$WINEPREFIX" "$STAGE_DIR/pattern/.wine"
du -sh "$STAGE_DIR/wine" "$STAGE_DIR/pattern"
