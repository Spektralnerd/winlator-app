#!/usr/bin/env bash
# glibc 2.39 für ARMv7 (armhf) mit den Android-Anpassungen von termux-pacman und dem
# SysV-Shared-Memory-Ersatz von Winlator, Präfix /data/data/com.winlator/files/rootfs.
# Entspricht dem Aufbau der aarch64-glibc im originalen Winlator-Root-FS:
#   ld.so/libc in rootfs/lib (-> usr/lib), Konfiguration in rootfs/etc, Daten in rootfs/usr/share
#
# Ergebnis: $STAGE_DIR/glibc  (Baum ab ROOT_PREFIX)
set -euo pipefail
. "$(dirname "$0")/env.sh"

SRC="$SRC_DIR/glibc-$GLIBC_VERSION"
TERMUX="$SRC_DIR/termux-glibc"
WINLATOR="$SRC_DIR/winlator"
BUILD="$BUILD_DIR/glibc"
DEST="$STAGE_DIR/glibc"

log "Quellen holen"
git_fetch "$GLIBC_REPO" "$GLIBC_TAG" "$SRC"
git_fetch "$TERMUX_GLIBC_REPO" "$TERMUX_GLIBC_COMMIT" "$TERMUX"
git_fetch "$WINLATOR_REPO" "$WINLATOR_COMMIT" "$WINLATOR"
PKG="$TERMUX/gpkg/glibc"

log "Termux-Patches anwenden"
cd "$SRC"
git checkout -q --force . && git clean -qfdx
for patch in $(cd "$PKG" && LC_ALL=C ls *.patch); do
    sed -e "s%@TERMUX_PREFIX@%$ROOT_PREFIX%g" \
        -e "s%@TERMUX_PREFIX_CLASSICAL@%$ROOT_PREFIX%g" "$PKG/$patch" | patch -s -p1 --no-backup-if-mismatch \
        || { echo "Patch $patch passt nicht"; exit 1; }
done

# eigene Korrekturen (armv7/patches/glibc/*.patch, u. a. aus dem 2.39-Wartungszweig)
for patch in "$ARMV7_DIR"/patches/glibc/*.patch; do
    [ -f "$patch" ] || continue
    log "Patch $(basename "$patch")"
    patch -s -p1 --no-backup-if-mismatch < "$patch" || { echo "Patch $(basename "$patch") passt nicht"; exit 1; }
done

# termux_step_pre_configure aus build.sh (Stand 2.39, Revision 4)
for i in shmem-android.h shmat.c shmctl.c shmdt.c shmget.c mprotect.c syscall.c fake-syscall.h; do
    install -Dm644 "$PKG/$i" "sysdeps/unix/sysv/linux/$i"
done
rm -f sysdeps/unix/sysv/linux/*/clone3.S
rm -f sysdeps/unix/sysv/linux/x86_64/configure*
mv sysdeps/unix/sysv/linux/arm/syscall.S sysdeps/unix/sysv/linux/arm/syscallS.S
for i in android_passwd_group.h android_passwd_group.c android_system_user_ids.h; do
    cp "$PKG/$i" nss/
done
bash "$PKG/gen-android-ids.sh" "$ROOT_PREFIX" nss/android_ids.h "$PKG/android_system_user_ids.h"
for i in aarch64 arm i386 x86_64/64; do
    {
        for j in $(awk '{printf "__NR_" $1 " "}' "$PKG/disabled-syscalls"); do
            grep "#define ${j} " "sysdeps/unix/sysv/linux/${i}/arch-syscall.h" || true
            sed -i "/#define ${j} /d" "sysdeps/unix/sysv/linux/${i}/arch-syscall.h"
        done
    } >> "sysdeps/unix/sysv/linux/${i}/disabled-syscall.h"
done

log "armv7: von Android gesperrte Systemaufrufe (seccomp, Android 8/9)"
# Android 8 und 9 sperren für 32-Bit-ARM zusätzlich link, lchown32 und pause; ohne diese
# Nummern weicht glibc auf linkat, fchownat und ppoll aus. Was sonst noch gesperrt ist (die
# time64-Aufrufe, SysV-Semaphoren usw.), fängt android_seccomp.c zur Laufzeit ab.
for j in link lchown lchown32 pause; do
    grep "#define __NR_${j} " sysdeps/unix/sysv/linux/arm/arch-syscall.h \
        >> sysdeps/unix/sysv/linux/arm/disabled-syscall.h || true
    sed -i "/#define __NR_${j} /d" sysdeps/unix/sysv/linux/arm/arch-syscall.h
done
install -m644 "$ARMV7_DIR/patches/glibc/android_seccomp.c" sysdeps/unix/sysv/linux/arm/android_seccomp.c
cat >> sysdeps/unix/sysv/linux/arm/Makefile <<'MAKE'

ifeq ($(subdir),misc)
sysdep_routines += android_seccomp
endif
MAKE

log "Winlator-Patch: SysV-Shared-Memory über den Winlator-Server (ANDROID_SYSVSHM_SERVER)"
cp "$WINLATOR"/glibc_patches/sysdeps/unix/sysv/linux/*.[ch] sysdeps/unix/sysv/linux/
# android_sysvshm.c ist eine eigene Übersetzungseinheit und muss in libc landen
cat >> sysdeps/unix/sysv/linux/Makefile <<'MAKE'

ifeq ($(subdir),sysvipc)
sysdep_routines += android_sysvshm
endif
MAKE

log "Konfigurieren"
rm -rf "$BUILD" && mkdir -p "$BUILD" && cd "$BUILD"
# nur die Kernel-Header (ohne die glibc-Header des Cross-Sysroots)
KH="$BUILD/kernel-headers"; mkdir -p "$KH"
for base in "${KERNEL_HEADERS:-}" /usr/$TARGET/include /usr/include/$TARGET /usr/include; do
    [ -n "$base" ] && [ -d "$base" ] || continue
    for d in linux asm asm-generic mtd rdma scsi sound video xen drm misc; do
        [ -d "$base/$d" ] && [ ! -e "$KH/$d" ] && cp -a "$base/$d" "$KH/$d"
    done
done
[ -f "$KH/linux/version.h" ] && [ -f "$KH/asm/unistd.h" ] || { echo "Kernel-Header für $TARGET fehlen"; exit 1; }
cat > configparms <<CONF
slibdir=$ROOT_PREFIX/lib
rtlddir=$ROOT_PREFIX/lib
sbindir=$ROOT_PREFIX/usr/bin
rootsbindir=$ROOT_PREFIX/usr/bin
CONF
# Ubuntu 24.04 stellt armhf-Compiler auf 64-Bit-time_t vor (_TIME_BITS=64); glibc selbst
# darf so nicht gebaut werden
NO_T64=""
# (Ausgabe erst einsammeln: grep -q würde gcc per SIGPIPE abbrechen und mit pipefail scheitern)
GCC_DEFINES="$($TARGET-gcc -dM -E - </dev/null)"
case "$GCC_DEFINES" in *"_TIME_BITS 64"*) NO_T64="-U_TIME_BITS -U_FILE_OFFSET_BITS";; esac
# Ubuntu-GCC schaltet außerdem _FORTIFY_SOURCE=3 ein, sobald optimiert wird (glibc nimmt es nur per -Wp zurück)
NO_T64="$NO_T64 -U_FORTIFY_SOURCE"
CC="$TARGET-gcc $NO_T64" CXX="$TARGET-g++ $NO_T64" AR="$TARGET-ar" RANLIB="$TARGET-ranlib" \
CFLAGS="-O2 -g0 -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard -mthumb" \
CXXFLAGS="-O2 -g0" \
"$SRC/configure" \
    --prefix="$ROOT_PREFIX/usr" \
    --libdir="$ROOT_PREFIX/lib" \
    --libexecdir="$ROOT_PREFIX/lib" \
    --sysconfdir="$ROOT_PREFIX/etc" \
    --localstatedir="$ROOT_PREFIX/var" \
    --host=$TARGET --build="$(gcc -dumpmachine)" \
    --with-headers="$KH" \
    --with-bugurl=https://github.com/Spektralnerd/winlator-app/issues \
    --enable-bind-now \
    --disable-multi-arch \
    --enable-stack-protector=strong \
    --disable-nscd \
    --disable-profile \
    --disable-werror \
    --disable-default-pie \
    --disable-crypt
# (--enable-fortify-source wie bei Termux scheitert mit GCC 13 an misc/syslog.c)

log "Bauen ($JOBS Jobs)"
make -j"$JOBS" -O

log "Installieren"
rm -rf "$DEST" && mkdir -p "$DEST"
make install_root="$DEST" install -j"$JOBS" >/dev/null
# Baum liegt jetzt unter $DEST$ROOT_PREFIX; flach auf ROOT_PREFIX umhängen
mv "$DEST$ROOT_PREFIX" "$DEST.tmp" && rm -rf "$DEST" && mv "$DEST.tmp" "$DEST"
cd "$DEST"
rm -rf usr/include/gnu etc/ld.so.cache usr/bin/{tzselect,zdump,zic} usr/share/info
install -m644 "$SRC/posix/gai.conf" etc/gai.conf
# lib als echter Ordner wie im aarch64-Root-FS: dort ist lib -> usr/lib
mkdir -p usr/lib
cp -a lib/. usr/lib/ && rm -rf lib && ln -s usr/lib lib
# C.UTF-8 braucht localedef auf dem Zielsystem; die Locale-Dateien sind zwischen
# Little-Endian-Systemen gleich, daher mit dem localedef des Bauhosts erzeugen
if command -v localedef >/dev/null; then
    mkdir -p usr/lib/locale
    I18NPATH="$SRC/localedata" localedef --no-archive --little-endian -c \
        -f "$SRC/localedata/charmaps/UTF-8" -i "$SRC/localedata/locales/C" usr/lib/locale/C.UTF-8 || true
fi
# Entwicklerdateien braucht das Gerät nicht (bleiben für Cross-Builds im Sysroot unten)
mkdir -p "$OUT_DIR"
tar -C "$DEST" -cf "$OUT_DIR/glibc-sysroot.tar" usr/include usr/lib/*.a usr/lib/*.o usr/lib/libc.so usr/lib/libm.so usr/lib/libpthread.so 2>/dev/null || true

log "Prüfen: von Android gesperrte Systemaufrufe"
grep -q "winlator-seccomp" "$DEST/usr/lib/libc.so.6" || { echo "android_seccomp fehlt in libc.so.6"; exit 1; }
# ld.so läuft, bevor libc den SIGSYS-Handler einrichtet: dort darf nichts Gesperrtes vorkommen;
# libc darf link, lchown32 und pause nicht mehr direkt aufrufen (der Rest sind Hinweise)
python3 "$ARMV7_DIR/scripts/check-syscalls.py" --objdump "$TARGET-objdump" --strict "$DEST/usr/lib/ld-linux-armhf.so.3"
python3 "$ARMV7_DIR/scripts/check-syscalls.py" --objdump "$TARGET-objdump" --forbid 9,198,29 "$DEST/usr/lib/libc.so.6"

log "fertig: $DEST"
file -L "$DEST/usr/lib/ld-linux-armhf.so.3" "$DEST/usr/lib/libc.so.6"
