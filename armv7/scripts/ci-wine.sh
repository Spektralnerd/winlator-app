#!/usr/bin/env bash
# Baut das 32-Bit-Wine (i386) samt Container-Vorlage in einem i386-Debian-12-Container.
# "linux32" sorgt dafür, dass uname/config.guess i686 melden statt x86_64 (Kernel des Hosts).
#
#   docker run --rm -v "$PWD:/w" -w /w i386/debian:bookworm linux32 bash armv7/scripts/ci-wine.sh
#
# Ergebnis: $WORK_DIR/wine-stage.tar (stage/wine, stage/pattern, out/common_dlls.json)
set -euo pipefail
cd "$(dirname "$0")"

if [ "${1:-}" != "--no-deps" ]; then
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -q
    apt-get install -y -q --no-install-recommends \
        ca-certificates git python3 file \
        gcc make flex bison gettext perl pkg-config gcc-mingw-w64-i686 \
        libx11-dev libxext-dev libxrender-dev libxrandr-dev libxi-dev libxcursor-dev \
        libxcomposite-dev libxfixes-dev libxinerama-dev libxxf86vm-dev \
        libfreetype-dev libfontconfig-dev libgl-dev libvulkan-dev \
        libasound2-dev libpulse-dev libgnutls28-dev libkrb5-dev libdbus-1-dev \
        libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
        xvfb xauth >/dev/null
    git config --global --add safe.directory '*'
fi

[ "$(uname -m)" = i686 ] || [ "$(uname -m)" = i386 ] || echo "Warnung: uname -m = $(uname -m), besser mit linux32 starten"

. ./env.sh
bash ./build-wine.sh

log "Ergebnisse packen"
tar -C "$WORK_DIR" -cf "$WORK_DIR/wine-stage.tar" stage/wine stage/pattern out/common_dlls.json out/wine-sonames.txt
ls -la "$WORK_DIR/wine-stage.tar"
