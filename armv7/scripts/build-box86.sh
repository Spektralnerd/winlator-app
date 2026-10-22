#!/usr/bin/env bash
# Box86 (ptitSeb) für ARMv7 mit Dynarec, gebaut gegen eine gewöhnliche armhf-glibc und
# mit dem Interpreter-Pfad der Winlator-glibc (rootfs/lib/ld-linux-armhf.so.3).
#
# Ergebnis: $STAGE_DIR/box86 (usr/local/bin/box86, lib/i386-linux-gnu/*)
#           $OUT_DIR/box86-$BOX86_VERSION.tzst  (Winlator-Komponentenformat)
set -euo pipefail
. "$(dirname "$0")/env.sh"

SRC="$SRC_DIR/box86"
BUILD="$BUILD_DIR/box86"
DEST="$STAGE_DIR/box86"

log "Quellen holen"
git_fetch "$BOX86_REPO" "$BOX86_COMMIT" "$SRC"
cd "$SRC"
git checkout -q --force . && git clean -qfdx
for p in "$ARMV7_DIR"/patches/box86/*.patch; do
    [ -f "$p" ] || continue
    log "Patch $(basename "$p")"
    patch -s -p1 --no-backup-if-mismatch < "$p"
done

# Debian/Ubuntu-armhf-Compiler mit 64-Bit-time_t als Vorgabe (Ubuntu 24.04) zurück auf das
# klassische 32-Bit-ABI stellen: Box86 reicht Strukturen des i386-Gasts 1:1 an die armhf-libc
# weiter, und i386 hat 32-Bit-time_t.
NO_T64=""
# (Ausgabe erst einsammeln: grep -q würde gcc per SIGPIPE abbrechen und mit pipefail scheitern)
GCC_DEFINES="$($TARGET-gcc -dM -E - </dev/null)"
case "$GCC_DEFINES" in *"_TIME_BITS 64"*) NO_T64="-U_TIME_BITS -U_FILE_OFFSET_BITS";; esac
ARCH_FLAGS="-marm -march=armv7-a -mfpu=neon -mfloat-abi=hard"

log "Konfigurieren"
rm -rf "$BUILD" && mkdir -p "$BUILD"
cat > "$BUILD/toolchain.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR armhf)
set(CMAKE_C_COMPILER $TARGET-gcc)
set(CMAKE_ASM_COMPILER $TARGET-gcc)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
EOF
cmake -S "$SRC" -B "$BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$BUILD/toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DARM_DYNAREC=ON \
    -DBAD_SIGNAL=ON \
    -DNO_CONF_INSTALL=ON \
    -DCMAKE_C_FLAGS="$ARCH_FLAGS $NO_T64" \
    -DCMAKE_ASM_FLAGS="$ARCH_FLAGS" \
    -DCMAKE_EXE_LINKER_FLAGS="-Wl,--dynamic-linker=$ROOT_PREFIX/lib/ld-linux-armhf.so.3" \
    -DCMAKE_INSTALL_PREFIX=/usr/local

log "Bauen ($JOBS Jobs)"
ninja -C "$BUILD" -j"$JOBS"

log "Installieren"
rm -rf "$DEST" && mkdir -p "$DEST/usr/local/bin" "$DEST/usr/lib/i386-linux-gnu"
install -m755 "$BUILD/box86" "$DEST/usr/local/bin/box86"
$TARGET-strip --strip-debug "$DEST/usr/local/bin/box86"
# x86-Bibliotheken, die Box86 selbst mitbringt (libgcc_s, libstdc++ usw. für i386)
if [ -d "$SRC/x86lib" ]; then
    cp -a "$SRC"/x86lib/* "$DEST/usr/lib/i386-linux-gnu/"
fi

file "$DEST/usr/local/bin/box86"
readelf -l "$DEST/usr/local/bin/box86" | grep -o "interpreter: [^]]*"

# Liste der Bibliotheken, die Box86 auf armhf-Bibliotheken abbildet (für check-rootfs.py)
grep -o 'GO("[^"]*"' "$SRC/src/library_list.h" | cut -d'"' -f2 | sort -u > "$OUT_DIR/box86-wrapped.txt"
echo "$(wc -l < "$OUT_DIR/box86-wrapped.txt") abgebildete Bibliotheken"

log "Winlator-Komponente packen"
mkdir -p "$OUT_DIR"
tar -C "$DEST" --owner=0 --group=0 -cf - usr/local/bin/box86 | zstd -19 -T0 -q -o "$OUT_DIR/box86-$BOX86_VERSION.tzst" -f
ls -la "$OUT_DIR/box86-$BOX86_VERSION.tzst"
