#!/usr/bin/env bash
# libxcb mit dem Socket-Pfad des Winlator-X-Servers. Auf Android gibt es kein /tmp; libxcb hat
# "/tmp/.X11-unix/X" fest eingebaut. Das originale aarch64-Root-FS hat deshalb ein libxcb mit
# "<rootfs>/tmp/.X11-unix/X"; hier dasselbe für armhf in der Version von Debian 12 (1.15).
# Ersetzt nur libxcb.so.1 (die Erweiterungsbibliotheken enthalten keinen Pfad).
#
# Ergebnis: $STAGE_DIR/libs/usr/lib/libxcb.so.1*
set -euo pipefail
. "$(dirname "$0")/env.sh"

VERSION=1.15
URL="https://xorg.freedesktop.org/archive/individual/lib/libxcb-$VERSION.tar.xz"
SRC="$SRC_DIR/libxcb-$VERSION"
DEST="$STAGE_DIR/libs/usr/lib"
SOCKET_DIR="$ROOT_PREFIX/tmp/.X11-unix"

[ -d "$DEST" ] || { echo "erst fetch-armhf-libs.sh ausführen"; exit 1; }

log "Quellen holen"
if [ ! -f "$SRC/configure" ]; then
    rm -rf "$SRC" && mkdir -p "$SRC_DIR"
    curl -fsSL "$URL" | tar -C "$SRC_DIR" -xJf -
fi

log "Socket-Pfad: $SOCKET_DIR/X<n>"
cd "$SRC"
grep -q '"/tmp/.X11-unix/X"' src/xcb_util.c || { echo "Socket-Pfad in xcb_util.c nicht gefunden"; exit 1; }
sed -i "s%\"/tmp/.X11-unix/X\"%\"$SOCKET_DIR/X\"%" src/xcb_util.c
grep -n "X11-unix" src/xcb_util.c

log "Bauen"
B="$BUILD_DIR/libxcb"; rm -rf "$B" && mkdir -p "$B" && cd "$B"
NO_T64=""
GCC_DEFINES="$($TARGET-gcc -dM -E - </dev/null)"
case "$GCC_DEFINES" in *"_TIME_BITS 64"*) NO_T64="-U_TIME_BITS -U_FILE_OFFSET_BITS";; esac
PKG_CONFIG_LIBDIR="/usr/lib/$TARGET/pkgconfig:/usr/share/pkgconfig" \
CFLAGS="-O2 -march=armv7-a -mfpu=neon -mfloat-abi=hard $NO_T64" \
"$SRC/configure" --host=$TARGET --prefix=/usr --disable-static --disable-devel-docs --without-doxygen > /dev/null
make -j"$JOBS" > /dev/null

REAL="$(readlink -f src/.libs/libxcb.so.1)"
rm -f "$DEST"/libxcb.so.1 "$DEST"/libxcb.so.1.*
install -m755 "$REAL" "$DEST/$(basename "$REAL")"
ln -sfn "$(basename "$REAL")" "$DEST/libxcb.so.1"
$TARGET-strip --strip-unneeded "$DEST/$(basename "$REAL")"
strings "$DEST/libxcb.so.1" | grep "X11-unix"
file "$DEST/libxcb.so.1"
