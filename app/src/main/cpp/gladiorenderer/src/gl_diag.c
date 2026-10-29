/*
 * Diagnose des Gladio-Servers, siehe include/gl_diag.h.
 *
 * Fehler, die das Programm selbst nie erfährt (der Client fragt glGetError mit GLADIO_NO_ERROR=1 nicht ab,
 * und der Freigabe-Bau protokollierte bisher weder Shader- noch GL-Fehler), landen hier im Protokoll. Die
 * Aufzeichnung eines Bilds ersetzt am Gerät ein Werkzeug wie RenderDoc: Sie zeigt, welcher Durchgang was
 * in welches Ziel zeichnet und wie das Ergebnis aussieht.
 */
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <time.h>
#include <zlib.h>

#include "gl_diag.h"
#include "gl_context.h"
#include "gl_framebuffer.h"
#include "gl_es2.h"

extern void gladio_printLastRequests(GLContext* context, const char* prefix);

GladioDiagStats gladio_diagStats;
static GladioDiagStats printedStats;
static pthread_mutex_t diagMutex = PTHREAD_MUTEX_INITIALIZER;
static char diagDir[512];

#define MAX_LOGGED_COMPILE_ERRORS 8
#define MAX_LOGGED_LINK_ERRORS 8
#define MAX_LOGGED_FRAMEBUFFERS 12
#define MAX_ERROR_KEYS 64
#define MAX_ERROR_LINES 80
#define HUNT_FRAMES 2
#define HUNT_INTERVAL_FRAMES 600

/* ---------------------------------------------------------------- Hilfen */

static const char* requestName(short code) {
    if (code == REQUEST_CODE_SWAP_DISPLAY_BUFFERS) return "SWAP";
    if (code == REQUEST_CODE_SET_CURRENT_RENDER_WINDOW) return "MAKE_CURRENT";
    if (code >= REQUEST_CODE_GL_DSA_START && code < REQUEST_CODE_GL_CALL_START) return "DSA";
    if (code < REQUEST_CODE_GL_CALL_START) return "?";
    const char* name = requestCodeToString(code);
    return name[0] == 'g' && name[1] == 'l' ? name + 2 : name;
}

/* Name einer GL-Konstante oder ihr Wert in hex (vier Puffer im Wechsel, je Thread) */
static const char* enumName(GLenum value) {
    static thread_local char buffers[4][16];
    static thread_local int next = 0;
    const char* name = glEnumToString(value);
    if (strcmp(name, "GL_UNKNOWN") != 0) return name[0] == 'G' && name[1] == 'L' && name[2] == '_' ? name + 3 : name;
    char* buffer = buffers[next++ & 3];
    snprintf(buffer, sizeof(buffers[0]), "0x%x", value);
    return buffer;
}

static const char* errorName(GLenum error) {
    switch (error) {
        case GL_INVALID_ENUM: return "GL_INVALID_ENUM";
        case GL_INVALID_VALUE: return "GL_INVALID_VALUE";
        case GL_INVALID_OPERATION: return "GL_INVALID_OPERATION";
        case GL_OUT_OF_MEMORY: return "GL_OUT_OF_MEMORY";
        case GL_INVALID_FRAMEBUFFER_OPERATION: return "GL_INVALID_FRAMEBUFFER_OPERATION";
        default: return enumName(error);
    }
}

static const char* framebufferStatusName(GLenum status) {
    switch (status) {
        case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT: return "INCOMPLETE_ATTACHMENT";
        case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT: return "INCOMPLETE_MISSING_ATTACHMENT";
        case GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS: return "INCOMPLETE_DIMENSIONS";
        case GL_FRAMEBUFFER_UNSUPPORTED: return "UNSUPPORTED";
        case GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE: return "INCOMPLETE_MULTISAMPLE";
        default: return enumName(status);
    }
}

/* Aufbau der Argumente, wo die Wortregel danebenliegt: i = 32-Bit-Zahl, f = Gleitkomma, b = Byte, * davor =
 * der Rest in dieser Art (z. B. glUniformMatrix4fv: Ort, Anzahl, ein Byte transpose, dann Gleitkommazahlen) */
static const char* argumentLayout(short requestCode) {
    switch (requestCode) {
        case REQUEST_CODE_GL_VERTEX_ATTRIB_POINTER: return "iiibii";
        case REQUEST_CODE_GL_UNIFORM_MATRIX2FV:
        case REQUEST_CODE_GL_UNIFORM_MATRIX2X3FV:
        case REQUEST_CODE_GL_UNIFORM_MATRIX2X4FV:
        case REQUEST_CODE_GL_UNIFORM_MATRIX3FV:
        case REQUEST_CODE_GL_UNIFORM_MATRIX3X2FV:
        case REQUEST_CODE_GL_UNIFORM_MATRIX3X4FV:
        case REQUEST_CODE_GL_UNIFORM_MATRIX4FV:
        case REQUEST_CODE_GL_UNIFORM_MATRIX4X2FV:
        case REQUEST_CODE_GL_UNIFORM_MATRIX4X3FV: return "iib*f";
        case REQUEST_CODE_GL_UNIFORM1FV:
        case REQUEST_CODE_GL_UNIFORM2FV:
        case REQUEST_CODE_GL_UNIFORM3FV:
        case REQUEST_CODE_GL_UNIFORM4FV: return "ii*f";
        case REQUEST_CODE_GL_UNIFORM1F:
        case REQUEST_CODE_GL_UNIFORM2F:
        case REQUEST_CODE_GL_UNIFORM3F:
        case REQUEST_CODE_GL_UNIFORM4F: return "i*f";
        case REQUEST_CODE_GL_UNIFORM1IV:
        case REQUEST_CODE_GL_UNIFORM2IV:
        case REQUEST_CODE_GL_UNIFORM3IV:
        case REQUEST_CODE_GL_UNIFORM4IV: return "ii*i";
        case REQUEST_CODE_GL_COLOR_MASK: return "bbbb";
        case REQUEST_CODE_GL_COLOR_MASKI: return "ibbbb";
        case REQUEST_CODE_GL_DEPTH_MASK: return "b";
        case REQUEST_CODE_GL_CLEAR_COLOR:
        case REQUEST_CODE_GL_BLEND_COLOR: return "ffff";
        case REQUEST_CODE_GL_CLEAR_DEPTHF: return "f";
        case REQUEST_CODE_GL_DEPTH_RANGEF:
        case REQUEST_CODE_GL_POLYGON_OFFSET: return "ff";
        case REQUEST_CODE_GL_VERTEX_ATTRIB4F: return "iffff";
        case REQUEST_CODE_GL_VERTEX_ATTRIB4NUBV: return "ibbbb";
        case REQUEST_CODE_GL_SAMPLER_PARAMETERF:
        case REQUEST_CODE_GL_TEX_PARAMETERF: return "iif";
        case REQUEST_CODE_GL_SAMPLER_PARAMETERFV:
        case REQUEST_CODE_GL_TEX_PARAMETERFV: return "ii*f";
        case REQUEST_CODE_GL_CLEAR_BUFFERFV: return "ii*f";
        default: return NULL;
    }
}

static int formatWord(char* out, int size, uint32_t w, char kind) {
    if (kind == 'f') {
        float f;
        memcpy(&f, &w, 4);
        return snprintf(out, size, " %.6gf", f);
    }
    if (kind == 'i') {
        if ((int32_t)w < 0 && (int32_t)w > -65536) return snprintf(out, size, " %d", (int32_t)w);
        return w < 4096 ? snprintf(out, size, " %u", w) : snprintf(out, size, " 0x%x", w);
    }
    /* unbekannt: ganze Zahlen dezimal bzw. hex, Werte mit Gleitkomma-Aussehen als Zahl mit f */
    uint32_t exponent = (w >> 23) & 0xff;
    if (w < 4096) return snprintf(out, size, " %u", w);
    if ((int32_t)w < 0 && (int32_t)w > -65536) return snprintf(out, size, " %d", (int32_t)w);
    if (exponent >= 0x60 && exponent <= 0x9f) return formatWord(out, size, w, 'f');
    return snprintf(out, size, " 0x%x", w);
}

/* Argumente einer Anfrage (höchstens maxWords Werte), dahinter eine eingebettete Zeichenkette (Namen bei
 * glGetUniformLocation & Co.) */
static void formatArgs(short requestCode, const ArrayBuffer* input, char* out, int size, int maxWords) {
    const uint8_t* data = (const uint8_t*)input->buffer;
    int pos = 0, offset = 0, values = 0;
    out[0] = '\0';
    if (!data) return;
    const char* layout = argumentLayout(requestCode);
    char restKind = '?';
    while (offset < input->size && values < maxWords && pos < size - 24) {
        char kind = restKind;
        if (layout && *layout) {
            if (*layout == '*') {
                restKind = layout[1];
                layout = "";
                continue;
            }
            kind = *layout++;
        }
        int written;
        if (kind == 'b') {
            written = snprintf(out + pos, size - pos, " %u", data[offset]);
            offset += 1;
        }
        else {
            if (offset + 4 > input->size) break;
            uint32_t w;
            memcpy(&w, data + offset, 4);
            written = formatWord(out + pos, size - pos, w, kind);
            offset += 4;
        }
        if (written < 0) break;
        pos = MIN(pos + written, size - 1);
        values++;
    }
    if (offset < input->size && values >= maxWords && pos < size - 8) pos += snprintf(out + pos, size - pos, " …");
    /* Zeichenkette ab Wort 1, 2 oder 3 (Programm/Index davor) */
    for (int start = 4; start <= 12 && start < input->size; start += 4) {
        int len = 0;
        while (start + len < input->size && data[start + len] >= 0x20 && data[start + len] < 0x7f && len < 64) len++;
        if (len >= 3 && (start + len == input->size || data[start + len] == 0) && pos < size - len - 8) {
            snprintf(out + pos, size - pos, " \"%.*s\"", len, data + start);
            break;
        }
    }
}

/* Name, unter dem das Programm eine Textur kennt (der Server legt eigene GL-Namen an) */
static GLuint clientTextureId(const GLTexture* texture) {
    if (!texture || !currentRenderer) return 0;
    SparseArray* textures = currentRenderer->clientState.textures;
    for (int i = 0; i < textures->size; i++) {
        if (textures->entries[i].value == texture) return textures->entries[i].key;
    }
    return 0;
}

static bool hasAttachment(const FBAttachmentInfo* info) {
    return info && info->type && info->id;
}

static void describeAttachment(const FBAttachmentInfo* info, char* out, int size) {
    if (!hasAttachment(info)) {
        snprintf(out, size, "-");
        return;
    }
    if (info->type == GL_RENDERBUFFER) {
        GLint previous = 0, width = 0, height = 0, format = 0, samples = 0;
        glGetIntegerv(GL_RENDERBUFFER_BINDING, &previous);
        glBindRenderbuffer(GL_RENDERBUFFER, info->id);
        glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &width);
        glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &height);
        glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_INTERNAL_FORMAT, &format);
        if (!GLES2) glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_SAMPLES, &samples);
        glBindRenderbuffer(GL_RENDERBUFFER, previous);
        if (samples > 1) snprintf(out, size, "Renderbuffer %u (%s %dx%d, %d Abtastwerte)", info->id, enumName(format), width, height, samples);
        else snprintf(out, size, "Renderbuffer %u (%s %dx%d)", info->id, enumName(format), width, height);
        return;
    }
    const GLTexture* texture = info->texture;
    if (!texture) {
        snprintf(out, size, "Textur GL %u (%s, Stufe %d)", info->id, enumName(info->type), info->level);
        return;
    }
    int width = MAX(1, texture->width >> info->level), height = MAX(1, texture->height >> info->level);
    const char* face = info->type >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && info->type <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z ? " Würfelseite" : "";
    snprintf(out, size, "Textur %u (%s %dx%d, Stufe %d%s)", clientTextureId(texture), enumName(texture->originFormat), width, height,
             info->level, face);
}

static GLuint currentDrawFramebuffer(void) {
    GLuint id = currentRenderer->clientState.framebuffer[indexOfGLTarget(GL_DRAW_FRAMEBUFFER)];
    return id ? id : currentRenderer->displayBuffer;
}

static void describeFramebuffer(GLuint id, char* out, int size) {
    GLRenderer* renderer = currentRenderer;
    GLFramebuffer* framebuffer = SparseArray_get(renderer->clientState.framebuffers, id);
    int pos = snprintf(out, size, "FBO %u%s", id, id == renderer->displayBuffer ? " (Fensterbild)" : "");
    if (!framebuffer || pos >= size) return;
    char part[160];
    for (int i = 0; i < MAX_FB_COLOR_ATTACHMENTS && pos < size; i++) {
        if (!hasAttachment(&framebuffer->colorAttachment[i])) continue;
        describeAttachment(&framebuffer->colorAttachment[i], part, sizeof(part));
        pos += snprintf(out + pos, size - pos, ", Farbe%d %s", i, part);
    }
    if (pos < size && hasAttachment(&framebuffer->depthAttachment)) {
        describeAttachment(&framebuffer->depthAttachment, part, sizeof(part));
        pos += snprintf(out + pos, size - pos, ", Tiefe %s", part);
    }
    if (pos < size && hasAttachment(&framebuffer->stencilAttachment) &&
        (framebuffer->stencilAttachment.id != framebuffer->depthAttachment.id || framebuffer->stencilAttachment.type != framebuffer->depthAttachment.type)) {
        describeAttachment(&framebuffer->stencilAttachment, part, sizeof(part));
        snprintf(out + pos, size - pos, ", Stencil %s", part);
    }
}

/* mehrzeiligen Text zeilenweise ins Protokoll */
static void printLines(const char* prefix, const char* text, int maxLines) {
    int lines = 0;
    while (text && *text && lines < maxLines) {
        const char* end = strchr(text, '\n');
        int len = end ? (int)(end - text) : (int)strlen(text);
        if (len > 0) {
            println("%s%.*s", prefix, len > 900 ? 900 : len, text);
            lines++;
        }
        text = end ? end + 1 : NULL;
    }
}

static FILE* openDiagFile(char* path, int size, const char* name) {
    if (!diagDir[0]) return NULL;
    snprintf(path, size, "%s/%s", diagDir, name);
    return fopen(path, "w");
}

static void writeNumberedSource(FILE* file, const char* source) {
    int line = 1;
    while (source && *source) {
        const char* end = strchr(source, '\n');
        int len = end ? (int)(end - source) : (int)strlen(source);
        fprintf(file, "%4d  %.*s\n", line++, len, source);
        source = end ? end + 1 : NULL;
    }
}

static char* shaderInfoLog(GLuint shader) {
    GLint length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
    char* log = calloc(1, length > 0 ? length + 1 : 2);
    if (length > 0) glGetShaderInfoLog(shader, length + 1, NULL, log);
    return log;
}

static char* programInfoLog(GLuint program) {
    GLint length = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
    char* log = calloc(1, length > 0 ? length + 1 : 2);
    if (length > 0) glGetProgramInfoLog(program, length + 1, NULL, log);
    return log;
}

static char* shaderSource(GLuint shader) {
    GLint length = 0;
    glGetShaderiv(shader, GL_SHADER_SOURCE_LENGTH, &length);
    char* source = calloc(1, length > 0 ? length + 1 : 2);
    if (length > 0) glGetShaderSource(shader, length + 1, NULL, source);
    return source;
}

void GladioDiag_setDir(const char* dir) {
    pthread_mutex_lock(&diagMutex);
    snprintf(diagDir, sizeof(diagDir), "%s", dir ? dir : "");
    pthread_mutex_unlock(&diagMutex);
}

/* ---------------------------------------------------------------- Shader- und Bindefehler */

void GladioDiag_shaderCompileFailed(struct ShaderObject* shader, GLuint programId, const char* source) {
    pthread_mutex_lock(&diagMutex);
    uint32_t number = ++gladio_diagStats.compileErrors;
    pthread_mutex_unlock(&diagMutex);
    if (number > MAX_LOGGED_COMPILE_ERRORS) return;

    char* log = shaderInfoLog(shader->id);
    const char* type = shader->type == GL_VERTEX_SHADER ? "Vertex-Shader" : shader->type == GL_FRAGMENT_SHADER ? "Fragment-Shader" : enumName(shader->type);
    println("gladio: %s %u (Programm %u) vom Treiber nicht übersetzt:", type, shader->id, programId);
    printLines("gladio:   ", log, 30);

    char path[640], name[64];
    snprintf(name, sizeof(name), "gladio-shaderfehler-%u.txt", number);
    FILE* file = openDiagFile(path, sizeof(path), name);
    if (file) {
        fprintf(file, "%s %u (Programm %u), Quelltext nach der Umsetzung für OpenGL ES:\n\n%s\n\n", type, shader->id, programId, log);
        writeNumberedSource(file, source);
        fclose(file);
        println("gladio:   Quelltext: %s", path);
    }
    else if (number <= 2) printLines("gladio:   | ", source, 400);
    free(log);
}

void GladioDiag_programLinkFailed(struct ShaderProgram* program) {
    pthread_mutex_lock(&diagMutex);
    uint32_t number = ++gladio_diagStats.linkErrors;
    pthread_mutex_unlock(&diagMutex);
    if (number > MAX_LOGGED_LINK_ERRORS) return;

    char* log = programInfoLog(program->id);
    char shaders[128] = {0};
    int pos = 0;
    for (int i = 0; i < program->attachedShaders.size && pos < (int)sizeof(shaders) - 24; i++) {
        ShaderObject* shader = program->attachedShaders.elements[i];
        pos += snprintf(shaders + pos, sizeof(shaders) - pos, "%s%u%s", i ? ", " : "", shader->id,
                        shader->compileStatus == COMPILE_STATUS_ERROR ? " (nicht übersetzt)" : "");
    }
    println("gladio: Programm %u vom Treiber nicht gebunden (Shader %s):", program->id, shaders);
    printLines("gladio:   ", log, 30);

    char path[640], name[64];
    snprintf(name, sizeof(name), "gladio-bindefehler-%u.txt", number);
    FILE* file = openDiagFile(path, sizeof(path), name);
    if (file) {
        fprintf(file, "Programm %u, Shader %s\n\n%s\n", program->id, shaders, log);
        for (int i = 0; i < program->attachedShaders.size; i++) {
            ShaderObject* shader = program->attachedShaders.elements[i];
            char* source = shaderSource(shader->id);
            fprintf(file, "\n===== %s %u =====\n", shader->type == GL_VERTEX_SHADER ? "Vertex-Shader" : "Fragment-Shader", shader->id);
            writeNumberedSource(file, source);
            free(source);
        }
        fclose(file);
        println("gladio:   Quelltexte: %s", path);
    }
    free(log);
}

/* ---------------------------------------------------------------- GL-Fehler */

static struct {
    GLenum error;
    short request;
    uint8_t count;
} errorKeys[MAX_ERROR_KEYS];
static int errorKeyCount = 0;
static int errorLines = 0;

/* je (Fehler, Anfrage) höchstens zwei Meldungen, insgesamt MAX_ERROR_LINES */
static bool shouldLogError(GLenum error, short request) {
    bool log = false;
    pthread_mutex_lock(&diagMutex);
    int i;
    for (i = 0; i < errorKeyCount; i++) {
        if (errorKeys[i].error == error && errorKeys[i].request == request) break;
    }
    if (i == errorKeyCount && errorKeyCount < MAX_ERROR_KEYS) {
        errorKeys[i].error = error;
        errorKeys[i].request = request;
        errorKeys[i].count = 0;
        errorKeyCount++;
    }
    if (i < errorKeyCount && errorKeys[i].count < 2 && errorLines < MAX_ERROR_LINES) {
        errorKeys[i].count++;
        errorLines++;
        log = true;
    }
    pthread_mutex_unlock(&diagMutex);
    return log;
}

static void reportError(GLContext* context, short requestCode, GLenum error, bool exact) {
    gladio_diagStats.glErrors++;
    if (context->diag.pendingError == GL_NO_ERROR) context->diag.pendingError = error;

#if IS_DEBUG_ENABLED(DEBUG_MODE_GL_ERROR)
    println("gladio: glError %s %x", requestCodeToString(requestCode), error);
    return;
#endif

    if (exact) {
        if (shouldLogError(error, requestCode)) {
            char args[320];
            formatArgs(requestCode, &context->inputBuffer, args, sizeof(args), 12);
            println("gladio: GL-Fehler %s bei %s(%d)%s – Bild %u", errorName(error), requestName(requestCode), context->inputBuffer.size, args,
                    context->diag.frames);
        }
        return;
    }

    if (shouldLogError(error, (short)-requestCode)) {
        println("gladio: GL-Fehler %s bis %s – Bild %u", errorName(error), requestName(requestCode), context->diag.frames);
        gladio_printLastRequests(context, "gladio:   davor");
    }
    /* Auslöser suchen: eine Weile nach jeder Anfrage prüfen */
    if (context->diag.huntUntilFrame <= context->diag.frames &&
        (!context->diag.hunted || context->diag.frames - context->diag.lastHuntFrame >= HUNT_INTERVAL_FRAMES)) {
        context->diag.hunted = true;
        context->diag.lastHuntFrame = context->diag.frames;
        context->diag.huntUntilFrame = context->diag.frames + HUNT_FRAMES;
        println("gladio: prüfe %d Bilder lang nach jeder Anfrage auf GL-Fehler", HUNT_FRAMES);
    }
}

GLenum GladioDiag_takePendingError(GLContext* context) {
    GLenum error = context->diag.pendingError;
    context->diag.pendingError = GL_NO_ERROR;
    return error;
}

/* ---------------------------------------------------------------- Zeichenziel */

static bool changesFramebuffer(short requestCode) {
    switch (requestCode) {
        case REQUEST_CODE_GL_BIND_FRAMEBUFFER:
        case REQUEST_CODE_GL_FRAMEBUFFER_RENDERBUFFER:
        case REQUEST_CODE_GL_FRAMEBUFFER_TEXTURE:
        case REQUEST_CODE_GL_FRAMEBUFFER_TEXTURE2D:
        case REQUEST_CODE_GL_FRAMEBUFFER_TEXTURE3D:
        case REQUEST_CODE_GL_FRAMEBUFFER_TEXTURE_LAYER:
        case REQUEST_CODE_GL_DRAW_BUFFER:
        case REQUEST_CODE_GL_DRAW_BUFFERS:
        case REQUEST_CODE_GL_RENDERBUFFER_STORAGE:
        case REQUEST_CODE_GL_RENDERBUFFER_STORAGE_MULTISAMPLE:
        case REQUEST_CODE_GL_DELETE_FRAMEBUFFERS:
        case REQUEST_CODE_GL_DELETE_RENDERBUFFERS:
        case REQUEST_CODE_GL_DELETE_TEXTURES:
        case REQUEST_CODE_SET_CURRENT_RENDER_WINDOW:
            return true;
        default:
            return requestCode >= REQUEST_CODE_GL_DSA_START && requestCode < REQUEST_CODE_GL_CALL_START;
    }
}

static void checkFramebuffer(GLContext* context, short requestCode) {
    GLenum status = glCheckFramebufferStatus(GLES2 ? GL_FRAMEBUFFER : GL_DRAW_FRAMEBUFFER);
    if (status == GL_FRAMEBUFFER_COMPLETE || status == 0) return;
    pthread_mutex_lock(&diagMutex);
    uint32_t number = ++gladio_diagStats.incompleteFramebuffers;
    pthread_mutex_unlock(&diagMutex);
    if (number > MAX_LOGGED_FRAMEBUFFERS) return;
    char description[640];
    describeFramebuffer(currentDrawFramebuffer(), description, sizeof(description));
    println("gladio: Zeichenziel unvollständig (%s) bei %s – Bild %u: %s", framebufferStatusName(status), requestName(requestCode),
            context->diag.frames, description);
}

/* ---------------------------------------------------------------- PNG */

static void putBE32(uint8_t* p, uint32_t v) {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

static void pngChunk(FILE* file, const char* type, const uint8_t* data, uint32_t length) {
    uint8_t header[8];
    putBE32(header, length);
    memcpy(header + 4, type, 4);
    fwrite(header, 1, 8, file);
    if (length) fwrite(data, 1, length, file);
    uLong crc = crc32(0L, (const Bytef*)type, 4);
    if (length) crc = crc32(crc, data, length);
    uint8_t trailer[4];
    putBE32(trailer, (uint32_t)crc);
    fwrite(trailer, 1, 4, file);
}

/* RGB, Zeilen von oben nach unten; Filter „Up“ (weiche Verläufe packen so deutlich besser) */
static bool writePng(const char* path, int width, int height, const uint8_t* rgb) {
    size_t stride = (size_t)width * 3;
    size_t rawSize = (stride + 1) * height;
    uint8_t* raw = malloc(rawSize);
    if (!raw) return false;
    for (int y = 0; y < height; y++) {
        uint8_t* row = raw + y * (stride + 1);
        const uint8_t* src = rgb + y * stride;
        const uint8_t* above = y > 0 ? src - stride : NULL;
        row[0] = 2;
        for (size_t x = 0; x < stride; x++) row[1 + x] = (uint8_t)(src[x] - (above ? above[x] : 0));
    }
    uLongf packedSize = compressBound(rawSize);
    uint8_t* packed = malloc(packedSize);
    bool ok = packed && compress2(packed, &packedSize, raw, rawSize, 6) == Z_OK;
    FILE* file = ok ? fopen(path, "wb") : NULL;
    if (file) {
        static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
        uint8_t ihdr[13];
        putBE32(ihdr, width);
        putBE32(ihdr + 4, height);
        ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
        fwrite(signature, 1, 8, file);
        pngChunk(file, "IHDR", ihdr, 13);
        pngChunk(file, "IDAT", packed, (uint32_t)packedSize);
        pngChunk(file, "IEND", NULL, 0);
        fclose(file);
    }
    free(raw);
    free(packed);
    return file != NULL;
}

/* ---------------------------------------------------------------- Aufzeichnung */

#define CAPTURE_MAX_TRACE_LINES 400000
#define CAPTURE_MAX_IMAGES 240
#define CAPTURE_MAX_PROGRAMS 1024
#define CAPTURE_IMAGE_WIDTH 400
#define CAPTURE_UNIFORM_DRAWS 8

typedef struct ProgramInfo {
    GLuint id;
    int samplerCount;
    struct { char name[40]; GLenum type; GLint location; } samplers[32];
    int attribCount;
    struct { char name[40]; GLenum type; GLint location; } attribs[16];
    int uniformCount;
    struct { char name[40]; GLenum type; int count; GLint location[16]; } uniforms[10];
} ProgramInfo;

static struct {
    volatile int requested;
    char requestDir[512];
    bool active;
    GLContext* context;
    char dir[640];
    FILE* trace;
    uint32_t requests, draws, images, errors;
    uint32_t passDraws, passContent, passIndex;
    GLuint passFbo;
    GLuint programs[CAPTURE_MAX_PROGRAMS];
    int programCount;
    ProgramInfo* infos;
    int infoCount, infoCapacity;
    char lastState[640];
    char lastSamplers[2048];
    char lastAttribs[2048];
    float* pixels;
    size_t pixelsSize;
    uint32_t startFrame;
    int64_t startNs;
} capture;

void GladioDiag_requestCapture(const char* dir) {
    pthread_mutex_lock(&diagMutex);
    snprintf(capture.requestDir, sizeof(capture.requestDir), "%s", dir && dir[0] ? dir : diagDir);
    pthread_mutex_unlock(&diagMutex);
    __atomic_store_n(&capture.requested, 1, __ATOMIC_RELEASE);
    println("gladio: Aufzeichnung angefordert (beginnt mit dem nächsten Bild)");
}

static ProgramInfo* programInfo(GLuint program) {
    for (int i = 0; i < capture.infoCount; i++) {
        if (capture.infos[i].id == program) return &capture.infos[i];
    }
    if (capture.infoCount == capture.infoCapacity) {
        int capacity = capture.infoCapacity ? capture.infoCapacity * 2 : 64;
        ProgramInfo* infos = realloc(capture.infos, capacity * sizeof(ProgramInfo));
        if (!infos) return NULL;
        capture.infos = infos;
        capture.infoCapacity = capacity;
    }
    ProgramInfo* info = &capture.infos[capture.infoCount++];
    memset(info, 0, sizeof(*info));
    info->id = program;

    GLint count = 0;
    glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &count);
    for (int i = 0; i < count; i++) {
        char name[64];
        GLint size = 0;
        GLenum type = 0;
        glGetActiveUniform(program, i, sizeof(name), NULL, &size, &type, name);
        char* bracket = strchr(name, '[');
        if (bracket) *bracket = '\0';
        bool sampler = type == GL_SAMPLER_2D || type == GL_SAMPLER_CUBE || type == GL_SAMPLER_3D || type == GL_SAMPLER_2D_SHADOW ||
                       type == GL_SAMPLER_2D_ARRAY || type == GL_SAMPLER_CUBE_SHADOW || type == GL_SAMPLER_2D_ARRAY_SHADOW;
        if (sampler) {
            for (int e = 0; e < size && info->samplerCount < 32; e++) {
                char element[64];
                if (size > 1) snprintf(element, sizeof(element), "%s[%d]", name, e);
                else snprintf(element, sizeof(element), "%s", name);
                snprintf(info->samplers[info->samplerCount].name, 40, "%s", element);
                info->samplers[info->samplerCount].type = type;
                info->samplers[info->samplerCount].location = glGetUniformLocation(program, element);
                info->samplerCount++;
            }
        }
        else if ((type == GL_FLOAT || type == GL_FLOAT_VEC2 || type == GL_FLOAT_VEC3 || type == GL_FLOAT_VEC4) && info->uniformCount < 10) {
            typeof(info->uniforms[0])* uniform = &info->uniforms[info->uniformCount++];
            snprintf(uniform->name, sizeof(uniform->name), "%s", name);
            uniform->type = type;
            uniform->count = MIN(size, 16);
            for (int e = 0; e < uniform->count; e++) {
                char element[64];
                if (size > 1) snprintf(element, sizeof(element), "%s[%d]", name, e);
                else snprintf(element, sizeof(element), "%s", name);
                uniform->location[e] = glGetUniformLocation(program, element);
            }
        }
    }
    count = 0;
    glGetProgramiv(program, GL_ACTIVE_ATTRIBUTES, &count);
    for (int i = 0; i < count && info->attribCount < 16; i++) {
        char name[64];
        GLint size = 0;
        GLenum type = 0;
        glGetActiveAttrib(program, i, sizeof(name), NULL, &size, &type, name);
        if (strncmp(name, "gl_", 3) == 0) continue;
        snprintf(info->attribs[info->attribCount].name, 40, "%s", name);
        info->attribs[info->attribCount].type = type;
        info->attribs[info->attribCount].location = glGetAttribLocation(program, name);
        info->attribCount++;
    }
    return info;
}

static GLenum samplerTarget(GLenum type) {
    switch (type) {
        case GL_SAMPLER_CUBE:
        case GL_SAMPLER_CUBE_SHADOW: return GL_TEXTURE_CUBE_MAP;
        case GL_SAMPLER_3D: return GL_TEXTURE_3D;
        case GL_SAMPLER_2D_ARRAY:
        case GL_SAMPLER_2D_ARRAY_SHADOW: return GL_TEXTURE_2D_ARRAY;
        default: return GL_TEXTURE_2D;
    }
}

static const char* filterName(GLint filter) {
    switch (filter) {
        case GL_NEAREST: return "N";
        case GL_LINEAR: return "L";
        case GL_NEAREST_MIPMAP_NEAREST: return "NmN";
        case GL_LINEAR_MIPMAP_NEAREST: return "LmN";
        case GL_NEAREST_MIPMAP_LINEAR: return "NmL";
        case GL_LINEAR_MIPMAP_LINEAR: return "LmL";
        default: return enumName(filter);
    }
}

static const char* wrapName(GLint wrap) {
    switch (wrap) {
        case GL_REPEAT: return "R";
        case GL_CLAMP_TO_EDGE: return "C";
        case GL_MIRRORED_REPEAT: return "M";
        case GL_CLAMP_TO_BORDER: return "B";
        default: return enumName(wrap);
    }
}

static void appendf(char* out, int size, int* pos, const char* format, ...) {
    if (*pos >= size - 1) return;
    va_list args;
    va_start(args, format);
    int written = vsnprintf(out + *pos, size - *pos, format, args);
    va_end(args);
    if (written > 0) *pos = MIN(*pos + written, size - 1);
}

static void captureSamplers(ProgramInfo* info, char* out, int size) {
    int pos = 0;
    GLClientState* clientState = &currentRenderer->clientState;
    for (int i = 0; i < info->samplerCount; i++) {
        GLint unit = -1;
        if (info->samplers[i].location >= 0) glGetUniformiv(info->id, info->samplers[i].location, &unit);
        if (unit < 0 || unit >= MAX_TEXTURES) {
            appendf(out, size, &pos, " %s=E%d", info->samplers[i].name, unit);
            continue;
        }
        GLenum target = samplerTarget(info->samplers[i].type);
        uint8_t targetIndex = indexOfGLTarget(target);
        GLTexture* texture = targetIndex < MAX_TEXTURE_TARGETS ? clientState->texture[unit][targetIndex] : NULL;
        appendf(out, size, &pos, " %s=E%d:", info->samplers[i].name, unit);
        if (!texture) {
            appendf(out, size, &pos, "keine");
            continue;
        }
        appendf(out, size, &pos, "T%u(%s %dx%d", clientTextureId(texture), enumName(texture->originFormat), texture->width, texture->height);
        if (!GLES2) {
            GLint sampler = 0, minFilter = 0, magFilter = 0, wrapS = 0, wrapT = 0, compareMode = 0, compareFunc = 0, maxLevel = 0;
            glActiveTexture(GL_TEXTURE0 + unit);
            glGetIntegerv(GL_SAMPLER_BINDING, &sampler);
            if (sampler) {
                glGetSamplerParameteriv(sampler, GL_TEXTURE_MIN_FILTER, &minFilter);
                glGetSamplerParameteriv(sampler, GL_TEXTURE_MAG_FILTER, &magFilter);
                glGetSamplerParameteriv(sampler, GL_TEXTURE_WRAP_S, &wrapS);
                glGetSamplerParameteriv(sampler, GL_TEXTURE_WRAP_T, &wrapT);
                glGetSamplerParameteriv(sampler, GL_TEXTURE_COMPARE_MODE, &compareMode);
                glGetSamplerParameteriv(sampler, GL_TEXTURE_COMPARE_FUNC, &compareFunc);
            }
            else {
                glGetTexParameteriv(target, GL_TEXTURE_MIN_FILTER, &minFilter);
                glGetTexParameteriv(target, GL_TEXTURE_MAG_FILTER, &magFilter);
                glGetTexParameteriv(target, GL_TEXTURE_WRAP_S, &wrapS);
                glGetTexParameteriv(target, GL_TEXTURE_WRAP_T, &wrapT);
                glGetTexParameteriv(target, GL_TEXTURE_COMPARE_MODE, &compareMode);
                glGetTexParameteriv(target, GL_TEXTURE_COMPARE_FUNC, &compareFunc);
            }
            glGetTexParameteriv(target, GL_TEXTURE_MAX_LEVEL, &maxLevel);
            appendf(out, size, &pos, " %s/%s %s%s", filterName(minFilter), filterName(magFilter), wrapName(wrapS), wrapName(wrapT));
            if (maxLevel != 1000) appendf(out, size, &pos, " max%d", maxLevel);
            if (compareMode == GL_COMPARE_REF_TO_TEXTURE) appendf(out, size, &pos, " Vgl %s", enumName(compareFunc));
            if (sampler) appendf(out, size, &pos, " S%d", sampler);
        }
        appendf(out, size, &pos, ")");
    }
    if (!GLES2) glActiveTexture(GL_TEXTURE0 + clientState->activeTexture);
    if (pos == 0) appendf(out, size, &pos, " keine");
}

static void captureAttribs(ProgramInfo* info, char* out, int size) {
    int pos = 0;
    for (int i = 0; i < info->attribCount; i++) {
        GLint location = info->attribs[i].location;
        if (location < 0) continue;
        GLint enabled = 0;
        glGetVertexAttribiv(location, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled);
        appendf(out, size, &pos, " %s@%d=", info->attribs[i].name, location);
        if (!enabled) {
            GLfloat value[4] = {0};
            glGetVertexAttribfv(location, GL_CURRENT_VERTEX_ATTRIB, value);
            appendf(out, size, &pos, "konst(%.4g,%.4g,%.4g,%.4g)", value[0], value[1], value[2], value[3]);
            continue;
        }
        GLint components = 0, type = 0, normalized = 0, stride = 0, buffer = 0, divisor = 0, integer = 0;
        void* pointer = NULL;
        glGetVertexAttribiv(location, GL_VERTEX_ATTRIB_ARRAY_SIZE, &components);
        glGetVertexAttribiv(location, GL_VERTEX_ATTRIB_ARRAY_TYPE, &type);
        glGetVertexAttribiv(location, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &normalized);
        glGetVertexAttribiv(location, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &stride);
        glGetVertexAttribiv(location, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &buffer);
        if (!GLES2) {
            glGetVertexAttribiv(location, GL_VERTEX_ATTRIB_ARRAY_DIVISOR, &divisor);
            glGetVertexAttribiv(location, GL_VERTEX_ATTRIB_ARRAY_INTEGER, &integer);
        }
        glGetVertexAttribPointerv(location, GL_VERTEX_ATTRIB_ARRAY_POINTER, &pointer);
        appendf(out, size, &pos, "%dx%s%s%s/%d+%lu P%d", components, enumName(type), normalized ? "n" : "", integer ? "i" : "", stride,
                (unsigned long)(uintptr_t)pointer, buffer);
        if (divisor) appendf(out, size, &pos, " div%d", divisor);
    }
    if (pos == 0) appendf(out, size, &pos, " keine");
}

static void captureUniforms(ProgramInfo* info) {
    for (int i = 0; i < info->uniformCount; i++) {
        typeof(info->uniforms[0])* uniform = &info->uniforms[i];
        int components = uniform->type == GL_FLOAT ? 1 : uniform->type == GL_FLOAT_VEC2 ? 2 : uniform->type == GL_FLOAT_VEC3 ? 3 : 4;
        char line[2048];
        int pos = 0;
        appendf(line, sizeof(line), &pos, "    Uniform %s:", uniform->name);
        for (int e = 0; e < uniform->count; e++) {
            if (uniform->location[e] < 0) continue;
            GLfloat value[4] = {0};
            glGetUniformfv(info->id, uniform->location[e], value);
            appendf(line, sizeof(line), &pos, " (");
            for (int c = 0; c < components; c++) appendf(line, sizeof(line), &pos, c ? ",%.4g" : "%.4g", value[c]);
            appendf(line, sizeof(line), &pos, ")");
        }
        fprintf(capture.trace, "%s\n", line);
    }
}

static const char* blendFactorName(GLint factor) {
    switch (factor) {
        case GL_ZERO: return "0";
        case GL_ONE: return "1";
        case GL_SRC_COLOR: return "Sc";
        case GL_ONE_MINUS_SRC_COLOR: return "1-Sc";
        case GL_SRC_ALPHA: return "Sa";
        case GL_ONE_MINUS_SRC_ALPHA: return "1-Sa";
        case GL_DST_ALPHA: return "Da";
        case GL_ONE_MINUS_DST_ALPHA: return "1-Da";
        case GL_DST_COLOR: return "Dc";
        case GL_ONE_MINUS_DST_COLOR: return "1-Dc";
        case GL_SRC_ALPHA_SATURATE: return "SaSat";
        case GL_CONSTANT_COLOR: return "Kc";
        case GL_ONE_MINUS_CONSTANT_COLOR: return "1-Kc";
        case GL_CONSTANT_ALPHA: return "Ka";
        case GL_ONE_MINUS_CONSTANT_ALPHA: return "1-Ka";
        default: return enumName(factor);
    }
}

static const char* funcName(GLint func) {
    switch (func) {
        case GL_NEVER: return "NEVER";
        case GL_LESS: return "LESS";
        case GL_EQUAL: return "EQUAL";
        case GL_LEQUAL: return "LEQUAL";
        case GL_GREATER: return "GREATER";
        case GL_NOTEQUAL: return "NOTEQUAL";
        case GL_GEQUAL: return "GEQUAL";
        case GL_ALWAYS: return "ALWAYS";
        default: return enumName(func);
    }
}

static void captureState(char* out, int size) {
    int pos = 0;
    GLint program = 0, viewport[4] = {0}, scissor[4] = {0};
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_VIEWPORT, viewport);
    appendf(out, size, &pos, "Programm %d; Ansicht %d,%d %dx%d", program, viewport[0], viewport[1], viewport[2], viewport[3]);
    if (glIsEnabled(GL_SCISSOR_TEST)) {
        glGetIntegerv(GL_SCISSOR_BOX, scissor);
        appendf(out, size, &pos, "; Schere %d,%d %dx%d", scissor[0], scissor[1], scissor[2], scissor[3]);
    }
    if (glIsEnabled(GL_BLEND)) {
        GLint srcRGB = 0, dstRGB = 0, srcA = 0, dstA = 0, eqRGB = 0, eqA = 0;
        glGetIntegerv(GL_BLEND_SRC_RGB, &srcRGB);
        glGetIntegerv(GL_BLEND_DST_RGB, &dstRGB);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &srcA);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &dstA);
        glGetIntegerv(GL_BLEND_EQUATION_RGB, &eqRGB);
        glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &eqA);
        appendf(out, size, &pos, "; Mischen %s(%s,%s)", eqRGB == GL_FUNC_ADD ? "" : enumName(eqRGB), blendFactorName(srcRGB), blendFactorName(dstRGB));
        if (srcA != srcRGB || dstA != dstRGB || eqA != eqRGB)
            appendf(out, size, &pos, " Alpha %s(%s,%s)", eqA == GL_FUNC_ADD ? "" : enumName(eqA), blendFactorName(srcA), blendFactorName(dstA));
    }
    else appendf(out, size, &pos, "; Mischen aus");
    GLboolean mask[4] = {1, 1, 1, 1};
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    appendf(out, size, &pos, "; Farbmaske %c%c%c%c", mask[0] ? 'R' : '-', mask[1] ? 'G' : '-', mask[2] ? 'B' : '-', mask[3] ? 'A' : '-');
    if (glIsEnabled(GL_DEPTH_TEST)) {
        GLint func = 0;
        GLboolean write = 0;
        glGetIntegerv(GL_DEPTH_FUNC, &func);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &write);
        appendf(out, size, &pos, "; Tiefe %s%s", funcName(func), write ? " schreibt" : "");
    }
    else appendf(out, size, &pos, "; Tiefe aus");
    if (glIsEnabled(GL_STENCIL_TEST)) {
        GLint func = 0, ref = 0, valueMask = 0, fail = 0, zfail = 0, zpass = 0, writeMask = 0;
        glGetIntegerv(GL_STENCIL_FUNC, &func);
        glGetIntegerv(GL_STENCIL_REF, &ref);
        glGetIntegerv(GL_STENCIL_VALUE_MASK, &valueMask);
        glGetIntegerv(GL_STENCIL_FAIL, &fail);
        glGetIntegerv(GL_STENCIL_PASS_DEPTH_FAIL, &zfail);
        glGetIntegerv(GL_STENCIL_PASS_DEPTH_PASS, &zpass);
        glGetIntegerv(GL_STENCIL_WRITEMASK, &writeMask);
        appendf(out, size, &pos, "; Stencil %s ref %d maske 0x%x ops %s/%s/%s schreibt 0x%x", funcName(func), ref, valueMask & 0xff, enumName(fail),
                enumName(zfail), enumName(zpass), writeMask & 0xff);
    }
    if (glIsEnabled(GL_CULL_FACE)) {
        GLint face = 0, front = 0;
        glGetIntegerv(GL_CULL_FACE_MODE, &face);
        glGetIntegerv(GL_FRONT_FACE, &front);
        appendf(out, size, &pos, "; Cull %s/%s", enumName(face), enumName(front));
    }
    if (glIsEnabled(GL_POLYGON_OFFSET_FILL)) {
        GLfloat factor = 0, units = 0;
        glGetFloatv(GL_POLYGON_OFFSET_FACTOR, &factor);
        glGetFloatv(GL_POLYGON_OFFSET_UNITS, &units);
        appendf(out, size, &pos, "; Versatz %g/%g", factor, units);
    }
    if (!GLES2) {
        appendf(out, size, &pos, "; Zeichenpuffer");
        for (int i = 0; i < 4; i++) {
            GLint buffer = GL_NONE;
            glGetIntegerv(GL_DRAW_BUFFER0 + i, &buffer);
            if (buffer == GL_NONE) continue;
            if (buffer >= GL_COLOR_ATTACHMENT0 && buffer <= GL_COLOR_ATTACHMENT15) appendf(out, size, &pos, " %d:F%d", i, buffer - GL_COLOR_ATTACHMENT0);
            else appendf(out, size, &pos, " %d:%s", i, enumName(buffer));
        }
    }
    GLint fbo = 0;
    glGetIntegerv(GLES2 ? GL_FRAMEBUFFER_BINDING : GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
    appendf(out, size, &pos, "; GL-FBO %d", fbo);
}

static void addProgram(GLuint program) {
    if (!program) return;
    for (int i = 0; i < capture.programCount; i++) {
        if (capture.programs[i] == program) return;
    }
    if (capture.programCount < CAPTURE_MAX_PROGRAMS) capture.programs[capture.programCount++] = program;
}

static void captureDraw(GLContext* context) {
    FILE* trace = capture.trace;
    char text[2048];
    captureState(text, sizeof(text));
    if (strcmp(text, capture.lastState) != 0) {
        fprintf(trace, "    Zustand: %s\n", text);
        snprintf(capture.lastState, sizeof(capture.lastState), "%s", text);
    }
    GLint program = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    if (!program) return;
    addProgram(program);
    ProgramInfo* info = programInfo(program);
    if (!info) return;
    captureSamplers(info, text, sizeof(text));
    if (strcmp(text, capture.lastSamplers) != 0) {
        fprintf(trace, "    Sampler:%s\n", text);
        snprintf(capture.lastSamplers, sizeof(capture.lastSamplers), "%s", text);
    }
    captureAttribs(info, text, sizeof(text));
    if (strcmp(text, capture.lastAttribs) != 0) {
        fprintf(trace, "    Attribute:%s\n", text);
        snprintf(capture.lastAttribs, sizeof(capture.lastAttribs), "%s", text);
    }
    if (capture.passDraws <= CAPTURE_UNIFORM_DRAWS) captureUniforms(info);
}

static float halfToFloat(uint16_t h) {
    uint32_t sign = (h >> 15) & 1, exponent = (h >> 10) & 0x1f, mantissa = h & 0x3ff;
    float value;
    if (exponent == 0) value = ldexpf((float)mantissa, -24);
    else if (exponent == 31) value = mantissa ? NAN : INFINITY;
    else value = ldexpf((float)(mantissa | 0x400), (int)exponent - 25);
    return sign ? -value : value;
}

/* Farbanhang lesen (als RGBA-Gleitkomma in capture.pixels); 0 = gelesen */
static int readAttachment(GLuint fbo, int index, int width, int height, bool* isFloat, char* note, int noteSize) {
    size_t needed = (size_t)width * height * 4 * sizeof(float);
    if (needed > capture.pixelsSize) {
        float* pixels = realloc(capture.pixels, needed);
        if (!pixels) {
            snprintf(note, noteSize, "kein Speicher");
            return -1;
        }
        capture.pixels = pixels;
        capture.pixelsSize = needed;
    }
    while (glGetError() != GL_NO_ERROR);

    GLint previousFbo = 0, previousReadBuffer = GL_NONE, packBuffer = 0, packAlignment = 4, packRowLength = 0, packSkipPixels = 0, packSkipRows = 0;
    glGetIntegerv(GLES2 ? GL_FRAMEBUFFER_BINDING : GL_READ_FRAMEBUFFER_BINDING, &previousFbo);
    glGetIntegerv(GL_PACK_ALIGNMENT, &packAlignment);
    glBindFramebuffer(GLES2 ? GL_FRAMEBUFFER : GL_READ_FRAMEBUFFER, fbo);
    GLint componentType = GL_UNSIGNED_NORMALIZED;
    if (!GLES2) {
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packBuffer);
        glGetIntegerv(GL_PACK_ROW_LENGTH, &packRowLength);
        glGetIntegerv(GL_PACK_SKIP_PIXELS, &packSkipPixels);
        glGetIntegerv(GL_PACK_SKIP_ROWS, &packSkipRows);
        glGetIntegerv(GL_READ_BUFFER, &previousReadBuffer);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        glReadBuffer(GL_COLOR_ATTACHMENT0 + index);
        glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + index, GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &componentType);
    }
    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    int result = 0;
    size_t count = (size_t)width * height * 4;
    *isFloat = componentType == GL_FLOAT;
    if (componentType == GL_INT || componentType == GL_UNSIGNED_INT) {
        snprintf(note, noteSize, "Ganzzahlformat, nicht gelesen");
        result = -1;
    }
    else if (*isFloat) {
        glReadPixels(0, 0, width, height, GL_RGBA, GL_FLOAT, capture.pixels);
        GLenum error = glGetError();
        if (error != GL_NO_ERROR) {
            /* nur EXT_color_buffer_half_float: der Treiber liest dann womöglich nur als Halbgleitkomma */
            uint16_t* halves = (uint16_t*)capture.pixels;
            glReadPixels(0, 0, width, height, GL_RGBA, GL_HALF_FLOAT, halves);
            error = glGetError();
            if (error == GL_NO_ERROR) {
                for (size_t i = count; i-- > 0;) capture.pixels[i] = halfToFloat(halves[i]);
            }
            else {
                snprintf(note, noteSize, "glReadPixels: %s", errorName(error));
                result = -1;
            }
        }
    }
    else {
        uint8_t* bytes = (uint8_t*)capture.pixels;
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, bytes);
        GLenum error = glGetError();
        if (error == GL_NO_ERROR) {
            for (size_t i = count; i-- > 0;) capture.pixels[i] = bytes[i] / 255.0f;
        }
        else {
            snprintf(note, noteSize, "glReadPixels: %s", errorName(error));
            result = -1;
        }
    }

    if (!GLES2) {
        glReadBuffer(previousReadBuffer);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, packBuffer);
        glPixelStorei(GL_PACK_ROW_LENGTH, packRowLength);
        glPixelStorei(GL_PACK_SKIP_PIXELS, packSkipPixels);
        glPixelStorei(GL_PACK_SKIP_ROWS, packSkipRows);
    }
    glPixelStorei(GL_PACK_ALIGNMENT, packAlignment);
    glBindFramebuffer(GLES2 ? GL_FRAMEBUFFER : GL_READ_FRAMEBUFFER, previousFbo);
    while (glGetError() != GL_NO_ERROR);
    return result;
}

/* Wertebereiche ins Protokoll, Bild verkleinert als PNG: links Farbe (auf 0..1 begrenzt), rechts Alpha
 * (über 1 logarithmisch bis zum Größtwert); NaN/unendlich magenta */
static void writeImage(int width, int height, bool isFloat, const char* fileName) {
    const float* pixels = capture.pixels;
    float minimum[4] = {INFINITY, INFINITY, INFINITY, INFINITY}, maximum[4] = {-INFINITY, -INFINITY, -INFINITY, -INFINITY};
    double sum[4] = {0};
    size_t finite[4] = {0}, nans[4] = {0}, positiveInf[4] = {0}, negativeInf[4] = {0};
    size_t invalid = 0, count = (size_t)width * height;
    for (size_t p = 0; p < count; p++) {
        const float* px = pixels + p * 4;
        bool bad = false;
        for (int c = 0; c < 4; c++) {
            float v = px[c];
            if (!isfinite(v)) {
                bad = true;
                if (isnan(v)) nans[c]++;
                else if (v > 0) positiveInf[c]++;
                else negativeInf[c]++;
                continue;
            }
            if (v < minimum[c]) minimum[c] = v;
            if (v > maximum[c]) maximum[c] = v;
            sum[c] += v;
            finite[c]++;
        }
        if (bad) invalid++;
    }
    static const char channels[] = "RGBA";
    char text[640];
    int pos = 0;
    for (int c = 0; c < 4; c++) {
        /* Mittel über die endlichen Werte des Kanals */
        if (minimum[c] > maximum[c]) appendf(text, sizeof(text), &pos, " %c -", channels[c]);
        else appendf(text, sizeof(text), &pos, " %c %.4g..%.4g (Mittel %.4g)", channels[c], minimum[c], maximum[c], sum[c] / (finite[c] ? finite[c] : 1));
    }
    if (invalid) {
        appendf(text, sizeof(text), &pos, "; %zu Pixel NaN/unendlich:", invalid);
        for (int c = 0; c < 4; c++) {
            if (nans[c]) appendf(text, sizeof(text), &pos, " %c %zu NaN", channels[c], nans[c]);
            if (positiveInf[c]) appendf(text, sizeof(text), &pos, " %c %zu +unendlich", channels[c], positiveInf[c]);
            if (negativeInf[c]) appendf(text, sizeof(text), &pos, " %c %zu -unendlich", channels[c], negativeInf[c]);
        }
    }
    fprintf(capture.trace, "   %s%s → %s\n", isFloat ? "Gleitkomma:" : "", text, fileName);

    int scale = (width + CAPTURE_IMAGE_WIDTH - 1) / CAPTURE_IMAGE_WIDTH;
    if (scale < 1) scale = 1;
    int outWidth = MAX(1, width / scale), outHeight = MAX(1, height / scale);
    int gap = 4, imageWidth = outWidth * 2 + gap;
    uint8_t* rgb = calloc((size_t)imageWidth * outHeight * 3, 1);
    if (!rgb) return;
    float alphaMax = maximum[3] > 1.0001f && isfinite(maximum[3]) ? maximum[3] : 1.0f;
    bool alphaLog = alphaMax > 1.0f || minimum[3] < -0.0001f;
    float logScale = alphaLog ? 1.0f / log2f(1.0f + alphaMax) : 1.0f;
    for (int y = 0; y < outHeight; y++) {
        for (int x = 0; x < outWidth; x++) {
            float acc[4] = {0};
            bool bad = false;
            for (int sy = 0; sy < scale; sy++) {
                const float* row = pixels + ((size_t)(y * scale + sy) * width + x * scale) * 4;
                for (int sx = 0; sx < scale; sx++) {
                    for (int c = 0; c < 4; c++) {
                        float v = row[sx * 4 + c];
                        if (!isfinite(v)) bad = true;
                        else acc[c] += v;
                    }
                }
            }
            /* GL zählt Zeilen von unten */
            uint8_t* out = rgb + ((size_t)(outHeight - 1 - y) * imageWidth + x) * 3;
            uint8_t* outAlpha = out + (outWidth + gap) * 3;
            if (bad) {
                out[0] = 255; out[1] = 0; out[2] = 255;
                outAlpha[0] = 255; outAlpha[1] = 0; outAlpha[2] = 255;
                continue;
            }
            float n = (float)(scale * scale);
            for (int c = 0; c < 3; c++) {
                float v = acc[c] / n;
                out[c] = (uint8_t)(v <= 0 ? 0 : v >= 1 ? 255 : v * 255.0f + 0.5f);
            }
            float a = acc[3] / n;
            float g = alphaLog ? log2f(1.0f + MAX(a, 0.0f)) * logScale : a;
            uint8_t gray = (uint8_t)(g <= 0 ? 0 : g >= 1 ? 255 : g * 255.0f + 0.5f);
            outAlpha[0] = outAlpha[1] = outAlpha[2] = gray;
        }
        /* Trennstreifen */
        uint8_t* stripe = rgb + ((size_t)(outHeight - 1 - y) * imageWidth + outWidth) * 3;
        for (int i = 0; i < gap * 3; i++) stripe[i] = 0x40;
    }
    char path[800];
    snprintf(path, sizeof(path), "%s/%s", capture.dir, fileName);
    if (!writePng(path, imageWidth, outHeight, rgb)) fprintf(capture.trace, "   (PNG nicht geschrieben: %s)\n", strerror(errno));
    free(rgb);
}

static void attachmentSizeOf(GLuint fbo, const FBAttachmentInfo* info, int* width, int* height) {
    *width = *height = 0;
    if (fbo == currentRenderer->displayBuffer && (!hasAttachment(info) || !info->texture)) {
        /* Fensterbild: der Server hängt seine eigene Textur an (ohne Eintrag beim Programm) */
        *width = currentRenderer->displaySize[0];
        *height = currentRenderer->displaySize[1];
        return;
    }
    if (!hasAttachment(info)) return;
    if (info->type == GL_RENDERBUFFER) {
        GLint previous = 0;
        glGetIntegerv(GL_RENDERBUFFER_BINDING, &previous);
        glBindRenderbuffer(GL_RENDERBUFFER, info->id);
        glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, width);
        glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, height);
        glBindRenderbuffer(GL_RENDERBUFFER, previous);
    }
    else if (info->texture) {
        *width = MAX(1, info->texture->width >> info->level);
        *height = MAX(1, info->texture->height >> info->level);
    }
}

static void snapshotFramebuffer(GLuint fbo, const char* reason, uint32_t draws, uint32_t content) {
    if (capture.images >= CAPTURE_MAX_IMAGES) return;
    GLRenderer* renderer = currentRenderer;
    GLFramebuffer* framebuffer = SparseArray_get(renderer->clientState.framebuffers, fbo);
    char description[700];
    describeFramebuffer(fbo, description, sizeof(description));
    uint32_t number = capture.images++;
    fprintf(capture.trace, "== Bild %03u (%s): %s – %u Zeichenaufrufe, %u Löschungen/Blits\n", number, reason, description, draws, content);

    bool any = false;
    int colorAttachments = GLES2 ? 1 : 4;
    for (int i = 0; i < colorAttachments; i++) {
        FBAttachmentInfo* info = framebuffer ? &framebuffer->colorAttachment[i] : NULL;
        bool display = fbo == renderer->displayBuffer && i == 0;
        if (!display && !hasAttachment(info)) continue;
        int width, height;
        attachmentSizeOf(fbo, info, &width, &height);
        if (width <= 0 || height <= 0) {
            fprintf(capture.trace, "   Farbe%d: Größe unbekannt\n", i);
            continue;
        }
        bool isFloat = false;
        char note[128] = {0};
        if (readAttachment(fbo, i, width, height, &isFloat, note, sizeof(note)) != 0) {
            fprintf(capture.trace, "   Farbe%d: %s\n", i, note);
            continue;
        }
        char fileName[64];
        if (i == 0) snprintf(fileName, sizeof(fileName), "bild-%03u.png", number);
        else snprintf(fileName, sizeof(fileName), "bild-%03u-farbe%d.png", number, i);
        fprintf(capture.trace, "   Farbe%d %dx%d:", i, width, height);
        writeImage(width, height, isFloat, fileName);
        any = true;
    }
    if (!any && framebuffer && (hasAttachment(&framebuffer->depthAttachment) || hasAttachment(&framebuffer->stencilAttachment)))
        fprintf(capture.trace, "   (nur Tiefe/Stencil)\n");
}

static bool isPassBoundary(GLContext* context, short requestCode) {
    const int* args = (const int*)context->inputBuffer.buffer;
    int size = context->inputBuffer.size;
    switch (requestCode) {
        case REQUEST_CODE_GL_BIND_FRAMEBUFFER:
        case REQUEST_CODE_GL_FRAMEBUFFER_RENDERBUFFER:
        case REQUEST_CODE_GL_FRAMEBUFFER_TEXTURE:
        case REQUEST_CODE_GL_FRAMEBUFFER_TEXTURE2D:
        case REQUEST_CODE_GL_FRAMEBUFFER_TEXTURE3D:
        case REQUEST_CODE_GL_FRAMEBUFFER_TEXTURE_LAYER:
            return size >= 4 && args && (args[0] == GL_FRAMEBUFFER || args[0] == GL_DRAW_FRAMEBUFFER);
        case REQUEST_CODE_GL_DRAW_BUFFER:
        case REQUEST_CODE_GL_DRAW_BUFFERS:
        case REQUEST_CODE_SET_CURRENT_RENDER_WINDOW:
            return true;
        default:
            return false;
    }
}

static bool isClearRequest(short requestCode) {
    return requestCode == REQUEST_CODE_GL_CLEAR || requestCode == REQUEST_CODE_GL_CLEAR_BUFFERFI || requestCode == REQUEST_CODE_GL_CLEAR_BUFFERFV ||
           requestCode == REQUEST_CODE_GL_CLEAR_BUFFERIV || requestCode == REQUEST_CODE_GL_CLEAR_BUFFERUIV ||
           requestCode == REQUEST_CODE_GL_BLIT_FRAMEBUFFER || requestCode == REQUEST_CODE_GL_COPY_TEX_IMAGE2D;
}

static void writeShaders(void) {
    char path[800];
    snprintf(path, sizeof(path), "%s/shader.txt", capture.dir);
    FILE* file = fopen(path, "w");
    if (!file) return;
    fprintf(file, "Programme dieses Bilds (%d), Quelltext nach der Umsetzung für OpenGL ES\n", capture.programCount);
    for (int p = 0; p < capture.programCount; p++) {
        GLuint program = capture.programs[p];
        if (!glIsProgram(program)) {
            fprintf(file, "\n######## Programm %u (inzwischen gelöscht)\n", program);
            continue;
        }
        GLint linked = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        fprintf(file, "\n######## Programm %u%s\n", program, linked ? "" : " – NICHT GEBUNDEN");
        if (!linked) {
            char* log = programInfoLog(program);
            fprintf(file, "%s\n", log);
            free(log);
        }
        ProgramInfo* info = programInfo(program);
        if (info) {
            fprintf(file, "Attribute:");
            for (int i = 0; i < info->attribCount; i++) fprintf(file, " %s@%d", info->attribs[i].name, info->attribs[i].location);
            fprintf(file, "\nSampler:");
            for (int i = 0; i < info->samplerCount; i++) fprintf(file, " %s(%s)", info->samplers[i].name, enumName(info->samplers[i].type));
            fprintf(file, "\n");
        }
        GLuint shaders[8];
        GLsizei count = 0;
        glGetAttachedShaders(program, 8, &count, shaders);
        for (int s = 0; s < count; s++) {
            GLint type = 0;
            glGetShaderiv(shaders[s], GL_SHADER_TYPE, &type);
            fprintf(file, "\n==== %s %u\n", type == GL_VERTEX_SHADER ? "Vertex-Shader" : type == GL_FRAGMENT_SHADER ? "Fragment-Shader" : enumName(type), shaders[s]);
            char* source = shaderSource(shaders[s]);
            writeNumberedSource(file, source);
            free(source);
        }
    }
    fclose(file);
    while (glGetError() != GL_NO_ERROR);
}

static void startCapture(GLContext* context) {
    __atomic_store_n(&capture.requested, 0, __ATOMIC_RELEASE);
    char base[512];
    pthread_mutex_lock(&diagMutex);
    snprintf(base, sizeof(base), "%s", capture.requestDir[0] ? capture.requestDir : diagDir);
    pthread_mutex_unlock(&diagMutex);
    if (!base[0] || !currentRenderer) {
        println("gladio: Aufzeichnung: kein Ordner für die Dateien");
        return;
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    snprintf(capture.dir, sizeof(capture.dir), "%s/gladio-%04d%02d%02d-%02d%02d%02d", base, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    if (mkdir(capture.dir, 0775) != 0 && errno != EEXIST) {
        println("gladio: Aufzeichnung: %s nicht anlegbar (%s)", capture.dir, strerror(errno));
        return;
    }
    char path[800];
    snprintf(path, sizeof(path), "%s/anfragen.txt", capture.dir);
    capture.trace = fopen(path, "w");
    if (!capture.trace) {
        println("gladio: Aufzeichnung: %s nicht schreibbar (%s)", path, strerror(errno));
        return;
    }
    setvbuf(capture.trace, NULL, _IOFBF, 1 << 20);

    capture.active = true;
    capture.context = context;
    capture.requests = capture.draws = capture.images = capture.errors = 0;
    capture.passDraws = capture.passContent = capture.passIndex = 0;
    capture.programCount = 0;
    capture.infoCount = 0;
    capture.lastState[0] = capture.lastSamplers[0] = capture.lastAttribs[0] = '\0';
    capture.passFbo = currentDrawFramebuffer();
    capture.startFrame = context->diag.frames;
    capture.startNs = gladio_nowNs();

    FILE* trace = capture.trace;
    fprintf(trace, "Gladio – Aufzeichnung eines Bilds (Bild %u dieser Verbindung, Socket %d), %04d-%02d-%02d %02d:%02d:%02d\n", context->diag.frames,
            context->clientFd, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    fprintf(trace, "Treiber: %s / %s / %s (%s), Pfad OpenGL ES %s\n", (const char*)glGetString(GL_VENDOR), (const char*)glGetString(GL_RENDERER),
            (const char*)glGetString(GL_VERSION), (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION), GLES2 ? "2.0" : "3");
    fprintf(trace, "Fenster %dx%d (FBO %u)\n", currentRenderer->displaySize[0], currentRenderer->displaySize[1], currentRenderer->displayBuffer);
    fprintf(trace, "Zeilen: Nummer Anfrage(Bytes) Argumente – ganze Zahlen dezimal bzw. hex, Gleitkommazahlen mit f, Namen in \"\";\n"
                   "  !! = GL-Fehler nach der Anfrage; zu Zeichenaufrufen Zustand/Sampler/Attribute (nur bei Änderung) und Uniforms\n"
                   "  (die ersten %d Aufrufe je Durchgang). Sampler: Name=Einheit:Textur(Format Größe Min/Mag-Filter Wrap-S/T, max = höchste\n"
                   "  Mipmap-Stufe, Vgl = Vergleichsmodus, S = Sampler-Objekt); Filter N/L, NmN = NEAREST_MIPMAP_NEAREST usw.; Wrap R/C/M/B.\n"
                   "  Attribute: Name@Ort=Komponenten x Typ (n normiert, i ganzzahlig)/Schrittweite+Versatz P Puffer, konst = kein Array.\n"
                   "  „== Bild“: Ergebnis eines Durchgangs (beim Wechsel des Zeichenziels, vor dem Bildwechsel), PNG links Farbe, rechts Alpha.\n\n",
            CAPTURE_UNIFORM_DRAWS);
    char description[700];
    describeFramebuffer(capture.passFbo, description, sizeof(description));
    fprintf(trace, "-- Durchgang 0: %s\n", description);
    println("gladio: Aufzeichnung beginnt: %s", capture.dir);
}

static void finishCapture(void) {
    writeShaders();
    double seconds = (gladio_nowNs() - capture.startNs) / 1e9;
    fprintf(capture.trace, "\nEnde: %u Anfragen, %u Zeichenaufrufe, %u Bilder, %u GL-Fehler, %d Programme, %.1f s\n", capture.requests, capture.draws,
            capture.images, capture.errors, capture.programCount, seconds);
    fclose(capture.trace);
    capture.trace = NULL;
    capture.active = false;
    capture.context = NULL;
    free(capture.pixels);
    capture.pixels = NULL;
    capture.pixelsSize = 0;
    free(capture.infos);
    capture.infos = NULL;
    capture.infoCount = capture.infoCapacity = 0;
    println("gladio: Aufzeichnung fertig: %s (%u Anfragen, %u Zeichenaufrufe, %u Bilder, %u GL-Fehler)", capture.dir, capture.requests,
            capture.draws, capture.images, capture.errors);
}

static void captureBeforeRequest(GLContext* context, short requestCode, bool drawing) {
    if (isPassBoundary(context, requestCode) && (capture.passDraws || capture.passContent))
        snapshotFramebuffer(capture.passFbo, "Durchgang zu Ende", capture.passDraws, capture.passContent);
    if (requestCode == REQUEST_CODE_SWAP_DISPLAY_BUFFERS) {
        if ((capture.passDraws || capture.passContent) && capture.passFbo != currentRenderer->displayBuffer)
            snapshotFramebuffer(capture.passFbo, "Durchgang zu Ende", capture.passDraws, capture.passContent);
        snapshotFramebuffer(currentRenderer->displayBuffer, "Bildausgabe", capture.passFbo == currentRenderer->displayBuffer ? capture.passDraws : 0,
                            capture.passFbo == currentRenderer->displayBuffer ? capture.passContent : 0);
        capture.passDraws = capture.passContent = 0;
    }
    if (capture.requests < CAPTURE_MAX_TRACE_LINES) {
        char args[480];
        formatArgs(requestCode, &context->inputBuffer, args, sizeof(args), 16);
        fprintf(capture.trace, "%6u %s(%d)%s\n", capture.requests, requestName(requestCode), context->inputBuffer.size, args);
    }
    capture.requests++;
}

static void captureAfterRequest(GLContext* context, short requestCode, bool drawing, GLenum error) {
    if (error != GL_NO_ERROR) {
        capture.errors++;
        fprintf(capture.trace, "    !! %s\n", errorName(error));
    }
    if (drawing) {
        capture.draws++;
        capture.passDraws++;
        if (capture.requests <= CAPTURE_MAX_TRACE_LINES) captureDraw(context);
    }
    else if (isClearRequest(requestCode)) capture.passContent++;
    else if (requestCode == REQUEST_CODE_GL_USE_PROGRAM && context->inputBuffer.size >= 4) addProgram(*(const GLuint*)context->inputBuffer.buffer);

    if (isPassBoundary(context, requestCode)) {
        GLuint fbo = currentDrawFramebuffer();
        bool changed = fbo != capture.passFbo || requestCode != REQUEST_CODE_GL_BIND_FRAMEBUFFER;
        if (changed || capture.passDraws || capture.passContent) {
            capture.passFbo = fbo;
            capture.passDraws = capture.passContent = 0;
            char description[700];
            describeFramebuffer(fbo, description, sizeof(description));
            fprintf(capture.trace, "-- Durchgang %u: %s\n", ++capture.passIndex, description);
            capture.lastState[0] = '\0';
        }
    }
}

/* ---------------------------------------------------------------- Anfrageschleife */

void GladioDiag_beforeRequest(GLContext* context, short requestCode, bool drawing) {
    if (!currentRenderer) return;
    if (drawing && context->diag.framebufferDirty) {
        context->diag.framebufferDirty = false;
        checkFramebuffer(context, requestCode);
    }
    if (capture.active && capture.context == context) captureBeforeRequest(context, requestCode, drawing);
}

void GladioDiag_afterRequest(GLContext* context, short requestCode, bool drawing) {
    if (!currentRenderer) return;
    bool capturing = capture.active && capture.context == context;
    bool exact = IS_DEBUG_ENABLED(DEBUG_MODE_GL_ERROR) || capturing || context->diag.frames < context->diag.huntUntilFrame;
    GLenum error = GL_NO_ERROR;
    if (requestCode != REQUEST_CODE_GL_GET_ERROR && (exact || drawing || requestCode == REQUEST_CODE_SWAP_DISPLAY_BUFFERS)) {
        error = glGetError();
        if (error != GL_NO_ERROR) reportError(context, requestCode, error, exact);
    }
    if (changesFramebuffer(requestCode)) context->diag.framebufferDirty = true;
    if (capturing) captureAfterRequest(context, requestCode, drawing, error);

    if (requestCode == REQUEST_CODE_SWAP_DISPLAY_BUFFERS) {
        context->diag.frames++;
        if (capturing) finishCapture();
        else if (!capture.active && __atomic_load_n(&capture.requested, __ATOMIC_ACQUIRE)) startCapture(context);
    }
}

void GladioDiag_endContext(GLContext* context) {
    if (capture.active && capture.context == context) {
        if (currentRenderer && (capture.passDraws || capture.passContent))
            snapshotFramebuffer(capture.passFbo, "Durchgang beim Ende der Verbindung", capture.passDraws, capture.passContent);
        fprintf(capture.trace, "\n(Verbindung beendet)\n");
        finishCapture();
    }
}

void GladioDiag_printStats(void) {
    GladioDiagStats now = gladio_diagStats;
    if (memcmp(&now, &printedStats, sizeof(now)) == 0) return;
    println("gladio: Diagnose seit der letzten Statistik: %u GL-Fehler, %u unvollständige Zeichenziele, %u Shader nicht übersetzt, %u Programme nicht gebunden",
            now.glErrors - printedStats.glErrors, now.incompleteFramebuffers - printedStats.incompleteFramebuffers,
            now.compileErrors - printedStats.compileErrors, now.linkErrors - printedStats.linkErrors);
    printedStats = now;
}
