#!/usr/bin/env bash
# Winlators eigene Hilfsprogramme in 32-Bit-Fassung:
#   winhandler.exe  Nachbau (armv7/winhandler), Brücke zwischen App und Wine
#   wfm.exe         Winlator File Manager (brunodev85/wfm) für i686
#   libcdio.dll     libcdio + libiso9660 (brunodev85/libcdio) für i686, von wfm.exe benutzt
#   winpreexec      Nachbau (armv7/winpreexec), nativ armhf
#
# Ergebnis: $STAGE_DIR/winhelpers (Baum ab ROOT_PREFIX, wie rootfs_patches.tzst)
set -euo pipefail
. "$(dirname "$0")/env.sh"

MINGW=i686-w64-mingw32
DEST="$STAGE_DIR/winhelpers"
WINDIR="$DEST/home/xuser/.wine/drive_c/windows"
rm -rf "$DEST" && mkdir -p "$WINDIR" "$DEST/usr/local/bin"

log "winhandler.exe"
$MINGW-gcc -O2 -s -Wall -mwindows -static-libgcc -o "$WINDIR/winhandler.exe" \
    "$ARMV7_DIR/winhandler/winhandler.c" -lws2_32

log "win-iconv (für libcdio)"
ICONV="$SRC_DIR/win-iconv"
git_fetch https://github.com/win-iconv/win-iconv v0.0.8 "$ICONV"
ICONV_OUT="$BUILD_DIR/win-iconv"; rm -rf "$ICONV_OUT" && mkdir -p "$ICONV_OUT/include" "$ICONV_OUT/lib"
$MINGW-gcc -O2 -c "$ICONV/win_iconv.c" -o "$ICONV_OUT/win_iconv.o"
$MINGW-ar rcs "$ICONV_OUT/lib/libiconv.a" "$ICONV_OUT/win_iconv.o"
cp "$ICONV/iconv.h" "$ICONV_OUT/include/"

log "libcdio (statisch) -> libcdio.dll"
CDIO="$SRC_DIR/libcdio"
git_fetch "$LIBCDIO_REPO" "$LIBCDIO_COMMIT" "$CDIO"
cd "$CDIO" && git clean -qfdx && autoreconf -fi >/dev/null 2>&1
B="$BUILD_DIR/libcdio"; rm -rf "$B" && mkdir -p "$B" && cd "$B"
"$CDIO/configure" --host=$MINGW --prefix="$B/inst" --enable-static --disable-shared \
    --disable-cxx --disable-cpp-progs --disable-example-progs --disable-cddb --disable-vcd-info \
    --without-cd-drive --without-cd-info --without-cdda-player --without-cd-read \
    --without-iso-info --without-iso-read --without-versioned-libs \
    CPPFLAGS="-I$ICONV_OUT/include" LDFLAGS="-L$ICONV_OUT/lib" >/dev/null
make -j"$JOBS" -C lib >/dev/null
make -C lib install >/dev/null
make -C include install >/dev/null
$MINGW-gcc -shared -s -o "$WINDIR/libcdio.dll" -Wl,--export-all-symbols \
    -Wl,--whole-archive "$B/inst/lib/libcdio.a" "$B/inst/lib/libiso9660.a" -Wl,--no-whole-archive \
    "$ICONV_OUT/lib/libiconv.a" -lwinmm -static-libgcc -Wl,--out-implib,"$B/libcdio.dll.a"

log "wfm.exe"
WFM="$SRC_DIR/wfm"
git_fetch "$WFM_REPO" "$WFM_COMMIT" "$WFM"
git -C "$WFM" checkout -q --force . && git -C "$WFM" clean -qfdx
for p in "$ARMV7_DIR"/patches/wfm/*.patch; do
    [ -f "$p" ] || continue
    log "Patch $(basename "$p")"
    git -C "$WFM" apply "$p"
done
B="$BUILD_DIR/wfm"; rm -rf "$B" && mkdir -p "$B"
for c in "$WFM"/src/*.c; do
    $MINGW-gcc -O2 -std=c99 -DUNICODE -D_UNICODE -DCOBJMACROS -DWINVER=0x0600 -Wall \
        -I"$WFM/include" -I"$WFM/include/libcdio" -c "$c" -o "$B/$(basename "${c%.c}").o"
done
$MINGW-windres -I"$WFM/include" -I"$WFM/include/libcdio" -I"$WFM/res" -i "$WFM/res/resource.rc" -o "$B/resource.o"
$MINGW-gcc -o "$WINDIR/wfm.exe" "$B"/*.o -s -lcomctl32 -lgdi32 -lole32 -luuid \
    "$B/../libcdio/libcdio.dll.a" -Wl,--subsystem,windows -static-libgcc

log "winpreexec (armhf)"
$TARGET-gcc -O2 -s -Wall -o "$DEST/usr/local/bin/winpreexec" "$ARMV7_DIR/winpreexec/winpreexec.c" \
    -Wl,--dynamic-linker="$ROOT_PREFIX/lib/ld-linux-armhf.so.3"

file "$WINDIR"/* "$DEST/usr/local/bin/winpreexec"
