# Gemeinsame Einstellungen für den 32-Bit-Unterbau (armv7/armhf + i386-Wine).
# Wird von allen Bauskripten eingebunden: . "$(dirname "$0")/env.sh"

# Ziel-Präfix auf dem Gerät (fest eingebrannt in ld.so, ICD-Dateien usw.)
export ROOT_PREFIX=/data/data/com.winlator/files/rootfs

# Arbeitsordner (über Umgebung überschreibbar)
export ARMV7_DIR="${ARMV7_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
export WORK_DIR="${WORK_DIR:-$ARMV7_DIR/work}"
export SRC_DIR="${SRC_DIR:-$WORK_DIR/src}"
export BUILD_DIR="${BUILD_DIR:-$WORK_DIR/build}"
# Ergebnisse je Bestandteil, jeweils als Baum unterhalb von ROOT_PREFIX
export STAGE_DIR="${STAGE_DIR:-$WORK_DIR/stage}"
export OUT_DIR="${OUT_DIR:-$WORK_DIR/out}"

export TARGET=arm-linux-gnueabihf
export JOBS="${JOBS:-$(nproc)}"

# Fest eingestellte Quellstände, damit Bauläufe reproduzierbar sind
export GLIBC_VERSION=2.39
export GLIBC_REPO=https://github.com/bminor/glibc
export GLIBC_TAG=glibc-2.39
# termux-pacman/glibc-packages: letzter Stand mit glibc 2.39 (Revision 4)
export TERMUX_GLIBC_REPO=https://github.com/termux-pacman/glibc-packages
export TERMUX_GLIBC_COMMIT=5f6b1f58d2abb8260233dd984588916393be30cd
# Winlator-Hauptrepo: glibc_patches (SysV-shm), android_alsa (ALSA-Plugin)
export WINLATOR_REPO=https://github.com/brunodev85/winlator
export WINLATOR_COMMIT="${WINLATOR_COMMIT:-b3d21498076dea3b2306ecbb03d168d77d26cc8f}"

export BOX86_REPO=https://github.com/ptitSeb/box86
export BOX86_COMMIT="${BOX86_COMMIT:-7dec0819b5f212910cc7ff030f5161742add121c}"
export BOX86_VERSION=0.3.9

export WINE_REPO=https://github.com/brunodev85/wine-10.10-custom
export WINE_COMMIT="${WINE_COMMIT:-2b9afc38d9531d384f038bbaaf3f637e515fb297}"
export WINE_VERSION=10.10

export MESA_REPO=https://github.com/brunodev85/mesa3d-custom
export MESA_COMMIT="${MESA_COMMIT:-fa711f80c79a7fc04e5cc5438542b5c3692ddcba}"
export VORTEK_REPO=https://github.com/brunodev85/vortek
export VORTEK_COMMIT="${VORTEK_COMMIT:-b1730c5def9b575672e671aee11d79ae7adc63d1}"
export GLADIO_REPO=https://github.com/brunodev85/gladio
export GLADIO_COMMIT="${GLADIO_COMMIT:-116c0d14dedbea3bd057f98f1db138bb1efe225e}"
export WFM_REPO=https://github.com/brunodev85/wfm
export WFM_COMMIT="${WFM_COMMIT:-6b1860895885a6d65c37815308260293341acb5f}"
export LIBCDIO_REPO=https://github.com/brunodev85/libcdio
export LIBCDIO_COMMIT="${LIBCDIO_COMMIT:-8c421d21c7d8b30433f52ed0a74045c7515945a2}"

mkdir -p "$SRC_DIR" "$BUILD_DIR" "$STAGE_DIR" "$OUT_DIR"

log() { printf '\n\033[1;34m==> %s\033[0m\n' "$*"; }

# git_fetch <url> <commit|tag|branch> <zielordner>: flacher Checkout eines festen Stands
git_fetch() {
    local url="$1" ref="$2" dir="$3"
    if [ -d "$dir/.git" ]; then
        if git -C "$dir" rev-parse -q --verify "$ref^{commit}" >/dev/null 2>&1; then
            git -C "$dir" checkout -q --force "$ref" && return 0
        fi
    else
        rm -rf "$dir"; mkdir -p "$dir"; git -C "$dir" init -q; git -C "$dir" remote add origin "$url"
    fi
    if ! git -C "$dir" fetch -q --depth=1 origin "$ref" 2>/dev/null; then
        # kurze Commit-IDs lassen sich nicht direkt holen: vollen Verlauf (ohne Dateiinhalte) laden
        git -C "$dir" fetch -q --filter=blob:none origin
    fi
    git -C "$dir" checkout -q --force FETCH_HEAD 2>/dev/null || git -C "$dir" checkout -q --force "$ref"
}
