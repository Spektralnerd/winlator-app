#!/usr/bin/env bash
# Baut alle armhf-Teile des 32-Bit-Unterbaus in einem Debian-12-Container (amd64):
#   glibc, Box86, alsa-lib + Plugin, Gladio-/Vortek-Client, Mesa, Debian-Bibliotheken,
#   Winlator-Hilfsprogramme.
# Debian 12, weil es für armhf noch das klassische 32-Bit-time_t hat (wie i386 für Box86).
#
#   docker run --rm -v "$PWD:/w" -w /w debian:bookworm bash armv7/scripts/ci-armhf.sh
#
# Ergebnis: $WORK_DIR/armhf-stage.tar (stage/ und out/)
# Mit --no-deps werden keine Pakete installiert (für einen schon eingerichteten Bauhost).
set -euo pipefail
cd "$(dirname "$0")"

if [ "${1:-}" != "--no-deps" ]; then
    export DEBIAN_FRONTEND=noninteractive
    dpkg --add-architecture armhf
    apt-get update -q
    ARMHF_DEV=(
        libx11-dev libx11-xcb-dev libxext-dev libxcb1-dev libxcb-randr0-dev libxfixes-dev
        libxau-dev libxdmcp-dev libpthread-stubs0-dev
        libxrandr-dev libxshmfence-dev libxxf86vm-dev libdrm-dev zlib1g-dev libexpat1-dev
        libzstd-dev libvulkan-dev
        # Systembibliotheken, die statisches LLVM (llvmpipe in build-mesa.sh) beim Linken nennt
        libncurses-dev libxml2-dev
    )
    apt-get install -y -q --no-install-recommends \
        ca-certificates git curl xz-utils zstd file patch patchelf python3 python3-mako \
        build-essential crossbuild-essential-armhf bison flex gawk gettext autopoint \
        autoconf automake libtool pkg-config cmake ninja-build meson \
        gcc-mingw-w64-i686 binutils-mingw-w64-i686 x11proto-dev xcb-proto python3-xcbgen \
        qemu-user-static \
        "${ARMHF_DEV[@]/%/:armhf}" >/dev/null
    git config --global --add safe.directory '*'
fi

. ./env.sh
# ARMHF_STEPS="build-mesa" baut nur einzelne Schritte (z. B. im Mesa-Testlauf)
for step in ${ARMHF_STEPS:-build-glibc build-box86 build-alsa build-clients build-mesa fetch-armhf-libs build-xcb build-winhelpers}; do
    log "### $step"
    bash "./$step.sh"
done

log "Ergebnisse packen"
tar -C "$WORK_DIR" -cf "$WORK_DIR/armhf-stage.tar" stage out
ls -la "$WORK_DIR/armhf-stage.tar"
