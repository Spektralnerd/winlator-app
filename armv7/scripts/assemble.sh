#!/usr/bin/env bash
# Setzt die gebauten Teile zu den Assets der armv7-App zusammen (Aufbau wie app/src/main/assets):
#   rootfs.tzst                 armhf-glibc, Debian-armhf-Bibliotheken, alsa-lib, i386-Wine,
#                               x86-Bibliotheken von Box86, Konfiguration
#   rootfs_patches.tzst         Original ohne 64-Bit-Programme, mit den 32-Bit-Hilfsprogrammen
#   container_pattern.tzst      Win32-Präfix
#   common_dlls.json
#   box86/box86-<v>.tzst
#   graphics_driver/{virgl,zink,gladio,vortek}-<v>.tzst
# und packt sie in $OUT_DIR/armv7-assets.tar (wird nach app/src/armv7/assets entpackt).
#
# Erwartet die Ergebnisse von ci-armhf.sh und ci-wine.sh in $STAGE_DIR/$OUT_DIR.
set -euo pipefail
. "$(dirname "$0")/env.sh"

APP="$(cd "$ARMV7_DIR/.." && pwd)/app"
UPSTREAM="${UPSTREAM_ASSETS:-$APP/src/main/assets}"
DEFVER="$APP/src/main/java/com/winlator/core/DefaultVersion.java"
ASSETS="$OUT_DIR/assets"
TMP="$BUILD_DIR/assemble"
LEVEL="${ZSTD_LEVEL:-19}"

need() { for p in "$@"; do [ -e "$p" ] || { echo "fehlt: $p"; exit 1; }; done; }
need "$UPSTREAM/rootfs.tzst" "$UPSTREAM/rootfs_patches.tzst" "$DEFVER"
for d in glibc box86 alsa libs gladio vortek mesa-virgl mesa-zink winhelpers wine/opt/wine pattern/.wine; do
    need "$STAGE_DIR/$d"
done
need "$OUT_DIR/box86-$BOX86_VERSION.tzst" "$OUT_DIR/common_dlls.json" "$OUT_DIR/box86-wrapped.txt"

ver() { sed -nE "s/.*String $1 = \"([^\"]+)\".*/\\1/p" "$DEFVER" | head -1; }
VIRGL="$(ver VIRGL)" ZINK="$(ver ZINK)" GLADIO="$(ver GLADIO)" VORTEK="$(ver VORTEK)"
[ -n "$VIRGL" ] && [ -n "$ZINK" ] && [ -n "$GLADIO" ] && [ -n "$VORTEK" ] || { echo "Versionen nicht gefunden"; exit 1; }

# <ordner> <ziel.tzst> [einträge...]: Tar (root:root, sortiert) mit zstd
pack() {
    local dir="$1" out="$2"; shift 2
    mkdir -p "$(dirname "$out")"
    tar -C "$dir" --owner=0 --group=0 --numeric-owner --sort=name -cf - "${@:-.}" |
        zstd -"$LEVEL" -T0 -q -f -o "$out"
    printf '  %-45s %s\n' "${out#$ASSETS/}" "$(du -h "$out" | cut -f1)"
}

rm -rf "$TMP" "$ASSETS" && mkdir -p "$TMP" "$ASSETS"

log "Root-FS zusammenstellen"
R="$TMP/rootfs"; mkdir -p "$R"
# architekturunabhängige Teile aus dem originalen Root-FS: Font-Konfiguration, Locales
# (glibc 2.39, Little Endian, auch für armhf gültig), PulseAudio-Client, DNS
U="$TMP/upstream"; mkdir -p "$U"
zstd -dc "$UPSTREAM/rootfs.tzst" | tar -C "$U" -xf - --wildcards \
    './etc/fonts*' './etc/pulse*' './etc/resolv.conf' './usr/share/fontconfig*' './usr/share/xml*' \
    './usr/lib/locale/*'
cp -a "$U/." "$R/"

# glibc (ohne Entwicklerdateien)
cp -a "$STAGE_DIR/glibc/." "$R/"
rm -rf "$R/usr/include"
find "$R/usr/lib" -maxdepth 1 -type f \( -name '*.a' -o -name '*.o' \) -delete
for f in "$R"/usr/lib/*.so; do
    [ -f "$f" ] && [ ! -L "$f" ] || continue
    case "$(head -c 200 "$f" | tr -d '\0')" in *"GNU ld script"*) rm -f "$f";; esac
done
# Debian-Bibliotheken, darüber die eigene alsa-lib
cp -a "$STAGE_DIR/libs/." "$R/"
cp -a "$STAGE_DIR/alsa/." "$R/"
# x86-Bibliotheken von Box86 (Box86 selbst kommt als Komponente box86/box86-<v>.tzst)
mkdir -p "$R/usr/lib/i386-linux-gnu"
cp -a "$STAGE_DIR/box86/usr/lib/i386-linux-gnu/." "$R/usr/lib/i386-linux-gnu/"
# Wine
cp -a "$STAGE_DIR/wine/opt" "$R/"

ln -sfn usr/bin "$R/bin"
[ -L "$R/lib" ] || { rm -rf "$R/lib"; ln -s usr/lib "$R/lib"; }
mkdir -p "$R/tmp" "$R/home" "$R/var/cache" "$R/usr/local/bin" "$R/usr/share/vulkan/icd.d" "$R/.winlator"
# libGL.so.1.7.0 kommt mit dem gewählten Grafiktreiber
ln -sfn libGL.so.1.7.0 "$R/usr/lib/libGL.so.1"
ln -sfn libGL.so.1 "$R/usr/lib/libGL.so"
[ -e "$R/usr/lib/libvulkan.so.1" ] && ln -sfn libvulkan.so.1 "$R/usr/lib/libvulkan.so"

# Die Debian-Bibliotheken suchen ihre Daten unter /usr und /etc; Winlator setzt diese
# Variablen beim Start (GuestProgramLauncherComponent, @ROOT@ = Root-FS-Ordner)
cat > "$R/.winlator/env" <<'EOF'
# Laufzeitpfade für die armhf-Bibliotheken im Root-FS (@ROOT@ = Root-FS-Ordner).
# Variablen aus den Container-Einstellungen haben Vorrang.
FONTCONFIG_PATH=@ROOT@/etc/fonts
FONTCONFIG_FILE=@ROOT@/etc/fonts/fonts.conf
XLOCALEDIR=@ROOT@/usr/share/X11/locale
XCURSOR_PATH=~/.local/share/icons:~/.icons:@ROOT@/usr/share/icons:@ROOT@/usr/share/pixmaps
XDG_DATA_DIRS=@ROOT@/usr/share
GIO_MODULE_DIR=@ROOT@/usr/lib/gio/modules
GST_PLUGIN_SYSTEM_PATH_1_0=@ROOT@/usr/lib/gstreamer-1.0
GST_PLUGIN_SCANNER_1_0=@ROOT@/usr/libexec/gstreamer-1.0/gst-plugin-scanner
PULSE_CLIENTCONFIG=@ROOT@/etc/pulse/client.conf
EOF

# Kennung dieses Root-FS-Builds, im Root-FS und daneben in den Assets: die App entpackt das
# Root-FS neu, wenn die mitgelieferte Kennung nicht zur installierten passt
ROOTFS_ID="${ROOTFS_KEY:-lokal}-$(date -u +%Y%m%d%H%M)"
echo "$ROOTFS_ID" > "$R/.winlator/rootfs-id"
log "Root-FS-Kennung: $ROOTFS_ID"

log "Root-FS prüfen"
# (CHECK_ROOTFS=warn: Fehler nur melden, z. B. für Testläufe mit unvollständigen Teilen)
python3 -I "$ARMV7_DIR/scripts/check-rootfs.py" "$R" "$OUT_DIR/box86-wrapped.txt" "$OUT_DIR/wine-sonames.txt" \
    "$STAGE_DIR/mesa-virgl" "$STAGE_DIR/mesa-zink" "$STAGE_DIR/gladio" "$STAGE_DIR/vortek" \
    || [ "${CHECK_ROOTFS:-strict}" = warn ]
for d in "$R" "$R/opt/wine" "$R/usr/lib"; do du -sh "$d"; done

log "rootfs_patches zusammenstellen"
P="$TMP/patches"; mkdir -p "$P"
zstd -dc "$UPSTREAM/rootfs_patches.tzst" | tar -C "$P" -xf -
# 64-Bit-Teile des Originals entfernen (GPUInfo/winaddons gibt es nur als x86-64)
rm -f "$P/opt/apps/GPUInfo.exe" "$P/opt/apps/winaddons.exe" "$P/usr/local/bin/winpreexec"
rm -rf "$P/usr/lib/alsa-lib"
WIN="$P/home/xuser/.wine/drive_c/windows"
if [ -d "$WIN/syswow64" ]; then
    mkdir -p "$WIN/system32" && cp -a "$WIN/syswow64/." "$WIN/system32/" && rm -rf "$WIN/syswow64"
fi
cp -a "$STAGE_DIR/winhelpers/." "$P/"
mkdir -p "$P/usr/lib/alsa-lib" && cp -a "$STAGE_DIR/alsa/usr/lib/alsa-lib/." "$P/usr/lib/alsa-lib/"
BAD="$(find "$P" -type f -exec file {} + | grep -E 'PE32\+|x86-64|aarch64' || true)"
[ -z "$BAD" ] || { echo "64-Bit-Dateien in rootfs_patches:"; echo "$BAD"; exit 1; }

log "Assets packen (zstd -$LEVEL)"
pack "$R" "$ASSETS/rootfs.tzst"
pack "$P" "$ASSETS/rootfs_patches.tzst"
pack "$STAGE_DIR/pattern" "$ASSETS/container_pattern.tzst" .wine
cp "$OUT_DIR/common_dlls.json" "$ASSETS/common_dlls.json"
echo "$ROOTFS_ID" > "$ASSETS/rootfs-id.txt"
mkdir -p "$ASSETS/box86" && cp "$OUT_DIR/box86-$BOX86_VERSION.tzst" "$ASSETS/box86/"
pack "$STAGE_DIR/mesa-virgl" "$ASSETS/graphics_driver/virgl-$VIRGL.tzst"
pack "$STAGE_DIR/mesa-zink" "$ASSETS/graphics_driver/zink-$ZINK.tzst"
pack "$STAGE_DIR/gladio" "$ASSETS/graphics_driver/gladio-$GLADIO.tzst"
pack "$STAGE_DIR/vortek" "$ASSETS/graphics_driver/vortek-$VORTEK.tzst"

tar -C "$ASSETS" --owner=0 --group=0 --numeric-owner --sort=name -cf "$OUT_DIR/armv7-assets.tar" .
ls -la "$OUT_DIR/armv7-assets.tar"
