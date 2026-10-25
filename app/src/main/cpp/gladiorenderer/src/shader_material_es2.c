/*
 * Spezialisierte Fixed-Function-Shader für den ES-2.0-Pfad (Mali-400).
 *
 * Der allgemeine Material-Shader (shader_material.c) entscheidet Texturumgebung, Alphatest,
 * Nebel und Lichtzahl zur Laufzeit über Uniforms. Auf der Mali-400 kostet das pro Pixel: der
 * Fragment-Prozessor rechnet praktisch alle Zweige aus (um die 100 Befehle statt einer Handvoll).
 * Deshalb wird hier für jede vorkommende Kombination aus Texturmodus, Alphafunktion, Nebelmodus
 * und Lichtkonfiguration ein eigener, geradliniger GLSL-ES-1.00-Shader erzeugt; die Kombination
 * (FFSpec) ist Teil des Material-Schlüssels (generateMaterialHash).
 *
 * Werte, die sich pro Zeichenaufruf ändern (Matrizen, Farben, Referenzwerte, Lichtparameter),
 * bleiben Uniforms – mit denselben Namen wie im allgemeinen Shader, damit setupMaterial() und
 * ShaderMaterial_updateUniforms() unverändert passen. Uniforms, die der spezialisierte Shader
 * nicht braucht (z. B. gd_TexEnv0.mode), fehlen; ihre Position ist -1.
 *
 * Abweichungen vom allgemeinen Shader (dort Näherungen, hier wie in der GL-Spezifikation):
 *   - GL_REPLACE übernimmt das Alpha der Textur, wenn das Texturformat Alpha hat, sonst das
 *     Fragment-Alpha (allgemein: immer Produkt aus beiden);
 *   - GL_DECAL lässt das Fragment-Alpha unverändert;
 *   - GL_SRC_ALPHA/GL_ONE_MINUS_SRC_ALPHA als RGB-Operand von GL_COMBINE (allgemein: 1.0);
 *   - ein Spotlicht ist eines mit Öffnungswinkel < 180°, auch bei Exponent 0 (allgemein: nur
 *     bei Exponent != 0 wird der Kegel beachtet).
 */
#include <stdarg.h>
#include "shader_material.h"
#include "gl_renderer.h"
#include "gl_es2.h"
#include "shader_chunks.h"

/* Codes wie ShaderMaterial_texEnvCode(), ShaderMaterial_alphaTestCode(), ShaderMaterial_fogModeCode() */
enum {
    TE_MODULATE = 1, TE_DECAL, TE_INTERPOLATE, TE_ADD, TE_ADD_SIGNED, TE_SUBTRACT, TE_COMBINE, TE_DOT3_RGB, TE_DOT3_RGBA, TE_REPLACE,
    TE_SRC_TEXTURE, TE_SRC_CONSTANT, TE_SRC_PRIMARY_COLOR, TE_SRC_PREVIOUS,
    TE_OP_SRC_COLOR, TE_OP_ONE_MINUS_SRC_COLOR, TE_OP_SRC_ALPHA, TE_OP_ONE_MINUS_SRC_ALPHA,
    AT_NEVER = 20, AT_LESS, AT_EQUAL, AT_LEQUAL, AT_GREATER, AT_NOTEQUAL, AT_GEQUAL, AT_ALWAYS,
    FOG_LINEAR = 30, FOG_EXP, FOG_EXP2
};

static void emit(ArrayBuffer* out, const char* format, ...) {
    char buffer[2048];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (length < 0) return;
    if (length >= (int)sizeof(buffer)) length = sizeof(buffer) - 1;
    ArrayBuffer_putBytes(out, buffer, length);
}

/* 0 = unbekannt, 1 = Format ohne Alpha (Alpha liest sich als 1.0), 2 = Format mit Alpha */
static int textureAlphaClass(GLint format) {
    switch (format) {
        case 1: case 3:
        case GL_RGB: case GL_BGR: case GL_R3_G3_B2: case GL_RGB4: case GL_RGB5: case GL_RGB8: case GL_RGB10: case GL_RGB12: case GL_RGB16:
        case GL_SRGB: case GL_SRGB8: case GL_RGB565: case GL_RGB16F: case GL_RGB32F:
        case GL_LUMINANCE: case GL_LUMINANCE4: case GL_LUMINANCE8: case GL_LUMINANCE12: case GL_LUMINANCE16: case GL_SLUMINANCE: case GL_SLUMINANCE8:
        case GL_RED: case GL_R8: case GL_RG: case GL_RG8:
        case GL_COMPRESSED_RGB: case GL_COMPRESSED_LUMINANCE: case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
            return 1;
        case 2: case 4:
        case GL_RGBA: case GL_BGRA: case GL_RGBA2: case GL_RGBA4: case GL_RGB5_A1: case GL_RGBA8: case GL_RGB10_A2: case GL_RGBA12: case GL_RGBA16:
        case GL_SRGB_ALPHA: case GL_SRGB8_ALPHA8: case GL_RGBA16F: case GL_RGBA32F:
        case GL_ALPHA: case GL_ALPHA4: case GL_ALPHA8: case GL_ALPHA12: case GL_ALPHA16:
        case GL_LUMINANCE_ALPHA: case GL_LUMINANCE4_ALPHA4: case GL_LUMINANCE6_ALPHA2: case GL_LUMINANCE8_ALPHA8:
        case GL_LUMINANCE12_ALPHA4: case GL_LUMINANCE12_ALPHA12: case GL_LUMINANCE16_ALPHA16: case GL_SLUMINANCE_ALPHA: case GL_SLUMINANCE8_ALPHA8:
        case GL_INTENSITY: case GL_INTENSITY4: case GL_INTENSITY8: case GL_INTENSITY12: case GL_INTENSITY16:
        case GL_COMPRESSED_RGBA: case GL_COMPRESSED_ALPHA: case GL_COMPRESSED_LUMINANCE_ALPHA: case GL_COMPRESSED_INTENSITY:
        case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT: case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT: case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
            return 2;
        default:
            return 0;
    }
}

static bool isAlphaOnlyFormat(GLint format) {
    switch (format) {
        case GL_ALPHA: case GL_ALPHA4: case GL_ALPHA8: case GL_ALPHA12: case GL_ALPHA16: case GL_COMPRESSED_ALPHA:
            return true;
        default:
            return false;
    }
}

void FFSpec_fromState(FFSpec* spec, GLRenderer* renderer, const MaterialOptions* options, bool points) {
    memset(spec, 0, sizeof(FFSpec));
    spec->points = points;

    if (options->lighting) {
        bool lightSpecular = false;
        for (int i = 0; i < MAX_LIGHTS; i++) {
            GLLight* light = &renderer->lights[i];
            if (!light->enabled) continue;
            uint8_t flags = 0;
            if (light->position[3] != 0.0f) {
                flags |= FF_LIGHT_POSITIONAL;
                /* Spot und Abschwächung gibt es in GL nur bei Punktlichtern; Spot: Öffnungswinkel
                 * (im Bogenmaß) kleiner als 180° (GL erlaubt 0–90° oder 180°) */
                if (light->spotCutoffExponent[0] < 3.0f) flags |= FF_LIGHT_SPOT;
                if (light->attenuation[0] != 1.0f || light->attenuation[1] != 0.0f || light->attenuation[2] != 0.0f) flags |= FF_LIGHT_ATTENUATION;
            }
            if (light->specular[0] != 0.0f || light->specular[1] != 0.0f || light->specular[2] != 0.0f) lightSpecular = true;
            spec->lightFlags[spec->numLights++] = flags;
        }

        spec->colorMaterial = ShaderMaterial_colorMaterialMask(renderer);
        spec->twoSide = renderer->state.lightModelTwoSide;
        bool materialSpecular = (spec->colorMaterial & FF_CM_SPECULAR) != 0;
        for (int i = 0; i < (spec->twoSide ? 2 : 1) && renderer->materials; i++) {
            const float* s = renderer->materials[i].specular;
            if (s[0] != 0.0f || s[1] != 0.0f || s[2] != 0.0f) materialSpecular = true;
        }
        spec->specular = lightSpecular && materialSpecular;
    }

    if (options->alphaTest) {
        spec->alphaFunc = (uint8_t)ShaderMaterial_alphaTestCode(renderer->state.alphaTest.enabled ? renderer->state.alphaTest.func : GL_ALWAYS);
    }

    if (options->fog) spec->fogMode = (uint8_t)ShaderMaterial_fogModeCode(renderer->state.fog.mode);

    for (int i = 0; i < options->numTextures && i < MAX_TEXTURES; i++) {
        if (!renderer->state.enabledTextures[i]) continue;
        FFTexUnit* unit = &spec->texUnits[i];
        TexEnv* texEnv = &renderer->state.texEnv[i];
        GLTexture* texture = renderer->clientState.texture[i][indexOfGLTarget(GL_TEXTURE_2D)];

        if (texture && isAlphaOnlyFormat(texture->originFormat) && texEnv->mode != GL_COMBINE) {
            /* Alpha-Textur: RGB bleibt die Fragmentfarbe (Ct = 0 in der GL-Tabelle), das Alpha wird
             * bei GL_REPLACE übernommen und sonst mit dem Fragment-Alpha multipliziert */
            unit->mode = TE_COMBINE;
            unit->combine[0] = TE_REPLACE;
            unit->combine[1] = texEnv->mode == GL_REPLACE ? TE_REPLACE : TE_MODULATE;
            unit->source[0] = TE_SRC_PREVIOUS;
            unit->source[1] = TE_SRC_PREVIOUS;
            unit->source[2] = TE_SRC_TEXTURE;
            unit->source[3] = TE_SRC_PREVIOUS;
            unit->operand[0] = TE_OP_SRC_COLOR;
            unit->operand[1] = TE_OP_SRC_COLOR;
            unit->operand[2] = TE_OP_SRC_ALPHA;
            unit->operand[3] = TE_OP_SRC_ALPHA;
        }
        else {
            unit->mode = ShaderMaterial_texEnvCode(texEnv->mode);
            if (unit->mode == TE_COMBINE) {
                for (int j = 0; j < 2; j++) unit->combine[j] = ShaderMaterial_texEnvCode(texEnv->combineRGBA[j]);
                for (int j = 0; j < 4; j++) {
                    unit->source[j] = ShaderMaterial_texEnvCode(texEnv->sourceRGBA[j]);
                    unit->operand[j] = ShaderMaterial_texEnvCode(texEnv->operandRGBA[j]);
                }
            }
        }
        if (unit->mode == 0) continue;

        if (texEnv->lodBias != 0.0f) unit->flags |= FF_TEXUNIT_LOD_BIAS;
        if (texEnv->rgbaScale[0] != 1.0f || texEnv->rgbaScale[1] != 1.0f) unit->flags |= FF_TEXUNIT_SCALE;
        int alphaClass = texture ? textureAlphaClass(texture->originFormat) : 1;
        if (alphaClass) unit->flags |= FF_TEXUNIT_ALPHA_KNOWN;
        if (alphaClass == 2) unit->flags |= FF_TEXUNIT_HAS_ALPHA;
    }
}

static const char* precisionHeader() {
    return "#version 100\n"
           "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
           "precision highp float;\n"
           "precision highp int;\n"
           "#else\n"
           "precision mediump float;\n"
           "precision mediump int;\n"
           "#endif\n";
}

/* ---------------------------------------------------------------- Vertex-Shader */

/* Materialwert einer Seite: Vertexfarbe bei GL_COLOR_MATERIAL (für diese Seite), sonst die Uniform */
static void materialExpr(char* dst, int size, const FFSpec* spec, int side, uint8_t cmBit, const char* member, const char* colorExpr) {
    bool useColor = (spec->colorMaterial & cmBit) && (spec->colorMaterial & (side == 0 ? FF_CM_FRONT : FF_CM_BACK));
    if (useColor) snprintf(dst, size, "%s", colorExpr);
    else snprintf(dst, size, "materials[%d].%s", side, member);
}

/* Beleuchtung einer Seite (Normale, Material) nach target */
static void emitLightingSide(ArrayBuffer* out, const FFSpec* spec, int side, const char* normalExpr, const char* target) {
    char expr[96];
    emit(out, "{\n");
    emit(out, "vec3 n = %s;\n", normalExpr);
    materialExpr(expr, sizeof(expr), spec, side, FF_CM_AMBIENT, "ambient", "gd_Color.rgb");
    emit(out, "vec3 mAmbient = %s;\n", expr);
    materialExpr(expr, sizeof(expr), spec, side, FF_CM_DIFFUSE, "diffuse", "gd_Color.rgb");
    emit(out, "vec3 mDiffuse = %s;\n", expr);
    materialExpr(expr, sizeof(expr), spec, side, FF_CM_DIFFUSE, "alpha", "gd_Color.a");
    emit(out, "float mAlpha = %s;\n", expr);
    materialExpr(expr, sizeof(expr), spec, side, FF_CM_EMISSION, "emission", "gd_Color.rgb");
    emit(out, "vec3 lit = %s + gd_LightModelAmbient * mAmbient;\n", expr);
    if (spec->specular) {
        materialExpr(expr, sizeof(expr), spec, side, FF_CM_SPECULAR, "specular.rgb", "gd_Color.rgb");
        emit(out, "vec3 mSpecular = %s;\n", expr);
        emit(out, "float shininess = materials[%d].specular.w;\n", side);
        emit(out, "vec3 viewDirection = normalize(-position.xyz);\n");
    }

    for (int i = 0; i < spec->numLights; i++) {
        uint8_t flags = spec->lightFlags[i];
        emit(out, "{\n");
        if (flags & FF_LIGHT_POSITIONAL) {
            emit(out, "vec3 lightDirection = normalize(lights[%d].position.xyz - position.xyz);\n", i);
        }
        else emit(out, "vec3 lightDirection = lights[%d].position.xyz;\n", i);

        bool hasFactor = (flags & (FF_LIGHT_SPOT | FF_LIGHT_ATTENUATION)) != 0;
        if (hasFactor) emit(out, "float factor = 1.0;\n");
        if (flags & FF_LIGHT_SPOT) {
            emit(out,
                "float angleCos = dot(-lights[%d].spotDirection, lightDirection);\n"
                "factor = angleCos >= cos(lights[%d].spotCutoffExponent.x) ? pow(max(angleCos, 0.0001), lights[%d].spotCutoffExponent.y) : 0.0;\n", i, i, i);
        }
        if (flags & FF_LIGHT_ATTENUATION) {
            emit(out,
                "float distance = length(lights[%d].position.xyz - position.xyz);\n"
                "factor /= lights[%d].attenuation.x + lights[%d].attenuation.y * distance + lights[%d].attenuation.z * (distance * distance);\n", i, i, i, i);
        }
        emit(out,
            "float NdotL = max(dot(n, lightDirection), 0.0);\n"
            "vec3 contribution = lights[%d].ambient * mAmbient + NdotL * lights[%d].diffuse * mDiffuse;\n", i, i);
        if (spec->specular) {
            emit(out, "if (NdotL > 0.0) contribution += pow(max(dot(n, normalize(lightDirection + viewDirection)), 0.0001), shininess) * lights[%d].specular * mSpecular;\n", i);
        }
        emit(out, hasFactor ? "lit += factor * contribution;\n" : "lit += contribution;\n");
        emit(out, "}\n");
    }
    emit(out, "%s = vec4(clamp(lit, 0.0, 1.0), mAlpha);\n", target);
    emit(out, "}\n");
}

/*
 * Beleuchtung wie in GL (je Vertex): Das Ergebnis ersetzt die Vertexfarbe, die nur über
 * GL_COLOR_MATERIAL eingeht; Alpha ist das der diffusen Materialfarbe. Bei
 * GL_LIGHT_MODEL_TWO_SIDE wird auch die Rückseite (Rückseitenmaterial, umgedrehte Normale)
 * berechnet; der Fragment-Shader wählt wie GL nach der Orientierung des Dreiecks.
 */
static void emitLighting(ArrayBuffer* out, const FFSpec* spec) {
    emit(out, "vec3 mvNormal = normalize(vec3(gd_ModelViewMatrix * vec4(gd_Normal, 0.0)));\n");
    emitLightingSide(out, spec, 0, "mvNormal", "gd_FrontColor");
    if (spec->twoSide) emitLightingSide(out, spec, 1, "-mvNormal", "gd_BackColor");
}

char* ShaderMaterialES2_vertexSource(const MaterialOptions* options) {
    const FFSpec* spec = options->spec;
    ArrayBuffer out = {0};

    emit(&out, "#version 100\n");
    if (options->transformVertex) {
        emit(&out, "uniform mat4 gd_ProjectionMatrix;\nuniform mat4 gd_ModelViewMatrix;\n");
    }
    emit(&out, "attribute vec4 gd_Vertex;\nattribute vec4 gd_Color;\nattribute vec3 gd_Normal;\n");
    emit(&out, "varying vec4 gd_FrontColor;\n");
    if (spec->numLights > 0 && spec->twoSide) emit(&out, "varying vec4 gd_BackColor;\n");

    bool useTexture = false;
    for (int i = 0; i < options->numTextures; i++) {
        if (!spec->texUnits[i].mode) continue;
        emit(&out, "attribute vec4 gd_MultiTexCoord%d;\nvarying vec4 gd_TexCoord%d;\n", i, i);
        useTexture = true;
    }
    if (useTexture) emit(&out, "uniform mat4 gd_TextureMatrix;\n");

    if (spec->numLights > 0) {
        emit(&out,
            "struct Light {\n"
                "vec3 ambient;\n"
                "vec3 diffuse;\n"
                "vec3 specular;\n"
                "vec4 position;\n"
                "vec3 attenuation;\n"
                "vec2 spotCutoffExponent;\n"
                "vec3 spotDirection;\n"
            "};\n"
            "struct Material {\n"
                "vec3 ambient;\n"
                "vec3 diffuse;\n"
                "vec4 specular;\n"
                "vec3 emission;\n"
                "float alpha;\n"
            "};\n"
            "uniform Light lights[%d];\n"
            "uniform vec3 gd_LightModelAmbient;\n"
            "uniform Material materials[2];\n", spec->numLights);
    }

    if (spec->fogMode) emit(&out, "varying float gd_FogFragCoord;\n");

    if (spec->points) {
        emit(&out, SHADER_CHUNK_POINT
            "float getPointSizeAttenuation(gd_PointParameters point, vec3 viewPosition) {\n"
                "float distance = length(viewPosition);\n"
                "return inversesqrt(point.distanceConstantAttenuation + point.distanceLinearAttenuation * distance + point.distanceQuadraticAttenuation * (distance * distance));\n"
            "}\n");
    }

    emit(&out, "void main() {\n");
    emit(&out, "gd_FrontColor = gd_Color;\n");
    for (int i = 0; i < options->numTextures; i++) {
        if (spec->texUnits[i].mode) emit(&out, "gd_TexCoord%d = gd_TextureMatrix * gd_MultiTexCoord%d;\n", i, i);
    }

    if (options->transformVertex) {
        emit(&out, "vec4 position = gd_ModelViewMatrix * gd_Vertex;\n");
    }
    else emit(&out, "vec4 position = gd_Vertex;\n");

    if (spec->numLights > 0) emitLighting(&out, spec);

    if (spec->fogMode) emit(&out, "gd_FogFragCoord = -position.z;\n");

    if (options->transformVertex) {
        emit(&out, "gl_Position = gd_ProjectionMatrix * position;\n");
    }
    else emit(&out, "gl_Position = position;\n");

    if (spec->points) {
        emit(&out, "if (gd_Point.size > 0.0) gl_PointSize = clamp(gd_Point.size * getPointSizeAttenuation(gd_Point, position.xyz), gd_Point.sizeMin, gd_Point.sizeMax);\n");
    }
    emit(&out, "}\n");

    ArrayBuffer_put(&out, '\0');
    return (char*)out.buffer;
}

/* ---------------------------------------------------------------- Fragment-Shader */

/* Quelle eines GL_COMBINE-Arguments; `tex` ist die Variable mit der Texturfarbe der Einheit */
static void combineSource(char* dst, int size, uint8_t code, int unit, bool alpha) {
    const char* swizzle = alpha ? "a" : "rgb";
    switch (code) {
        case TE_SRC_PREVIOUS: snprintf(dst, size, "finalColor.%s", swizzle); break;
        case TE_SRC_TEXTURE: snprintf(dst, size, "tex%d.%s", unit, swizzle); break;
        case TE_SRC_CONSTANT: snprintf(dst, size, "gd_TexEnv%d.color.%s", unit, swizzle); break;
        case TE_SRC_PRIMARY_COLOR: snprintf(dst, size, "primaryColor.%s", swizzle); break;
        default: snprintf(dst, size, alpha ? "1.0" : "vec3(1.0)"); break;
    }
}

/* Operand eines GL_COMBINE-Arguments (sourceCode: Quelle, operandCode: Verknüpfung) */
static void combineOperand(char* dst, int size, uint8_t operandCode, uint8_t sourceCode, int unit, bool alpha) {
    char source[64];
    if (alpha) {
        combineSource(source, sizeof(source), sourceCode, unit, true);
        switch (operandCode) {
            case TE_OP_SRC_ALPHA: snprintf(dst, size, "%s", source); break;
            case TE_OP_ONE_MINUS_SRC_ALPHA: snprintf(dst, size, "(1.0 - %s)", source); break;
            default: snprintf(dst, size, "1.0"); break;
        }
    }
    else {
        switch (operandCode) {
            case TE_OP_SRC_COLOR:
                combineSource(dst, size, sourceCode, unit, false);
                break;
            case TE_OP_ONE_MINUS_SRC_COLOR:
                combineSource(source, sizeof(source), sourceCode, unit, false);
                snprintf(dst, size, "(vec3(1.0) - %s)", source);
                break;
            case TE_OP_SRC_ALPHA:
                combineSource(source, sizeof(source), sourceCode, unit, true);
                snprintf(dst, size, "vec3(%s)", source);
                break;
            case TE_OP_ONE_MINUS_SRC_ALPHA:
                combineSource(source, sizeof(source), sourceCode, unit, true);
                snprintf(dst, size, "vec3(1.0 - %s)", source);
                break;
            default:
                snprintf(dst, size, "vec3(1.0)");
                break;
        }
    }
}

static void emitTexUnit(ArrayBuffer* out, int unit, const FFTexUnit* tu) {
    if (tu->flags & FF_TEXUNIT_LOD_BIAS) {
        emit(out, "vec4 tex%d = texture2DProj(gd_Texture%d, gd_TexCoord%d, gd_TexEnv%d.lodBias);\n", unit, unit, unit, unit);
    }
    else emit(out, "vec4 tex%d = texture2DProj(gd_Texture%d, gd_TexCoord%d);\n", unit, unit, unit);

    /* Argumente: c0/a0 = Textur bzw. Arg0, c1/a1 = bisherige Fragmentfarbe bzw. Arg1 */
    char c0[96], c1[96], a0[96], a1[96];
    int modeRGB = tu->mode;
    int modeAlpha = TE_MODULATE;
    const char* alpha = NULL; /* fester Alpha-Ausdruck; sonst nach modeAlpha */
    if (tu->mode == TE_COMBINE) {
        modeRGB = tu->combine[0];
        modeAlpha = tu->combine[1];
        combineOperand(c0, sizeof(c0), tu->operand[0], tu->source[0], unit, false);
        combineOperand(c1, sizeof(c1), tu->operand[1], tu->source[1], unit, false);
        combineOperand(a0, sizeof(a0), tu->operand[2], tu->source[2], unit, true);
        combineOperand(a1, sizeof(a1), tu->operand[3], tu->source[3], unit, true);
    }
    else {
        snprintf(c0, sizeof(c0), "tex%d.rgb", unit);
        snprintf(c1, sizeof(c1), "finalColor.rgb");
        snprintf(a0, sizeof(a0), "tex%d.a", unit);
        snprintf(a1, sizeof(a1), "finalColor.a");
        if (tu->mode == TE_DECAL) alpha = "a1";
        else if (tu->mode == TE_REPLACE && (tu->flags & FF_TEXUNIT_ALPHA_KNOWN)) {
            /* Alpha der Textur, wenn das Format eines hat, sonst das des Fragments */
            alpha = (tu->flags & FF_TEXUNIT_HAS_ALPHA) ? "a0" : "a1";
        }
    }

    emit(out, "{\n");
    emit(out, "vec3 c0 = %s;\nvec3 c1 = %s;\nfloat a0 = %s;\nfloat a1 = %s;\n", c0, c1, a0, a1);

    const char* rgb;
    switch (modeRGB) {
        case TE_MODULATE: rgb = "c0 * c1"; break;
        case TE_DECAL: rgb = "c1 * (1.0 - a0) + c0 * a0"; break;
        case TE_INTERPOLATE: emit(out, "vec3 cc = gd_TexEnv%d.color.rgb;\n", unit); rgb = "c1 * (vec3(1.0) - c0) + cc * c0"; break;
        case TE_ADD: rgb = "c0 + c1"; break;
        case TE_ADD_SIGNED: rgb = "c0 + c1 - 0.5"; break;
        case TE_SUBTRACT: rgb = "c0 - c1"; break;
        case TE_DOT3_RGB:
        case TE_DOT3_RGBA:
            emit(out, "float dot3 = 4.0 * dot(c0 - 0.5, c1 - 0.5);\n");
            rgb = "vec3(dot3)";
            alpha = modeRGB == TE_DOT3_RGBA ? "dot3" : "a0";
            break;
        default: rgb = "c0"; break; /* REPLACE und Unbekanntes */
    }
    emit(out, "vec3 rgb = %s;\n", rgb);

    if (!alpha) {
        switch (modeAlpha) {
            case TE_MODULATE: alpha = "a0 * a1"; break;
            case TE_INTERPOLATE: emit(out, "float ca = gd_TexEnv%d.color.a;\n", unit); alpha = "a1 * (1.0 - a0) + ca * a0"; break;
            case TE_ADD: alpha = "a0 + a1"; break;
            case TE_ADD_SIGNED: alpha = "a0 + a1 - 0.5"; break;
            case TE_SUBTRACT: alpha = "a0 - a1"; break;
            default: alpha = "a0"; break; /* REPLACE und Unbekanntes */
        }
    }
    emit(out, "float alpha = %s;\n", alpha);

    if (tu->flags & FF_TEXUNIT_SCALE) {
        emit(out, "finalColor = vec4(rgb * gd_TexEnv%d.rgbaScale.x, alpha * gd_TexEnv%d.rgbaScale.y);\n", unit, unit);
    }
    else emit(out, "finalColor = vec4(rgb, alpha);\n");
    emit(out, "}\n");
}

char* ShaderMaterialES2_fragmentSource(const MaterialOptions* options) {
    const FFSpec* spec = options->spec;
    ArrayBuffer out = {0};

    emit(&out, "%s", precisionHeader());
    emit(&out, "varying vec4 gd_FrontColor;\n");
    bool twoSided = spec->numLights > 0 && spec->twoSide;
    if (twoSided) emit(&out, "varying vec4 gd_BackColor;\n");

    bool useTexture = false;
    for (int i = 0; i < options->numTextures; i++) {
        if (!spec->texUnits[i].mode) continue;
        /* Struktur wie im allgemeinen Shader, damit die Uniform-Namen gd_TexEnv%d.color usw. passen */
        if (!useTexture) emit(&out, "struct gd_TexEnvParameters { vec4 color; vec2 rgbaScale; float lodBias; };\n");
        useTexture = true;
        emit(&out, "uniform sampler2D gd_Texture%d;\nvarying vec4 gd_TexCoord%d;\nuniform gd_TexEnvParameters gd_TexEnv%d;\n", i, i, i);
    }

    if (spec->fogMode) emit(&out, SHADER_CHUNK_FOG "varying float gd_FogFragCoord;\n");
    if (spec->alphaFunc) emit(&out, "uniform vec2 gd_AlphaTest;\n");
    if (options->pointSprite) emit(&out, "uniform sampler2D gd_PointSprite;\n" SHADER_CHUNK_POINT);

    emit(&out, "void main() {\n");
    /* Primärfarbe; bei zweiseitiger Beleuchtung nach Orientierung des Dreiecks (wie GL) */
    emit(&out, twoSided ? "vec4 primaryColor = gl_FrontFacing ? gd_FrontColor : gd_BackColor;\n" : "vec4 primaryColor = gd_FrontColor;\n");
    emit(&out, "vec4 finalColor = primaryColor;\n");

    if (options->pointSprite) {
        emit(&out,
            "if (gd_Point.size > 0.0) {\n"
                "finalColor *= texture2D(gd_PointSprite, gl_PointCoord);\n"
                "if (finalColor.a <= 0.1) discard;\n"
            "}\n");
    }

    for (int i = 0; i < options->numTextures; i++) {
        if (spec->texUnits[i].mode) emitTexUnit(&out, i, &spec->texUnits[i]);
    }

    switch (spec->fogMode) {
        case FOG_LINEAR:
            emit(&out, "finalColor.rgb = mix(finalColor.rgb, gd_Fog.color.rgb, smoothstep(gd_Fog.start, gd_Fog.end, gd_FogFragCoord));\n");
            break;
        case FOG_EXP:
            emit(&out, "finalColor.rgb = mix(finalColor.rgb, gd_Fog.color.rgb, 1.0 - clamp(exp(-gd_Fog.density * gd_FogFragCoord), 0.0, 1.0));\n");
            break;
        case FOG_EXP2:
            emit(&out, "finalColor.rgb = mix(finalColor.rgb, gd_Fog.color.rgb, 1.0 - clamp(exp2(-gd_Fog.density * gd_Fog.density * gd_FogFragCoord * gd_FogFragCoord * 1.442695), 0.0, 1.0));\n");
            break;
        default:
            break;
    }

    /* Alphatest: verwerfen, wenn die Bedingung NICHT erfüllt ist */
    switch (spec->alphaFunc) {
        case AT_NEVER: emit(&out, "discard;\n"); break;
        case AT_LESS: emit(&out, "if (finalColor.a >= gd_AlphaTest.y) discard;\n"); break;
        case AT_EQUAL: emit(&out, "if (finalColor.a != gd_AlphaTest.y) discard;\n"); break;
        case AT_LEQUAL: emit(&out, "if (finalColor.a > gd_AlphaTest.y) discard;\n"); break;
        case AT_GREATER: emit(&out, "if (finalColor.a <= gd_AlphaTest.y) discard;\n"); break;
        case AT_NOTEQUAL: emit(&out, "if (finalColor.a == gd_AlphaTest.y) discard;\n"); break;
        case AT_GEQUAL: emit(&out, "if (finalColor.a < gd_AlphaTest.y) discard;\n"); break;
        default: break; /* ALWAYS oder kein Alphatest */
    }

    emit(&out, "gl_FragColor = finalColor;\n}\n");
    ArrayBuffer_put(&out, '\0');
    return (char*)out.buffer;
}
