#!/usr/bin/env bash
# Die Root-FS-Seite von Gladio (OpenGL über GLES der App) und Vortek (Vulkan über den
# Vulkan-Treiber des Geräts) für armhf.
#
# Ergebnis: $STAGE_DIR/gladio, $STAGE_DIR/vortek (Bäume ab ROOT_PREFIX)
set -euo pipefail
. "$(dirname "$0")/env.sh"

ARCH_FLAGS="-march=armv7-a -mfpu=neon -mfloat-abi=hard"
NO_T64=""
GCC_DEFINES="$($TARGET-gcc -dM -E - </dev/null)"
case "$GCC_DEFINES" in *"_TIME_BITS 64"*) NO_T64="-U_TIME_BITS -U_FILE_OFFSET_BITS";; esac

cmake_cross() { # <quelle> <bauordner> [zusätzliche CFLAGS]
    cat > "$2.toolchain.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR armhf)
set(CMAKE_C_COMPILER $TARGET-gcc)
EOF
    cmake -S "$1" -B "$2" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$2.toolchain.cmake" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_FLAGS="$ARCH_FLAGS $NO_T64 ${3:-}" -DCMAKE_SHARED_LINKER_FLAGS="-Wl,-rpath=$ROOT_PREFIX/lib" >/dev/null
    ninja -C "$2" -j"$JOBS"
}

# ---- Gladio ----
log "Gladio-Client"
SRC="$SRC_DIR/gladio"
git_fetch "$GLADIO_REPO" "$GLADIO_COMMIT" "$SRC"
cd "$SRC" && git checkout -q --force . && git clean -qfdx
# Anpassungen an den Server der App (Protokoll, glXGetProcAddressARB, glLightModelfv, ...):
python3 -I "$(dirname "$0")/patch-gladio-client.py" "$SRC"
cmake_cross "$SRC" "$BUILD_DIR/gladio"
DEST="$STAGE_DIR/gladio"; rm -rf "$DEST" && mkdir -p "$DEST/usr/lib"
install -m755 "$BUILD_DIR/gladio/libGL.so" "$DEST/usr/lib/libGL.so.1.7.0"
$TARGET-strip --strip-unneeded "$DEST/usr/lib/libGL.so.1.7.0"
file "$DEST/usr/lib/libGL.so.1.7.0"

# ---- Vortek ----
log "Vortek-Client"
SRC="$SRC_DIR/vortek"
git_fetch "$VORTEK_REPO" "$VORTEK_COMMIT" "$SRC"
cd "$SRC" && git checkout -q --force . && git clean -qfdx
# Vulkan-Header in der Fassung, die auch der Vortek-Server in der App benutzt (1.3.302),
# vollständig mit vulkan_xlib.h (der Client meldet sich als Xlib-WSI); dazu die X11-Header
VKH="$SRC_DIR/vulkan-headers"
git_fetch https://github.com/KhronosGroup/Vulkan-Headers v1.3.302 "$VKH"
INC="-I$VKH/include"
if ! echo '#include <X11/Xlib.h>' | $TARGET-gcc -E -x c - >/dev/null 2>&1; then
    # nur für Testbauten ohne libx11-dev:armhf: die (architekturneutralen) X11-Header des Bauhosts
    mkdir -p "$BUILD_DIR/x11inc" && ln -sfn /usr/include/X11 "$BUILD_DIR/x11inc/X11"
    INC="$INC -I$BUILD_DIR/x11inc"
fi
cmake_cross "$SRC" "$BUILD_DIR/vortek" "$INC"
DEST="$STAGE_DIR/vortek"; rm -rf "$DEST" && mkdir -p "$DEST/usr/lib" "$DEST/usr/share/vulkan/icd.d"
install -m755 "$BUILD_DIR/vortek/libvulkan_vortek.so" "$DEST/usr/lib/libvulkan_vortek.so"
$TARGET-strip --strip-unneeded "$DEST/usr/lib/libvulkan_vortek.so"
cat > "$DEST/usr/share/vulkan/icd.d/vortek_icd.armhf.json" <<EOF
{
    "ICD": {
        "api_version": "1.1.128",
        "library_path": "$ROOT_PREFIX/lib/libvulkan_vortek.so"
    },
    "file_format_version": "1.0.0"
}
EOF
file "$DEST/usr/lib/libvulkan_vortek.so"
