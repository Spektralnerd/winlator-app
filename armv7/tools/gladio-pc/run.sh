#!/bin/bash
# Führt ein OpenGL-Programm über den Gladio-Client gegen den nativ gebauten Gladio-Server aus.
#
#   run.sh [--es 2|3] [--dump DIR] [--frames 1,10|all] [--log DATEI] -- PROGRAMM [ARGS...]
#
#   --es 2      Server mit einem ES-2.0-Kontext (Mesa: MESA_GLES_VERSION_OVERRIDE=2.0,
#               MESA_GLSL_VERSION_OVERRIDE=100), so wie ihn die Mali-400 bietet
#   --es 3      ES 3.2 (Vorgabe; Kontrolllauf wie auf einem modernen Gerät)
#   --dump DIR  fertige Bilder (bei glXSwapBuffers) als PPM ablegen
#
# Braucht einen X-Server unter $DISPLAY für das Fenster des Programms (z. B. Xvfb :0).
# Die GL-Befehle selbst gehen am X-Server vorbei durch den Gladio-Ringpuffer zum Server.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ES=3
DUMP=""
FRAMES="1,10"
LOG=""
while [ $# -gt 0 ]; do
    case "$1" in
        --es) ES=$2; shift 2;;
        --dump) DUMP=$2; shift 2;;
        --frames) FRAMES=$2; shift 2;;
        --log) LOG=$2; shift 2;;
        --) shift; break;;
        *) echo "unbekannte Option $1"; exit 1;;
    esac
done
[ $# -gt 0 ] || { echo "kein Programm angegeben"; exit 1; }
[ -x "$HERE/build/gladio-pc-server" ] || { echo "erst 'make' in $HERE"; exit 1; }

if ! xdpyinfo >/dev/null 2>&1; then
    export DISPLAY=${DISPLAY:-:0}
    echo "starte Xvfb $DISPLAY"
    Xvfb "$DISPLAY" -screen 0 1024x768x24+32 >/dev/null 2>&1 &
    XVFB_PID=$!
    sleep 1
fi

SOCK=${GLADIO_SOCKET:-/tmp/gladio-pc-$$.sock}
[ -n "$LOG" ] || LOG=/tmp/gladio-pc-$$.log
export GLADIO_SOCKET=$SOCK GLADIO_PC_ES=$ES GLADIO_PC_DUMP=$DUMP GLADIO_PC_DUMP_FRAMES=$FRAMES
export EGL_PLATFORM=${EGL_PLATFORM:-x11}
if [ "$ES" = 2 ]; then
    export MESA_GLES_VERSION_OVERRIDE=${MESA_GLES_VERSION_OVERRIDE:-2.0}
    export MESA_GLSL_VERSION_OVERRIDE=${MESA_GLSL_VERSION_OVERRIDE:-100}
fi
"$HERE/build/gladio-pc-server" >"$LOG" 2>&1 &
SERVER_PID=$!
for i in $(seq 1 50); do [ -S "$SOCK" ] && break; sleep 0.1; done
[ -S "$SOCK" ] || { echo "Server nicht gestartet:"; cat "$LOG"; exit 1; }

set +e
LD_LIBRARY_PATH="$HERE/build/client${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" "$@"
RC=$?
set -e
sleep 0.5
kill $SERVER_PID 2>/dev/null || true
[ -n "$XVFB_PID" ] && kill $XVFB_PID 2>/dev/null || true
rm -f "$SOCK"
echo "--- Serverprotokoll ($LOG):"
cat "$LOG"
exit $RC
