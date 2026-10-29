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
- glActiveTexture/glClientActiveTexture gehen erst vor der nächsten anderen Anfrage hinaus und
  nur, wenn sich die Einheit gegenüber der zuletzt gesendeten geändert hat (Worms 3D schaltet vor
  jedem Zeichenaufruf alle acht Einheiten durch: ~8400 von 13000 Anfragen je Bild)
- der Client führt Matrixstapel (wie der Server: ein Texturstapel, höchstens 32 Einträge),
  Matrixmodus und Tiefenmaske mit und beantwortet glGetFloatv/glGetDoublev(GL_*_MATRIX),
  glGetIntegerv(GL_MATRIX_MODE) und glGetBooleanv(GL_DEPTH_WRITEMASK) selbst, statt auf den Server
  zu warten (Worms 3D: 29 Matrixabfragen je Bild); was er nicht nachvollziehen kann, erfragt er
- glGetDoublev schrieb die Antwort in eine einzelne float-Variable (bei Matrizen 16 Werte: Stapel
  überschrieben); jetzt alle Werte in das Feld des Aufrufers
- glXSwapBuffers wartet nur noch auf das vorige Bild, nicht auf das eben übergebene: ein Bild darf
  beim Server in Arbeit sein, während das Programm das nächste vorbereitet (GLADIO_SYNC_SWAP=1:
  wie früher auf jedes warten)
- glMapBuffer/glMapBufferRange und glGetBufferSubData auf einem GL_PIXEL_PACK_BUFFER warten erst auf
  den Server: der schreibt die Pixel aus glReadPixels erst beim Abarbeiten in den geteilten Speicher
  (vorher las das Programm alte Daten, beim ersten Mal Nullen)
- glVertexAttribPointer mit Größe GL_BGRA aus einem gebundenen Puffer (wined3d: D3DCOLOR-Farben ab
  GL 3.2) schickt beim Zeichnen keine Daten mehr – vorher ging der Puffer vom Anfang bis zum letzten
  Eckpunkt mit jedem Zeichenaufruf hinaus (LEGO Racers im ES-3.0-Pfad: über 100 MB je Bild). Der
  Server liest die Farben aus dem geteilten Speicher und stellt nur die gezeichneten Eckpunkte um
  (Kennung GLADIO_VERTEX_ATTRIB_BGRA_BUFFER = 0x80 im normalized-Byte; gl_context.c)
- glUnmapBuffer überträgt den abgebildeten Bereich nur noch, wenn er ohne GL_MAP_FLUSH_EXPLICIT_BIT
  abgebildet war (GL: mit dem Bit gelten nur die ausdrücklich übertragenen Teile). wined3d bildet
  ohne ARB_buffer_storage bei jedem Hochladen den ganzen Streaming-Puffer (512 KB) ab, überträgt den
  benutzten Teil und gibt ihn frei – vorher ging bei jeder Freigabe der ganze Puffer noch einmal
  hinüber (ES-3.0-Pfad, LEGO Racers: ~230 Mal je Bild). Dazu zählt der Versatz von
  glFlushMappedBufferRange wie in GL ab dem Anfang des abgebildeten Bereichs (vorher als Versatz im
  Puffer; das fiel nicht auf, solange die Freigabe ohnehin alles übertrug)
- glFlushMappedBufferRange und glBufferSubData warten nicht mehr auf den Server (Kennung
  GLADIO_NO_REPLY im Ziel): der Server liest die Daten beim Abarbeiten aus dem geteilten Speicher.
  Damit das stimmt, merkt sich der Client je Puffer den Bereich, den der Server noch lesen wird (auch
  BGRA-Farben beim Zeichnen und die Quelle von glCopyBufferSubData), und wartet, bevor er mit
  glBufferData/glBufferSubData dort hineinschreibt; glMapBufferRange/glMapBuffer zum Schreiben ohne
  GL_MAP_UNSYNCHRONIZED_BIT (oder mit GL_MAP_INVALIDATE_BUFFER_BIT) wartet immer – wie GL selbst.
  wined3d lädt je Zeichenaufruf Eckpunkte und Indizes hoch: vorher zwei Rundreisen je Aufruf (ES 3:
  abbilden und übertragen, ES 2: glBufferSubData), mit CSMT jedes Mal mit dem Anwendungsthread im
  Schlepptau. glCopyBufferSubData: der Server führt den geteilten Speicher des Ziels nach, der
  Client wartet vor dem Lesen daraus (glGetBufferSubData, Abbilden zum Lesen)
- Pufferziele GL_COPY_READ_BUFFER und GL_COPY_WRITE_BUFFER (MAX_BUFFER_TARGETS 8): vorher schrieb
  GLBuffer_setBound für sie (indexOfGLTarget: 255) hinter das Feld vao->buffer; unbekannte Ziele
  werden jetzt übergangen
- Wine im WoW64-Betrieb (64-Bit-Variante, 32-Bit-Programm), wenn der geteilte Speicher über 4 GB liegt (Box64
  legt ihn mit BOX64_MMAP32=1, der Vorgabe, unter 2 GB, solange dort Platz ist): Wine bildet über eine eigene
  Kopie ab, schreibt deren Adresse in die ersten vier Byte des Bereichs und kopiert erst in glUnmapBuffer
  zurück. Deshalb
  * gehen ausdrückliche Übertragungen (glFlushMappedBufferRange ohne GL_MAP_PERSISTENT_BIT) erst beim Freigeben
    hinaus – vorher las der Server die alten Daten, sobald die Übertragung ohne Rundreise lief
  * liefert glGetBufferPointerv den Anfang des abgebildeten Bereichs (vorher den Pufferanfang, auch ohne
    Abbildung), GL_BUFFER_ACCESS_FLAGS nach glMapBuffer Bits und GL_BUFFER_MAP_LENGTH die Pufferlänge (Wine
    kopierte nach glMapBuffer nichts zurück)
  * wartet der 64-Bit-Client vor dem Abbilden, bis der Server die ersten vier Byte des Bereichs nicht mehr
    lesen muss, und stellt sie nach einer Abbildung nur zum Lesen wieder her (dort kopiert Wine nicht zurück)
- BGRA-Farben aus einem Puffer: vorgemerkt wird nur der Bereich der gezeichneten Eckpunkte und der Indizes
  (vorher der ganze Puffer – das hätte jedes Abbilden des Streaming-Puffers warten lassen). Indizierte Aufrufe
  mit Elementpuffer lesen die Indizes für persistente Puffer jetzt aus dessen geteiltem Speicher (vorher an der
  Adresse "Versatz im Puffer")
- glBufferData mit GL_STREAM_DRAW legt den Speicher mit GL_CLIENT_STORAGE_BIT an: der Server der armv7-App
  zeichnet solche Puffer über ES 2.0 aus einer eigenen Kopie (Mali-400, gl_es2.c)
- glGetQueryObjectuiv(GL_QUERY_RESULT_AVAILABLE) für Verdeckungsabfragen und glClientWaitSync beantwortet der Client
  ohne Rundreise, wo die Reihenfolge es erlaubt: Die GPU arbeitet Abfragen und Fences in der Reihenfolge ab, in der
  sie enden – ist eine fertig, sind es die früheren auch; ist eine nicht fertig, sind es die späteren auch (das gilt
  1 ms). wined3d fragt mit CSMT alle 100 Befehle jede offene Abfrage ab (Outlast: ~2000 Rundreisen je Bild)
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

# ---- weniger Anfragen und Wartezeiten (Worms 3D: 13000 Anfragen je Bild, davon 8400 Einheitenwechsel,
#      29 Matrixabfragen, auf die der Client jeweils wartet) ----
# Textureinheiten: glActiveTexture/glClientActiveTexture erst vor der nächsten anderen Anfrage senden
# und nur, wenn sich der Wert gegenüber dem zuletzt gesendeten geändert hat.
sub("include/gl_client_state.h",
    "    struct {\n        short unpackRowLength;\n        short unpackImageHeight;\n    } pixelStore;\n#endif",
    "    struct {\n        short unpackRowLength;\n        short unpackImageHeight;\n    } pixelStore;\n"
    "\n"
    "    /* patch-gladio-client.py: zuletzt an den Server geschickte Textureinheiten (-1 = unbekannt) und\n"
    "     * die im Client mitgeführten Matrizen und Tiefenmaske (Abfragen ohne Warten auf den Server) */\n"
    "    int8_t sentActiveTexture;\n"
    "    int8_t sentActiveTexCoord;\n"
    "    struct GladioShadow* shadow;\n"
    "#endif")
sub("include/gladio.h",
    "static inline bool gl_send(RingBuffer* ring, short requestCode, void* data, int size) {\n",
    "/* patch-gladio-client.py: verzögerte Einheitenwechsel (Bit 0 glActiveTexture, Bit 1 glClientActiveTexture) */\n"
    "extern _Thread_local unsigned char gladio_unitPending;\n"
    "extern void gladio_flushTextureUnits(RingBuffer* ring);\n"
    "/* patch-gladio-client.py: ausstehende Antworten auf glXSwapBuffers abholen, bis höchstens keep übrig sind */\n"
    "extern bool gladio_drainSwapReplies(int keep);\n"
    "\n"
    "static inline bool gl_send(RingBuffer* ring, short requestCode, void* data, int size) {\n"
    "    if (gladio_unitPending && requestCode != REQUEST_CODE_GL_ACTIVE_TEXTURE && requestCode != REQUEST_CODE_GL_CLIENT_ACTIVE_TEXTURE) {\n"
    "        gladio_flushTextureUnits(ring);\n"
    "    }\n")
sub("src/gl_calls.c",
    "    currentGLContext->clientState->activeTexture = texture - GL_TEXTURE0;\n"
    "    gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_ACTIVE_TEXTURE, &texture, sizeof(GLenum));\n",
    "    currentGLContext->clientState->activeTexture = texture - GL_TEXTURE0;\n"
    "    gladio_unitPending |= 1;\n")
sub("src/gl_calls.c",
    "    currentGLContext->clientState->activeTexCoord = texture - GL_TEXTURE0;\n"
    "    gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_CLIENT_ACTIVE_TEXTURE, &texture, sizeof(GLenum));\n",
    "    currentGLContext->clientState->activeTexCoord = texture - GL_TEXTURE0;\n"
    "    gladio_unitPending |= 2;\n")

# sinf/cosf (glRotatef im Schatten) brauchen libm
sub("CMakeLists.txt", "            src/gl_vao.c)", "            src/gl_vao.c)\ntarget_link_libraries(GL m)")

# Hilfsfunktionen: Einheiten nachsenden, Matrizen wie der Server rechnen (Kopie aus winlator/mat4.h),
# ausstehende Bildantworten abholen
sub("src/gl_calls.c", "pthread_mutex_t gl_call_mutex = PTHREAD_MUTEX_INITIALIZER;\n",
    "pthread_mutex_t gl_call_mutex = PTHREAD_MUTEX_INITIALIZER;\n"
    r"""
/* ---- patch-gladio-client.py: Einheitenwechsel, Matrizen, Tiefenmaske, Bildantworten ---- */
_Thread_local unsigned char gladio_unitPending = 0;

void gladio_flushTextureUnits(RingBuffer* ring) {
    unsigned char pending = gladio_unitPending;
    gladio_unitPending = 0;
    GLClientState* clientState = currentGLContext ? currentGLContext->clientState : NULL;
    if (!clientState || !ring) return;
    if ((pending & 1) && clientState->sentActiveTexture != (int8_t)clientState->activeTexture) {
        GLenum unit = GL_TEXTURE0 + clientState->activeTexture;
        gl_send(ring, REQUEST_CODE_GL_ACTIVE_TEXTURE, &unit, sizeof(GLenum));
        clientState->sentActiveTexture = (int8_t)clientState->activeTexture;
    }
    if ((pending & 2) && clientState->sentActiveTexCoord != (int8_t)clientState->activeTexCoord) {
        GLenum unit = GL_TEXTURE0 + clientState->activeTexCoord;
        gl_send(ring, REQUEST_CODE_GL_CLIENT_ACTIVE_TEXTURE, &unit, sizeof(GLenum));
        clientState->sentActiveTexCoord = (int8_t)clientState->activeTexCoord;
    }
}

bool gladio_drainSwapReplies(int keep) {
    static ArrayBuffer swapReply = {0};
    GLContext* context = currentGLContext;
    if (!context || !context->clientRing) return true;
    while (context->pendingSwaps > keep) {
        if (!gl_recv(context->clientRing, NULL, &swapReply)) return false;
        context->pendingSwaps--;
    }
    return true;
}

/* Matrixstapel wie im Server (gl_renderer.c: höchstens 32 Einträge, ein Texturstapel für alle Einheiten) */
#define SHADOW_STACK_DEPTH 32
typedef struct GladioShadow {
    bool broken;          /* Zustand nicht mehr nachvollziehbar (unbekannter Matrixmodus): alles beim Server erfragen */
    int8_t matrixMode;    /* 0 Modelview, 1 Projektion, 2 Textur */
    int8_t depthMask;     /* -1 unbekannt (nach glPopAttrib) */
    uint8_t depth[3];
    uint32_t known[3];    /* Bit i: Eintrag i des Stapels bekannt */
    float stack[3][SHADOW_STACK_DEPTH][16];
} GladioShadow;

static void shadow_identity(float m[16]) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void shadow_transpose(float matrix[16]) {
    float tmp;
    tmp = matrix[1]; matrix[1] = matrix[4]; matrix[4] = tmp;
    tmp = matrix[2]; matrix[2] = matrix[8]; matrix[8] = tmp;
    tmp = matrix[6]; matrix[6] = matrix[9]; matrix[9] = tmp;
    tmp = matrix[3]; matrix[3] = matrix[12]; matrix[12] = tmp;
    tmp = matrix[7]; matrix[7] = matrix[13]; matrix[13] = tmp;
    tmp = matrix[11]; matrix[11] = matrix[14]; matrix[14] = tmp;
}

static void shadow_multiply(float result[16], const float ma[16], const float mb[16]) {
    float a11 = ma[0], a12 = ma[4], a13 = ma[8], a14 = ma[12];
    float a21 = ma[1], a22 = ma[5], a23 = ma[9], a24 = ma[13];
    float a31 = ma[2], a32 = ma[6], a33 = ma[10], a34 = ma[14];
    float a41 = ma[3], a42 = ma[7], a43 = ma[11], a44 = ma[15];
    float b11 = mb[0], b12 = mb[4], b13 = mb[8], b14 = mb[12];
    float b21 = mb[1], b22 = mb[5], b23 = mb[9], b24 = mb[13];
    float b31 = mb[2], b32 = mb[6], b33 = mb[10], b34 = mb[14];
    float b41 = mb[3], b42 = mb[7], b43 = mb[11], b44 = mb[15];
    result[0] = a11 * b11 + a12 * b21 + a13 * b31 + a14 * b41;
    result[4] = a11 * b12 + a12 * b22 + a13 * b32 + a14 * b42;
    result[8] = a11 * b13 + a12 * b23 + a13 * b33 + a14 * b43;
    result[12] = a11 * b14 + a12 * b24 + a13 * b34 + a14 * b44;
    result[1] = a21 * b11 + a22 * b21 + a23 * b31 + a24 * b41;
    result[5] = a21 * b12 + a22 * b22 + a23 * b32 + a24 * b42;
    result[9] = a21 * b13 + a22 * b23 + a23 * b33 + a24 * b43;
    result[13] = a21 * b14 + a22 * b24 + a23 * b34 + a24 * b44;
    result[2] = a31 * b11 + a32 * b21 + a33 * b31 + a34 * b41;
    result[6] = a31 * b12 + a32 * b22 + a33 * b32 + a34 * b42;
    result[10] = a31 * b13 + a32 * b23 + a33 * b33 + a34 * b43;
    result[14] = a31 * b14 + a32 * b24 + a33 * b34 + a34 * b44;
    result[3] = a41 * b11 + a42 * b21 + a43 * b31 + a44 * b41;
    result[7] = a41 * b12 + a42 * b22 + a43 * b32 + a44 * b42;
    result[11] = a41 * b13 + a42 * b23 + a43 * b33 + a44 * b43;
    result[15] = a41 * b14 + a42 * b24 + a43 * b34 + a44 * b44;
}

static GladioShadow* gladioShadow(void) {
    GLClientState* clientState = currentGLContext ? currentGLContext->clientState : NULL;
    if (!clientState) return NULL;
    GladioShadow* shadow = clientState->shadow;
    if (!shadow) {
        shadow = calloc(1, sizeof(GladioShadow));
        if (!shadow) return NULL;
        shadow->depthMask = 1;
        for (int i = 0; i < 3; i++) {
            shadow->depth[i] = 1;
            shadow->known[i] = 1;
            shadow_identity(shadow->stack[i][0]);
        }
        clientState->shadow = shadow;
    }
    return shadow->broken ? NULL : shadow;
}

/* aktuelle Matrix des aktuellen Stapels; NULL, wenn unbekannt (requireKnown) oder kein Schatten */
static float* shadowTop(bool requireKnown) {
    GladioShadow* shadow = gladioShadow();
    if (!shadow) return NULL;
    if (shadow->matrixMode < 0) {
        shadow->broken = true;
        return NULL;
    }
    int m = shadow->matrixMode, d = shadow->depth[m] - 1;
    if (requireKnown && !(shadow->known[m] & (1u << d))) return NULL;
    return shadow->stack[m][d];
}

static void shadowSetTopKnown(void) {
    GladioShadow* shadow = gladioShadow();
    if (shadow && shadow->matrixMode >= 0) shadow->known[shadow->matrixMode] |= 1u << (shadow->depth[shadow->matrixMode] - 1);
}

static void shadowLoad(const float* m, bool transpose) {
    float* top = shadowTop(false);
    if (!top) return;
    if (m) memcpy(top, m, 16 * sizeof(float));
    else shadow_identity(top);
    if (transpose) shadow_transpose(top);
    shadowSetTopKnown();
}

static void shadowMultiply(const float* m, bool transpose) {
    float* top = shadowTop(true);
    if (!top) return;
    float tmp[16];
    memcpy(tmp, m, sizeof(tmp));
    if (transpose) shadow_transpose(tmp);
    shadow_multiply(top, top, tmp);
}

static void shadowTranslate(float x, float y, float z) {
    float* matrix = shadowTop(true);
    if (!matrix) return;
    matrix[12] = matrix[0] * x + matrix[4] * y + matrix[8] * z + matrix[12];
    matrix[13] = matrix[1] * x + matrix[5] * y + matrix[9] * z + matrix[13];
    matrix[14] = matrix[2] * x + matrix[6] * y + matrix[10] * z + matrix[14];
    matrix[15] = matrix[3] * x + matrix[7] * y + matrix[11] * z + matrix[15];
}

static void shadowScale(float x, float y, float z) {
    float* matrix = shadowTop(true);
    if (!matrix) return;
    for (int i = 0; i < 4; i++) {
        matrix[i] *= x;
        matrix[4 + i] *= y;
        matrix[8 + i] *= z;
    }
}

static void shadowRotate(float angleDegrees, float x, float y, float z) {
    float* matrix = shadowTop(true);
    if (!matrix) return;
    float angle = angleDegrees * (M_PI / 180.0f);
    float len = sqrt(x * x + y * y + z * z), s, c, t;
    if (fabs(len) < 0.000001) return;
    len = 1 / len;
    x *= len;
    y *= len;
    z *= len;
    s = sinf(angle);
    c = cosf(angle);
    t = 1 - c;
    float a00 = matrix[0], a01 = matrix[1], a02 = matrix[2], a03 = matrix[3];
    float a10 = matrix[4], a11 = matrix[5], a12 = matrix[6], a13 = matrix[7];
    float a20 = matrix[8], a21 = matrix[9], a22 = matrix[10], a23 = matrix[11];
    float b00 = x * x * t + c, b01 = y * x * t + z * s, b02 = z * x * t - y * s;
    float b10 = x * y * t - z * s, b11 = y * y * t + c, b12 = z * y * t + x * s;
    float b20 = x * z * t + y * s, b21 = y * z * t - x * s, b22 = z * z * t + c;
    matrix[0] = a00 * b00 + a10 * b01 + a20 * b02;
    matrix[1] = a01 * b00 + a11 * b01 + a21 * b02;
    matrix[2] = a02 * b00 + a12 * b01 + a22 * b02;
    matrix[3] = a03 * b00 + a13 * b01 + a23 * b02;
    matrix[4] = a00 * b10 + a10 * b11 + a20 * b12;
    matrix[5] = a01 * b10 + a11 * b11 + a21 * b12;
    matrix[6] = a02 * b10 + a12 * b11 + a22 * b12;
    matrix[7] = a03 * b10 + a13 * b11 + a23 * b12;
    matrix[8] = a00 * b20 + a10 * b21 + a20 * b22;
    matrix[9] = a01 * b20 + a11 * b21 + a21 * b22;
    matrix[10] = a02 * b20 + a12 * b21 + a22 * b22;
    matrix[11] = a03 * b20 + a13 * b21 + a23 * b22;
}

static void shadowFrustum(float left, float right, float bottom, float top, float vnear, float vfar) {
    float* matrix = shadowTop(true);
    if (!matrix) return;
    float m[16] = {0};
    m[0] = 2.0f * vnear / (right - left);
    m[5] = 2.0f * vnear / (top - bottom);
    m[8] = (right + left) / (right - left);
    m[9] = (top + bottom) / (top - bottom);
    m[10] = - (vfar + vnear) / (vfar - vnear);
    m[14] = - 2.0f * vfar * vnear / (vfar - vnear);
    m[11] = -1;
    shadow_multiply(matrix, matrix, m);
}

static void shadowOrtho(float left, float right, float top, float bottom, float vnear, float vfar) {
    float* matrix = shadowTop(true);
    if (!matrix) return;
    float w = 1.0f / (right - left), h = 1.0f / (top - bottom), p = 1.0f / (vfar - vnear);
    float m[16] = {0};
    m[0] = 2 * w;
    m[12] = -((right + left) * w);
    m[5] = 2 * h;
    m[13] = -((top + bottom) * h);
    m[10] = -2 * p;
    m[14] = -((vfar + vnear) * p);
    m[15] = 1;
    shadow_multiply(matrix, matrix, m);
}

static void shadowPush(void) {
    GladioShadow* shadow = gladioShadow();
    if (!shadow) return;
    if (shadow->matrixMode < 0) {
        shadow->broken = true;
        return;
    }
    int m = shadow->matrixMode, d = shadow->depth[m];
    if (d >= SHADOW_STACK_DEPTH) return;
    memcpy(shadow->stack[m][d], shadow->stack[m][d - 1], 16 * sizeof(float));
    if (shadow->known[m] & (1u << (d - 1))) shadow->known[m] |= 1u << d;
    else shadow->known[m] &= ~(1u << d);
    shadow->depth[m] = d + 1;
}

static void shadowPop(void) {
    GladioShadow* shadow = gladioShadow();
    if (!shadow) return;
    if (shadow->matrixMode < 0) {
        shadow->broken = true;
        return;
    }
    if (shadow->depth[shadow->matrixMode] > 1) shadow->depth[shadow->matrixMode]--;
}

static int shadowMatrixIndex(GLenum pname) {
    switch (pname) {
        case GL_MODELVIEW_MATRIX: return 0;
        case GL_PROJECTION_MATRIX: return 1;
        case GL_TEXTURE_MATRIX: return 2;
        default: return -1;
    }
}

/* Matrixabfrage aus dem Schatten beantworten (16 Werte); false: beim Server fragen */
static bool shadowGetMatrix(GLenum pname, float* data) {
    int m = shadowMatrixIndex(pname);
    GladioShadow* shadow = m >= 0 ? gladioShadow() : NULL;
    if (!shadow) return false;
    int d = shadow->depth[m] - 1;
    if (!(shadow->known[m] & (1u << d))) return false;
    memcpy(data, shadow->stack[m][d], 16 * sizeof(float));
    return true;
}

/* Antwort des Servers auf eine Matrixabfrage merken */
static void shadowLearnMatrix(GLenum pname, const void* data, int size) {
    int m = shadowMatrixIndex(pname);
    GladioShadow* shadow = m >= 0 && size >= (int)(16 * sizeof(float)) ? gladioShadow() : NULL;
    if (!shadow) return;
    int d = shadow->depth[m] - 1;
    memcpy(shadow->stack[m][d], data, 16 * sizeof(float));
    shadow->known[m] |= 1u << d;
}
""")

# Matrixoperationen und Zustände im Schatten nachführen (nach dem Senden, wie der Server in Reihenfolge)
sub("src/gl_calls.c",
    "    gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_MATRIX_MODE, &mode, sizeof(GLenum));\n",
    "    gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_MATRIX_MODE, &mode, sizeof(GLenum));\n"
    "    GladioShadow* shadow = gladioShadow();\n"
    "    if (shadow) shadow->matrixMode = mode >= GL_MODELVIEW && mode <= GL_TEXTURE ? (int8_t)(mode - GL_MODELVIEW) : -1;\n")
sub("src/gl_calls.c",
    "    gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_LOAD_IDENTITY, NULL, 0);\n",
    "    gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_LOAD_IDENTITY, NULL, 0);\n"
    "    shadowLoad(NULL, false);\n")
sub("src/gl_calls.c",
    "    gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_LOAD_MATRIXF, m, 16 * sizeof(GLfloat));\n",
    "    gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_LOAD_MATRIXF, m, 16 * sizeof(GLfloat));\n"
    "    shadowLoad(m, false);\n")
sub("src/gl_calls.c",
    "    gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_LOAD_TRANSPOSE_MATRIXF, m, 16 * sizeof(GLfloat));\n",
    "    gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_LOAD_TRANSPOSE_MATRIXF, m, 16 * sizeof(GLfloat));\n"
    "    shadowLoad(m, true);\n")
sub("src/gl_calls.c",
    "    gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_MULT_MATRIXF, m, 16 * sizeof(GLfloat));\n",
    "    gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_MULT_MATRIXF, m, 16 * sizeof(GLfloat));\n"
    "    shadowMultiply(m, false);\n")
sub("src/gl_calls.c",
    "    gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_MULT_TRANSPOSE_MATRIXF, m, 16 * sizeof(GLfloat));\n",
    "    gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_MULT_TRANSPOSE_MATRIXF, m, 16 * sizeof(GLfloat));\n"
    "    shadowMultiply(m, true);\n")
sub("src/gl_calls.c",
    "    gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_TRANSLATEF, outputBuffer.buffer, outputBuffer.size);\n",
    "    gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_TRANSLATEF, outputBuffer.buffer, outputBuffer.size);\n"
    "    shadowTranslate(x, y, z);\n")
sub("src/gl_calls.c",
    "    gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_SCALEF, outputBuffer.buffer, outputBuffer.size);\n",
    "    gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_SCALEF, outputBuffer.buffer, outputBuffer.size);\n"
    "    shadowScale(x, y, z);\n")
sub("src/gl_calls.c",
    "    gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_ROTATEF, outputBuffer.buffer, outputBuffer.size);\n",
    "    gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_ROTATEF, outputBuffer.buffer, outputBuffer.size);\n"
    "    shadowRotate(angle, x, y, z);\n")
sub("src/gl_calls.c",
    "    gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_FRUSTUM, outputBuffer.buffer, outputBuffer.size);\n",
    "    gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_FRUSTUM, outputBuffer.buffer, outputBuffer.size);\n"
    "    shadowFrustum(left, right, bottom, top, zNear, zFar);\n")
sub("src/gl_calls.c",
    "    gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_ORTHO, outputBuffer.buffer, outputBuffer.size);\n",
    "    gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_ORTHO, outputBuffer.buffer, outputBuffer.size);\n"
    "    shadowOrtho(left, right, top, bottom, zNear, zFar);\n")
sub("src/gl_calls.c",
    "    gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_PUSH_MATRIX, NULL, 0);\n",
    "    gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_PUSH_MATRIX, NULL, 0);\n"
    "    shadowPush();\n")
sub("src/gl_calls.c",
    "    gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_POP_MATRIX, NULL, 0);\n",
    "    gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_POP_MATRIX, NULL, 0);\n"
    "    shadowPop();\n")
sub("src/gl_calls.c",
    "    gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_DEPTH_MASK, &flag, sizeof(GLboolean));\n",
    "    gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_DEPTH_MASK, &flag, sizeof(GLboolean));\n"
    "    GladioShadow* shadow = gladioShadow();\n"
    "    if (shadow) shadow->depthMask = flag ? 1 : 0;\n")
# glPopAttrib stellt Tiefenmaske und aktive Einheit des Servers wieder her: beides gilt danach als unbekannt
sub("src/gl_calls.c",
    "    gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_POP_ATTRIB, NULL, 0);\n",
    "    gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_POP_ATTRIB, NULL, 0);\n"
    "    GladioShadow* shadow = gladioShadow();\n"
    "    if (shadow) shadow->depthMask = -1;\n"
    "    currentGLContext->clientState->sentActiveTexture = -1;\n")
sub("src/gl_calls.c",
    "    gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_POP_CLIENT_ATTRIB, NULL, 0);\n",
    "    gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_POP_CLIENT_ATTRIB, NULL, 0);\n"
    "    currentGLContext->clientState->sentActiveTexCoord = -1;\n")

# Abfragen aus dem Schatten beantworten, Antworten des Servers merken
sub("src/gl_calls.c",
    "void glGetFloatv(GLenum pname, GLfloat* data) {\n"
    "    GL_CALL_LOCK();\n"
    "    GL_SEND_CHECKED(REQUEST_CODE_GL_GET_FLOATV, &pname, sizeof(GLenum));\n"
    "    GL_RECV_CHECKED();\n"
    "    memcpy(data, inputBuffer.buffer, inputBuffer.size);\n",
    "void glGetFloatv(GLenum pname, GLfloat* data) {\n"
    "    GL_CALL_LOCK();\n"
    "    if (shadowGetMatrix(pname, data)) {\n"
    "        GL_CALL_UNLOCK();\n"
    "        return;\n"
    "    }\n"
    "    GL_SEND_CHECKED(REQUEST_CODE_GL_GET_FLOATV, &pname, sizeof(GLenum));\n"
    "    GL_RECV_CHECKED();\n"
    "    memcpy(data, inputBuffer.buffer, inputBuffer.size);\n"
    "    shadowLearnMatrix(pname, inputBuffer.buffer, inputBuffer.size);\n")
sub("src/gl_calls.c",
    "void glGetIntegerv(GLenum pname, GLint* data) {\n"
    "    GL_CALL_LOCK();\n"
    "    GL_SEND_CHECKED(REQUEST_CODE_GL_GET_INTEGERV, &pname, sizeof(GLenum));\n",
    "void glGetIntegerv(GLenum pname, GLint* data) {\n"
    "    GL_CALL_LOCK();\n"
    "    if (pname == GL_MATRIX_MODE) {\n"
    "        GladioShadow* shadow = gladioShadow();\n"
    "        if (shadow && shadow->matrixMode >= 0) {\n"
    "            data[0] = GL_MODELVIEW + shadow->matrixMode;\n"
    "            GL_CALL_UNLOCK();\n"
    "            return;\n"
    "        }\n"
    "    }\n"
    "    GL_SEND_CHECKED(REQUEST_CODE_GL_GET_INTEGERV, &pname, sizeof(GLenum));\n")
sub("src/gl_calls.c",
    "void glGetBooleanv(GLenum pname, GLboolean* data) {\n"
    "    GL_CALL_LOCK();\n"
    "    GL_SEND_CHECKED(REQUEST_CODE_GL_GET_BOOLEANV, &pname, sizeof(GLenum));\n"
    "    GL_RECV_CHECKED();\n"
    "    memcpy(data, inputBuffer.buffer, inputBuffer.size);\n",
    "void glGetBooleanv(GLenum pname, GLboolean* data) {\n"
    "    GL_CALL_LOCK();\n"
    "    if (pname == GL_DEPTH_WRITEMASK) {\n"
    "        GladioShadow* shadow = gladioShadow();\n"
    "        if (shadow && shadow->depthMask >= 0) {\n"
    "            data[0] = shadow->depthMask ? GL_TRUE : GL_FALSE;\n"
    "            GL_CALL_UNLOCK();\n"
    "            return;\n"
    "        }\n"
    "    }\n"
    "    GL_SEND_CHECKED(REQUEST_CODE_GL_GET_BOOLEANV, &pname, sizeof(GLenum));\n"
    "    GL_RECV_CHECKED();\n"
    "    memcpy(data, inputBuffer.buffer, inputBuffer.size);\n"
    "    if (pname == GL_DEPTH_WRITEMASK && inputBuffer.size >= 1) {\n"
    "        GladioShadow* shadow = gladioShadow();\n"
    "        if (shadow) shadow->depthMask = data[0] ? 1 : 0;\n"
    "    }\n")
# glGetDoublev schrieb die Antwort (z. B. 16 Werte einer Matrix) in eine einzelne float-Variable
sub("src/gl_calls.c",
    "void glGetDoublev(GLenum pname, GLdouble* data) {\n"
    "    GLfloat dataf;\n"
    "    glGetFloatv(pname, &dataf);\n"
    "    *data = dataf;\n"
    "}",
    "void glGetDoublev(GLenum pname, GLdouble* data) {\n"
    "    float matrix[16];\n"
    "    GL_CALL_LOCK();\n"
    "    if (shadowGetMatrix(pname, matrix)) {\n"
    "        for (int i = 0; i < 16; i++) data[i] = matrix[i];\n"
    "        GL_CALL_UNLOCK();\n"
    "        return;\n"
    "    }\n"
    "    GL_SEND_CHECKED(REQUEST_CODE_GL_GET_FLOATV, &pname, sizeof(GLenum));\n"
    "    GL_RECV_CHECKED();\n"
    "    const GLfloat* values = (const GLfloat*)inputBuffer.buffer;\n"
    "    for (int i = 0, count = inputBuffer.size / (int)sizeof(GLfloat); i < count; i++) data[i] = values[i];\n"
    "    shadowLearnMatrix(pname, inputBuffer.buffer, inputBuffer.size);\n"
    "    GL_CALL_UNLOCK();\n"
    "}")

# glXSwapBuffers: nicht auf das eben übergebene Bild warten, nur auf das vorige (ein Bild darf beim
# Server in Arbeit sein, während das Programm das nächste vorbereitet). GLADIO_SYNC_SWAP=1: wie früher.
sub("include/gl_context.h", "    CommandBuffer commandBuffer;\n    uint8_t lockCount;\n} GLContext;",
    "    CommandBuffer commandBuffer;\n    uint8_t lockCount;\n    int pendingSwaps; /* patch-gladio-client.py: Antworten auf glXSwapBuffers, die noch ausstehen */\n} GLContext;")
sub("include/gladio.h",
    "#define GL_RECV_CHECKED(...) \\\n    do { \\\n        if (!gl_recv(currentGLContext->clientRing, NULL, &inputBuffer)) { \\",
    "#define GL_RECV_CHECKED(...) \\\n    do { \\\n        if (!gladio_drainSwapReplies(0) || !gl_recv(currentGLContext->clientRing, NULL, &inputBuffer)) { \\")
sub("src/glx_calls.c",
    "    GL_SEND_CHECKED(REQUEST_CODE_SWAP_DISPLAY_BUFFERS, &drawableId, sizeof(int));\n    GL_RECV_CHECKED();\n    GL_CALL_UNLOCK();",
    "    GL_SEND_CHECKED(REQUEST_CODE_SWAP_DISPLAY_BUFFERS, &drawableId, sizeof(int));\n"
    "    static int maxPendingSwaps = -1;\n"
    "    if (maxPendingSwaps < 0) {\n"
    "        const char* syncSwap = getenv(\"GLADIO_SYNC_SWAP\");\n"
    "        maxPendingSwaps = syncSwap && atoi(syncSwap) ? 0 : 1;\n"
    "    }\n"
    "    currentGLContext->pendingSwaps++;\n"
    "    gladio_drainSwapReplies(maxPendingSwaps);\n"
    "    GL_CALL_UNLOCK();")
# vor dem Ende eines Kontexts: ausstehende Bilder abwarten, verzögerte Einheitenwechsel senden
sub("include/gl_context.h",
    "static inline void destroyGLContextForCallingThread() {\n    if (!currentGLContext) return;\n",
    "static inline void destroyGLContextForCallingThread() {\n    if (!currentGLContext) return;\n    gladio_drainSwapReplies(0);\n")
sub("src/glx_calls.c",
    "void glXDestroyContext(Display* dpy, GLXContext ctx) {\n    GLX_CALL_LOCK();\n",
    "void glXDestroyContext(Display* dpy, GLXContext ctx) {\n    GLX_CALL_LOCK();\n"
    "    if (currentGLContext && currentGLContext->glxContext == ctx) gladio_drainSwapReplies(0);\n"
    "    if (ctx->clientState.shadow) free(ctx->clientState.shadow);\n")
sub("src/glx_calls.c",
    "Bool glXMakeCurrent(Display* dpy, GLXDrawable drawable, GLXContext ctx) {\n    GLX_CALL_LOCK();\n",
    "Bool glXMakeCurrent(Display* dpy, GLXDrawable drawable, GLXContext ctx) {\n    GLX_CALL_LOCK();\n"
    "    /* verzögerte Einheitenwechsel gehören zum bisherigen Kontext */\n"
    "    if (currentGLContext && gladio_unitPending) gladio_flushTextureUnits(currentGLContext->serverRing);\n")

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

# Pack-Puffer (glReadPixels in ein Pufferobjekt) füllt der Server im geteilten Speicher erst, wenn er die
# Anfrage abarbeitet: vor glMapBuffer(Range)/glGetBufferSubData darauf warten, sonst las das Programm alte
# Daten (beim ersten Mal Nullen)
sub("src/gl_calls.c",
    "void glGetBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, void* data) {\n",
    "/* patch-gladio-client.py: vor dem Lesen eines Pack-Puffers auf alle früheren Anfragen warten (erst die\n"
    " * ausstehenden Bildantworten abholen, dann ein glFinish-Rundlauf). Aufruf mit gesperrtem GL_CALL_LOCK. */\n"
    "static void gladio_waitForPixelPackBuffer(GLenum target) {\n"
    "    if (target != GL_PIXEL_PACK_BUFFER || !currentGLContext) return;\n"
    "    if (!gladio_drainSwapReplies(0)) return;\n"
    "    if (gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_FINISH, NULL, 0)) {\n"
    "        gl_recv(currentGLContext->clientRing, NULL, &inputBuffer);\n"
    "    }\n"
    "}\n"
    "\n"
    "void glGetBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, void* data) {\n")
sub("src/gl_calls.c",
    "    if (buffer) memcpy(data, buffer->mappedData + offset, size);\n",
    "    if (buffer && buffer->mappedData) {\n"
    "        gladio_waitForPixelPackBuffer(target);\n"
    "        memcpy(data, buffer->mappedData + offset, size);\n"
    "    }\n")
sub("src/gl_calls.c",
    "    buffer->mapped = true;\n\n    void* result = buffer->mappedData + offset;\n",
    "    buffer->mapped = true;\n\n    gladio_waitForPixelPackBuffer(target);\n    void* result = buffer->mappedData + offset;\n")

# ---- glFlushMappedBufferRange / glUnmapBuffer: GL_MAP_FLUSH_EXPLICIT_BIT beachten ----
sub("src/gl_calls.c",
    "void glFlushMappedBufferRange(GLenum target, GLintptr offset, GLsizeiptr length) {\n"
    "    if (target == GL_PIXEL_UNPACK_BUFFER || length == 0) return;\n",
    "/* Versatz im Puffer (glUnmapBuffer, persistente Puffer beim Zeichnen) */\n"
    "void gladio_flushBufferRange(GLenum target, GLintptr offset, GLsizeiptr length) {\n"
    "    if (target == GL_PIXEL_UNPACK_BUFFER || length == 0) return;\n")
sub("src/gl_calls.c",
    "void glFlushMappedBufferRangeEXT(GLenum target, GLintptr offset, GLsizeiptr length) {\n",
    "/* GL: der Versatz zählt ab dem Anfang des abgebildeten Bereichs. Ohne GL_MAP_PERSISTENT_BIT darf GL den Puffer\n"
    " * erst nach dem Freigeben wieder benutzen: die Bereiche gehen erst mit glUnmapBuffer hinaus. Wine im\n"
    " * WoW64-Betrieb (32-Bit-Programm, Abbildung über 4 GB) schreibt seine Kopie der Abbildung erst beim Freigeben\n"
    " * in den geteilten Speicher zurück – vorher las der Server dort noch die alten Daten. */\n"
    "void glFlushMappedBufferRange(GLenum target, GLintptr offset, GLsizeiptr length) {\n"
    "    GL_CALL_LOCK();\n"
    "    GLBuffer* buffer = GLBuffer_getBound(target);\n"
    "    if (buffer && buffer->mapped) {\n"
    "        offset += buffer->mapOffset;\n"
    "        if (!(buffer->access & GL_MAP_PERSISTENT_BIT)) {\n"
    "            gladio_recordFlush(buffer, offset, length);\n"
    "            GL_CALL_UNLOCK();\n"
    "            return;\n"
    "        }\n"
    "    }\n"
    "    GL_CALL_UNLOCK();\n"
    "    gladio_flushBufferRange(target, offset, length);\n"
    "}\n"
    "\n"
    "void glFlushMappedBufferRangeEXT(GLenum target, GLintptr offset, GLsizeiptr length) {\n")
sub("src/gl_calls.c",
    "    if (buffer->mapLength == 0 && (buffer->access == GL_WRITE_ONLY || buffer->access == GL_READ_WRITE)) {\n"
    "        glFlushMappedBufferRange(target, 0, buffer->size);\n"
    "    }\n"
    "    else if (buffer->mapLength > 0 && (buffer->access & GL_MAP_WRITE_BIT)) {\n"
    "        glFlushMappedBufferRange(target, buffer->mapOffset, buffer->mapLength);\n"
    "    }\n",
    "    if (buffer->mapped) gladio_unmapHead(buffer);\n"
    "    if (buffer->mapLength == 0 && (buffer->access == GL_WRITE_ONLY || buffer->access == GL_READ_WRITE)) {\n"
    "        gladio_flushBufferRange(target, 0, buffer->size);\n"
    "    }\n"
    "    else if (buffer->mapLength > 0 && (buffer->access & GL_MAP_WRITE_BIT) && !(buffer->access & GL_MAP_FLUSH_EXPLICIT_BIT)) {\n"
    "        gladio_flushBufferRange(target, buffer->mapOffset, buffer->mapLength);\n"
    "    }\n"
    "    /* vorgemerkte ausdrückliche Übertragungen (glFlushMappedBufferRange) */\n"
    "    for (int i = 0; i < buffer->flushCount; i++) {\n"
    "        gladio_flushBufferRange(target, buffer->flushRanges[i][0], buffer->flushRanges[i][1] - buffer->flushRanges[i][0]);\n"
    "    }\n"
    "    buffer->flushCount = 0;\n")
sub("src/gl_buffer.c",
    "extern void getRangeIndices(const void* indices, GLsizei count, GLenum indexType, GLint basevertex, GLuint* range);\n",
    "extern void getRangeIndices(const void* indices, GLsizei count, GLenum indexType, GLint basevertex, GLuint* range);\n"
    "extern void gladio_flushBufferRange(GLenum target, GLintptr offset, GLsizeiptr length);\n")
sub("include/gl_buffer.h", "    bool mapped;\n} GLBuffer;",
    "    bool mapped;\n"
    "    /* patch-gladio-client.py: mit GL_MAP_FLUSH_EXPLICIT_BIT übertragene Bereiche (Versatz im Puffer, Ende\n"
    "     * ausschließlich), gesendet erst beim Freigeben; mapHead: die ersten vier Byte des abgebildeten Bereichs\n"
    "     * beim Abbilden (Wine im WoW64-Betrieb überschreibt sie) */\n"
    "    int flushRanges[GLADIO_FLUSH_RANGES][2];\n"
    "    uint8_t flushCount;\n"
    "    uint32_t mapHead;\n"
    "} GLBuffer;")
sub("include/gl_buffer.h", "typedef struct GLBuffer {\n",
    "#include <stdint.h>\n"
    "\n"
    "#define GLADIO_FLUSH_RANGES 4\n"
    "\n"
    "typedef struct GLBuffer {\n")

# ---- Übertragungen ohne Rundreise ----
# glFlushMappedBufferRange und glBufferSubData warten nicht auf den Server; er liest die Daten erst
# beim Abarbeiten aus dem geteilten Speicher. Der Client merkt sich je Puffer den Bereich, den der
# Server noch lesen wird (auch BGRA-Farben beim Zeichnen, glCopyBufferSubData), und wartet, bevor er
# selbst hineinschreibt (glBufferData, glBufferSubData) – synchronisiertes Abbilden zum Schreiben
# wartet immer, wie in GL. Jedes Warten erhöht den Zähler syncEpoch des Kontexts: ältere Bereiche
# gelten dann als abgearbeitet.
sub("include/gl_context.h",
    "    int pendingSwaps; /* patch-gladio-client.py: Antworten auf glXSwapBuffers, die noch ausstehen */\n} GLContext;",
    "    int pendingSwaps; /* patch-gladio-client.py: Antworten auf glXSwapBuffers, die noch ausstehen */\n"
    "    unsigned syncEpoch; /* patch-gladio-client.py: zählt das Warten auf den Server (gladio_serverBarrier) */\n"
    "} GLContext;")
sub("include/gl_buffer.h", "    uint32_t mapHead;\n} GLBuffer;",
    "    uint32_t mapHead;\n"
    "    /* patch-gladio-client.py: Bereich, den der Server noch aus dem geteilten Speicher liest; gilt,\n"
    "     * solange pendingEpoch dem syncEpoch von pendingContext entspricht */\n"
    "    int pendingStart, pendingEnd;\n"
    "    unsigned pendingEpoch;\n"
    "    void* pendingContext;\n"
    "} GLBuffer;")
sub("include/gl_buffer.h",
    "extern void flushMappedPersistentBuffers(GLint first, GLsizei count, const void* indices, GLenum indexType, GLint basevertex);\n",
    "extern void flushMappedPersistentBuffers(GLint first, GLsizei count, const void* indices, GLenum indexType, GLint basevertex);\n"
    "extern void gladio_markPending(GLBuffer* buffer, GLintptr offset, GLsizeiptr size);\n")
sub("src/gl_calls.c", "pthread_mutex_t gl_call_mutex = PTHREAD_MUTEX_INITIALIZER;\n",
    "pthread_mutex_t gl_call_mutex = PTHREAD_MUTEX_INITIALIZER;\n"
    "static void gladio_serverBarrier(void);\n"
    "\n"
    "/* Bereich, den der Server beim Abarbeiten noch aus dem geteilten Speicher des Puffers liest */\n"
    "void gladio_markPending(GLBuffer* buffer, GLintptr offset, GLsizeiptr size) {\n"
    "    GLContext* context = currentGLContext;\n"
    "    if (!buffer || !context || size <= 0) return;\n"
    "    if (buffer->pendingContext != context || buffer->pendingEpoch != context->syncEpoch || buffer->pendingStart >= buffer->pendingEnd) {\n"
    "        buffer->pendingContext = context;\n"
    "        buffer->pendingEpoch = context->syncEpoch;\n"
    "        buffer->pendingStart = offset;\n"
    "        buffer->pendingEnd = offset + size;\n"
    "        return;\n"
    "    }\n"
    "    if (offset < buffer->pendingStart) buffer->pendingStart = offset;\n"
    "    if (offset + size > buffer->pendingEnd) buffer->pendingEnd = offset + size;\n"
    "}\n"
    "\n"
    "/* vor dem Schreiben in den geteilten Speicher: liest der Server dort noch, erst abwarten */\n"
    "static void gladio_waitPending(GLBuffer* buffer, GLintptr offset, GLsizeiptr size) {\n"
    "    GLContext* context = currentGLContext;\n"
    "    if (!buffer || !context || buffer->pendingContext != context || buffer->pendingEpoch != context->syncEpoch) return;\n"
    "    if (offset < buffer->pendingEnd && offset + size > buffer->pendingStart) gladio_serverBarrier();\n"
    "}\n"
    "\n"
    "/* ausdrücklich übertragenen Bereich bis glUnmapBuffer vormerken (überlappende und angrenzende Bereiche\n"
    " * zusammenlegen; sind alle Plätze belegt, wächst der letzte) */\n"
    "static void gladio_recordFlush(GLBuffer* buffer, GLintptr offset, GLsizeiptr length) {\n"
    "    if (length <= 0 || offset >= buffer->size || offset + length <= 0) return;\n"
    "    int start = (int)MAX(offset, 0), end = (int)MIN(offset + length, (GLintptr)buffer->size);\n"
    "    for (int i = 0; i < buffer->flushCount; i++) {\n"
    "        int* range = buffer->flushRanges[i];\n"
    "        if (start <= range[1] && end >= range[0]) {\n"
    "            range[0] = MIN(range[0], start);\n"
    "            range[1] = MAX(range[1], end);\n"
    "            return;\n"
    "        }\n"
    "    }\n"
    "    if (buffer->flushCount < GLADIO_FLUSH_RANGES) {\n"
    "        int* range = buffer->flushRanges[buffer->flushCount++];\n"
    "        range[0] = start;\n"
    "        range[1] = end;\n"
    "        return;\n"
    "    }\n"
    "    int* range = buffer->flushRanges[GLADIO_FLUSH_RANGES - 1];\n"
    "    range[0] = MIN(range[0], start);\n"
    "    range[1] = MAX(range[1], end);\n"
    "}\n"
    "\n"
    "/* Abbildung beginnt: Wine im WoW64-Betrieb (32-Bit-Programm, geteilter Speicher über 4 GB) legt eine Kopie an\n"
    " * und schreibt deren Adresse in die ersten vier Byte des abgebildeten Bereichs; beim Freigeben zum Schreiben\n"
    " * kopiert es die Kopie samt der alten vier Byte zurück, beim Freigeben nur zum Lesen nicht. Der Server darf sie\n"
    " * vorher nicht mehr lesen müssen, und nach dem Lesen stellt der Client sie wieder her. 32-Bit-Bau: nichts zu tun. */\n"
    "static void gladio_mapHead(GLBuffer* buffer, GLintptr offset) {\n"
    "#if UINTPTR_MAX > 0xffffffff\n"
    "    if (!buffer->mappedData || offset < 0 || offset + 4 > buffer->size) return;\n"
    "    gladio_waitPending(buffer, offset, 4);\n"
    "    memcpy(&buffer->mapHead, (char*)buffer->mappedData + offset, 4);\n"
    "#endif\n"
    "}\n"
    "\n"
    "static void gladio_unmapHead(GLBuffer* buffer) {\n"
    "#if UINTPTR_MAX > 0xffffffff\n"
    "    int access = buffer->access;\n"
    "    bool legacyAccess = access == GL_READ_ONLY || access == GL_WRITE_ONLY || access == GL_READ_WRITE;\n"
    "    bool writes = legacyAccess ? access != GL_READ_ONLY : (access & GL_MAP_WRITE_BIT) != 0;\n"
    "    if (writes || !buffer->mappedData || buffer->mapOffset < 0 || buffer->mapOffset + 4 > buffer->size) return;\n"
    "    memcpy((char*)buffer->mappedData + buffer->mapOffset, &buffer->mapHead, 4);\n"
    "#endif\n"
    "}\n")
sub("src/gl_calls.c",
    "    ArrayBuffer_rewind(&outputBuffer);\n"
    "    ArrayBuffer_putInt(&outputBuffer, target);\n"
    "    ArrayBuffer_putInt(&outputBuffer, offset);\n"
    "    ArrayBuffer_putInt(&outputBuffer, length);\n"
    "    GL_SEND_CHECKED(REQUEST_CODE_GL_FLUSH_MAPPED_BUFFER_RANGE, outputBuffer.buffer, outputBuffer.size);\n"
    "    GL_RECV_CHECKED();\n",
    "    ArrayBuffer_rewind(&outputBuffer);\n"
    "    ArrayBuffer_putInt(&outputBuffer, target | 0x40000000); /* GLADIO_NO_REPLY */\n"
    "    ArrayBuffer_putInt(&outputBuffer, offset);\n"
    "    ArrayBuffer_putInt(&outputBuffer, length);\n"
    "    gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_FLUSH_MAPPED_BUFFER_RANGE, outputBuffer.buffer, outputBuffer.size);\n"
    "    gladio_markPending(buffer, offset, length);\n")
sub("src/gl_calls.c",
    "static void gladio_waitForPixelPackBuffer(GLenum target) {\n",
    "/* wartet, bis der Server alle früheren Anfragen abgearbeitet hat (glFlushMappedBufferRange ohne\n"
    " * Daten: nur Antwort) */\n"
    "static void gladio_serverBarrier(void) {\n"
    "    if (!currentGLContext || !gladio_drainSwapReplies(0)) return;\n"
    "    ArrayBuffer_rewind(&outputBuffer);\n"
    "    ArrayBuffer_putInt(&outputBuffer, GL_ARRAY_BUFFER);\n"
    "    ArrayBuffer_putInt(&outputBuffer, 0);\n"
    "    ArrayBuffer_putInt(&outputBuffer, 0);\n"
    "    if (gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_FLUSH_MAPPED_BUFFER_RANGE, outputBuffer.buffer, outputBuffer.size)) {\n"
    "        gl_recv(currentGLContext->clientRing, NULL, &inputBuffer);\n"
    "    }\n"
    "    currentGLContext->syncEpoch++;\n"
    "}\n"
    "\n"
    "static void gladio_waitForPixelPackBuffer(GLenum target) {\n")
sub("src/gl_calls.c",
    "    buffer->mapped = true;\n\n    gladio_waitForPixelPackBuffer(target);\n",
    "    buffer->mapped = true;\n\n    gladio_waitForPixelPackBuffer(target);\n"
    "    {\n"
    "        /* Schreiben, das in GL auf ältere Befehle wartet: der Server muss frühere Übertragungen aus\n"
    "         * diesem Speicher erst abgearbeitet haben (glMapBuffer: GL_WRITE_ONLY/GL_READ_WRITE) */\n"
    "        bool legacyAccess = access == GL_READ_ONLY || access == GL_WRITE_ONLY || access == GL_READ_WRITE;\n"
    "        bool writes = legacyAccess ? access != GL_READ_ONLY : (access & GL_MAP_WRITE_BIT) != 0;\n"
    "        bool synchronized = legacyAccess || !(access & GL_MAP_UNSYNCHRONIZED_BIT) || (access & GL_MAP_INVALIDATE_BUFFER_BIT);\n"
    "        if (writes && synchronized && target != GL_PIXEL_PACK_BUFFER) gladio_serverBarrier();\n"
    "        bool reads = legacyAccess ? access != GL_WRITE_ONLY : (access & GL_MAP_READ_BIT) != 0;\n"
    "        if (reads) gladio_waitPending(buffer, offset, length > 0 ? length : buffer->size);\n"
    "        buffer->flushCount = 0;\n"
    "        gladio_mapHead(buffer, offset);\n"
    "    }\n")
sub("src/gl_calls.c", "    if (data) memcpy(buffer->mappedData, data, size);\n",
    "    if (data) gladio_waitPending(buffer, 0, size);\n"
    "    if (data) memcpy(buffer->mappedData, data, size);\n")
sub("src/gl_calls.c", "    if (data) memcpy(buffer->mappedData + offset, data, size);\n",
    "    if (data) gladio_waitPending(buffer, offset, size);\n"
    "    if (data) memcpy(buffer->mappedData + offset, data, size);\n")
sub("src/gl_calls.c",
    "        ArrayBuffer_putInt(&outputBuffer, target);\n"
    "        ArrayBuffer_putInt(&outputBuffer, offset);\n"
    "        ArrayBuffer_putInt(&outputBuffer, size);\n"
    "        GL_SEND_CHECKED(REQUEST_CODE_GL_BUFFER_SUB_DATA, outputBuffer.buffer, outputBuffer.size);\n"
    "        GL_RECV_CHECKED();\n",
    "        ArrayBuffer_putInt(&outputBuffer, target | 0x40000000); /* GLADIO_NO_REPLY */\n"
    "        ArrayBuffer_putInt(&outputBuffer, offset);\n"
    "        ArrayBuffer_putInt(&outputBuffer, size);\n"
    "        gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_BUFFER_SUB_DATA, outputBuffer.buffer, outputBuffer.size);\n"
    "        gladio_markPending(buffer, offset, size);\n")
# glCopyBufferSubData: der Server liest die Quelle beim Abarbeiten aus dem geteilten Speicher
sub("src/gl_calls.c",
    "    ArrayBuffer_putInt(&outputBuffer, size);\n"
    "    gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_COPY_BUFFER_SUB_DATA, outputBuffer.buffer, outputBuffer.size);\n",
    "    ArrayBuffer_putInt(&outputBuffer, size);\n"
    "    gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_COPY_BUFFER_SUB_DATA, outputBuffer.buffer, outputBuffer.size);\n"
    "    gladio_markPending(GLBuffer_getBound(readTarget), readOffset, size);\n"
    "    gladio_markPending(GLBuffer_getBound(writeTarget), writeOffset, size); /* der Server schreibt das Ziel auch in den geteilten Speicher */\n")
# Lesen aus dem geteilten Speicher: ausstehende Kopien dorthin erst abwarten
sub("src/gl_calls.c",
    "        gladio_waitForPixelPackBuffer(target);\n        memcpy(data, buffer->mappedData + offset, size);\n",
    "        gladio_waitForPixelPackBuffer(target);\n        gladio_waitPending(buffer, offset, size);\n        memcpy(data, buffer->mappedData + offset, size);\n")
# Pufferziele GL_COPY_READ_BUFFER/GL_COPY_WRITE_BUFFER (wined3d kopiert Puffer darüber): vorher lieferte
# indexOfGLTarget 255 und GLBuffer_setBound schrieb hinter vao->buffer; unbekannte Ziele übergehen
sub("include/gladio.h", "#define MAX_BUFFER_TARGETS 6\n", "#define MAX_BUFFER_TARGETS 8 /* mit GL_COPY_READ_BUFFER/GL_COPY_WRITE_BUFFER (6, 7) */\n")
sub("include/gladio.h",
    "        case GL_TEXTURE_BUFFER:\n            return 5;\n        default:",
    "        case GL_TEXTURE_BUFFER:\n            return 5;\n        case GL_COPY_READ_BUFFER:\n            return 6;\n        case GL_COPY_WRITE_BUFFER:\n            return 7;\n        default:")
sub("src/gl_buffer.c",
    "GLBuffer* GLBuffer_getBound(GLenum target) {\n"
    "    GLClientState* clientState = currentGLContext->clientState;\n"
    "    return clientState->vao->buffer[indexOfGLTarget(target)];\n"
    "}\n",
    "GLBuffer* GLBuffer_getBound(GLenum target) {\n"
    "    GLClientState* clientState = currentGLContext->clientState;\n"
    "    uint8_t index = indexOfGLTarget(target);\n"
    "    return index < MAX_BUFFER_TARGETS ? clientState->vao->buffer[index] : NULL;\n"
    "}\n")
sub("src/gl_buffer.c",
    "    clientState->vao->buffer[indexOfGLTarget(target)] = buffer;\n",
    "    uint8_t index = indexOfGLTarget(target);\n"
    "    if (index < MAX_BUFFER_TARGETS) clientState->vao->buffer[index] = buffer;\n")

# GL_BUFFER_MAP_POINTER: Anfang des abgebildeten Bereichs, NULL ohne Abbildung (vorher immer der Pufferanfang;
# Wine im WoW64-Betrieb findet darüber seine Kopie der Abbildung und schreibt sie beim Freigeben dorthin zurück)
sub("src/gl_calls.c",
    "    *params = buffer ? buffer->mappedData : NULL;\n",
    "    *params = buffer && buffer->mapped && buffer->mappedData ? (char*)buffer->mappedData + buffer->mapOffset : NULL;\n")
# GL_BUFFER_ACCESS_FLAGS als Bits und GL_BUFFER_MAP_LENGTH auch nach glMapBuffer (Wine im WoW64-Betrieb kopiert
# beim Freigeben nur mit GL_MAP_WRITE_BIT und nur GL_BUFFER_MAP_LENGTH Byte zurück: nach glMapBuffer vorher nichts)
sub("src/gl_buffer.c",
    "        case GL_BUFFER_ACCESS:\n"
    "        case GL_BUFFER_ACCESS_FLAGS:\n"
    "            *params = buffer->access;\n"
    "            break;\n",
    "        case GL_BUFFER_ACCESS:\n"
    "            if (buffer->access == GL_READ_ONLY || buffer->access == GL_WRITE_ONLY || buffer->access == GL_READ_WRITE) *params = buffer->access;\n"
    "            else if ((buffer->access & (GL_MAP_READ_BIT | GL_MAP_WRITE_BIT)) == GL_MAP_READ_BIT) *params = GL_READ_ONLY;\n"
    "            else if ((buffer->access & (GL_MAP_READ_BIT | GL_MAP_WRITE_BIT)) == GL_MAP_WRITE_BIT) *params = GL_WRITE_ONLY;\n"
    "            else *params = GL_READ_WRITE;\n"
    "            break;\n"
    "        case GL_BUFFER_ACCESS_FLAGS:\n"
    "            if (buffer->access == GL_READ_ONLY) *params = GL_MAP_READ_BIT;\n"
    "            else if (buffer->access == GL_WRITE_ONLY) *params = GL_MAP_WRITE_BIT;\n"
    "            else if (buffer->access == GL_READ_WRITE) *params = GL_MAP_READ_BIT | GL_MAP_WRITE_BIT;\n"
    "            else *params = buffer->access;\n"
    "            break;\n")
sub("src/gl_buffer.c",
    "        case GL_BUFFER_MAP_LENGTH:\n"
    "            *params = buffer->mapLength;\n",
    "        case GL_BUFFER_MAP_LENGTH:\n"
    "            *params = buffer->mapped && buffer->mapLength == 0 ? buffer->size - buffer->mapOffset : buffer->mapLength;\n")

# BGRA-Farben liest der Server beim Zeichnen aus dem geteilten Speicher: genau die gezeichneten Eckpunkte vormerken
# (Versatz und Schrittweite je Attribut; Indizes aus dem Elementpuffer – die liest der Server auch dort)
sub("include/gl_vao.h", "    GLuint bgraBuffer;\n} GLVertexArrayObject;",
    "    GLuint bgraBuffer;\n"
    "    /* patch-gladio-client.py: Attribute mit GL_BGRA aus einem Puffer (Quelle, Versatz, Schrittweite) und\n"
    "     * eingeschaltete Attribute */\n"
    "    uint32_t bgraMask;\n"
    "    uint32_t attribEnabled;\n"
    "    GLuint bgraSource[VERTEX_ATTRIB_COUNT];\n"
    "    int bgraOffset[VERTEX_ATTRIB_COUNT];\n"
    "    short bgraStride[VERTEX_ATTRIB_COUNT];\n"
    "} GLVertexArrayObject;")
sub("src/gl_calls.c",
    "    if (index < VERTEX_ATTRIB_COUNT) GLVertexArrayObject_setAttribState(clientState, index, VERTEX_ATTRIB_DISABLED, false);\n"
    "    gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_DISABLE_VERTEX_ATTRIB_ARRAY, &index, sizeof(GLuint));\n",
    "    if (index < VERTEX_ATTRIB_COUNT) GLVertexArrayObject_setAttribState(clientState, index, VERTEX_ATTRIB_DISABLED, false);\n"
    "    if (index < 32) clientState->vao->attribEnabled &= ~(1u << index);\n"
    "    gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_DISABLE_VERTEX_ATTRIB_ARRAY, &index, sizeof(GLuint));\n")
sub("src/gl_calls.c",
    "    if (currentGLContext->clientState->arbProgram[0]) index += GENERIC_VERTEX_ARRAY_INDEX;\n"
    "    gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_ENABLE_VERTEX_ATTRIB_ARRAY, &index, sizeof(GLuint));\n",
    "    if (currentGLContext->clientState->arbProgram[0]) index += GENERIC_VERTEX_ARRAY_INDEX;\n"
    "    if (index < 32) currentGLContext->clientState->vao->attribEnabled |= 1u << index;\n"
    "    gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_ENABLE_VERTEX_ATTRIB_ARRAY, &index, sizeof(GLuint));\n")
# vorher wurden bei indizierten Aufrufen mit Elementpuffer die Indizes an der Adresse "Versatz im Puffer" gelesen
sub("src/gl_buffer.c",
    "void flushMappedPersistentBuffers(GLint first, GLsizei count, const void* indices, GLenum indexType, GLint basevertex) {\n"
    "    if (currentGLContext->clientState->persistentBuffers.size == 0) return;\n"
    "    \n"
    "    if (indexType != GL_NONE) {\n"
    "        GLuint range[2];\n"
    "        getRangeIndices(indices, count, indexType, basevertex, range);\n"
    "        first = range[0];\n"
    "        count = (range[1] - range[0]) + 1;\n"
    "    }\n"
    "    \n"
    "    GLClientState* clientState = currentGLContext->clientState;\n"
    "    for (int i = 0; i < clientState->persistentBuffers.size; i++) {\n"
    "        GLBuffer* buffer = clientState->persistentBuffers.elements[i];\n"
    "        if (buffer->mapped) {\n"
    "            int offset = first * buffer->drawStride + buffer->mapOffset;\n"
    "            int length = count * buffer->drawStride;\n"
    "            \n"
    "            glFlushMappedBufferRange(buffer->type, offset, length);\n"
    "        }\n"
    "    }\n"
    "}",
    "void flushMappedPersistentBuffers(GLint first, GLsizei count, const void* indices, GLenum indexType, GLint basevertex) {\n"
    "    GLClientState* clientState = currentGLContext->clientState;\n"
    "    GLVertexArrayObject* vao = clientState->vao;\n"
    "    uint32_t bgraMask = vao->bgraMask & vao->attribEnabled;\n"
    "    if (count <= 0 || (!bgraMask && clientState->persistentBuffers.size == 0)) return;\n"
    "\n"
    "    /* gezeichnete Eckpunkte; Indizes aus einem Elementpuffer stehen in dessen geteiltem Speicher */\n"
    "    GLBuffer* elementBuffer = NULL;\n"
    "    if (indexType != GL_NONE) {\n"
    "        elementBuffer = vao->buffer[indexOfGLTarget(GL_ELEMENT_ARRAY_BUFFER)];\n"
    "        if (elementBuffer) {\n"
    "            uint64_t indexOffset = (uint64_t)(uintptr_t)indices, indexBytes = (uint64_t)count * sizeofGLType(indexType);\n"
    "            indices = elementBuffer->mappedData && indexOffset + indexBytes <= (uint64_t)elementBuffer->size ? (const char*)elementBuffer->mappedData + indexOffset : NULL;\n"
    "        }\n"
    "    }\n"
    "    GLuint range[2] = {(GLuint)first, (GLuint)first + count - 1};\n"
    "    bool haveRange = indexType == GL_NONE;\n"
    "    if (indexType != GL_NONE && indices) {\n"
    "        getRangeIndices(indices, count, indexType, basevertex, range);\n"
    "        haveRange = range[0] <= range[1];\n"
    "    }\n"
    "\n"
    "    /* GL_BGRA aus einem Puffer: der Server liest Farben und Indizes beim Abarbeiten aus dem geteilten Speicher */\n"
    "    if (bgraMask) {\n"
    "        if (elementBuffer && indices) gladio_markPending(elementBuffer, (const char*)indices - (const char*)elementBuffer->mappedData, count * sizeofGLType(indexType));\n"
    "        for (uint32_t mask = bgraMask; mask; mask &= mask - 1) {\n"
    "            int i = __builtin_ctz(mask);\n"
    "            GLBuffer* source = SparseArray_get(clientState->buffers, vao->bgraSource[i]);\n"
    "            if (!source) continue;\n"
    "            int64_t start = 0, end = source->size;\n"
    "            if (haveRange) {\n"
    "                start = MAX((int64_t)vao->bgraOffset[i] + (int64_t)range[0] * vao->bgraStride[i], 0);\n"
    "                end = MIN((int64_t)vao->bgraOffset[i] + (int64_t)range[1] * vao->bgraStride[i] + 4, (int64_t)source->size);\n"
    "            }\n"
    "            if (start < end) gladio_markPending(source, (GLintptr)start, (GLsizeiptr)(end - start));\n"
    "        }\n"
    "    }\n"
    "\n"
    "    for (int i = 0; i < clientState->persistentBuffers.size; i++) {\n"
    "        GLBuffer* buffer = clientState->persistentBuffers.elements[i];\n"
    "        if (!buffer->mapped) continue;\n"
    "        if (haveRange) {\n"
    "            int offset = range[0] * buffer->drawStride + buffer->mapOffset;\n"
    "            int length = (range[1] - range[0] + 1) * buffer->drawStride;\n"
    "            gladio_flushBufferRange(buffer->type, offset, length);\n"
    "        }\n"
    "        else gladio_flushBufferRange(buffer->type, buffer->mapOffset, buffer->mapLength > 0 ? buffer->mapLength : buffer->size - buffer->mapOffset);\n"
    "    }\n"
    "}")

# ---- GL_BGRA aus einem Puffer: der Server stellt die Farben selbst um ----
sub("src/gl_calls.c",
    "        if (!arrayBuffer || size == GL_BGRA) {\n"
    "            clientState->vao->attribs[index].stride = stride > 0 ? stride : MIN(4, size) * sizeofGLType(type);\n"
    "            clientState->vao->attribs[index].pointer = arrayBuffer ? arrayBuffer->mappedData : pointer;\n"
    "            GLVertexArrayObject_setAttribState(clientState, index, VERTEX_ATTRIB_ENABLED, false);\n"
    "            if (!arrayBuffer) pointer = (void*)0;\n"
    "        }\n"
    "        else GLVertexArrayObject_setAttribState(clientState, index, VERTEX_ATTRIB_DISABLED, true);\n",
    "        if (index < 32) clientState->vao->bgraMask &= ~(1u << index);\n"
    "        if (!arrayBuffer) {\n"
    "            clientState->vao->attribs[index].stride = stride > 0 ? stride : MIN(4, size) * sizeofGLType(type);\n"
    "            clientState->vao->attribs[index].pointer = (void*)pointer;\n"
    "            GLVertexArrayObject_setAttribState(clientState, index, VERTEX_ATTRIB_ENABLED, false);\n"
    "            pointer = (void*)0;\n"
    "        }\n"
    "        else {\n"
    "            GLVertexArrayObject_setAttribState(clientState, index, VERTEX_ATTRIB_DISABLED, true);\n"
    "            /* GL_BGRA aus dem Puffer: der Server liest die Farben selbst aus dem geteilten Speicher */\n"
    "            if (size == GL_BGRA) {\n"
    "                normalized |= 0x80; /* GLADIO_VERTEX_ATTRIB_BGRA_BUFFER */\n"
    "                if (index < VERTEX_ATTRIB_COUNT && index < 32) {\n"
    "                    clientState->vao->bgraMask |= 1u << index;\n"
    "                    clientState->vao->bgraSource[index] = arrayBuffer->id;\n"
    "                    clientState->vao->bgraOffset[index] = (int)(uintptr_t)pointer;\n"
    "                    clientState->vao->bgraStride[index] = stride > 0 ? stride : 4;\n"
    "                }\n"
    "            }\n"
    "        }\n")

# GL_STREAM_DRAW beim Anlegen als GL_CLIENT_STORAGE_BIT melden: der Server der armv7-App zeichnet solche
# Puffer über ES 2.0 von Anfang an aus einer eigenen Kopie (Mali-400: glBufferSubData in einen Puffer, aus dem
# im selben Bild schon gezeichnet wurde, kopiert den ganzen Puffer). GL_BUFFER_USAGE bleibt der Wert des Programms.
sub("src/gl_calls.c",
    "        GLbitfield flags = usage == GL_DYNAMIC_DRAW ? GL_DYNAMIC_STORAGE_BIT : 0;\n"
    "        glBufferStorage(target, size, data, flags);\n"
    "        return;\n",
    "        GLbitfield flags = usage == GL_DYNAMIC_DRAW ? GL_DYNAMIC_STORAGE_BIT : 0;\n"
    "        if (usage == GL_STREAM_DRAW) flags |= GL_CLIENT_STORAGE_BIT;\n"
    "        glBufferStorage(target, size, data, flags);\n"
    "        GL_CALL_LOCK();\n"
    "        buffer->usage = usage;\n"
    "        GL_CALL_UNLOCK();\n"
    "        return;\n")

# Verdeckungsabfragen und Fences: „fertig?“ ohne Rundreise beantworten. wined3d fragt mit CSMT alle 100 Befehle jede
# offene Abfrage und jeden Fence ab (Outlast: ~2000 Rundreisen je Bild, jede mit Warten auf den Server). Die GPU arbeitet
# sie in der Reihenfolge ab, in der sie enden: Ist eine fertig, sind es die früher beendeten auch; ist eine nicht fertig,
# sind es die später beendeten auch – das gilt 1 ms lang, danach fragt der Client wieder. Ergebnisse holt er weiter beim
# Server (falls eine Annahme doch nicht stimmt, wartet der dort kurz).
sub("src/gl_calls.c", "void glBeginQuery(GLenum target, GLuint id) {\n    GL_CALL_LOCK();\n",
    "/* ---- Verdeckungsabfragen und Fences: fertig/nicht fertig ohne Rundreise (patch-gladio-client.py) ---- */\n"
    "#include <time.h>\n"
    "#ifndef GL_ANY_SAMPLES_PASSED_CONSERVATIVE\n"
    "#define GL_ANY_SAMPLES_PASSED_CONSERVATIVE 0x8D6A\n"
    "#endif\n"
    "#define GLADIO_POLL_SLOTS 4096\n"
    "#define GLADIO_POLL_VALID_NS 1000000LL\n"
    "typedef struct GladioPollState {\n"
    "    uint32_t counter;      /* Nummern in der Reihenfolge, in der die GPU abarbeitet (Ende der Abfrage, Fence) */\n"
    "    uint32_t knownDone;    /* alle mit Nummer <= knownDone sind fertig */\n"
    "    uint32_t knownPending; /* alle mit Nummer >= knownPending waren zu pendingTime nicht fertig (0: nichts bekannt) */\n"
    "    int64_t pendingTime;\n"
    "} GladioPollState;\n"
    "static GladioPollState gladioQueryPoll, gladioFencePoll;\n"
    "static struct { GLuint id; uint32_t seq; } gladioQuerySeqs[GLADIO_POLL_SLOTS];\n"
    "static struct { uintptr_t sync; uint32_t seq; } gladioFenceSeqs[GLADIO_POLL_SLOTS];\n"
    "static GLuint gladioActiveQueries[3];\n"
    "\n"
    "static int64_t gladioPollNow(void) {\n"
    "    struct timespec ts;\n"
    "    clock_gettime(CLOCK_MONOTONIC, &ts);\n"
    "    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;\n"
    "}\n"
    "\n"
    "static int gladioOcclusionSlot(GLenum target) {\n"
    "    return target == GL_SAMPLES_PASSED ? 0 : target == GL_ANY_SAMPLES_PASSED ? 1 : target == GL_ANY_SAMPLES_PASSED_CONSERVATIVE ? 2 : -1;\n"
    "}\n"
    "\n"
    "/* 1 = fertig, 0 = nicht fertig, -1 = beim Server fragen */\n"
    "static int gladioPollKnown(GladioPollState* state, uint32_t seq) {\n"
    "    if (!seq) return -1;\n"
    "    if (seq <= state->knownDone) return 1;\n"
    "    if (state->knownPending && seq >= state->knownPending && gladioPollNow() - state->pendingTime < GLADIO_POLL_VALID_NS) return 0;\n"
    "    return -1;\n"
    "}\n"
    "\n"
    "static void gladioPollLearn(GladioPollState* state, uint32_t seq, int done) {\n"
    "    if (!seq) return;\n"
    "    if (done) {\n"
    "        if (seq > state->knownDone) state->knownDone = seq;\n"
    "        if (state->knownPending && state->knownPending <= seq) state->knownPending = 0;\n"
    "        return;\n"
    "    }\n"
    "    int64_t now = gladioPollNow();\n"
    "    if (!state->knownPending || now - state->pendingTime >= GLADIO_POLL_VALID_NS || seq < state->knownPending) {\n"
    "        state->knownPending = seq;\n"
    "        state->pendingTime = now;\n"
    "    }\n"
    "}\n"
    "\n"
    "static void gladioNoteBeginQuery(GLenum target, GLuint id) {\n"
    "    int slot = gladioOcclusionSlot(target);\n"
    "    if (slot >= 0) gladioActiveQueries[slot] = id;\n"
    "}\n"
    "\n"
    "static void gladioNoteEndQuery(GLenum target) {\n"
    "    int slot = gladioOcclusionSlot(target);\n"
    "    if (slot < 0 || !gladioActiveQueries[slot]) return;\n"
    "    GLuint id = gladioActiveQueries[slot];\n"
    "    gladioActiveQueries[slot] = 0;\n"
    "    gladioQuerySeqs[id % GLADIO_POLL_SLOTS].id = id;\n"
    "    gladioQuerySeqs[id % GLADIO_POLL_SLOTS].seq = ++gladioQueryPoll.counter;\n"
    "}\n"
    "\n"
    "static uint32_t gladioQuerySeqOf(GLuint id) {\n"
    "    return gladioQuerySeqs[id % GLADIO_POLL_SLOTS].id == id ? gladioQuerySeqs[id % GLADIO_POLL_SLOTS].seq : 0;\n"
    "}\n"
    "\n"
    "static uint32_t gladioFenceSlot(GLsync sync) {\n"
    "    uintptr_t key = (uintptr_t)sync;\n"
    "    return (uint32_t)((key ^ (key >> 4) ^ (key >> 13)) % GLADIO_POLL_SLOTS);\n"
    "}\n"
    "\n"
    "static void gladioNoteFence(GLsync sync) {\n"
    "    if (!sync) return;\n"
    "    uint32_t slot = gladioFenceSlot(sync);\n"
    "    gladioFenceSeqs[slot].sync = (uintptr_t)sync;\n"
    "    gladioFenceSeqs[slot].seq = ++gladioFencePoll.counter;\n"
    "}\n"
    "\n"
    "static uint32_t gladioFenceSeqOf(GLsync sync) {\n"
    "    uint32_t slot = gladioFenceSlot(sync);\n"
    "    return sync && gladioFenceSeqs[slot].sync == (uintptr_t)sync ? gladioFenceSeqs[slot].seq : 0;\n"
    "}\n"
    "\n"
    "static void gladioForgetFence(GLsync sync) {\n"
    "    uint32_t slot = gladioFenceSlot(sync);\n"
    "    if (sync && gladioFenceSeqs[slot].sync == (uintptr_t)sync) gladioFenceSeqs[slot].sync = 0;\n"
    "}\n"
    "\n"
    "void glBeginQuery(GLenum target, GLuint id) {\n    GL_CALL_LOCK();\n    gladioNoteBeginQuery(target, id);\n")
sub("src/gl_calls.c",
    "void glEndQuery(GLenum target) {\n    GL_CALL_LOCK();\n    gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_END_QUERY, &target, sizeof(GLenum));\n",
    "void glEndQuery(GLenum target) {\n    GL_CALL_LOCK();\n    gl_send(currentGLContext->serverRing, REQUEST_CODE_GL_END_QUERY, &target, sizeof(GLenum));\n    gladioNoteEndQuery(target);\n")
sub("src/gl_calls.c",
    "void glGetQueryObjectuiv(GLuint id, GLenum pname, GLuint* params) {\n"
    "    GL_CALL_LOCK();\n"
    "    ArrayBuffer_rewind(&outputBuffer);\n"
    "    ArrayBuffer_putInt(&outputBuffer, id);\n"
    "    ArrayBuffer_putInt(&outputBuffer, pname);\n"
    "    GL_SEND_CHECKED(REQUEST_CODE_GL_GET_QUERY_OBJECTUIV, outputBuffer.buffer, outputBuffer.size);\n"
    "    GL_RECV_CHECKED();\n"
    "    memcpy(params, inputBuffer.buffer, inputBuffer.size);\n"
    "    GL_CALL_UNLOCK();\n"
    "}\n",
    "void glGetQueryObjectuiv(GLuint id, GLenum pname, GLuint* params) {\n"
    "    GL_CALL_LOCK();\n"
    "    uint32_t querySeq = pname == GL_QUERY_RESULT_AVAILABLE ? gladioQuerySeqOf(id) : 0;\n"
    "    int known = gladioPollKnown(&gladioQueryPoll, querySeq);\n"
    "    if (known >= 0) {\n"
    "        *params = known ? GL_TRUE : GL_FALSE;\n"
    "        GL_CALL_UNLOCK();\n"
    "        return;\n"
    "    }\n"
    "    ArrayBuffer_rewind(&outputBuffer);\n"
    "    ArrayBuffer_putInt(&outputBuffer, id);\n"
    "    ArrayBuffer_putInt(&outputBuffer, pname);\n"
    "    GL_SEND_CHECKED(REQUEST_CODE_GL_GET_QUERY_OBJECTUIV, outputBuffer.buffer, outputBuffer.size);\n"
    "    GL_RECV_CHECKED();\n"
    "    memcpy(params, inputBuffer.buffer, inputBuffer.size);\n"
    "    if (querySeq) gladioPollLearn(&gladioQueryPoll, querySeq, *params != GL_FALSE);\n"
    "    GL_CALL_UNLOCK();\n"
    "}\n")
sub("src/gl_calls.c",
    "    GLsync sync = (GLsync)(intptr_t)ArrayBuffer_getLong(&inputBuffer);\n    GL_CALL_UNLOCK();\n    return sync;\n}",
    "    GLsync sync = (GLsync)(intptr_t)ArrayBuffer_getLong(&inputBuffer);\n    gladioNoteFence(sync);\n    GL_CALL_UNLOCK();\n    return sync;\n}")
sub("src/gl_calls.c",
    "GLenum glClientWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout) {\n    GL_CALL_LOCK();\n    ArrayBuffer_rewind(&outputBuffer);\n",
    "GLenum glClientWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout) {\n"
    "    GL_CALL_LOCK();\n"
    "    uint32_t fenceSeq = gladioFenceSeqOf(sync);\n"
    "    int known = gladioPollKnown(&gladioFencePoll, fenceSeq);\n"
    "    if (known > 0 || (known == 0 && timeout == 0)) {\n"
    "        GL_CALL_UNLOCK();\n"
    "        return known ? GL_ALREADY_SIGNALED : GL_TIMEOUT_EXPIRED;\n"
    "    }\n"
    "    ArrayBuffer_rewind(&outputBuffer);\n")
sub("src/gl_calls.c",
    "    GLenum result = ArrayBuffer_getInt(&inputBuffer);\n    GL_CALL_UNLOCK();\n    return result;\n}\n\nvoid glClipPlane",
    "    GLenum result = ArrayBuffer_getInt(&inputBuffer);\n"
    "    if (result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED) gladioPollLearn(&gladioFencePoll, fenceSeq, 1);\n"
    "    else if (result == GL_TIMEOUT_EXPIRED) gladioPollLearn(&gladioFencePoll, fenceSeq, 0);\n"
    "    GL_CALL_UNLOCK();\n    return result;\n}\n\nvoid glClipPlane")
sub("src/gl_calls.c", "void glDeleteSync(GLsync sync) {\n    GL_CALL_LOCK();\n",
    "void glDeleteSync(GLsync sync) {\n    GL_CALL_LOCK();\n    gladioForgetFence(sync);\n")

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
