/*
 * GLSL-ES-1.00-Ausgabe des Shader-Konverters (OpenGL-ES-2.0-Pfad, z. B. Mali-400).
 *
 * Der Konverter in shader_converter.c übersetzt die GLSL-Quellen der Anwendung (1.10 bis
 * 3.30) zeilenweise nach GLSL ES 3.20. Für ES 2.0 muss das Ergebnis GLSL ES 1.00 sein:
 *   - attribute/varying statt in/out, Fragmentausgabe über gl_FragColor,
 *   - texture2D/textureCube/texture2DProj statt texture()/textureProj(),
 *     texture2DLodEXT/texture2DGradEXT (GL_EXT_shader_texture_lod) im Fragment-Shader,
 *   - keine Layout-/Interpolationsqualifizierer, keine vorzeichenlosen Ganzzahlen,
 *   - Ersatzfunktionen für shadow2D, transpose, round, trunc,
 *   - gl_FragDepth/gl_ClipVertex werden auf Attrappen umgeleitet,
 *   - mediump im Fragment-Shader, wenn der Treiber kein highp bietet.
 * Wine (wined3d) bekommt vom Server OpenGL 2.1 / GLSL 1.20 gemeldet und erzeugt deshalb
 * ohnehin Shader im alten Stil (gl_TexCoord, gl_FragData, attribute/varying), für die hier
 * wenig umzuschreiben bleibt.
 */
#include "shader_converter_es2.h"
#include "gl_es2.h"
#include "string_utils.h"

/* Vorkommen, für die Ersatzfunktionen oder Erweiterungen in den Kopf gehören */
#define ES2_USES_SHADOW (1<<0)
#define ES2_USES_SHADOW_PROJ (1<<1)
#define ES2_USES_TRANSPOSE (1<<2)
#define ES2_USES_ROUND (1<<3)
#define ES2_USES_TRUNC (1<<4)
#define ES2_USES_DERIVATIVES (1<<5)
#define ES2_USES_TEXTURE_LOD (1<<6)
#define ES2_USES_FRAG_DEPTH (1<<7)
#define ES2_USES_CLIP_VERTEX (1<<8)
#define ES2_USES_CLIP_DISTANCE (1<<9)
#define ES2_USES_FRAG_COORD (1<<10)

static bool isIdentChar(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

/* Ersetzt die Wortvorkommen von search durch replace (gibt neuen Puffer zurück, gibt subject frei). */
static char* replaceWord(char* subject, const char* search, const char* replace) {
    return strwrd_replace((char*)search, (char*)replace, subject);
}

static bool containsWord(char* line, const char* word) {
    return strwrd(line, (char*)word, NULL) != NULL;
}

/* Liefert den Typ des Samplers im ersten Argument eines Aufrufs; openParen zeigt auf '('. */
static GLenum samplerTypeOfCall(ShaderObject* shader, char* openParen) {
    char* start = openParen + 1;
    while (*start == ' ') start++;
    char* end = start;
    while (isIdentChar(*end)) end++;
    if (end == start) return GL_SAMPLER_2D;
    char old = *end;
    *end = '\0';
    ShaderVariable* variable = ArrayMap_get(&shader->code.variables, start);
    *end = old;
    return variable ? variable->type : GL_SAMPLER_2D;
}

/* Entfernt das letzte Argument eines Aufrufs, dessen '(' bei openParen steht. */
static void dropLastArgument(char* openParen) {
    int depth = 0;
    char* lastComma = NULL;
    for (char* chr = openParen; *chr; chr++) {
        if (*chr == '(') depth++;
        else if (*chr == ')') {
            depth--;
            if (depth == 0) {
                if (lastComma) memmove(lastComma, chr, strlen(chr) + 1);
                return;
            }
        }
        else if (*chr == ',' && depth == 1) lastComma = chr;
    }
}

/* Ersetzt den Funktionsnamen vor openParen (Länge nameLen) durch replace; liefert neue Zeile. */
static char* renameCall(char* line, char* name, int nameLen, const char* replace) {
    int start = name - line;
    char* result = substr_replace((char*)replace, start, start + nameLen, line);
    free(line);
    return result;
}

/*
 * Wandelt Texturaufrufe um. GLSL ES 1.00 kennt texture2D, texture2DProj, textureCube und (nur im
 * Vertex-Shader) texture2DLod; Lod/Grad im Fragment-Shader brauchen GL_EXT_shader_texture_lod.
 */
static char* convertTextureCalls(ShaderObject* shader, char* line) {
    bool fragment = shader->type == GL_FRAGMENT_SHADER;
    int pos = 0;

    while (line[pos]) {
        if (!isIdentChar(line[pos]) || (pos > 0 && isIdentChar(line[pos-1]))) {
            pos++;
            continue;
        }
        int start = pos;
        while (isIdentChar(line[pos])) pos++;
        int len = pos - start;
        char* paren = line + pos;
        while (*paren == ' ') paren++;
        if (*paren != '(') continue;

        char name[32];
        if (len >= (int)sizeof(name)) continue;
        memcpy(name, line + start, len);
        name[len] = '\0';

        const char* replace = NULL;
        bool dropLod = false;
        GLenum samplerType = GL_NONE;

        if (!strcmp(name, "texture") || !strcmp(name, "textureLod") || !strcmp(name, "textureProj") ||
            !strcmp(name, "textureGrad") || !strcmp(name, "textureProjLod") || !strcmp(name, "textureProjGrad")) {
            samplerType = samplerTypeOfCall(shader, paren);
            bool cube = samplerType == GL_SAMPLER_CUBE || samplerType == GL_SAMPLER_CUBE_SHADOW;
            bool shadow = samplerType == GL_SAMPLER_2D_SHADOW || samplerType == GL_SAMPLER_CUBE_SHADOW;
            bool proj = strstr(name, "Proj") != NULL;
            bool lod = strstr(name, "Lod") != NULL;
            bool grad = strstr(name, "Grad") != NULL;

            if (shadow) {
                replace = proj ? "gd_shadow2DProj" : "gd_shadow2D";
                shader->code.es2Flags |= proj ? ES2_USES_SHADOW_PROJ : ES2_USES_SHADOW;
                dropLod = lod || grad;
            }
            else if (lod) {
                if (!fragment) replace = cube ? "textureCubeLod" : (proj ? "texture2DProjLod" : "texture2DLod");
                else if (gles.shaderTextureLod) {
                    replace = cube ? "textureCubeLodEXT" : (proj ? "texture2DProjLodEXT" : "texture2DLodEXT");
                    shader->code.es2Flags |= ES2_USES_TEXTURE_LOD;
                }
                else {
                    replace = cube ? "textureCube" : (proj ? "texture2DProj" : "texture2D");
                    dropLod = true;
                }
            }
            else if (grad) {
                if (fragment && gles.shaderTextureLod) {
                    replace = cube ? "textureCubeGradEXT" : (proj ? "texture2DProjGradEXT" : "texture2DGradEXT");
                    shader->code.es2Flags |= ES2_USES_TEXTURE_LOD;
                }
                else {
                    replace = cube ? "textureCube" : (proj ? "texture2DProj" : "texture2D");
                    dropLod = true; /* zwei Gradienten */
                    dropLastArgument(paren);
                }
            }
            else replace = cube ? "textureCube" : (proj ? "texture2DProj" : "texture2D");
        }
        else if (!strcmp(name, "shadow2D") || !strcmp(name, "shadow2DProj") || !strcmp(name, "shadow2DLod") || !strcmp(name, "shadow2DProjLod")) {
            bool proj = strstr(name, "Proj") != NULL;
            replace = proj ? "gd_shadow2DProj" : "gd_shadow2D";
            shader->code.es2Flags |= proj ? ES2_USES_SHADOW_PROJ : ES2_USES_SHADOW;
            dropLod = strstr(name, "Lod") != NULL;
        }
        else if (fragment && (!strcmp(name, "texture2DLod") || !strcmp(name, "texture2DProjLod") || !strcmp(name, "textureCubeLod"))) {
            if (gles.shaderTextureLod) {
                replace = !strcmp(name, "texture2DLod") ? "texture2DLodEXT" : !strcmp(name, "texture2DProjLod") ? "texture2DProjLodEXT" : "textureCubeLodEXT";
                shader->code.es2Flags |= ES2_USES_TEXTURE_LOD;
            }
            else {
                replace = !strcmp(name, "texture2DLod") ? "texture2D" : !strcmp(name, "texture2DProjLod") ? "texture2DProj" : "textureCube";
                dropLod = true;
            }
        }
        else if (!strcmp(name, "texture2DGradARB") || !strcmp(name, "texture2DProjGradARB") || !strcmp(name, "textureCubeGradARB")) {
            if (fragment && gles.shaderTextureLod) {
                replace = !strcmp(name, "texture2DGradARB") ? "texture2DGradEXT" : !strcmp(name, "texture2DProjGradARB") ? "texture2DProjGradEXT" : "textureCubeGradEXT";
                shader->code.es2Flags |= ES2_USES_TEXTURE_LOD;
            }
            else {
                replace = !strcmp(name, "texture2DGradARB") ? "texture2D" : !strcmp(name, "texture2DProjGradARB") ? "texture2DProj" : "textureCube";
                dropLod = true;
                dropLastArgument(paren);
            }
        }
        else if (!strcmp(name, "texture3D") || !strcmp(name, "texture3DProj") || !strcmp(name, "texture1D") || !strcmp(name, "texture1DProj")) {
            /* keine 3D-Texturen in ES 2.0: als 2D behandeln */
            replace = strstr(name, "Proj") ? "texture2DProj" : "texture2D";
        }
        else if (!strcmp(name, "transpose")) {
            replace = "gd_transpose";
            shader->code.es2Flags |= ES2_USES_TRANSPOSE;
        }
        else if (!strcmp(name, "round") || !strcmp(name, "roundEven")) {
            replace = "gd_round";
            shader->code.es2Flags |= ES2_USES_ROUND;
        }
        else if (!strcmp(name, "trunc")) {
            replace = "gd_trunc";
            shader->code.es2Flags |= ES2_USES_TRUNC;
        }
        else if (!strcmp(name, "dFdx") || !strcmp(name, "dFdy") || !strcmp(name, "fwidth")) {
            shader->code.es2Flags |= ES2_USES_DERIVATIVES;
        }

        if (replace) {
            if (dropLod) dropLastArgument(paren);
            line = renameCall(line, line + start, len, replace);
            pos = start + strlen(replace);
        }
    }

    return line;
}

/* Entfernt layout(...) am Zeilenanfang. */
static char* stripLayoutQualifier(char* line) {
    char* layout = strstr(line, "layout");
    if (!layout || !cstartswith("layout", ltrim(line))) return line;
    char* paren = strchr(layout, '(');
    if (!paren) return line;
    int depth = 0;
    char* end = paren;
    for (; *end; end++) {
        if (*end == '(') depth++;
        else if (*end == ')' && --depth == 0) break;
    }
    if (!*end) return line;
    end++;
    while (*end == ' ') end++;
    char* result = strdup(end);
    free(line);
    return result;
}

/*
 * Globale in/out-Deklarationen nach attribute/varying. Im Fragment-Shader wird die erste
 * Ausgabevariable zu gl_FragColor; weitere Ausgaben landen in Attrappen.
 */
static char* convertGlobalDeclaration(ShaderObject* shader, char* line) {
    line = stripLayoutQualifier(line);

    bool invariant = false;
    for (;;) {
        char* text = ltrim(line);
        if (cstartswith("invariant ", text)) {
            invariant = true;
            char* result = strdup(text + 10);
            free(line);
            line = result;
        }
        else if (cstartswith("flat ", text) || cstartswith("smooth ", text) || cstartswith("centroid ", text) || cstartswith("noperspective ", text)) {
            char* result = strdup(strchr(text, ' ') + 1);
            free(line);
            line = result;
        }
        else break;
    }

    char* text = ltrim(line);
    char* result = NULL;
    if (cstartswith("in ", text)) {
        result = strjoin(0, 3, invariant ? "invariant " : "", shader->type == GL_VERTEX_SHADER ? "attribute " : "varying ", text + 3);
    }
    else if (cstartswith("out ", text)) {
        if (shader->type == GL_VERTEX_SHADER) {
            result = strjoin(0, 3, invariant ? "invariant " : "", "varying ", text + 4);
        }
        else {
            /* "out vec4 name[n];" -> Name und Feldgröße bestimmen */
            char* decl = strdup(text + 4);
            char* semicolon = strchr(decl, ';');
            if (semicolon) *semicolon = '\0';
            char* bracket = strchr(decl, '[');
            if (bracket) *bracket = '\0';
            char* name = strrchr(trim(decl), ' ');
            name = name ? name + 1 : decl;

            if (!shader->code.es2FragOutput && !bracket) {
                shader->code.es2FragOutput = strdup(name);
                char buffer[256];
                snprintf(buffer, sizeof(buffer), "#define %s gl_FragColor", name);
                result = strdup(buffer);
            }
            else {
                if (!shader->code.es2FragOutput && bracket) {
                    shader->code.es2FragOutput = strdup(name);
                    shader->code.es2FragOutputIsArray = true;
                }
                result = strdup(text + 4); /* gewöhnliche globale Variable */
            }
            free(decl);
        }
    }
    else if (invariant) {
        result = strjoin(0, 2, "invariant ", text);
    }

    if (result) {
        free(line);
        return result;
    }
    return line;
}

/* Entfernt das u/U-Suffix von Ganzzahlliteralen (ES 1.00 kennt kein unsigned). */
static void stripUnsignedSuffix(char* line) {
    for (char* chr = line; *chr; chr++) {
        if ((*chr == 'u' || *chr == 'U') && chr > line && isdigit((unsigned char)chr[-1]) && !isIdentChar(chr[1])) {
            /* Rückwärts prüfen, dass es eine Zahl und kein Bezeichner ist */
            char* start = chr - 1;
            while (start > line && isdigit((unsigned char)start[-1])) start--;
            if (start == line || !isIdentChar(start[-1])) memmove(chr, chr + 1, strlen(chr));
        }
    }
}

char* ShaderConverterES2_convertLine(ShaderObject* shader, char* line, int scopeBefore) {
    if (line[0] == '#') return line;

    if (scopeBefore == 0) line = convertGlobalDeclaration(shader, line);

    if (strstr(line, "flat") || strstr(line, "smooth") || strstr(line, "centroid") || strstr(line, "noperspective")) {
        line = replaceWord(line, "flat", "");
        line = replaceWord(line, "smooth", "");
        line = replaceWord(line, "centroid", "");
        line = replaceWord(line, "noperspective", "");
    }

    if (strstr(line, "uint") || strstr(line, "uvec") || strstr(line, "usampler")) {
        line = replaceWord(line, "uint", "int");
        line = replaceWord(line, "uvec2", "ivec2");
        line = replaceWord(line, "uvec3", "ivec3");
        line = replaceWord(line, "uvec4", "ivec4");
        line = replaceWord(line, "usampler2D", "sampler2D");
        line = replaceWord(line, "usamplerCube", "samplerCube");
    }
    stripUnsignedSuffix(line);

    if (strstr(line, "sampler")) {
        line = replaceWord(line, "sampler2DShadow", "sampler2D");
        line = replaceWord(line, "sampler1DShadow", "sampler2D");
        line = replaceWord(line, "sampler3D", "sampler2D");
        line = replaceWord(line, "sampler1D", "sampler2D");
        line = replaceWord(line, "sampler2DRect", "sampler2D");
        line = replaceWord(line, "samplerCubeShadow", "samplerCube");
        line = replaceWord(line, "isampler2D", "sampler2D");
    }

    if (strstr(line, "texture") || strstr(line, "shadow2D") || strstr(line, "transpose") || strstr(line, "round") ||
        strstr(line, "trunc") || strstr(line, "dFd") || strstr(line, "fwidth")) {
        line = convertTextureCalls(shader, line);
    }

    if (containsWord(line, "gl_FragCoord")) shader->code.es2Flags |= ES2_USES_FRAG_COORD;
    return line;
}

/* Ersetzungen für eingebaute Variablen, die der ES-3.2-Pfad unverändert lässt. */
const char* ShaderConverterES2_replaceBuiltin(ShaderObject* shader, const char* name, int nameLen) {
    if (shader->type == GL_FRAGMENT_SHADER) {
        if (nameLen == 12 && !strncmp(name, "gl_FragDepth", 12)) {
            shader->code.es2Flags |= ES2_USES_FRAG_DEPTH;
            return gles.fragDepth ? "gl_FragDepthEXT" : "gd_FragDepth";
        }
    }
    else {
        if (nameLen == 13 && !strncmp(name, "gl_ClipVertex", 13)) {
            shader->code.es2Flags |= ES2_USES_CLIP_VERTEX;
            return "gd_ClipVertex";
        }
        if (nameLen == 15 && !strncmp(name, "gl_ClipDistance", 15)) {
            shader->code.es2Flags |= ES2_USES_CLIP_DISTANCE;
            return "gd_ClipDistance";
        }
    }
    return NULL;
}

/* Höchster konstanter Index von gd_TexCoord[...] in allen Shadern des Programms, sonst MAX_TEXTURES. */
int ShaderConverterES2_texCoordArraySize(ShaderProgram* program) {
    int size = 0;
    for (int i = 0; i < program->attachedShaders.size; i++) {
        ShaderObject* shader = program->attachedShaders.elements[i];
        for (int j = 0; j < shader->code.lines.size; j++) {
            char* line = shader->code.lines.elements[j];
            /* bereits eingefügte Deklarationen anderer Shader des Programms zählen nicht */
            if (cstartswith("varying ", line) || cstartswith("in ", line) || cstartswith("out ", line)) continue;
            char* ptr = line;
            while ((ptr = strstr(ptr, "gd_TexCoord"))) {
                ptr += 11;
                if (*ptr != '[') continue;
                char* value = ptr + 1;
                while (*value == ' ') value++;
                if (!isdigit((unsigned char)*value)) return MAX_TEXTURES;
                int index = (int)strtol(value, &value, 10);
                while (*value == ' ') value++;
                if (*value != ']') return MAX_TEXTURES;
                if (index + 1 > size) size = index + 1;
            }
        }
    }
    return size > 0 ? MIN(size, MAX_TEXTURES) : 1;
}

void ShaderConverterES2_injectHeader(ShaderObject* shader, void (*insertLine)(ShaderObject*, int, char*), int* head) {
    insertLine(shader, (*head)++, strdup("#version 100"));

    if (shader->type == GL_FRAGMENT_SHADER) {
        if ((shader->code.es2Flags & ES2_USES_DERIVATIVES) && gles.standardDerivatives) {
            insertLine(shader, (*head)++, strdup("#extension GL_OES_standard_derivatives : enable"));
        }
        if (shader->code.es2Flags & ES2_USES_TEXTURE_LOD) {
            insertLine(shader, (*head)++, strdup("#extension GL_EXT_shader_texture_lod : enable"));
        }
        if ((shader->code.es2Flags & ES2_USES_FRAG_DEPTH) && gles.fragDepth) {
            insertLine(shader, (*head)++, strdup("#extension GL_EXT_frag_depth : enable"));
        }
        insertLine(shader, (*head)++, strdup("#ifdef GL_FRAGMENT_PRECISION_HIGH"));
        insertLine(shader, (*head)++, strdup("precision highp float;"));
        insertLine(shader, (*head)++, strdup("precision highp int;"));
        insertLine(shader, (*head)++, strdup("#else"));
        insertLine(shader, (*head)++, strdup("precision mediump float;"));
        insertLine(shader, (*head)++, strdup("precision mediump int;"));
        insertLine(shader, (*head)++, strdup("#endif"));

        if ((shader->code.es2Flags & ES2_USES_FRAG_DEPTH) && !gles.fragDepth) insertLine(shader, (*head)++, strdup("float gd_FragDepth;"));
    }
    else {
        if (shader->code.es2Flags & ES2_USES_CLIP_VERTEX) insertLine(shader, (*head)++, strdup("vec4 gd_ClipVertex;"));
        if (shader->code.es2Flags & ES2_USES_CLIP_DISTANCE) insertLine(shader, (*head)++, strdup("float gd_ClipDistance[8];"));
    }

    if (shader->code.es2Flags & (ES2_USES_SHADOW | ES2_USES_SHADOW_PROJ)) {
        insertLine(shader, (*head)++, strdup("vec4 gd_shadow2D(sampler2D s, vec3 c) { return vec4(c.z <= texture2D(s, c.xy).r ? 1.0 : 0.0); }"));
        if (shader->code.es2Flags & ES2_USES_SHADOW_PROJ) {
            insertLine(shader, (*head)++, strdup("vec4 gd_shadow2DProj(sampler2D s, vec4 c) { return gd_shadow2D(s, c.xyz / c.w); }"));
        }
    }
    if (shader->code.es2Flags & ES2_USES_TRANSPOSE) {
        insertLine(shader, (*head)++, strdup("mat2 gd_transpose(mat2 m) { return mat2(m[0][0], m[1][0], m[0][1], m[1][1]); }"));
        insertLine(shader, (*head)++, strdup("mat3 gd_transpose(mat3 m) { return mat3(m[0][0], m[1][0], m[2][0], m[0][1], m[1][1], m[2][1], m[0][2], m[1][2], m[2][2]); }"));
        insertLine(shader, (*head)++, strdup("mat4 gd_transpose(mat4 m) { return mat4(m[0][0], m[1][0], m[2][0], m[3][0], m[0][1], m[1][1], m[2][1], m[3][1], m[0][2], m[1][2], m[2][2], m[3][2], m[0][3], m[1][3], m[2][3], m[3][3]); }"));
    }
    if (shader->code.es2Flags & ES2_USES_ROUND) {
        insertLine(shader, (*head)++, strdup("float gd_round(float x) { return floor(x + 0.5); }"));
        insertLine(shader, (*head)++, strdup("vec2 gd_round(vec2 x) { return floor(x + 0.5); }"));
        insertLine(shader, (*head)++, strdup("vec3 gd_round(vec3 x) { return floor(x + 0.5); }"));
        insertLine(shader, (*head)++, strdup("vec4 gd_round(vec4 x) { return floor(x + 0.5); }"));
    }
    if (shader->code.es2Flags & ES2_USES_TRUNC) {
        insertLine(shader, (*head)++, strdup("float gd_trunc(float x) { return sign(x) * floor(abs(x)); }"));
        insertLine(shader, (*head)++, strdup("vec2 gd_trunc(vec2 x) { return sign(x) * floor(abs(x)); }"));
        insertLine(shader, (*head)++, strdup("vec3 gd_trunc(vec3 x) { return sign(x) * floor(abs(x)); }"));
        insertLine(shader, (*head)++, strdup("vec4 gd_trunc(vec4 x) { return sign(x) * floor(abs(x)); }"));
    }
}

void ShaderConverterES2_finishFragmentOutput(ShaderObject* shader, void (*insertLine)(ShaderObject*, int, char*), ShaderFunction** mainFunctions, int numMainFunctions) {
    if (shader->type != GL_FRAGMENT_SHADER || !shader->code.es2FragOutput || !shader->code.es2FragOutputIsArray) return;
    char buffer[256];
    snprintf(buffer, sizeof(buffer), "gl_FragColor = %s[0];", shader->code.es2FragOutput);
    for (int i = 0; i < numMainFunctions; i++) insertLine(shader, mainFunctions[i]->lineEnd, strdup(buffer));
}
