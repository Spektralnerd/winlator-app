#!/usr/bin/env python3
"""Passt den Gladio-Client (github.com/brunodev85/gladio, libGL.so in der Root-FS) an den
geänderten Server in der App an – gemeinsam für den armhf-Bau (build-clients.sh) und das
PC-Testgeschirr (armv7/tools/gladio-pc).

    patch-gladio-client.py QUELLE            # an Ort und Stelle patchen
    patch-gladio-client.py QUELLE ZIEL       # Kopie anlegen und die Kopie patchen
    ... [--align64]                          # Ringpufferkopf wie auf einem 64-Bit-Server
                                             # (32-Bit-Client am PC-Server)
    ... [--fbconfig-defaults]                # unbekannte GLXFBConfig-Attribute mit Vorgabewerten
                                             # beantworten statt mit Fehler (nur PC: Wine 9 bricht
                                             # sonst ab; Wine 10.10 in der App kommt mit dem Fehler
                                             # zurecht, dort bleibt das Original)

Änderungen:
- "long"-Felder des Protokolls fest 8 Byte breit (der Server in der App erwartet das)
- Socket-Pfad über die Umgebungsvariable GLADIO_SOCKET wählbar (Vorgabe: fester Pfad)
- glXGetProcAddressARB liefert auch GL-Funktionen dieser Bibliothek (Wine und viele
  Programme holen Erweiterungsfunktionen darüber; im Original gab es nur glX-Funktionen)
- Visuals in der Tiefe des Standard-Visuals statt fest 32 (Wine lässt nur Pixelformate zu,
  deren Visual die Tiefe des Standard-Visuals hat; Winlators X-Server: 32, Xvfb: 24)
- glLightModelfv/glLightModeliv werden übertragen statt nur "unimplemented" zu melden
  (Worms 3D ruft sie in jedem Bild auf)
- glTexParameteriv(GL_TEXTURE_SWIZZLE_RGBA) überträgt alle vier Werte (wined3d: L8-Texturen
  als GL_R8 mit Swizzle)
- indizierte Zeichenaufrufe mit Client-Arrays des festen Funktionsmodells schicken den
  Vertexbereich einmal statt je Index einen Vertex (Kennung GLADIO_DRAW_RANGED im Modus; der
  Server der armv7-App versteht beides, siehe gl_context.c)
"""
import pathlib
import shutil
import sys

args = [a for a in sys.argv[1:] if not a.startswith("--")]
flags = [a for a in sys.argv[1:] if a.startswith("--")]
if not args or len(args) > 2 or any(f not in ("--align64", "--fbconfig-defaults") for f in flags):
    raise SystemExit(__doc__)
src = pathlib.Path(args[0])
dst = pathlib.Path(args[1]) if len(args) == 2 else src
align64 = "--align64" in flags
fbconfigDefaults = "--fbconfig-defaults" in flags

if dst != src:
    if dst.exists():
        shutil.rmtree(dst)
    shutil.copytree(src, dst, ignore=shutil.ignore_patterns(".git"))


def sub(rel, old, new, count=1):
    p = dst / rel
    s = p.read_text()
    n = s.count(old)
    if n != count:
        raise SystemExit(f"{rel}: erwartet {count}x, gefunden {n}x: {old!r}")
    p.write_text(s.replace(old, new))


# ---- Protokoll: long -> int64_t ----
sub("include/arrays.h", "extern void ArrayBuffer_putLong(ArrayBuffer* arrayBuffer, long value);",
    "#include <stdint.h>\nextern void ArrayBuffer_putLong(ArrayBuffer* arrayBuffer, int64_t value);")
sub("include/arrays.h", "extern long ArrayBuffer_getLong(ArrayBuffer* arrayBuffer);",
    "extern int64_t ArrayBuffer_getLong(ArrayBuffer* arrayBuffer);")
sub("src/arrays.c", "void ArrayBuffer_putLong(ArrayBuffer* arrayBuffer, long value) {\n    ARRAY_BUFFER_PUT_TYPE(arrayBuffer, long, value);",
    "void ArrayBuffer_putLong(ArrayBuffer* arrayBuffer, int64_t value) {\n    ARRAY_BUFFER_PUT_TYPE(arrayBuffer, int64_t, value);")
sub("src/arrays.c", "long ArrayBuffer_getLong(ArrayBuffer* arrayBuffer) {\n    ARRAY_BUFFER_GET_TYPE(arrayBuffer, long);",
    "int64_t ArrayBuffer_getLong(ArrayBuffer* arrayBuffer) {\n    ARRAY_BUFFER_GET_TYPE(arrayBuffer, int64_t);")
sub("src/gl_calls.c", "ArrayBuffer_putLong(&outputBuffer, (long)sync);", "ArrayBuffer_putLong(&outputBuffer, (int64_t)(intptr_t)sync);", 2)
sub("src/gl_calls.c", "GLsync sync = (GLsync)ArrayBuffer_getLong(&inputBuffer);", "GLsync sync = (GLsync)(intptr_t)ArrayBuffer_getLong(&inputBuffer);")
sub("src/gl_calls.c", "gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_DELETE_SYNC, &sync, sizeof(long));",
    "int64_t syncValue = (int64_t)(intptr_t)sync;\n    gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_DELETE_SYNC, &syncValue, sizeof(int64_t));")
sub("src/gl_calls.c", "GL_SEND_CHECKED(REQUEST_CODE_GL_IS_SYNC, &sync, sizeof(long), GL_RETURN);",
    "int64_t syncValue = (int64_t)(intptr_t)sync;\n    GL_SEND_CHECKED(REQUEST_CODE_GL_IS_SYNC, &syncValue, sizeof(int64_t), GL_RETURN);")

# ---- glTexParameteriv(GL_TEXTURE_SWIZZLE_RGBA): vier Werte statt einem übertragen ----
sub("include/gladio.h", "        case GL_POSITION:\n        case GL_PATCH_DEFAULT_OUTER_LEVEL:\n            return 4;",
    "        case GL_POSITION:\n        case GL_PATCH_DEFAULT_OUTER_LEVEL:\n        case GL_TEXTURE_SWIZZLE_RGBA:\n            return 4;")

# ---- Socket-Pfad ----
sub("src/main.c", "strncpy(server_addr.sun_path, X11_SERVER_PATH,",
    'const char* gladioSock = getenv("GLADIO_SOCKET"); strncpy(server_addr.sun_path, gladioSock ? gladioSock : X11_SERVER_PATH,')

# ---- glXGetProcAddressARB: GL-Funktionen aus dieser Bibliothek ----
sub("src/glx_calls.c", "    else if (strcmp(name, \"glXWaitX\") == 0) return (__GLXextFuncPtr)glXWaitX;\n    return NULL;\n}",
    "    else if (strcmp(name, \"glXWaitX\") == 0) return (__GLXextFuncPtr)glXWaitX;\n"
    "    /* GL-Funktionen aus dieser Bibliothek selbst */\n"
    "    static void* self = NULL;\n"
    "    if (!self) {\n"
    "        Dl_info info;\n"
    "        if (dladdr((void*)glXGetProcAddressARB, &info) && info.dli_fname) self = dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD);\n"
    "        if (!self) self = RTLD_DEFAULT;\n"
    "    }\n"
    "    return (__GLXextFuncPtr)dlsym(self, name);\n}")
sub("src/glx_calls.c", "#include \"gladio.h\"\n", "#define _GNU_SOURCE\n#include <dlfcn.h>\n#include \"gladio.h\"\n")

# ---- Visuals in der Tiefe des Standard-Visuals ----
sub("src/glx_calls.c", "    visualInfo.depth = 32;\n    visualInfo.class = TrueColor;",
    "    visualInfo.depth = DefaultDepth(dpy, DefaultScreen(dpy));\n    visualInfo.class = TrueColor;", 2)

# ---- glLightModelfv / glLightModeliv ----
sub("src/gl_calls.c",
    "void glLightModelfv(GLenum pname, const GLfloat* params) {\n    println(MSG_DEBUG_UNIMPLEMENTED_GLCALL, \"glLightModelfv\");\n}",
    "void glLightModelfv(GLenum pname, const GLfloat* params) {\n"
    "    GL_CALL_LOCK();\n"
    "    ArrayBuffer_rewind(&outputBuffer);\n"
    "    ArrayBuffer_putInt(&outputBuffer, pname);\n"
    "    GLfloat values[4] = {0.0f, 0.0f, 0.0f, 0.0f};\n"
    "    memcpy(values, params, (pname == GL_LIGHT_MODEL_AMBIENT ? 4 : 1) * sizeof(GLfloat));\n"
    "    ArrayBuffer_putBytes(&outputBuffer, values, sizeof(values));\n"
    "    gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_LIGHT_MODELFV, outputBuffer.buffer, outputBuffer.size);\n"
    "    GL_CALL_UNLOCK();\n}")
sub("src/gl_calls.c",
    "void glLightModeliv(GLenum pname, const GLint* params) {\n    println(MSG_DEBUG_UNIMPLEMENTED_GLCALL, \"glLightModeliv\");\n}",
    "void glLightModeliv(GLenum pname, const GLint* params) {\n"
    "    GLfloat values[4] = {0.0f, 0.0f, 0.0f, 0.0f};\n"
    "    if (pname == GL_LIGHT_MODEL_AMBIENT) {\n"
    "        for (int p = 0; p < 4; p++) values[p] = (GLfloat)((double)params[p] / (double)INT32_MAX);\n"
    "    }\n"
    "    else values[0] = (GLfloat)params[0];\n"
    "    glLightModelfv(pname, values);\n}")

# ---- indizierte Zeichenaufrufe: Vertexbereich einmal statt je Index (GLADIO_DRAW_RANGED) ----
sub("src/main.c",
    "void writeUnboundVertexArrays(GLint first, GLsizei count, const void* indices, GLenum indexType, GLint basevertex) {",
    "/* Alle eingeschalteten Arrays sind Client-Arrays des festen Funktionsmodells (ohne Programm)? */\n"
    "static bool onlyLegacyClientArrays(GLClientState* clientState) {\n"
    "    bool any = false;\n"
    "    for (int i = 0; i < clientState->vao->maxEnabledAttribs; i++) {\n"
    "        GLVertexAttrib* attrib = &clientState->vao->attribs[i];\n"
    "        if (!attrib->state) continue;\n"
    "        if (attrib->state != VERTEX_ATTRIB_LEGACY_ENABLED || !attrib->pointer || GLClientState_isLegacyEnabledWithProgram(clientState, i)) return false;\n"
    "        any = true;\n"
    "    }\n"
    "    return any;\n"
    "}\n"
    "\n"
    "void writeUnboundVertexArrays(GLint first, GLsizei count, const void* indices, GLenum indexType, GLint basevertex) {")
sub("src/main.c",
    "        else ArrayBuffer_putBytes(&outputBuffer, indices, count * sizeofGLType(indexType));\n\n        int indexCount = 0;",
    "        else ArrayBuffer_putBytes(&outputBuffer, indices, count * sizeofGLType(indexType));\n"
    "\n"
    "        /* Server der armv7-App: Vertexbereich [min, max] einmal senden statt je Index einen Vertex.\n"
    "         * Kennung 0x40000000 (GLADIO_DRAW_RANGED) im Modus = erstes int der Anfrage; dahinter\n"
    "         * Indexversatz (basevertex - min) und Vertexzahl, dann die Arrays in Attributreihenfolge. */\n"
    "        if (!elementArrayBuffer && count > 0 && onlyLegacyClientArrays(clientState)) {\n"
    "            GLuint range[2];\n"
    "            getRangeIndices(indices, count, indexType, basevertex, range);\n"
    "            int vertexCount = (int)(range[1] - range[0]) + 1;\n"
    "            *(int*)outputBuffer.buffer |= 0x40000000;\n"
    "            ArrayBuffer_putInt(&outputBuffer, basevertex - (int)range[0]);\n"
    "            ArrayBuffer_putInt(&outputBuffer, vertexCount);\n"
    "            for (int i = 0; i < clientState->vao->maxEnabledAttribs; i++) {\n"
    "                GLVertexAttrib* attrib = &clientState->vao->attribs[i];\n"
    "                if (attrib->state != VERTEX_ATTRIB_LEGACY_ENABLED) continue;\n"
    "                ArrayBuffer_putBytes(&outputBuffer, (char*)attrib->pointer + (size_t)range[0] * attrib->stride, vertexCount * attrib->stride);\n"
    "            }\n"
    "            return;\n"
    "        }\n"
    "\n"
    "        int indexCount = 0;")

if fbconfigDefaults:
    sub("src/glx_calls.c",
        "    *value = 0;\n    return 1;\n}",
        "    /* Vorgabewerte für Attribute, die der Server nicht mitschickt (GLX-Attributnummern) */\n"
        "    switch (attribute) {\n"
        "        case 1: case 4: *value = 1; break;                       /* GLX_USE_GL, GLX_RGBA */\n"
        "        case 0x8012: *value = 1; break;                           /* GLX_X_RENDERABLE */\n"
        "        case 0x22: *value = 0x8002; break;                        /* GLX_X_VISUAL_TYPE = GLX_TRUE_COLOR */\n"
        "        case 0x20: case 0x23: *value = 0x8000; break;             /* GLX_CONFIG_CAVEAT, GLX_TRANSPARENT_TYPE = GLX_NONE */\n"
        "        case 0x8060: *value = 0x8063; break;                      /* GLX_SWAP_METHOD_OML = GLX_SWAP_UNDEFINED_OML */\n"
        "        default: *value = 0; break;\n"
        "    }\n"
        "    return 0;\n}")

if align64:
    sub("src/ring_buffer.c", "        uint32_t status; \\\n        void* buffer; \\",
        "        uint32_t status; \\\n        void* buffer __attribute__((aligned(8))); \\")
    # Sync-Objekte des 64-Bit-Servers passen nicht in einen 32-Bit-GLsync: Tabelle im Client
    sub("src/gl_calls.c", "pthread_mutex_t gl_call_mutex = PTHREAD_MUTEX_INITIALIZER;\n",
        "pthread_mutex_t gl_call_mutex = PTHREAD_MUTEX_INITIALIZER;\n"
        "\n"
        "/* 32-Bit-Client am 64-Bit-Server: Sync-Objekte sind dort 64-Bit-Zeiger */\n"
        "#define SYNC_TABLE_SIZE 1024\n"
        "static int64_t syncTable[SYNC_TABLE_SIZE];\n"
        "static int64_t syncToServer(GLsync s) { uintptr_t i = (uintptr_t)s; return (i >= 1 && i <= SYNC_TABLE_SIZE) ? syncTable[i - 1] : 0; }\n"
        "static GLsync syncFromServer(int64_t v) {\n"
        "    if (!v) return NULL;\n"
        "    for (int i = 0; i < SYNC_TABLE_SIZE; i++) if (!syncTable[i]) { syncTable[i] = v; return (GLsync)(uintptr_t)(i + 1); }\n"
        "    return NULL;\n"
        "}\n"
        "static void syncRelease(GLsync s) { uintptr_t i = (uintptr_t)s; if (i >= 1 && i <= SYNC_TABLE_SIZE) syncTable[i - 1] = 0; }\n")
    sub("src/gl_calls.c", "GLsync sync = (GLsync)(intptr_t)ArrayBuffer_getLong(&inputBuffer);",
        "GLsync sync = syncFromServer(ArrayBuffer_getLong(&inputBuffer));")
    sub("src/gl_calls.c", "ArrayBuffer_putLong(&outputBuffer, (int64_t)(intptr_t)sync);", "ArrayBuffer_putLong(&outputBuffer, syncToServer(sync));", 2)
    sub("src/gl_calls.c", "int64_t syncValue = (int64_t)(intptr_t)sync;", "int64_t syncValue = syncToServer(sync);", 2)
    sub("src/gl_calls.c", "gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_DELETE_SYNC, &syncValue, sizeof(int64_t));",
        "gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_DELETE_SYNC, &syncValue, sizeof(int64_t));\n    syncRelease(sync);")

print(f"Gladio-Client in {dst} gepatcht{' (Ringpuffer 64-Bit-Ausrichtung)' if align64 else ''}{' (FBConfig-Vorgabewerte)' if fbconfigDefaults else ''}")
