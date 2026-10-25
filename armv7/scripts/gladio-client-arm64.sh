#!/usr/bin/env bash
# Gladio-Client für die 64-Bit-Variante (arm64-v8a: Box64 + WoW64-Wine, originale Root-FS) mit
# denselben Anpassungen wie der armhf-Client (patch-gladio-client.py): Lichtmodell, Swizzle,
# glGetDoublev, Vertexbereich statt je Index, verzögerte Einheitenwechsel, Matrixabfragen im
# Client, ein Bild Vorlauf. Gebaut mit aarch64-linux-gnu-gcc von Ubuntu 24.04 (glibc 2.39, wie die
# Root-FS des Originals).
#
#   gladio-client-arm64.sh ZIEL.tzst      Paket wie graphics_driver/gladio-*.tzst des Originals
#                                        (./usr/lib/libGL.so.1.7.0); daneben ZIEL ohne .tzst + .id
#                                        mit der Prüfsumme (XServerDisplayActivity entpackt den
#                                        Client neu, wenn sie sich ändert)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$(realpath -m "$1")
CC=${CC:-aarch64-linux-gnu-gcc}
STRIP=${STRIP:-aarch64-linux-gnu-strip}
WORK=${WORK:-$(mktemp -d)}
GLADIO_REPO=https://github.com/brunodev85/gladio
GLADIO_COMMIT=$(sed -n 's/.*GLADIO_COMMIT:-\([0-9a-f]*\).*/\1/p' "$HERE/env.sh")
[ -n "$GLADIO_COMMIT" ] || { echo "GLADIO_COMMIT nicht gefunden"; exit 1; }

SRC="$WORK/gladio"
rm -rf "$SRC" && mkdir -p "$SRC"
git -C "$SRC" init -q
git -C "$SRC" remote add origin "$GLADIO_REPO"
git -C "$SRC" fetch -q --depth=1 origin "$GLADIO_COMMIT"
git -C "$SRC" checkout -q FETCH_HEAD
python3 -I "$HERE/patch-gladio-client.py" "$SRC"

cat > "$WORK/aarch64.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER $CC)
EOF
cmake -S "$SRC" -B "$WORK/build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$WORK/aarch64.cmake" \
    -DCMAKE_BUILD_TYPE=Release >/dev/null
ninja -C "$WORK/build"

PKG="$WORK/pkg"
rm -rf "$PKG" && mkdir -p "$PKG/usr/lib"
install -m755 "$WORK/build/libGL.so" "$PKG/usr/lib/libGL.so.1.7.0"
$STRIP --strip-unneeded "$PKG/usr/lib/libGL.so.1.7.0"
file "$PKG/usr/lib/libGL.so.1.7.0"
# höchstens glibc 2.39 (Root-FS des Originals)
NEWEST=$(readelf -V "$PKG/usr/lib/libGL.so.1.7.0" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)
echo "neueste glibc-Version: $NEWEST"
case "$NEWEST" in GLIBC_2.[4-9][0-9]*) echo "::error::braucht $NEWEST, die Root-FS hat 2.39"; exit 1;; esac
readelf -d "$PKG/usr/lib/libGL.so.1.7.0" | grep NEEDED

mkdir -p "$(dirname "$OUT")"
tar --owner=0 --group=0 --numeric-owner -C "$PKG" -cf - . | zstd -q -19 -f -o "$OUT"
sha256sum "$OUT" | cut -c1-16 > "${OUT%.tzst}.id"
echo "Paket: $OUT ($(du -h "$OUT" | cut -f1)), Kennung $(cat "${OUT%.tzst}.id")"
