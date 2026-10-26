#!/usr/bin/env bash
# OpenGL-Treiber „Software“ für die originale 64-Bit-Root-FS (Varianten arm64 und armv7hybrid):
# Mesa llvmpipe/softpipe aus brunodev85/mesa3d-custom (virgl-23.1.9, derselbe Mesa-Stand wie die
# VirGL-Bibliothek des Originals, GLX über Xlib wie dort), aber nur mit dem Gallium-Treiber swrast,
# eingebautem glapi und statisch gelinktem LLVM 15. Die VirGL-Bibliothek des Originals bleibt
# unverändert; der 32-Bit-Build hat llvmpipe in seiner eigenen VirGL-Bibliothek (build-mesa.sh).
#
# Baut nativ für den Bauhost, gedacht für arm64-Ubuntu 24.04 (GitHub-Runner ubuntu-24.04-arm):
# glibc 2.39 und libstdc++ von GCC 13 (6.0.32) wie in der Root-FS des Originals. Pakete:
#   meson ninja-build pkg-config python3-mako bison flex llvm-15-dev libx11-dev libxext-dev
#   libxcb1-dev libxcb-randr0-dev x11proto-dev libexpat1-dev zlib1g-dev libzstd-dev
#   (für die Probe am Ende zusätzlich xvfb)
# Zum Ausprobieren der Bauschritte auf einem x86-PC: ALLOW_HOST_ARCH=1 (ohne Root-FS-Prüfung).
#
#   mesa-llvmpipe-arm64.sh ZIEL.tzst [ROOTFS.tzst]
#     ZIEL.tzst    Paket wie graphics_driver/virgl-*.tzst des Originals (./usr/lib/libGL.so.1.7.0),
#                  daneben ZIEL ohne .tzst + .id mit der Prüfsumme (die App entpackt das Paket neu,
#                  wenn sie sich ändert)
#     ROOTFS.tzst  Root-FS des Originals (app/src/main/assets/rootfs.tzst): jede Bibliothek, die
#                  die libGL braucht, muss dort samt den verlangten Symbolversionen vorhanden sein
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ARMV7_DIR=$(cd "$HERE/.." && pwd)
OUT=$(realpath -m "$1")
ROOTFS_TZST=${2:+$(realpath "$2")}
WORK=$(realpath -m "${WORK:-$ARMV7_DIR/work/llvmpipe64}")
JOBS=${JOBS:-$(nproc)}
LLVM_VER=${LLVM_VER:-15}
ROOT_PREFIX=/data/data/com.winlator/files/rootfs
MESA_REPO=$(sed -n 's/^export MESA_REPO=//p' "$HERE/env.sh")
MESA_COMMIT=$(sed -n 's/.*MESA_COMMIT:-\([0-9a-f]*\).*/\1/p' "$HERE/env.sh")
[ -n "$MESA_REPO" ] && [ -n "$MESA_COMMIT" ] || { echo "MESA_REPO/MESA_COMMIT nicht in env.sh"; exit 1; }
log() { printf '\n==> %s\n' "$*"; }

NATIVE=1
case "$(uname -m)" in
    aarch64) ;;
    *) [ "${ALLOW_HOST_ARCH:-0}" = 1 ] || { echo "baut nativ für aarch64, der Bauhost ist $(uname -m) (zum Ausprobieren: ALLOW_HOST_ARCH=1)"; exit 1; }
       NATIVE=0;;
esac
mkdir -p "$WORK"

log "Mesa-Quellen ($MESA_COMMIT, nur virgl-23.1.9)"
SRC="$WORK/mesa3d-custom"
if [ ! -d "$SRC/.git" ]; then
    git clone -q --filter=blob:none --no-checkout "$MESA_REPO" "$SRC"
fi
git -C "$SRC" sparse-checkout set virgl-23.1.9
git -C "$SRC" checkout -q --force "$MESA_COMMIT"
git -C "$SRC" checkout -q --force . && git -C "$SRC" clean -qfdx
M="$SRC/virgl-23.1.9"
# Stellschrauben der Texturfilterung (WINLATOR_LP_LOD_BIAS, WINLATOR_LP_MAG_NEAREST) wie im 32-Bit-Build;
# die übrigen Mesa-Patches sind für ARMv7 (NEON) bzw. die Diagnose des 32-Bit-Startprotokolls
git -C "$SRC" apply "$ARMV7_DIR/patches/mesa/0009-lp-sampler-knobs.patch"
# Mesa 23.1 prüft die mako-Version mit distutils, das es ab Python 3.12 nicht mehr gibt
python3 - "$M/meson.build" <<'PYEOF'
import sys
path = sys.argv[1]
text = open(path).read()
old = 'from distutils.version import StrictVersion\nimport mako\nassert StrictVersion(mako.__version__) >= StrictVersion("0.8.0")\n'
if old not in text:
    sys.exit("mako-Prüfung in meson.build nicht gefunden")
open(path, 'w').write(text.replace(old, 'import mako\n'))
PYEOF

log "LLVM $LLVM_VER (statisch)"
LLVM_CONFIG_REAL=$(command -v "llvm-config-$LLVM_VER" || echo "/usr/lib/llvm-$LLVM_VER/bin/llvm-config")
[ -x "$LLVM_CONFIG_REAL" ] || { echo "llvm-config $LLVM_VER fehlt (Paket llvm-$LLVM_VER-dev)"; exit 1; }
ls "$("$LLVM_CONFIG_REAL" --libdir)/libLLVMCore.a" >/dev/null || { echo "statische LLVM-Bibliotheken fehlen"; exit 1; }
# terminfo: LLVM fragt darüber nur, ob ein Terminal Farben kann. Die Root-FS des Originals hat keine
# libtinfo; statt ihrer kommt ein Ersatz ohne Terminal (Antwort: keine Farben) statisch in die libGL.
STUB="$WORK/stub"
mkdir -p "$STUB"
cat > "$STUB/tinfo_stub.c" <<'EOF'
#define HIDDEN __attribute__((visibility("hidden")))
struct term;
HIDDEN struct term *set_curterm(struct term *t) { (void)t; return 0; }
HIDDEN int del_curterm(struct term *t) { (void)t; return 0; }
HIDDEN int setupterm(const char *term, int fd, int *errret) { (void)term; (void)fd; if (errret) *errret = -1; return -1; }
HIDDEN int tigetnum(const char *cap) { (void)cap; return -2; }
EOF
cc -O2 -fPIC -c "$STUB/tinfo_stub.c" -o "$STUB/tinfo_stub.o"
rm -f "$STUB/libtinfo_stub.a" && ar rcs "$STUB/libtinfo_stub.a" "$STUB/tinfo_stub.o"
# Polly und Z3 braucht llvmpipe nicht (Debian/Ubuntu bauen LLVM damit, die Archive liegen in anderen Paketen)
mkdir -p "$WORK/bin"
cat > "$WORK/bin/llvm-config" <<EOF
#!/bin/sh
out="\$("$LLVM_CONFIG_REAL" "\$@")" || exit \$?
printf '%s\n' "\$out" | sed -E -e ':a' -e 's#(^| )(-lPolly|-lPollyISL|-lz3|[^ ]*/libPolly[^ ]*\.a|[^ ]*/libz3\.so[^ ]*)( |\$)#\1#' -e 'ta' \
    -e 's#(^| )-ltinfo( |\$)#\1$STUB/libtinfo_stub.a\2#g'
EOF
chmod +x "$WORK/bin/llvm-config"
echo "llvm-config $("$WORK/bin/llvm-config" --version), Ziel $("$WORK/bin/llvm-config" --host-target)"
echo "Systembibliotheken (statisch): $("$WORK/bin/llvm-config" --link-static --system-libs)"
cat > "$WORK/native.ini" <<EOF
[binaries]
llvm-config = '$WORK/bin/llvm-config'
EOF

log "Mesa bauen (swrast: llvmpipe + softpipe, GLX über Xlib)"
B="$WORK/build"
rm -rf "$B"
meson setup "$B" "$M" --native-file "$WORK/native.ini" \
    -Dbuildtype=release -Dstrip=true -Dprefix="$ROOT_PREFIX/usr" -Dlibdir=lib \
    -Dplatforms=x11 -Dglx=xlib -Ddri3=disabled -Dvulkan-drivers= -Dgallium-drivers=swrast \
    -Dgallium-va=disabled -Dgallium-vdpau=disabled -Dgallium-xa=disabled -Dgallium-opencl=disabled \
    -Dlmsensors=disabled -Degl=disabled -Dgbm=disabled -Dgles1=disabled -Dgles2=disabled \
    -Dvalgrind=disabled -Dlibunwind=disabled -Dshared-glapi=disabled \
    -Dllvm=enabled -Dshared-llvm=disabled
ninja -C "$B" -j"$JOBS"
grep -q -- "-DLLVM_AVAILABLE" "$B/build.ninja" || { echo "Mesa wurde ohne LLVM gebaut (kein llvmpipe)"; exit 1; }

PKG="$WORK/pkg"
rm -rf "$PKG" "$B/inst" && mkdir -p "$PKG/usr/lib"
DESTDIR="$B/inst" meson install -C "$B" --no-rebuild >/dev/null
LIB=$(find "$B/inst" -name 'libGL.so.1.*' -type f | head -1)
G="$PKG/usr/lib/libGL.so.1.7.0"
install -m755 "$LIB" "$G"
file "$G"
ls -la "$G"
readelf -d "$G" | grep -E 'NEEDED|SONAME'
grep -aq llvmpipe "$G" || { echo "llvmpipe fehlt in der libGL"; exit 1; }
for v in GLIBC GLIBCXX CXXABI; do
    echo "neueste $v-Version: $(readelf -V -W "$G" | grep -o "${v}_[0-9.]*" | sort -uV | tail -1)"
done

if [ -n "$ROOTFS_TZST" ] && [ "$NATIVE" = 1 ]; then
    log "Gegen die Root-FS des Originals prüfen"
    R="$WORK/rootfs"
    rm -rf "$R" && mkdir -p "$R"
    zstd -dc "$ROOTFS_TZST" | tar -x -C "$R" 2>/dev/null || true
    python3 - "$G" "$R" <<'PYEOF'
import os, re, subprocess, sys
lib, root = sys.argv[1], sys.argv[2]
def readelf(args, path):
    return subprocess.run(['readelf'] + args + [path], capture_output=True, text=True, check=True).stdout
needed = re.findall(r'\(NEEDED\)\s+Shared library: \[([^\]]+)\]', readelf(['-d'], lib))
# verlangte Versionen je Bibliothek (Version needs section)
wanted, cur = {}, None
for line in readelf(['-V', '-W'], lib).splitlines():
    m = re.search(r'File: (\S+)', line)
    if m:
        cur = m.group(1); wanted.setdefault(cur, set()); continue
    m = re.search(r'Name: (\S+)', line)
    if m and cur: wanted[cur].add(m.group(1))
bad = []
for name in needed:
    path = None
    for d in ('usr/lib', 'lib', 'usr/lib/aarch64-linux-gnu', 'lib/aarch64-linux-gnu'):
        p = os.path.join(root, d, name)
        if os.path.exists(p):
            path = os.path.realpath(p); break
    if path is None or not path.startswith(os.path.realpath(root)):
        bad.append(f'{name} fehlt in der Root-FS'); continue
    defined = set(re.findall(r'Name: (\S+)', readelf(['-V', '-W'], path).split('Version needs section')[0]))
    missing = sorted(wanted.get(name, set()) - defined)
    print(f'{name}: {os.path.relpath(path, root)}' + (f' – fehlt {missing}' if missing else ' ok'))
    if missing: bad.append(f'{name}: {missing}')
if bad:
    print('Root-FS passt nicht: ' + '; '.join(bad)); sys.exit(1)
PYEOF
    rm -rf "$R"
fi

if [ "${SMOKE_TEST:-1}" = 1 ] && command -v xvfb-run >/dev/null; then
    log "Probe: GLX-Kontext auf Xvfb, Dreieck zeichnen, Pixel lesen"
    T="$WORK/probe"
    rm -rf "$T" && mkdir -p "$T/lib"
    ln -s "$G" "$T/lib/libGL.so.1"
    cat > "$T/probe.c" <<'EOF'
#include <stdio.h>
#include <string.h>
#include <X11/Xlib.h>
#include <GL/gl.h>
#include <GL/glx.h>
int main(void) {
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) { puts("kein Display"); return 1; }
    int attrs[] = {GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, GLX_DEPTH_SIZE, 16, None};
    XVisualInfo *vi = glXChooseVisual(dpy, DefaultScreen(dpy), attrs);
    if (!vi) { puts("kein Visual"); return 1; }
    XSetWindowAttributes swa = {0};
    swa.colormap = XCreateColormap(dpy, RootWindow(dpy, vi->screen), vi->visual, AllocNone);
    Window win = XCreateWindow(dpy, RootWindow(dpy, vi->screen), 0, 0, 64, 64, 0, vi->depth, InputOutput, vi->visual, CWColormap, &swa);
    XMapWindow(dpy, win);
    GLXContext ctx = glXCreateContext(dpy, vi, NULL, True);
    if (!ctx || !glXMakeCurrent(dpy, win, ctx)) { puts("kein Kontext"); return 1; }
    printf("GL_RENDERER: %s\nGL_VERSION: %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    glViewport(0, 0, 64, 64);
    glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glBegin(GL_TRIANGLES);
    glColor3f(1, 0, 0); glVertex2f(-0.8f, -0.8f); glVertex2f(0.8f, -0.8f); glVertex2f(0.0f, 0.8f);
    glEnd();
    glFinish();
    unsigned char mid[4], corner[4];
    glReadPixels(32, 24, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, mid);
    glReadPixels(1, 62, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, corner);
    printf("Mitte %u,%u,%u  Ecke %u,%u,%u\n", mid[0], mid[1], mid[2], corner[0], corner[1], corner[2]);
    glXSwapBuffers(dpy, win);
    int ok = strstr((const char *)glGetString(GL_RENDERER), "llvmpipe") && mid[0] == 255 && mid[1] == 0 &&
             corner[0] >= 63 && corner[0] <= 65 && corner[2] >= 190 && corner[2] <= 192;
    glXMakeCurrent(dpy, None, NULL);
    glXDestroyContext(dpy, ctx);
    XCloseDisplay(dpy);
    puts(ok ? "Probe ok" : "Probe fehlgeschlagen");
    return ok ? 0 : 1;
}
EOF
    cc -O1 -I"$M/include" "$T/probe.c" -o "$T/probe" -L"$T/lib" -l:libGL.so.1 -lX11
    LD_LIBRARY_PATH="$T/lib" LP_NUM_THREADS=2 xvfb-run -a -s "-screen 0 640x480x24" "$T/probe"
fi

log "Paket"
mkdir -p "$(dirname "$OUT")"
tar --owner=0 --group=0 --numeric-owner -C "$PKG" -cf - . | zstd -q -19 -T0 -f -o "$OUT"
sha256sum "$OUT" | cut -c1-16 > "${OUT%.tzst}.id"
ls -la "$OUT" "${OUT%.tzst}.id"
