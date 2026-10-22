#!/usr/bin/env bash
# Holt die offiziellen Android-Builds von FluidSynth (dieselbe Version wie die arm64-Bibliotheken
# in app/src/main/jniLibs) und legt die armeabi-v7a-Bibliotheken nach app/src/armv7/jniLibs.
# Ohne diese Dateien baut die App trotzdem, nur ohne MIDI-Ausgabe.
set -euo pipefail
cd "$(dirname "$0")/../.."

VERSION="${FLUIDSYNTH_VERSION:-2.4.5}"
ABI=armeabi-v7a
DEST=app/src/armv7/jniLibs/$ABI
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

AUTH=()
[ -n "${GITHUB_TOKEN:-}" ] && AUTH=(-H "Authorization: Bearer $GITHUB_TOKEN")
URL="$(curl -fsSL "${AUTH[@]}" "https://api.github.com/repos/FluidSynth/fluidsynth/releases/tags/v$VERSION" |
       python3 -c 'import json,sys; a=[x["browser_download_url"] for x in json.load(sys.stdin)["assets"] if "android" in x["name"].lower()]; print(a[0] if a else "")')"
[ -n "$URL" ] || { echo "Kein Android-Paket für FluidSynth $VERSION gefunden"; exit 1; }

echo "Lade $URL"
curl -fsSL -o "$TMP/fluidsynth.zip" "$URL"
unzip -q "$TMP/fluidsynth.zip" -d "$TMP/x"
SRC="$(find "$TMP/x" -type d -path "*/$ABI" | head -1)"
[ -n "$SRC" ] || { echo "Kein $ABI-Ordner im FluidSynth-Paket"; find "$TMP/x" -maxdepth 3; exit 1; }

mkdir -p "$DEST"
# genau die Bibliotheken, die die arm64-Variante mitbringt (siehe midihandler/CMakeLists.txt)
for lib in $(ls app/src/main/jniLibs/arm64-v8a | grep -E '^lib(FLAC|c\+\+_shared|fluidsynth|fluidsynth-assetloader|gio-2\.0|glib-2\.0|gmodule-2\.0|gobject-2\.0|gthread-2\.0|instpatch-1\.0|oboe|ogg|opus|pcre|pcreposix|sndfile|vorbis|vorbisenc|vorbisfile|omp)\.so$'); do
    if [ -f "$SRC/$lib" ]; then cp "$SRC/$lib" "$DEST/"; else echo "Hinweis: $lib fehlt im FluidSynth-Paket"; fi
done
# libfluidsynth braucht libomp.so; liegt sie nicht bei, die aus dem NDK nehmen
if ! [ -f "$DEST/libomp.so" ] && [ -f "$SRC/libomp.so" ]; then cp "$SRC/libomp.so" "$DEST/"; fi
if ! [ -f "$DEST/libomp.so" ] && [ -n "${ANDROID_NDK_HOME:-}" ]; then
    OMP="$(find "$ANDROID_NDK_HOME" -path "*linux/arm/libomp.so" | head -1)"
    [ -n "$OMP" ] && cp "$OMP" "$DEST/"
fi
ls -la "$DEST"
