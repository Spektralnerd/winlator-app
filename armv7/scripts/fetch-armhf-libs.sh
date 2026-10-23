#!/usr/bin/env bash
# Laufzeitbibliotheken für armhf aus Debian 12 (bookworm) holen, also aus einer Distribution
# mit dem klassischen 32-Bit-time_t (wie i386, das Box86 1:1 auf die armhf-Bibliotheken abbildet).
# Läuft in einem Debian-bookworm-Container mit "dpkg --add-architecture armhf".
#
# Übernommen wird nur, was gebraucht wird: eine Startliste von Bibliotheken und
# GStreamer-Plugins (wie im aarch64-Root-FS) plus alles, was diese per NEEDED nachziehen.
#
# Ergebnis: $STAGE_DIR/libs (usr/lib flach wie im Winlator-Root-FS, dazu Daten unter usr/share)
set -euo pipefail
. "$(dirname "$0")/env.sh"

DEST="$STAGE_DIR/libs"
DEBS="$BUILD_DIR/debs"
EXTRACT="$BUILD_DIR/debs-extract"

# Debian-Pakete, aus denen geschöpft wird (Abhängigkeiten werden mit aufgelöst)
PACKAGES=(
    libgcc-s1 libstdc++6 zlib1g libzstd1 libbz2-1.0 liblzma5 libbrotli1 libexpat1 libffi8
    libx11-6 libx11-xcb1 libx11-data libxau6 libxdmcp6 libxcomposite1 libxcursor1
    libxext6 libxfixes3 libxi6 libxinerama1 libxrandr2 libxrender1 libxxf86vm1 libxshmfence1
    libxcb1 libxcb-dri2-0 libxcb-dri3-0 libxcb-present0 libxcb-randr0 libxcb-render0
    libxcb-shm0 libxcb-sync1 libxcb-xfixes0 libxcb-glx0
    libdrm2 libvulkan1 libfreetype6 libpng16-16 libfontconfig1
    libglib2.0-0 libgnutls30 libgssapi-krb5-2 libkrb5-3
    libpulse0 libpulse-mainloop-glib0 libpulse-simple0 libsndfile1 libmpg123-0 libjpeg62-turbo libmpeg2-4
    libusb-1.0-0 libudev1 libsdl2-2.0-0 libdbus-1-3
    libgstreamer1.0-0 libgstreamer-plugins-base1.0-0
    gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-ugly
    libtinfo6 libxml2
)
# Startliste: Sonamen, die im Root-FS liegen sollen
SEED_LIBS=(
    libgcc_s.so.1 libstdc++.so.6 libz.so.1 libzstd.so.1 libbz2.so.1.0 libexpat.so.1 libffi.so.8
    libX11.so.6 libX11-xcb.so.1 libXau.so.6 libXdmcp.so.6 libXcomposite.so.1 libXcursor.so.1
    libXext.so.6 libXfixes.so.3 libXi.so.6 libXinerama.so.1 libXrandr.so.2 libXrender.so.1
    libXxf86vm.so.1 libxshmfence.so.1 libxcb.so.1 libxcb-dri2.so.0 libxcb-dri3.so.0
    libxcb-present.so.0 libxcb-randr.so.0 libxcb-render.so.0 libxcb-shm.so.0 libxcb-sync.so.1
    libxcb-xfixes.so.0 libxcb-glx.so.0
    libdrm.so.2 libvulkan.so.1 libfreetype.so.6 libpng16.so.16 libfontconfig.so.1
    libglib-2.0.so.0 libgobject-2.0.so.0 libgio-2.0.so.0 libgmodule-2.0.so.0 libgthread-2.0.so.0
    libgnutls.so.30 libgssapi_krb5.so.2 libkrb5.so.3
    libpulse.so.0 libpulse-simple.so.0 libpulse-mainloop-glib.so.0 libsndfile.so.1
    libmpg123.so.0 libjpeg.so.62 libmpeg2.so.0 libusb-1.0.so.0 libudev.so.1 libSDL2-2.0.so.0
    libdbus-1.so.3
    libgstreamer-1.0.so.0 libgstbase-1.0.so.0 libgstapp-1.0.so.0 libgstaudio-1.0.so.0
    libgstvideo-1.0.so.0 libgsttag-1.0.so.0 libgstpbutils-1.0.so.0 libgstriff-1.0.so.0
    libgstallocators-1.0.so.0 libgstfft-1.0.so.0 libgstrtp-1.0.so.0 libgstcontroller-1.0.so.0
)
# GStreamer-Plugins wie im aarch64-Root-FS (ohne libav/fdkaac/mpegpsdemux/videoparsersbad)
SEED_GST=(
    apetag app asf audioconvert audiofx audiomixer audioparsers audioresample avi compositor
    coreelements deinterlace encoding id3demux isomp4 matroska mpeg2dec mpg123 ogg playback
    rawparse typefindfunctions videobox videoconvertscale videocrop videofilter videomixer
    videorate volume wavparse
)
# Liefert unsere glibc
GLIBC_SONAMES='^(libc\.so\.6|libm\.so\.6|libdl\.so\.2|libpthread\.so\.0|librt\.so\.1|libresolv\.so\.2|libutil\.so\.1|libanl\.so\.1|libnsl\.so\.1|libBrokenLocale\.so\.1|libnss_.*|libthread_db\.so\.1|libmvec\.so\.1|ld-linux-armhf\.so\.3)$'

log "Pakete laden (armhf)"
mkdir -p "$DEBS" && cd "$DEBS"
ALL="$(apt-cache depends --recurse --no-recommends --no-suggests --no-conflicts --no-breaks \
        --no-replaces --no-enhances "${PACKAGES[@]/%/:armhf}" 2>/dev/null |
       grep -E '^[a-z0-9]' | sed 's/:armhf$//' | sort -u)"
for p in $ALL; do
    case "$p" in libc6|libc-bin|gcc-12-base|debconf|perl-base|dpkg|tar|adduser|passwd|login) continue;; esac
    ls "${p}"_*_armhf.deb >/dev/null 2>&1 || ls "${p}"_*_all.deb >/dev/null 2>&1 || \
        apt-get download -q "$p:armhf" >/dev/null 2>&1 || apt-get download -q "$p" >/dev/null 2>&1 || echo "  (übersprungen: $p)"
done
echo "$(ls *.deb | wc -l) Pakete"

log "Entpacken"
rm -rf "$EXTRACT" && mkdir -p "$EXTRACT"
for d in *.deb; do dpkg-deb -x "$d" "$EXTRACT"; done

# was die eigenen Mesa-Bibliotheken brauchen (llvmpipe zieht ggf. libtinfo nach)
for f in "$STAGE_DIR"/mesa-*/usr/lib/libGL.so.1.7.0; do
    [ -f "$f" ] || continue
    for n in $(readelf -d "$f" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
        echo "$n" | grep -Eq "$GLIBC_SONAMES" || SEED_LIBS+=("$n")
    done
done

log "Benötigte Bibliotheken bestimmen (NEEDED-Hülle)"
rm -rf "$DEST" && mkdir -p "$DEST/usr/lib/gstreamer-1.0" "$DEST/usr/share" "$DEST/usr/libexec/gstreamer-1.0"
python3 -I - "$EXTRACT" "$DEST" "$GLIBC_SONAMES" "${SEED_LIBS[*]}" "${SEED_GST[*]}" <<'PY'
import os, re, shutil, subprocess, sys
extract, dest, glibc_re, seed_libs, seed_gst = sys.argv[1:6]
glibc = re.compile(glibc_re)
libdirs = [os.path.join(extract, d) for d in ("lib/arm-linux-gnueabihf", "usr/lib/arm-linux-gnueabihf",
                                             "usr/lib/arm-linux-gnueabihf/pulseaudio")]
gstdir = os.path.join(extract, "usr/lib/arm-linux-gnueabihf/gstreamer-1.0")
def find(soname):
    for d in libdirs:
        p = os.path.join(d, soname)
        if os.path.exists(p): return p
    return None
def needed(path):
    out = subprocess.run(["readelf", "-d", path], capture_output=True, text=True).stdout
    return re.findall(r"\(NEEDED\)\s+Shared library: \[([^\]]+)\]", out)
todo, done, missing = [], set(), set()
for s in seed_libs.split():
    todo.append(("lib", s))
for g in seed_gst.split():
    p = os.path.join(gstdir, f"libgst{g}.so")
    if os.path.exists(p): todo.append(("gst", p))
    else: missing.add(f"gst:{g}")
copied = 0
while todo:
    kind, item = todo.pop()
    if kind == "lib":
        if item in done or glibc.match(item): continue
        done.add(item)
        path = find(item)
        if not path: missing.add(item); continue
        real = os.path.realpath(path)
        shutil.copy2(real, os.path.join(dest, "usr/lib", os.path.basename(real)))
        if os.path.basename(real) != item:
            link = os.path.join(dest, "usr/lib", item)
            if not os.path.lexists(link): os.symlink(os.path.basename(real), link)
        copied += 1
        deps = needed(real)
    else:
        shutil.copy2(item, os.path.join(dest, "usr/lib/gstreamer-1.0", os.path.basename(item)))
        copied += 1
        deps = needed(item)
    for n in deps:
        if n not in done and not glibc.match(n): todo.append(("lib", n))
print(f"{copied} Dateien übernommen")
if missing: print("FEHLT:", " ".join(sorted(missing)))
PY

# RUNPATH-Einträge zeigen auf Debian-Pfade (/usr/lib/arm-linux-gnueabihf/...); im Root-FS liegt
# alles flach in usr/lib, das ld.so ohnehin durchsucht
for f in $(find "$DEST/usr/lib" -type f -name '*.so*'); do
    if readelf -d "$f" 2>/dev/null | grep -E '\((RUNPATH|RPATH)\)' >/dev/null; then patchelf --remove-rpath "$f"; fi
done

# GStreamer-Plugin-Scanner (läuft als eigener Prozess) samt Interpreter-Pfad
SCANNER="$(find "$EXTRACT" -name gst-plugin-scanner -type f | head -1)"
if [ -n "$SCANNER" ]; then
    install -m755 "$SCANNER" "$DEST/usr/libexec/gstreamer-1.0/gst-plugin-scanner"
    patchelf --set-interpreter "$ROOT_PREFIX/lib/ld-linux-armhf.so.3" "$DEST/usr/libexec/gstreamer-1.0/gst-plugin-scanner"
fi
# Locale-Daten für libX11 (XLOCALEDIR)
[ -d "$EXTRACT/usr/share/X11" ] && cp -a "$EXTRACT/usr/share/X11" "$DEST/usr/share/"

# jede Abhängigkeit der Mesa-Bibliotheken muss jetzt im Root-FS liegen
for f in "$STAGE_DIR"/mesa-*/usr/lib/libGL.so.1.7.0; do
    [ -f "$f" ] || continue
    for n in $(readelf -d "$f" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
        echo "$n" | grep -Eq "$GLIBC_SONAMES" && continue
        [ -e "$DEST/usr/lib/$n" ] || { echo "$f braucht $n, das im Root-FS fehlt"; exit 1; }
    done
done

log "Übersicht"
du -sh "$DEST"
ls "$DEST/usr/lib" | grep -v '^gstreamer' | sed -E 's/\.so.*//' | sort -u | tr '\n' ' '; echo
echo "GStreamer-Plugins: $(ls "$DEST/usr/lib/gstreamer-1.0" | wc -l)"
