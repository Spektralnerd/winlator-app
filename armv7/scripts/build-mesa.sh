#!/usr/bin/env bash
# Mesa für armhf aus brunodev85/mesa3d-custom, Einstellungen wie im README dort (gallium-xlib-GLX,
# denn Winlators X-Server hat keine vollständige GLX-Erweiterung):
#   virgl-23.1.9  VirGL (GALLIUM_DRIVER=virpipe) + Software (llvmpipe, softpipe) in einer libGL
#   zink-22.2.5   Zink (OpenGL über Vulkan)
# Braucht die armhf-Entwicklerpakete aus Debian (siehe ci-armhf.sh).
#
# llvmpipe: LLVM 15 aus Debian (armhf, statisch in die libGL gelinkt; Debians eigenes Mesa für
# armhf baut ebenfalls auf LLVM 15). Die Pakete werden nur in einen eigenen Ordner entpackt;
# llvm-config ist ein armhf-Programm und läuft unter qemu-arm-static.
# MESA_LLVM=disabled baut wie bisher nur softpipe.
#
# Ergebnis: $STAGE_DIR/mesa-virgl, $STAGE_DIR/mesa-zink (jeweils usr/lib/libGL.so.1.7.0)
set -euo pipefail
. "$(dirname "$0")/env.sh"

SRC="$SRC_DIR/mesa3d-custom"
VARIANTS="${MESA_VARIANTS:-virgl zink}"
LLVM="${MESA_LLVM:-enabled}"
LLVM_VER="${MESA_LLVM_VERSION:-15}"

log "Quellen holen (nur die benötigten Unterordner)"
if [ ! -d "$SRC/.git" ]; then
    git clone -q --filter=blob:none --no-checkout "$MESA_REPO" "$SRC"
fi
git -C "$SRC" sparse-checkout set virgl-23.1.9 zink-22.2.5
git -C "$SRC" checkout -q --force "$MESA_COMMIT"
git -C "$SRC" checkout -q --force . && git -C "$SRC" clean -qfdx
# u. a.: softpipe auch neben VirGL (sonst hat „Software“ ohne llvmpipe keinen Treiber und
# jeder OpenGL-Kontext scheitert)
for p in "$ARMV7_DIR"/patches/mesa/*.patch; do
    [ -f "$p" ] || continue
    log "Patch $(basename "$p")"
    git -C "$SRC" apply "$p"
done

# pkg-config für armhf
mkdir -p "$BUILD_DIR/bin"
cat > "$BUILD_DIR/bin/$TARGET-pkg-config" <<EOF
#!/bin/sh
export PKG_CONFIG_LIBDIR=/usr/lib/$TARGET/pkgconfig:/usr/share/pkgconfig
export PKG_CONFIG_SYSROOT_DIR=
exec pkg-config "\$@"
EOF
chmod +x "$BUILD_DIR/bin/$TARGET-pkg-config"
export PATH="$BUILD_DIR/bin:$PATH"

if [ "$LLVM" = enabled ]; then
    log "LLVM $LLVM_VER für armhf (Debian-Pakete, llvm-config unter qemu)"
    LS="$BUILD_DIR/llvm-armhf"
    LLVM_CONFIG="$LS/usr/lib/llvm-$LLVM_VER/bin/llvm-config"
    if [ ! -x "$LLVM_CONFIG" ]; then
        D="$BUILD_DIR/llvm-debs"
        rm -rf "$LS" "$D" && mkdir -p "$LS" "$D"
        # llvm-config samt allem, was es zum Laufen braucht; dazu Header und statische Bibliotheken
        PKGS="$(apt-cache depends --recurse --no-recommends --no-suggests --no-conflicts --no-breaks \
                --no-replaces --no-enhances "llvm-$LLVM_VER:armhf" "libllvm$LLVM_VER:armhf" 2>/dev/null |
                grep -E '^[a-z0-9]' | sed 's/:armhf$//' | sort -u)"
        (cd "$D"
         for p in $PKGS "llvm-$LLVM_VER-dev"; do
             case "$p" in libc6|libc-bin|gcc-12-base|debconf|perl-base|dpkg|tar|adduser|passwd|login) continue;; esac
             apt-get download -q "$p:armhf" >/dev/null 2>&1 || apt-get download -q "$p" >/dev/null 2>&1 \
                 || echo "  (übersprungen: $p)"
         done)
        for d in "$D"/*.deb; do dpkg-deb -x "$d" "$LS"; done
        [ -x "$LLVM_CONFIG" ] || { echo "llvm-config für armhf fehlt"; exit 1; }
        ls "$LS/usr/lib/llvm-$LLVM_VER/lib/libLLVMCore.a" >/dev/null || { echo "statische LLVM-Bibliotheken fehlen"; exit 1; }
    fi
    QEMU="$(command -v qemu-arm-static || command -v qemu-arm || true)"
    [ -n "$QEMU" ] || { echo "qemu-arm(-static) fehlt"; exit 1; }
    # llvm-config leitet seine Pfade vom eigenen Ort ab; falls es doch feste Debian-Pfade
    # (/usr/lib/llvm-N) nennt, werden sie auf den entpackten Ordner umgebogen
    cat > "$BUILD_DIR/bin/llvm-config-armhf" <<EOF
#!/bin/sh
out="\$("$QEMU" -L "$LS" "$LLVM_CONFIG" "\$@")" || exit \$?
printf '%s\n' "\$out" | sed -E 's#(^|[ =]|-I|-L)/usr/lib/llvm-$LLVM_VER#\1$LS/usr/lib/llvm-$LLVM_VER#g'
EOF
    chmod +x "$BUILD_DIR/bin/llvm-config-armhf"
    echo "llvm-config $(llvm-config-armhf --version), Ziel $(llvm-config-armhf --host-target)"
    echo "Systembibliotheken (statisch): $(llvm-config-armhf --link-static --system-libs)"
fi

cat > "$BUILD_DIR/meson-armhf.ini" <<EOF
[binaries]
c = '$TARGET-gcc'
cpp = '$TARGET-g++'
ar = '$TARGET-ar'
strip = '$TARGET-strip'
pkg-config = '$TARGET-pkg-config'
pkgconfig = '$TARGET-pkg-config'
$( [ "$LLVM" = enabled ] && echo "llvm-config = '$BUILD_DIR/bin/llvm-config-armhf'" )

[host_machine]
system = 'linux'
cpu_family = 'arm'
cpu = 'armv7'
endian = 'little'

[built-in options]
c_args = ['-march=armv7-a', '-mfpu=neon', '-mfloat-abi=hard']
cpp_args = ['-march=armv7-a', '-mfpu=neon', '-mfloat-abi=hard']
EOF

COMMON=(-Dbuildtype=release -Dstrip=true -Dprefix="$ROOT_PREFIX/usr" -Dlibdir=lib
        -Dplatforms=x11 -Dglx=xlib -Ddri3=disabled -Dvulkan-drivers= -Dgallium-va=disabled
        -Dgallium-vdpau=disabled -Dgallium-xa=disabled -Dgallium-opencl=disabled
        -Dlmsensors=disabled -Degl=disabled -Dgbm=disabled -Dgles1=disabled -Dgles2=disabled
        -Dvalgrind=disabled -Dlibunwind=disabled -Dshared-glapi=disabled)

for v in $VARIANTS; do
    case "$v" in
        virgl) DIR=virgl-23.1.9; OPTS=(-Dgallium-drivers=virgl,swrast -Dllvm=$LLVM -Dshared-llvm=disabled);;
        zink)  DIR=zink-22.2.5;  OPTS=(-Dgallium-drivers=zink -Ddri-drivers= -Dllvm=disabled -Dshared-llvm=disabled);;
        *) echo "unbekannte Variante $v"; exit 1;;
    esac
    log "Mesa $DIR"
    B="$BUILD_DIR/mesa-$v"
    rm -rf "$B"
    meson setup "$B" "$SRC/$DIR" --cross-file "$BUILD_DIR/meson-armhf.ini" "${COMMON[@]}" "${OPTS[@]}"
    ninja -C "$B" -j"$JOBS"
    DEST="$STAGE_DIR/mesa-$v"
    rm -rf "$DEST" && DESTDIR="$B/inst" meson install -C "$B" --no-rebuild >/dev/null
    mkdir -p "$DEST/usr/lib"
    LIB="$(find "$B/inst" -name 'libGL.so.1.*' -type f | head -1)"
    install -m755 "$LIB" "$DEST/usr/lib/libGL.so.1.7.0"
    file "$DEST/usr/lib/libGL.so.1.7.0"
    ls -la "$DEST/usr/lib/libGL.so.1.7.0"
    readelf -d "$DEST/usr/lib/libGL.so.1.7.0" | grep NEEDED
    if [ "$v" = virgl ] && [ "$LLVM" = enabled ]; then
        grep -q "^#define LLVM_AVAILABLE\|-DLLVM_AVAILABLE" "$B/build.ninja" \
            || { echo "Mesa wurde ohne LLVM gebaut (kein llvmpipe)"; exit 1; }
    fi
done
