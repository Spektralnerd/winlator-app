#!/usr/bin/env bash
# alsa-lib (wie im aarch64-Root-FS: 1.2.13, Präfix rootfs) und das ALSA-Plugin von Winlator
# (android_aserver, spricht mit dem ALSA-Server der App) für armhf.
#
# Ergebnis: $STAGE_DIR/alsa
set -euo pipefail
. "$(dirname "$0")/env.sh"

ALSA_REPO=https://github.com/alsa-project/alsa-lib
ALSA_TAG=v1.2.13
SRC="$SRC_DIR/alsa-lib"
WINLATOR="$SRC_DIR/winlator"
BUILD="$BUILD_DIR/alsa-lib"
DEST="$STAGE_DIR/alsa"

log "Quellen holen"
git_fetch "$ALSA_REPO" "$ALSA_TAG" "$SRC"
git_fetch "$WINLATOR_REPO" "$WINLATOR_COMMIT" "$WINLATOR"

log "alsa-lib bauen"
cd "$SRC" && git clean -qfdx && autoreconf -fi >/dev/null 2>&1
rm -rf "$BUILD" && mkdir -p "$BUILD" && cd "$BUILD"
NO_T64=""
GCC_DEFINES="$($TARGET-gcc -dM -E - </dev/null)"
case "$GCC_DEFINES" in *"_TIME_BITS 64"*) NO_T64="-U_TIME_BITS -U_FILE_OFFSET_BITS";; esac
CFLAGS="-O2 -march=armv7-a -mfpu=neon -mfloat-abi=hard $NO_T64" \
"$SRC/configure" --host=$TARGET \
    --prefix="$ROOT_PREFIX/usr" --libdir="$ROOT_PREFIX/lib" \
    --sysconfdir="$ROOT_PREFIX/etc" --localstatedir="$ROOT_PREFIX/var" \
    --with-plugindir="$ROOT_PREFIX/lib/alsa-lib" \
    --disable-python --disable-static >/dev/null
make -j"$JOBS" >/dev/null
rm -rf "$DEST" && make install DESTDIR="$BUILD/inst" >/dev/null
mkdir -p "$DEST/usr/lib/alsa-lib" "$DEST/usr/share" "$DEST/etc/alsa/conf.d" "$BUILD/sdk"
cp -a "$BUILD/inst$ROOT_PREFIX/lib/"lib*.so* "$DEST/usr/lib/"
cp -a "$BUILD/inst$ROOT_PREFIX/usr/share/alsa" "$DEST/usr/share/"
cp -a "$BUILD/inst$ROOT_PREFIX/usr/include" "$BUILD/sdk/"

log "ALSA-Plugin android_aserver"
$TARGET-gcc -O2 -Wall -fPIC -DPIC -shared $NO_T64 -march=armv7-a -mfpu=neon -mfloat-abi=hard \
    -I"$BUILD/sdk/include" -I"$WINLATOR/android_alsa/include" \
    -o "$DEST/usr/lib/alsa-lib/libasound_module_pcm_android_aserver.so" \
    "$WINLATOR/android_alsa/module_pcm_android_aserver.c" \
    -L"$DEST/usr/lib" -lasound
install -m644 "$WINLATOR/android_alsa/android_aserver.conf" "$DEST/etc/alsa/conf.d/android_aserver.conf"
# wie im aarch64-Root-FS: alsa.conf lädt nur die Konfiguration des Winlator-Servers
install -m644 "$WINLATOR/android_alsa/alsa.conf" "$DEST/usr/share/alsa/alsa.conf"
$TARGET-strip --strip-unneeded "$DEST"/usr/lib/*.so.* "$DEST"/usr/lib/alsa-lib/*.so 2>/dev/null || true

file "$DEST/usr/lib/libasound.so.2" "$DEST/usr/lib/alsa-lib/libasound_module_pcm_android_aserver.so"
