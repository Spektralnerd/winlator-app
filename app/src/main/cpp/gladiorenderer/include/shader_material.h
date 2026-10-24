#ifndef GLADIO_SHADER_MATERIAL_H
#define GLADIO_SHADER_MATERIAL_H

#include "gladio.h"
#include "arb_program.h"

/*
 * Die Shader bekommen Modi als kleine Zahlen statt der GL-Enums: In GLSL ES 1.00 hat ein
 * mediump int (mehr bietet z. B. die Mali-400 im Fragment-Shader nicht) nur etwa 10 Bit,
 * und mediump float stellt Werte wie 34160 (GL_COMBINE) nicht exakt dar.
 * ShaderMaterial_texEnvCode() usw. übersetzen die Enums beim Setzen der Uniforms.
 */
#define TEXENV_MODE_MODULATE "1"
#define TEXENV_MODE_DECAL "2"
#define TEXENV_MODE_INTERPOLATE "3"
#define TEXENV_MODE_ADD "4"
#define TEXENV_MODE_ADD_SIGNED "5"
#define TEXENV_MODE_SUBTRACT "6"
#define TEXENV_MODE_COMBINE "7"
#define TEXENV_MODE_DOT3_RGB "8"
#define TEXENV_MODE_DOT3_RGBA "9"
#define TEXENV_MODE_REPLACE "10"
#define TEXENV_COMBINE_TEXTURE "11"
#define TEXENV_COMBINE_CONSTANT "12"
#define TEXENV_COMBINE_PRIMARY_COLOR "13"
#define TEXENV_COMBINE_PREVIOUS "14"
#define TEXENV_COMBINE_SRC_COLOR "15"
#define TEXENV_COMBINE_ONE_MINUS_SRC_COLOR "16"
#define TEXENV_COMBINE_SRC_ALPHA "17"
#define TEXENV_COMBINE_ONE_MINUS_SRC_ALPHA "18"

#define ALPHA_TEST_FUNC_NEVER "20"
#define ALPHA_TEST_FUNC_LESS "21"
#define ALPHA_TEST_FUNC_EQUAL "22"
#define ALPHA_TEST_FUNC_LEQUAL "23"
#define ALPHA_TEST_FUNC_GREATER "24"
#define ALPHA_TEST_FUNC_NOTEQUAL "25"
#define ALPHA_TEST_FUNC_GEQUAL "26"
#define ALPHA_TEST_FUNC_ALWAYS "27"

#define FOG_MODE_LINEAR "30"
#define FOG_MODE_EXP "31"
#define FOG_MODE_EXP2 "32"

static inline int ShaderMaterial_texEnvCode(int mode) {
    switch (mode) {
        case GL_MODULATE: return 1;
        case GL_DECAL: return 2;
        case GL_INTERPOLATE: return 3;
        case GL_ADD: return 4;
        case GL_ADD_SIGNED: return 5;
        case GL_SUBTRACT: return 6;
        case GL_COMBINE: return 7;
        case GL_DOT3_RGB: return 8;
        case GL_DOT3_RGBA: return 9;
        case GL_REPLACE: return 10;
        case GL_TEXTURE: return 11;
        case GL_CONSTANT: return 12;
        case GL_PRIMARY_COLOR: return 13;
        case GL_PREVIOUS: return 14;
        case GL_SRC_COLOR: return 15;
        case GL_ONE_MINUS_SRC_COLOR: return 16;
        case GL_SRC_ALPHA: return 17;
        case GL_ONE_MINUS_SRC_ALPHA: return 18;
        default: return 0;
    }
}

static inline float ShaderMaterial_alphaTestCode(int func) {
    return func >= GL_NEVER && func <= GL_ALWAYS ? (float)(20 + (func - GL_NEVER)) : 27.0f;
}

static inline float ShaderMaterial_fogModeCode(int mode) {
    switch (mode) {
        case GL_LINEAR: return 30.0f;
        case GL_EXP: return 31.0f;
        case GL_EXP2: return 32.0f;
        default: return 0.0f;
    }
}

/*
 * Fester Zustand der Fixed-Function-Pipeline, der im ES-2.0-Pfad in den Shader einkompiliert wird
 * (shader_material_es2.c). Nur uint8_t-Felder, damit der Hash über die Bytes eindeutig ist.
 */
#define FF_LIGHT_POSITIONAL 1   /* position.w > 0 */
#define FF_LIGHT_SPOT 2         /* Spot-Exponent != 0 */
#define FF_LIGHT_ATTENUATION 4  /* Abschwächung != (1, 0, 0) */

#define FF_TEXUNIT_LOD_BIAS 1    /* GL_TEXTURE_LOD_BIAS != 0 */
#define FF_TEXUNIT_SCALE 2       /* RGB-/Alpha-Skalierung != 1 */
#define FF_TEXUNIT_ALPHA_KNOWN 4 /* FF_TEXUNIT_HAS_ALPHA ist aussagekräftig */
#define FF_TEXUNIT_HAS_ALPHA 8   /* Texturformat hat einen Alphakanal */

typedef struct FFTexUnit {
    uint8_t mode;       /* 0 = Einheit aus, sonst Code wie ShaderMaterial_texEnvCode() */
    uint8_t combine[2]; /* GL_COMBINE: Funktion für RGB und Alpha */
    uint8_t source[4];  /* GL_COMBINE: Quelle 0/1 RGB, Quelle 0/1 Alpha */
    uint8_t operand[4]; /* GL_COMBINE: Operand 0/1 RGB, Operand 0/1 Alpha */
    uint8_t flags;      /* FF_TEXUNIT_* */
} FFTexUnit;

typedef struct FFSpec {
    uint8_t numLights;              /* eingeschaltete Lichter (dicht gepackt wie in setLightUniforms) */
    uint8_t lightFlags[MAX_LIGHTS]; /* FF_LIGHT_* je Licht */
    uint8_t alphaFunc;              /* Code wie ShaderMaterial_alphaTestCode(), 0 = kein Alphatest */
    uint8_t fogMode;                /* Code wie ShaderMaterial_fogModeCode(), 0 = kein Nebel */
    uint8_t points;                 /* Stapel enthält Punkte: gl_PointSize wird berechnet */
    FFTexUnit texUnits[MAX_TEXTURES];
} FFSpec;

typedef struct MaterialOptions {
    bool lighting;
    bool alphaTest;
    bool fog;
    bool pointSprite;
    bool transformVertex;
    uint8_t numTextures;
    ARBProgram* vertexProgram;
    ARBProgram* fragmentProgram;
    const FFSpec* spec; /* ES 2.0: einkompilierter Fixed-Function-Zustand, sonst NULL */
} MaterialOptions;

typedef struct ShaderMaterial {
    GLuint program;

    struct {
        int attributes[VERTEX_ATTRIB_COUNT];
        int projectionMatrix;
        int modelViewMatrix;
        int textureMatrix;
        int alphaTest;
        int useTexture;
        int texture[MAX_TEXTURES];
        int texEnv[MAX_TEXTURES][7];
        int point[7];
        int fog[5];

        int lights[MAX_LIGHTS][7];
        int numLights;
        int lightModelAmbient;

        int materials[2][4];
    } location;
} ShaderMaterial;

extern ShaderMaterial* ShaderMaterial_create(MaterialOptions* options);
extern void ShaderMaterial_destroy(ShaderMaterial* material);
extern void ShaderMaterial_updatePointUniforms(ShaderMaterial* material, GLRenderer* renderer);
extern void ShaderMaterial_updateUniforms(ShaderMaterial* material, GLRenderer* renderer, MaterialOptions* options);

/* shader_material_es2.c: Fixed-Function-Zustand erfassen und spezialisierte GLSL-ES-1.00-Shader erzeugen */
extern void FFSpec_fromState(FFSpec* spec, GLRenderer* renderer, const MaterialOptions* options, bool points);
extern char* ShaderMaterialES2_vertexSource(const MaterialOptions* options);
extern char* ShaderMaterialES2_fragmentSource(const MaterialOptions* options);

static inline uint32_t generateMaterialHash(MaterialOptions* options) {
    enum { BASE_KEY_SIZE = 21 };
    char key[BASE_KEY_SIZE + sizeof(FFSpec)] = {0, 0, 0, 0, '-', 0, 0, 0, 0, '-', INT2CHR(options->lighting), '-', INT2CHR(options->alphaTest), '-', INT2CHR(options->fog), '-', INT2CHR(options->pointSprite), '-', INT2CHR(options->transformVertex), '-', INT2CHR(options->numTextures)};
    *(int*)(key+0) = options->vertexProgram ? options->vertexProgram->id : 0;
    *(int*)(key+5) = options->fragmentProgram ? options->fragmentProgram->id : 0;
    if (!options->spec) return fnv1aHash32(key, BASE_KEY_SIZE);
    memcpy(key + BASE_KEY_SIZE, options->spec, sizeof(FFSpec));
    return fnv1aHash32(key, sizeof(key));
}

static inline void ShaderMaterial_getFogUniformLocations(GLuint program, int* locations) {
    char uniformName[64] = {0};
    const char* fogStructNames[] = {"color", "density", "start", "end", "scale"};
    for (int i = 0; i < ARRAY_SIZE(fogStructNames); i++) {
        sprintf(uniformName, "gd_Fog.%s", fogStructNames[i]);
        locations[i] = glGetUniformLocation(program, uniformName);
    }
}

static inline void ShaderMaterial_getPointUniformLocations(GLuint program, int* locations) {
    char uniformName[64] = {0};
    const char* pointStructNames[] = {"size", "sizeMin", "sizeMax", "fadeThresholdSize", "distanceConstantAttenuation", "distanceLinearAttenuation", "distanceQuadraticAttenuation"};
    for (int i = 0; i < ARRAY_SIZE(pointStructNames); i++) {
        sprintf(uniformName, "gd_Point.%s", pointStructNames[i]);
        locations[i] = glGetUniformLocation(program, uniformName);
    }
}

#endif