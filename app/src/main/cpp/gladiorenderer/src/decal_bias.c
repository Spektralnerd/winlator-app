#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <strings.h>
#include <stdint.h>
#include "decal_bias.h"
#include "winlator.h"

static struct {
    int enabled;         /* -1: noch nicht gelesen */
    float biasFactor, biasUnits;
    uint64_t draws;      /* Zeichenaufrufe mit Versatz seit der letzten Statistik */
} decal = {.enabled = -1, .biasFactor = -0.5f, .biasUnits = -2.0f};

static void readSettings(void) {
    decal.enabled = 1;
    const char* env = getenv("GLADIO_DECAL_BIAS");
    if (!env || !*env) return;
    if (strcmp(env, "0") == 0 || strcasecmp(env, "off") == 0) {
        decal.enabled = 0;
        return;
    }
    float factor, units;
    if (sscanf(env, "%f,%f", &factor, &units) == 2) {
        decal.biasFactor = factor;
        decal.biasUnits = units;
    }
    println("gladio: Tiefenversatz für Aufkleber: Faktor %g, Einheiten %g", decal.biasFactor, decal.biasUnits);
}

void DecalBias_capability(GLRenderer* renderer, GLenum cap, bool state) {
    if (!renderer) return;
    if (cap == GL_DEPTH_TEST) renderer->state.decal.depthTest = state;
    else if (cap == GL_POLYGON_OFFSET_FILL) renderer->state.decal.offsetFill = state;
}

void DecalBias_polygonOffset(GLRenderer* renderer, float factor, float units) {
    if (!renderer) return;
    renderer->state.decal.factor = factor;
    renderer->state.decal.units = units;
}

bool DecalBias_begin(GLRenderer* renderer) {
    if (!renderer) return false;
    ShaderProgram* program = renderer->clientState.program;
    if (!program || !program->fragmentDiscards || !renderer->state.decal.depthTest || renderer->state.decal.offsetFill) return false;
    if (decal.enabled < 0) readSettings();
    if (!decal.enabled) return false;
    glPolygonOffset(decal.biasFactor, decal.biasUnits);
    glEnable(GL_POLYGON_OFFSET_FILL);
    decal.draws++;
    return true;
}

void DecalBias_end(GLRenderer* renderer, bool active) {
    if (!active) return;
    glDisable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(renderer->state.decal.factor, renderer->state.decal.units);
}

void DecalBias_printStats(uint32_t frames) {
    if (decal.draws == 0) return;
    println("gladio: Tiefenversatz für Aufkleber (Shader mit discard): %u Zeichenaufrufe je Bild, Faktor %g, Einheiten %g",
            (uint32_t)(decal.draws / (frames ? frames : 1)), decal.biasFactor, decal.biasUnits);
    decal.draws = 0;
}

/* steht das Wort "discard" im Quelltext? (Kommentare zählen mit – wined3d schreibt keine) */
bool ShaderSource_hasDiscard(const char* source, int size) {
    static const char word[] = "discard";
    const int len = sizeof(word) - 1;
    if (!source || size < len) return false;
    for (int i = 0; i + len <= size; i++) {
        if (source[i] != 'd' || memcmp(source + i, word, len) != 0) continue;
        bool wordStart = i == 0 || !(isalnum((unsigned char)source[i-1]) || source[i-1] == '_');
        bool wordEnd = i + len >= size || !(isalnum((unsigned char)source[i+len]) || source[i+len] == '_');
        if (wordStart && wordEnd) return true;
    }
    return false;
}
