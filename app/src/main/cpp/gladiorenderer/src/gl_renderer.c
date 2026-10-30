#include "gladio.h"
#include "gl_renderer.h"
#include "shader_material.h"
#include "gl_context.h"
#include "arb_program.h"
#include "gl_es2.h"

thread_local GLRenderer* currentRenderer = NULL;

static void initRaster(GLRenderer* renderer) {
    if (renderer->raster) return;
    GLRaster* raster = calloc(1, sizeof(GLRaster));

    const float position[] = {0.0f, 0.0f, 0.0f, 1.0f};
    memcpy(raster->position, position, sizeof(position));

    raster->pixelZoom[0] = 1.0f;
    raster->pixelZoom[1] = 1.0f;

    glGenTextures(1, &raster->textureId);
    glBindTexture(GL_TEXTURE_2D, raster->textureId);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    GLTexture* texture = GLTexture_getBound(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, texture ? texture->id : 0);

    renderer->raster = raster;
}

static void initMaterials(GLRenderer* renderer) {
    if (renderer->materials) return;
    GLMaterial* materials = calloc(2, sizeof(GLMaterial));

    const float ambient[] = {0.2f, 0.2f, 0.2f};
    const float diffuse[] = {0.8f, 0.8f, 0.8f};

    memcpy(materials[0].ambient, ambient, sizeof(ambient));
    memcpy(materials[0].diffuse, diffuse, sizeof(diffuse));
    memcpy(materials[1].ambient, ambient, sizeof(ambient));
    memcpy(materials[1].diffuse, diffuse, sizeof(diffuse));
    materials[0].alpha = materials[1].alpha = 1.0f;

    renderer->materials = materials;
}

/* Vorgaben laut GL: Umgebungslicht 0 für alle Lichter, diffus/spekular 1 nur für GL_LIGHT0 */
static void initLight(GLLight* light, int index) {
    const float ambient[] = {0.0f, 0.0f, 0.0f};
    const float diffuse[] = {1.0f, 1.0f, 1.0f};
    const float black[] = {0.0f, 0.0f, 0.0f};
    const float position[] = {0.0f, 0.0f, 1.0f, 0.0f};
    const float attenuation[] = {1.0f, 0.0f, 0.0f};
    const float spotDirection[] = {0.0f, 0.0f, -1.0f};

    memcpy(light->ambient, ambient, sizeof(ambient));
    memcpy(light->diffuse, index == 0 ? diffuse : black, sizeof(diffuse));
    memcpy(light->specular, index == 0 ? diffuse : black, sizeof(diffuse));
    memcpy(light->position, position, sizeof(position));
    memcpy(light->attenuation, attenuation, sizeof(attenuation));
    memcpy(light->spotDirection, spotDirection, sizeof(spotDirection));
    light->spotCutoffExponent[0] = TO_RADIANS(180);
}

void GLRenderer_initOnEGLContext(GLRenderer* renderer) {
    const float color[] = {1.0f, 1.0f, 1.0f, 1.0f};
    const float normal[] = {0.0f, 0.0f, 1.0f};
    const float texCoord[] = {0.0f, 0.0f, 0.0f, 1.0f};

    const float lightModelAmbient[] = {0.2f, 0.2f, 0.2f, 1.0f};
    memcpy(renderer->state.color, color, sizeof(color));
    memcpy(renderer->state.normal, normal, sizeof(normal));
    memcpy(renderer->state.lightModelAmbient, lightModelAmbient, sizeof(lightModelAmbient));

    for (int i = 0; i < MAX_TEXTURES; i++) {
        memcpy(renderer->state.texCoords[i], texCoord, sizeof(texCoord));

        GLfloat param = GL_MODULATE;
        GLfloat scale = 1.0f;
        renderer->clientState.activeTexture = i;
        GLRenderer_setTexEnvParams(renderer, GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &param);
        GLRenderer_setTexEnvParams(renderer, GL_TEXTURE_ENV, GL_COMBINE_RGB, &param);
        GLRenderer_setTexEnvParams(renderer, GL_TEXTURE_ENV, GL_RGB_SCALE, &scale);
        GLRenderer_setTexEnvParams(renderer, GL_TEXTURE_ENV, GL_ALPHA_SCALE, &scale);
    }
    renderer->clientState.activeTexture = 0;
    renderer->state.polygonMode = GL_FILL;

    renderer->state.point.size = 1.0f;
    renderer->state.point.sizeMin = 0.0f;
    renderer->state.point.sizeMax = 1.0f;
    renderer->state.point.fadeThresholdSize = 1.0f;
    renderer->state.point.spriteCoordOrigin = GL_UPPER_LEFT;
    const float distanceAttenuation[] = {1.0f, 0.0f, 0.0f};
    memcpy(renderer->state.point.distanceAttenuation, distanceAttenuation, sizeof(distanceAttenuation));

    renderer->state.fog.mode = GL_EXP;
    renderer->state.fog.end = 1.0f;
    renderer->state.fog.density = 1.0f;

    renderer->state.alphaTest.func = GL_ALWAYS;
    renderer->state.alphaTest.ref = 0.0f;

    renderer->state.colorMaterial.face = GL_FRONT_AND_BACK;
    renderer->state.colorMaterial.mode = GL_AMBIENT_AND_DIFFUSE;

    renderer->state.shadeModel = GL_SMOOTH;
    renderer->state.clampReadColor = false;

    for (int i = 0; i < MAX_LIGHTS; i++) initLight(&renderer->lights[i], i);
    /* Vorgabematerial (ambient 0.2, diffuse 0.8) auch ohne glMaterial-Aufruf der Anwendung */
    initMaterials(renderer);

    glGenBuffers(ARRAY_SIZE(renderer->bufferIds), renderer->bufferIds);

    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (!GLES2) {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    }

    renderer->queriesStartTime = nanoTime();
}

/* Stapel des festen Funktionsmodells (glBegin/glEnd, Client-Arrays der Anwendung) als Client-Arrays zeichnen:
 * der Treiber kopiert die Daten beim Zeichnen in seinen Bildspeicher. Das ist billiger als je Stapel neue
 * Pufferobjekte anzulegen (glBufferData = neue GPU-Speicherzuteilung) – auf ES 2.0 immer, auf ES 3.x,
 * solange kein Vertex-Array-Objekt der Anwendung gebunden ist (ES 3 erlaubt Client-Arrays nur mit dem
 * Vorgabe-VAO). Ein Intro mit ~3000 glBegin/glEnd je Bild lief über ES 3 so am PC 3 statt 20 Bilder/s. */
static inline bool useClientArrays(GLRenderer* renderer) {
    return GLES2 || renderer->clientState.vao == &renderer->clientState.defaultVAO;
}

static void bindVertexBuffer(GLRenderer* renderer, GLuint bufferId, GLint location, int itemSize, int size, const void* data) {
    if (useClientArrays(renderer)) {
        /* die Geometriepuffer bleiben bis zum nächsten Stapel gültig */
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        GLRenderer_enableVertexAttribute(renderer, location);
        glVertexAttribPointer(location, itemSize, GL_FLOAT, GL_FALSE, 0, data);
        return;
    }
    glBindBuffer(GL_ARRAY_BUFFER, bufferId);
    glBufferData(GL_ARRAY_BUFFER, size, data, GL_DYNAMIC_DRAW);
    GLRenderer_enableVertexAttribute(renderer, location);
    glVertexAttribPointer(location, itemSize, GL_FLOAT, GL_FALSE, 0, (void*)0);
}

static void normalizeTexCoords(GLTexture* texture, ArrayBuffer* texCoords) {
    float* vectors = (float*)texCoords->buffer;
    float invWidth = 1.0f / texture->width;
    float invHeight = 1.0f / texture->height;
    for (int i = 0, count = texCoords->size / sizeof(float); i < count; i += 4) {
        vectors[i+0] *= invWidth;
        vectors[i+1] *= invHeight;
    }
}

static void updateVertexBuffers(GLRenderer* renderer, int* attribLocations) {
    Geometry* geometry = &renderer->geometry;
    if (useClientArrays(renderer)) glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0); /* Indizes als Client-Array (siehe bindVertexBuffer) */
    else {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, renderer->bufferIds[0]);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, geometry->indices.size, geometry->indices.buffer, GL_DYNAMIC_DRAW);
    }

    if (attribLocations[POSITION_ARRAY_INDEX] != -1) {
        bindVertexBuffer(renderer, renderer->bufferIds[1], attribLocations[POSITION_ARRAY_INDEX], 4, geometry->vertices.size, geometry->vertices.buffer);
    }

    if (attribLocations[COLOR_ARRAY_INDEX] != -1) {
        if (geometry->colors.size > 0) {
            bindVertexBuffer(renderer, renderer->bufferIds[2], attribLocations[COLOR_ARRAY_INDEX], 4, geometry->colors.size, geometry->colors.buffer);
        }
        else {
            GLRenderer_disableVertexAttribute(renderer, attribLocations[COLOR_ARRAY_INDEX]);
            glVertexAttrib4fv(attribLocations[COLOR_ARRAY_INDEX], renderer->state.color);
        }
    }

    if (attribLocations[NORMAL_ARRAY_INDEX] != -1 && geometry->normals.size > 0) {
        bindVertexBuffer(renderer, renderer->bufferIds[3], attribLocations[NORMAL_ARRAY_INDEX], 3, geometry->normals.size, geometry->normals.buffer);
    }
    else if (attribLocations[NORMAL_ARRAY_INDEX] != -1) {
        /* feste Normale für den ganzen Stapel (glNormal ohne Normalen-Array) */
        GLRenderer_disableVertexAttribute(renderer, attribLocations[NORMAL_ARRAY_INDEX]);
        glVertexAttrib3fv(attribLocations[NORMAL_ARRAY_INDEX], renderer->state.normal);
    }

    for (int i = 0, j = TEXCOORD_ARRAY_INDEX, k, count; i < MAX_TEXTURES; i++, j++) {
        if (attribLocations[j] != -1 && geometry->texCoords[i].size > 0) {
            GLTexture* texture = renderer->clientState.texture[i][indexOfGLTarget(GL_TEXTURE_2D)];
            if (texture && texture->normalizeCoords) normalizeTexCoords(texture, &geometry->texCoords[i]);
            bindVertexBuffer(renderer, renderer->bufferIds[j+1], attribLocations[j], 4, geometry->texCoords[i].size, geometry->texCoords[i].buffer);
        }
        else if (attribLocations[j] != -1) {
            /* feste Texturkoordinate für den ganzen Stapel (glTexCoord/glMultiTexCoord ohne Array) */
            GLRenderer_disableVertexAttribute(renderer, attribLocations[j]);
            float texCoord[4];
            memcpy(texCoord, renderer->state.texCoords[i], sizeof(texCoord));
            GLTexture* texture = renderer->clientState.texture[i][indexOfGLTarget(GL_TEXTURE_2D)];
            if (texture && texture->normalizeCoords && texture->width > 0 && texture->height > 0) {
                texCoord[0] /= texture->width;
                texCoord[1] /= texture->height;
            }
            glVertexAttrib4fv(attribLocations[j], texCoord);
        }
        geometry->texCoords[i].size = 0;
    }

    geometry->indices.size = 0;
    geometry->vertices.size = 0;
    geometry->colors.size = 0;
    geometry->normals.size = 0;
}

static GLenum parseDrawMode(GLenum drawMode, GLenum polygonMode) {
    if (polygonMode == GL_POINT) {
        drawMode = GL_POINTS;
    }
    else if (polygonMode == GL_LINE) {
        drawMode = GL_LINES;
    }
    if (drawMode == GL_QUADS ||
        drawMode == GL_QUAD_STRIP ||
        drawMode == GL_TRIANGLE_STRIP ||
        drawMode == GL_TRIANGLE_FAN ||
        drawMode == GL_POLYGON) {
        drawMode = GL_TRIANGLES;
    }
    return drawMode;
}

static void updatePointSmoothTexture(GLRenderer* renderer) {
    if (renderer->state.point.smooth && !renderer->state.point.sprite) {
        if (renderer->circleTexture == 0) {
            glGenTextures(1, &renderer->circleTexture);
            glBindTexture(GL_TEXTURE_2D, renderer->circleTexture);

            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

            const uint8_t white[] = {255, 255, 255, 255};
            uint8_t* pixels = drawSmoothCircle(64, 64, white);
            glTexImage2D(GL_TEXTURE_2D, 0, GLES2 ? GL_RGBA : GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
            free(pixels);
            glBindTexture(GL_TEXTURE_2D, 0);
        }

        glBindTexture(GL_TEXTURE_2D, renderer->circleTexture);
    }
}

/* Stapel enthält Punkte (dann berechnet der Vertex-Shader gl_PointSize) */
static bool hasPointMeshes(GLRenderer* renderer) {
    ArrayDeque* meshes = &renderer->meshes;
    for (int i = meshes->head; i != meshes->tail; i = (i + 1) & (meshes->size - 1)) {
        Mesh* mesh = meshes->elements[i];
        if (mesh && parseDrawMode(mesh->mode, renderer->state.polygonMode) == GL_POINTS) return true;
    }
    return false;
}

static ShaderMaterial* setCurrentMaterial(GLRenderer* renderer, MaterialOptions* options) {
    FFSpec spec;
    if (GLES2 && !options->vertexProgram && !options->fragmentProgram) {
        /* ES 2.0: Fixed-Function-Zustand einkompilieren statt per Uniform zu verzweigen */
        FFSpec_fromState(&spec, renderer, options, hasPointMeshes(renderer));
        options->spec = &spec;
    }
    uint32_t hash = generateMaterialHash(options);
    ShaderMaterial* material = SparseArray_get(&renderer->materialMap, hash);
    if (!material) {
        material = ShaderMaterial_create(options);
        SparseArray_put(&renderer->materialMap, hash, material);
    }

    glUseProgram(material->program);
    ShaderMaterial_updateUniforms(material, renderer, options);
    options->spec = NULL;
    return material;
}

bool GLRenderer_useARBProgram(GLRenderer* renderer, bool fullUpdate) {
    if (!ARBProgram_isActive(GL_VERTEX_PROGRAM_ARB) && !ARBProgram_isActive(GL_FRAGMENT_PROGRAM_ARB)) return false;
    GLClientState* clientState = &renderer->clientState;
    ARBProgram* vertexProgram = clientState->arbProgram[indexOfGLTarget(GL_VERTEX_PROGRAM_ARB)];
    ARBProgram* fragmentProgram = clientState->arbProgram[indexOfGLTarget(GL_FRAGMENT_PROGRAM_ARB)];

    uint8_t numTextures = vertexProgram && fragmentProgram ? MAX(vertexProgram->numTextures, fragmentProgram->numTextures) : MAX_TEXTURES;
    ShaderMaterial* material = vertexProgram ? vertexProgram->material : fragmentProgram->material;
    if (!material) {
        MaterialOptions options = {false, true, false, false, false, numTextures, vertexProgram, fragmentProgram};
        material = setCurrentMaterial(renderer, &options);
        if (vertexProgram) vertexProgram->material = material;
        if (fragmentProgram) fragmentProgram->material = material;
    }
    else {
        glUseProgram(material->program);
        if (fullUpdate) {
            MaterialOptions options = {false, true, false, false, false, numTextures, vertexProgram, fragmentProgram};
            ShaderMaterial_updateUniforms(material, renderer, &options);
        }
    }

    if (fullUpdate && material->location.attributes[COLOR_ARRAY_INDEX] != -1) {
        GLVertexAttrib* colorAttrib = &clientState->vao->attribs[COLOR_ARRAY_INDEX];
        if (!colorAttrib->state && !colorAttrib->boundArrayBuffer) {
            GLRenderer_disableVertexAttribute(renderer, material->location.attributes[COLOR_ARRAY_INDEX]);
            glVertexAttrib4fv(material->location.attributes[COLOR_ARRAY_INDEX], renderer->state.color);
        }
    }
    return true;
}

void GLRenderer_drawImmediate(GLRenderer* renderer) {
    if (!renderer || ArrayDeque_isEmpty(&renderer->meshes)) return;
    GLClientState* clientState = &renderer->clientState;
    int64_t flushStart = gladio_nowNs();

    ShaderMaterial* material = NULL;
    if (!clientState->program && !ARBProgram_isActive(GL_VERTEX_PROGRAM_ARB)) {
        uint8_t numTextures = 0;
        for (int i = 0; i < MAX_TEXTURES; i++) if (renderer->state.enabledTextures[i]) numTextures = i+1;

        bool pointSprite = renderer->state.point.sprite || renderer->state.point.smooth;
        MaterialOptions options = {renderer->state.lighting, renderer->state.alphaTest.enabled, renderer->state.fog.enabled, pointSprite, true, numTextures, NULL, NULL};
        material = setCurrentMaterial(renderer, &options);
        updateVertexBuffers(renderer, material->location.attributes);
    }
    else {
        ShaderConverter_updateBoundProgram();
        if (clientState->program) {
            updateVertexBuffers(renderer, clientState->program->location.attributes);
        }
        else updateVertexBuffers(renderer, clientState->arbProgram[0]->material->location.attributes);
    }

    int64_t driverDrawStart = gladio_nowNs();
    gladio_batchStats.setupNs += driverDrawStart - flushStart;
    gladio_batchStats.batches++;
    while (!ArrayDeque_isEmpty(&renderer->meshes)) {
        Mesh* mesh = ArrayDeque_removeFirst(&renderer->meshes);
        GLenum drawMode = parseDrawMode(mesh->mode, renderer->state.polygonMode);

        if (material) {
            if (drawMode == GL_POINTS) {
                ShaderMaterial_updatePointUniforms(material, renderer);
                /* Der Kreis für geglättete Punkte gehört auf Einheit 0 (gd_PointSprite), auch wenn
                 * beim Ausgeben eine andere Einheit aktiv ist (glActiveTexture gibt den Stapel nicht aus) */
                uint8_t activeTexture = renderer->clientState.activeTexture;
                if (activeTexture != 0) glActiveTexture(GL_TEXTURE0);
                updatePointSmoothTexture(renderer);
                if (activeTexture != 0) glActiveTexture(GL_TEXTURE0 + activeTexture);
            }
            else ShaderMaterial_disablePointSize(material);
        }

        int count = mesh->end - mesh->start;
        const void* indices = useClientArrays(renderer) ? (const void*)(renderer->geometry.indices.buffer + mesh->start * sizeof(short)) : (const void*)(mesh->start * sizeof(short));
        glDrawElements(drawMode, count, GL_UNSIGNED_SHORT, indices);
        gladio_batchStats.driverDraws++;
        MEMFREE(mesh);

        if (drawMode == GL_POINTS && renderer->state.point.smooth && !renderer->state.point.sprite) {
            uint8_t activeTexture = renderer->clientState.activeTexture;
            GLTexture* texture = renderer->clientState.texture[0][indexOfGLTarget(GL_TEXTURE_2D)];
            if (activeTexture != 0) glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, texture ? texture->id : 0);
            if (activeTexture != 0) glActiveTexture(GL_TEXTURE0 + activeTexture);
        }
    }

    /* Kein glFlush je Stapel (früher auf ES 3): auf Kachel-Renderern (Mali) lässt ein glFlush mitten im
     * Bild den Treiber das Bild bis hierher rendern und Farbe+Tiefe in den Speicher schreiben – bei
     * Tausenden Stapeln je Bild kostet das ein Vielfaches des Zeichnens. Für die Richtigkeit ist er
     * nicht nötig. */
    gladio_batchStats.driverDrawNs += gladio_nowNs() - driverDrawStart;
    renderer->geometry.indices.position = 0;
    renderer->geometry.vertices.position = 0;
    GLRenderer_disableUnusedVertexAttributes(renderer);

    if (clientState->program) {
        for (int i = 0; i < VERTEX_ATTRIB_COUNT; i++) {
            if (clientState->vao->attribs[i].boundArrayBuffer > 0) {
                GLVertexAttrib* vertexAttrib = &clientState->vao->attribs[i];
                glBindBuffer(GL_ARRAY_BUFFER, clientState->vao->attribs[i].boundArrayBuffer);
                int location = clientState->program->location.attributes[i];
                GLRenderer_enableVertexAttribute(renderer, location);
                glVertexAttribPointer(location, vertexAttrib->size, vertexAttrib->type, vertexAttrib->normalized, vertexAttrib->stride, vertexAttrib->pointer);
            }
        }
    }

    GLBuffer* arrayBuffer = clientState->vao->buffer[indexOfGLTarget(GL_ARRAY_BUFFER)];
    GLBuffer* elementArrayBuffer = clientState->vao->buffer[indexOfGLTarget(GL_ELEMENT_ARRAY_BUFFER)];
    glBindBuffer(GL_ARRAY_BUFFER, arrayBuffer ? arrayBuffer->id : 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, elementArrayBuffer ? elementArrayBuffer->id : 0);
    gladio_batchStats.flushNs += gladio_nowNs() - flushStart;
}

void GLRenderer_beginImmediate(GLRenderer* renderer, GLenum mode) {
    if (renderer->activeMesh) return;
    Mesh* prevMesh = ArrayDeque_getLast(&renderer->meshes);
    if (prevMesh && parseDrawMode(prevMesh->mode, renderer->state.polygonMode) == GL_TRIANGLES &&
        parseDrawMode(mode, renderer->state.polygonMode) == GL_TRIANGLES) {
        prevMesh->mode = mode;
        prevMesh->indexCount = 0;
        prevMesh->end = renderer->geometry.indices.position;
        renderer->activeMesh = prevMesh;
        return;
    }

    Mesh* mesh = calloc(1, sizeof(Mesh));
    mesh->mode = mode;
    mesh->start = mesh->end = renderer->geometry.indices.position;
    renderer->activeMesh = mesh;
    ArrayDeque_addLast(&renderer->meshes, mesh);
}

void GLRenderer_endImmediate(GLRenderer* renderer) {
    if (renderer->activeMesh) {
        renderer->activeMesh->end = renderer->geometry.indices.position;
        renderer->activeMesh = NULL;

        renderer->geometry.colors.position = 0;
        renderer->geometry.normals.position = 0;

        for (int i = 0; i < MAX_TEXTURES; i++) renderer->geometry.texCoords[i].position = 0;
    }
}

float* GLRenderer_getMatrixFromStack(GLRenderer* renderer, int index) {
    static float identityMatrix[] = IDENTITY_MATRIX;
    ArrayList* matrixStack = &renderer->matrixStack[index];
    return matrixStack->size > 0 ? matrixStack->elements[matrixStack->size-1] : identityMatrix;
}

float* GLRenderer_getCurrentMatrix(GLRenderer* renderer) {
    ArrayList* matrixStack = &renderer->matrixStack[renderer->matrixIndex];
    if (matrixStack->size == 0) ArrayList_add(matrixStack, mat4_alloc());
    return matrixStack->elements[matrixStack->size-1];
}

void GLRenderer_pushMatrix(GLRenderer* renderer) {
    ArrayList* matrixStack = &renderer->matrixStack[renderer->matrixIndex];
    if (matrixStack->size < 32) {
        float* matrix = GLRenderer_getCurrentMatrix(renderer);
        ArrayList_add(matrixStack, mat4_clone(matrix));
    }
}

void GLRenderer_popMatrix(GLRenderer* renderer) {
    ArrayList* matrixStack = &renderer->matrixStack[renderer->matrixIndex];
    if (matrixStack->size > 1) {
        float* matrix = ArrayList_removeAt(matrixStack, matrixStack->size-1);
        MEMFREE(matrix);
    }
}

/*
 * Die Attribute des Zeichenstapels (Farbe, Normale, Texturkoordinaten je Einheit) bleiben
 * auf die Vertices ausgerichtet. Jedes ist entweder leer – dann gilt für alle Vertices des Stapels
 * der aktuelle Wert (glColor, glNormal, glTexCoord), gesetzt beim Ausgeben – oder hat genau einen
 * Eintrag je Vertex. Ändert sich der aktuelle Wert mitten im Stapel oder bringt ein Zeichenaufruf
 * ein Array mit, bekommen die bisherigen Vertices den bis dahin gültigen Wert eingetragen. Vorher
 * konnten zwei zusammengefasste Zeichenaufrufe mit verschiedener fester Farbe beide die letzte
 * bekommen oder Arrays gegeneinander verrutschen (batchtest.c im PC-Prüfstand).
 */
static ArrayBuffer* batchAttrib(GLRenderer* renderer, int arrayIdx, int* outComps, float** outValue) {
    if (arrayIdx == COLOR_ARRAY_INDEX) {
        *outComps = 4;
        *outValue = renderer->state.color;
        return &renderer->geometry.colors;
    }
    if (arrayIdx == NORMAL_ARRAY_INDEX) {
        *outComps = 3;
        *outValue = renderer->state.normal;
        return &renderer->geometry.normals;
    }
    int unit = arrayIdx - TEXCOORD_ARRAY_INDEX;
    *outComps = 4;
    *outValue = renderer->state.texCoords[unit];
    return &renderer->geometry.texCoords[unit];
}

#define BATCH_ATTRIB_FIRST COLOR_ARRAY_INDEX
#define BATCH_ATTRIB_END (TEXCOORD_ARRAY_INDEX + MAX_TEXTURES)

static inline int batchVertexCount(GLRenderer* renderer) {
    return renderer->geometry.vertices.size / (int)(4 * sizeof(float));
}

/* Einträge des Attributs bis vertexCount mit dem aktuellen Wert auffüllen */
static void padBatchAttrib(GLRenderer* renderer, int arrayIdx, int vertexCount) {
    int comps;
    float* value;
    ArrayBuffer* buffer = batchAttrib(renderer, arrayIdx, &comps, &value);
    int elementSize = comps * (int)sizeof(float);
    int targetSize = vertexCount * elementSize;
    if (buffer->size >= targetSize) return;
    ENSURE_ARRAY_CAPACITY(targetSize, buffer->capacity, buffer->buffer, 1);
    for (int offset = buffer->size; offset < targetSize; offset += elementSize) memcpy(buffer->buffer + offset, value, elementSize);
    buffer->size = targetSize;
}

/* glColor/glNormal/glTexCoord/glMultiTexCoord: aktuellen Wert setzen, mit Ausrichtung (s. o.) */
void GLRenderer_setCurrentAttrib(GLRenderer* renderer, int arrayIdx, const float* newValue) {
    int comps;
    float* value;
    ArrayBuffer* buffer = batchAttrib(renderer, arrayIdx, &comps, &value);
    if (buffer->size == 0) {
        int vertexCount = batchVertexCount(renderer);
        if (vertexCount > 0 && memcmp(value, newValue, comps * sizeof(float)) != 0) padBatchAttrib(renderer, arrayIdx, vertexCount);
    }
    memcpy(value, newValue, comps * sizeof(float));
    buffer->position++;
}

/*
 * Vor dem Einlesen eines Zeichenaufrufs mit Client-Arrays (addedVertices neue Vertices):
 * Attribute mit Array bekommen für die bisherigen Vertices den aktuellen Wert, Attribute ohne
 * Array, die im Stapel schon je Vertex geführt werden, für die neuen.
 */
void GLRenderer_prepareBatchArrays(GLRenderer* renderer, int addedVertices) {
    GLVertexAttrib* attribs = renderer->clientState.vao->attribs;
    int vertexCount = batchVertexCount(renderer);
    for (int arrayIdx = BATCH_ATTRIB_FIRST; arrayIdx < BATCH_ATTRIB_END; arrayIdx++) {
        if (attribs[arrayIdx].state == VERTEX_ATTRIB_LEGACY_ENABLED) {
            padBatchAttrib(renderer, arrayIdx, vertexCount);
            continue;
        }
        int comps;
        float* value;
        ArrayBuffer* buffer = batchAttrib(renderer, arrayIdx, &comps, &value);
        if (buffer->size > 0) padBatchAttrib(renderer, arrayIdx, vertexCount + addedVertices);
    }
}

/* Stapel vorher ausgeben, wenn addedVertices weitere Vertices nicht mehr in 16-Bit-Indizes passen */
void GLRenderer_reserveBatchVertices(GLRenderer* renderer, int addedVertices) {
    if (renderer->geometry.vertices.position + addedVertices > 65535 && !ArrayDeque_isEmpty(&renderer->meshes) && !renderer->activeMesh) {
        GLRenderer_drawImmediate(renderer);
    }
}

void GLRenderer_addVertex(GLRenderer* renderer, GLfloat x, GLfloat y, GLfloat z, GLfloat w) {
    if (!renderer->activeMesh) return;
    {
        /* Attribute, die im Stapel je Vertex geführt werden, bekommen den aktuellen Wert */
        int vertexCount = batchVertexCount(renderer);
        for (int arrayIdx = BATCH_ATTRIB_FIRST; arrayIdx < BATCH_ATTRIB_END; arrayIdx++) {
            int comps;
            float* value;
            ArrayBuffer* buffer = batchAttrib(renderer, arrayIdx, &comps, &value);
            if (buffer->size > 0) padBatchAttrib(renderer, arrayIdx, vertexCount + 1);
        }
        ArrayBuffer_putFloat4(&renderer->geometry.vertices, x, y, z, w);
        GLRenderer_addArrayElement(renderer, renderer->geometry.vertices.position++);
        return;
    }
    ArrayBuffer_putFloat4(&renderer->geometry.vertices, x, y, z, w);

    if (renderer->geometry.colors.position > 0 || renderer->geometry.colors.size > 0) {
        ArrayBuffer_putBytes(&renderer->geometry.colors, renderer->state.color, 4 * sizeof(float));
    }

    if (renderer->geometry.normals.position > 0) {
        ArrayBuffer_putBytes(&renderer->geometry.normals, renderer->state.normal, 3 * sizeof(float));
    }

    for (int i = 0; i < MAX_TEXTURES; i++) {
        if (renderer->geometry.texCoords[i].position > 0) {
            ArrayBuffer_putBytes(&renderer->geometry.texCoords[i], renderer->state.texCoords[i], 4 * sizeof(float));
        }
    }

    int index = renderer->geometry.vertices.position++;
    GLRenderer_addArrayElement(renderer, index);
}

void GLRenderer_addArrayElement(GLRenderer* renderer, int index) {
    if (!renderer->activeMesh) return;

    ArrayBuffer_putShort(&renderer->geometry.indices, index);
    renderer->geometry.indices.position++;
    renderer->activeMesh->indexCount++;

    switch (renderer->activeMesh->mode) {
        case GL_QUADS:
            if ((renderer->activeMesh->indexCount % 4) == 0) {
                int last = renderer->geometry.indices.position - 1;
                short* indices = (short*)renderer->geometry.indices.buffer;
                ArrayBuffer_putShort(&renderer->geometry.indices, indices[last-3]);
                ArrayBuffer_putShort(&renderer->geometry.indices, indices[last-1]);
                renderer->geometry.indices.position += 2;
            }
            break;
        case GL_TRIANGLE_STRIP:
        case GL_QUAD_STRIP:
            if (renderer->activeMesh->indexCount > 3) {
                int last = renderer->geometry.indices.position - 1;
                short* indices = (short*)renderer->geometry.indices.buffer;
                int first = renderer->activeMesh->end;
                short index1 = renderer->activeMesh->indexCount > 4 ? indices[MAX(first+2, last-6)] : indices[last-2];
                short index2 = renderer->activeMesh->indexCount > 4 ? indices[MAX(first+2, last-3)] : indices[last-1];

                if (renderer->activeMesh->indexCount % 2 == 0) {
                    ArrayBuffer_putShort(&renderer->geometry.indices, index2);
                    ArrayBuffer_putShort(&renderer->geometry.indices, index1);
                }
                else {
                    ArrayBuffer_putShort(&renderer->geometry.indices, index1);
                    ArrayBuffer_putShort(&renderer->geometry.indices, index2);
                }
                renderer->geometry.indices.position += 2;
            }
            break;
        case GL_TRIANGLE_FAN:
        case GL_POLYGON:
            if (renderer->activeMesh->indexCount > 3) {
                int last = renderer->geometry.indices.position - 1;
                short* indices = (short*)renderer->geometry.indices.buffer;
                int first = renderer->activeMesh->end;
                short index2 = renderer->activeMesh->indexCount > 4 ? indices[MAX(first+2, last-3)] : indices[last-1];

                ArrayBuffer_putShort(&renderer->geometry.indices, indices[first]);
                ArrayBuffer_putShort(&renderer->geometry.indices, index2);
                renderer->geometry.indices.position += 2;
            }
            break;
    }
}

static bool isDepthFormat(GLint format) {
    switch (format) {
        case GL_DEPTH_COMPONENT:
        case GL_DEPTH_COMPONENT16:
        case GL_DEPTH_COMPONENT24:
        case GL_DEPTH_COMPONENT32:
        case GL_DEPTH_COMPONENT32F:
        case GL_DEPTH_STENCIL:
        case GL_DEPTH24_STENCIL8:
        case GL_DEPTH32F_STENCIL8:
            return true;
        default:
            return false;
    }
}

void GLRenderer_setCapabilityState(GLRenderer* renderer, GLenum cap, bool state, int index) {
    switch (cap) {
        case GL_LIGHTING:
            renderer->state.lighting = state;
            break;
        case GL_COLOR_MATERIAL:
            renderer->state.colorMaterial.enabled = state;
            break;
        case GL_ALPHA_TEST:
            renderer->state.alphaTest.enabled = state;
            break;
        case GL_TEXTURE_1D:
        case GL_TEXTURE_2D:
        case GL_TEXTURE_3D:
        case GL_TEXTURE_CUBE_MAP:
        case GL_TEXTURE_RECTANGLE: {
            uint8_t activeTexture = renderer->clientState.activeTexture;
            if (state) BITMASK_SET(renderer->state.enabledTextures[activeTexture], getTexTargetFlag(cap));
            else BITMASK_UNSET(renderer->state.enabledTextures[activeTexture], getTexTargetFlag(cap));
            break;
        }
        case GL_FOG:
            renderer->state.fog.enabled = state;
            break;
        case GL_POINT_SMOOTH:
            renderer->state.point.smooth = state;
            break;
        case GL_POINT_SPRITE:
            renderer->state.point.sprite = state;
            break;
        case GL_LINE_SMOOTH:
        case GL_LINE_STIPPLE:
        case GL_POLYGON_STIPPLE:
        case GL_POLYGON_OFFSET_LINE:
        case GL_VERTEX_PROGRAM_POINT_SIZE:
        case GL_NORMALIZE:
        case GL_TEXTURE_CUBE_MAP_SEAMLESS:
        case GL_DEPTH_CLAMP:
        /* kennt ES nicht (Multisampling gilt bei Multisample-Puffern immer); wined3d schaltet GL_MULTISAMPLE
         * beim Start ein, der Treiber meldete GL_INVALID_ENUM */
        case GL_MULTISAMPLE:
        case GL_SAMPLE_ALPHA_TO_ONE:
        case GL_POLYGON_SMOOTH:
        case GL_POLYGON_OFFSET_POINT:
        case GL_COLOR_LOGIC_OP:
            break;
        case GL_VERTEX_PROGRAM_ARB:
        case GL_FRAGMENT_PROGRAM_ARB:
        case GL_VERTEX_PROGRAM_TWO_SIDE:
            renderer->state.enabledARBPrograms[indexOfGLTarget(cap)] = state;
            break;
        case GL_PRIMITIVE_RESTART:
            cap = GL_PRIMITIVE_RESTART_FIXED_INDEX;
        default:
            if (cap >= GL_LIGHT0 && cap <= GL_LIGHT7) {
                GLRenderer_setLightParams(renderer, cap, GL_LIGHTING, &state);
                return;
            }
            if ((cap >= GL_TEXTURE_GEN_S && cap <= GL_TEXTURE_GEN_Q) || (cap >= GL_CLIP_PLANE0 && cap <= GL_CLIP_PLANE5)) {
                if (state) println("gladio:setCapabilityState: unimplemented cap %x", cap);
                return;
            }

            if (!GLES_isCapabilitySupported(cap)) break;
            if (index != -1 && !GLES2) {
                if (state) glEnablei(cap, index);
                else glDisablei(cap, index);
            }
            else if (state) glEnable(cap);
            else glDisable(cap);
            break;
    }
}

void GLRenderer_setMaterialParams(GLRenderer* renderer, GLenum face, GLenum pname, void* params) {
    initMaterials(renderer);

    for (int i = 0; i < 2; i++) {
        if ((face == GL_FRONT && i == 0) || (face == GL_BACK && i == 1) || face == GL_FRONT_AND_BACK) {
            GLMaterial* material = &renderer->materials[i];
            switch (pname) {
                case GL_AMBIENT:
                    memcpy(material->ambient, params, sizeof(material->ambient));
                    break;
                case GL_DIFFUSE:
                    memcpy(material->diffuse, params, sizeof(material->diffuse));
                    material->alpha = ((float*)params)[3];
                    break;
                case GL_SPECULAR:
                    memcpy(material->specular, params, 3 * sizeof(float));
                    break;
                case GL_EMISSION:
                    memcpy(material->emission, params, sizeof(material->emission));
                    break;
                case GL_SHININESS:
                    material->specular[3] = *(float*)params;
                    break;
                case GL_AMBIENT_AND_DIFFUSE:
                    memcpy(material->ambient, params, sizeof(material->ambient));
                    memcpy(material->diffuse, params, sizeof(material->diffuse));
                    material->alpha = ((float*)params)[3];
                    break;
            }
        }
    }
}

void GLRenderer_setLightParams(GLRenderer* renderer, GLenum id, GLenum pname, void* params) {
    int index = id - GL_LIGHT0;
    GLLight* light = index < MAX_LIGHTS ? &renderer->lights[index] : NULL;
    if (!light) return;

    switch (pname) {
        case GL_POSITION: {
            memcpy(light->position, params, sizeof(light->position));
            float* modelViewMatrix = GLRenderer_getMatrixFromStack(renderer, MODEL_VIEW_MATRIX_INDEX);
            /* GL: Position (w != 0) und Richtung (w = 0) mit der aktuellen Modelview-Matrix ins
             * Augensystem; Richtungen mit deren oberem 3x3-Teil (nicht der Inversen) */
            if (light->position[3] != 0.0f) {
                vec3_apply_mat4(light->position, modelViewMatrix);
            }
            else vec3_transform_direction(light->position, modelViewMatrix);
            break;
        }
        case GL_AMBIENT:
            memcpy(light->ambient, params, sizeof(light->ambient));
            break;
        case GL_DIFFUSE:
            memcpy(light->diffuse, params, sizeof(light->diffuse));
            break;
        case GL_SPECULAR:
            memcpy(light->specular, params, sizeof(light->specular));
            break;
        case GL_LIGHTING:
            light->enabled = *(bool*)params;
            break;
        case GL_CONSTANT_ATTENUATION:
            light->attenuation[0] = *(float*)params;
            break;
        case GL_LINEAR_ATTENUATION:
            light->attenuation[1] = *(float*)params;
            break;
        case GL_QUADRATIC_ATTENUATION:
            light->attenuation[2] = *(float*)params;
            break;
        case GL_SPOT_CUTOFF:
            light->spotCutoffExponent[0] = TO_RADIANS(*(float*)params);
            break;
        case GL_SPOT_EXPONENT:
            light->spotCutoffExponent[1] = *(float*)params;
            break;
        case GL_SPOT_DIRECTION: {
            memcpy(light->spotDirection, params, sizeof(light->spotDirection));
            float* modelViewMatrix = GLRenderer_getMatrixFromStack(renderer, MODEL_VIEW_MATRIX_INDEX);
            vec3_transform_direction(light->spotDirection, modelViewMatrix);
            break;
        }
    }
}

void GLRenderer_setLightModelParams(GLRenderer* renderer, GLenum pname, GLfloat* params) {
    switch (pname) {
        case GL_LIGHT_MODEL_AMBIENT:
            memcpy(renderer->state.lightModelAmbient, params, sizeof(renderer->state.lightModelAmbient));
            break;
        case GL_LIGHT_MODEL_TWO_SIDE:
            renderer->state.lightModelTwoSide = params[0] != 0.0f;
            break;
        case GL_LIGHT_MODEL_LOCAL_VIEWER:
        case GL_LIGHT_MODEL_COLOR_CONTROL:
            break;
        default:
            println("gladio:setLightModelParams: unimplemented pname %x", pname);
            break;
    }
}

void GLRenderer_setTexEnvParams(GLRenderer* renderer, GLenum target, GLenum pname, float* params) {
    int activeTexture = renderer->clientState.activeTexture;
    TexEnv* texEnv = &renderer->state.texEnv[activeTexture];
    switch (pname) {
        case GL_TEXTURE_ENV_MODE: {
            GLenum param = (GLenum)params[0];
            if (param == GL_MODULATE ||
                param == GL_REPLACE ||
                param == GL_DECAL ||
                param == GL_BLEND ||
                param == GL_INTERPOLATE ||
                param == GL_ADD ||
                param == GL_COMBINE) {
                if (param == GL_BLEND) param = GL_INTERPOLATE;
                texEnv->mode = param;
            }
            break;
        }
        case GL_TEXTURE_ENV_COLOR:
            memcpy(texEnv->color, params, sizeof(texEnv->color));
            break;
        case GL_RGB_SCALE:
            texEnv->rgbaScale[0] = params[0];
            break;
        case GL_ALPHA_SCALE:
            texEnv->rgbaScale[1] = params[0];
            break;
        case GL_COMBINE_RGB:
            texEnv->combineRGBA[0] = (GLenum)params[0];
            break;
        case GL_COMBINE_ALPHA:
            texEnv->combineRGBA[1] = (GLenum)params[0];
            break;
        case GL_SOURCE0_RGB:
            texEnv->sourceRGBA[0] = (GLenum)params[0];
            break;
        case GL_SOURCE1_RGB:
            texEnv->sourceRGBA[1] = (GLenum)params[0];
            break;
        case GL_SOURCE0_ALPHA:
            texEnv->sourceRGBA[2] = (GLenum)params[0];
            break;
        case GL_SOURCE1_ALPHA:
            texEnv->sourceRGBA[3] = (GLenum)params[0];
            break;
        case GL_OPERAND0_RGB:
            texEnv->operandRGBA[0] = (GLenum)params[0];
            break;
        case GL_OPERAND1_RGB:
            texEnv->operandRGBA[1] = (GLenum)params[0];
            break;
        case GL_OPERAND0_ALPHA:
            texEnv->operandRGBA[2] = (GLenum)params[0];
            break;
        case GL_OPERAND1_ALPHA:
            texEnv->operandRGBA[3] = (GLenum)params[0];
            break;
        case GL_TEXTURE_LOD_BIAS:
            texEnv->lodBias = params[0];
            break;
        default:
            println("gladio:setTexEnvParams: unimplemented pname %x", pname);
            break;
    }
}

void GLRenderer_setFogParams(GLRenderer* renderer, GLenum pname, GLfloat* params) {
    switch (pname) {
        case GL_FOG_MODE:
            renderer->state.fog.mode = (GLenum)params[0];
            break;
        case GL_FOG_DENSITY:
            renderer->state.fog.density = params[0];
            break;
        case GL_FOG_START:
            renderer->state.fog.start = params[0];
            break;
        case GL_FOG_END:
            renderer->state.fog.end = params[0];
            break;
        case GL_FOG_COLOR:
            memcpy(renderer->state.fog.color, params, sizeof(renderer->state.fog.color));
            break;
        default:
            println("gladio:setFogParams: unimplemented pname %x", pname);
            break;
    }
}

void GLRenderer_setPointParams(GLRenderer* renderer, GLenum pname, void* params) {
    switch (pname) {
        case GL_POINT_SIZE_MIN:
            renderer->state.point.sizeMin = *(float*)(params);
            break;
        case GL_POINT_SIZE_MAX:
            renderer->state.point.sizeMax = *(float*)(params);
            break;
        case GL_POINT_FADE_THRESHOLD_SIZE:
            renderer->state.point.fadeThresholdSize = *(float*)(params);
            break;
        case GL_POINT_DISTANCE_ATTENUATION:
            memcpy(renderer->state.point.distanceAttenuation, params, sizeof(renderer->state.point.distanceAttenuation));
            break;
        case GL_POINT_SPRITE_COORD_ORIGIN:
            renderer->state.point.spriteCoordOrigin = *(int*)(params);
            break;
        default:
            println("gladio:setPointParams: unimplemented pname %x", pname);
            break;
    }
}

void GLRenderer_destroy(GLRenderer* renderer) {
    if (!renderer) return;
    glDeleteBuffers(ARRAY_SIZE(renderer->bufferIds), renderer->bufferIds);
    if (renderer->bgraStream.buffer > 0) {
        glDeleteBuffers(1, &renderer->bgraStream.buffer);
        renderer->bgraStream.buffer = 0;
    }

    GLVertexArrayObject_delete(&renderer->clientState, 0);
    GLRenderer_resetFrameCount(renderer);

    if (renderer->circleTexture > 0) {
        glDeleteTextures(1, &renderer->circleTexture);
        renderer->circleTexture = 0;
    }

    for (int i = 0; i < renderer->materialMap.size; i++) {
        ShaderMaterial* material = renderer->materialMap.entries[i].value;
        if (material) ShaderMaterial_destroy(material);
    }
    SparseArray_free(&renderer->materialMap, false);
    MEMFREE(renderer->materials);
    MEMFREE(renderer->endedQueries.clientIds);
    MEMFREE(renderer->endedQueries.serials);
    renderer->endedQueries.head = renderer->endedQueries.count = renderer->endedQueries.capacity = 0;

    ArrayBuffer_free(&renderer->geometry.indices);
    ArrayBuffer_free(&renderer->geometry.vertices);
    ArrayBuffer_free(&renderer->geometry.colors);
    ArrayBuffer_free(&renderer->geometry.normals);

    for (int i = 0; i < MAX_TEXTURES; i++) {
        ArrayBuffer_free(&renderer->geometry.texCoords[i]);
    }

    ArrayDeque_free(&renderer->meshes, true);

    if (renderer->raster) {
        glDeleteTextures(1, &renderer->raster->textureId);
        free(renderer->raster);
    }
}

/* Zustandsabfragen schreiben im angefragten Typ: glGetBooleanv, glGetIntegerv und glGetFloatv teilen sich
 * GLRenderer_getParamsv. Vorher wurde oft ohne Rücksicht auf den Typ geschrieben – 4 Byte in die 1 Byte
 * große Antwort von glGetBooleanv, die Bits eines float als Ganzzahl bei glGetIntegerv(GL_FOG_DENSITY) –,
 * und die Größe der Antwort passte nicht zur Zahl der Werte (glGetIntegerv(GL_LIGHTING) lieferte 1 Byte,
 * glGetIntegerv(GL_MAX_VIEWPORT_DIMS) schrieb 8 Byte in einen 4 Byte großen Puffer). */
static inline int paramElementSize(GLenum type) {
    return type == GL_BOOL ? sizeof(GLboolean) : sizeof(GLint);
}

static void putParami(void* params, GLenum type, int index, GLint value) {
    if (!params) return;
    if (type == GL_FLOAT) ((GLfloat*)params)[index] = (GLfloat)value;
    else if (type == GL_BOOL) ((GLboolean*)params)[index] = value != 0 ? GL_TRUE : GL_FALSE;
    else ((GLint*)params)[index] = value;
}

/* isColor: Farben und Normalen bildet glGetIntegerv linear von [-1, 1] auf den ganzen Ganzzahlbereich ab,
 * alle anderen Werte werden gerundet */
static void putParamsf(void* params, GLenum type, const float* values, int count, bool isColor) {
    if (!params) return;
    for (int i = 0; i < count; i++) {
        float value = values[i];
        if (type == GL_FLOAT) ((GLfloat*)params)[i] = value;
        else if (type == GL_BOOL) ((GLboolean*)params)[i] = value != 0.0f ? GL_TRUE : GL_FALSE;
        else {
            double v = isColor ? CLAMP((double)value, -1.0, 1.0) * 2147483647.0 : (double)value;
            GLint result;
            if (v != v) result = 0;
            else if (v >= 2147483647.0) result = INT32_MAX;
            else if (v <= -2147483648.0) result = INT32_MIN;
            else result = (GLint)(v < 0.0 ? v - 0.5 : v + 0.5);
            ((GLint*)params)[i] = result;
        }
    }
}

/* Die Aufrufer legen den Antwortpuffer mit der Größe aus einem ersten Aufruf ohne params an (Rückgabe in
 * Byte) und füllen ihn mit 0 vor: Was der Treiber nicht kennt, beantwortet er nicht. */
int GLRenderer_getParamsv(GLRenderer* renderer, GLenum pname, GLenum type, void* params) {
    int count = 1;

    switch (pname) {
        case GL_PROJECTION_MATRIX:
        case GL_MODELVIEW_MATRIX:
        case GL_TEXTURE_MATRIX: {
            int index = pname == GL_MODELVIEW_MATRIX ? MODEL_VIEW_MATRIX_INDEX :
                        pname == GL_PROJECTION_MATRIX ? PROJECTION_MATRIX_INDEX : TEXTURE_MATRIX_INDEX;
            count = 16;
            putParamsf(params, type, GLRenderer_getMatrixFromStack(renderer, index), count, false);
            break;
        }
        case GL_MODELVIEW_STACK_DEPTH:
        case GL_PROJECTION_STACK_DEPTH:
        case GL_TEXTURE_STACK_DEPTH: {
            int index = pname == GL_MODELVIEW_STACK_DEPTH ? MODEL_VIEW_MATRIX_INDEX :
                        pname == GL_PROJECTION_STACK_DEPTH ? PROJECTION_MATRIX_INDEX : TEXTURE_MATRIX_INDEX;
            int depth = renderer->matrixStack[index].size;
            putParami(params, type, 0, depth > 0 ? depth : 1);
            break;
        }
        case GL_MATRIX_MODE:
            /* kennt OpenGL ES nicht: der Treiber meldete GL_INVALID_ENUM, die Antwort war unbestimmt */
            putParami(params, type, 0, GL_MODELVIEW + renderer->matrixIndex);
            break;
        case GL_MAX_PROJECTION_STACK_DEPTH:
        case GL_MAX_TEXTURE_STACK_DEPTH:
            putParami(params, type, 0, 2);
            break;
        case GL_MAX_MODELVIEW_STACK_DEPTH:
            putParami(params, type, 0, 32);
            break;
        case GL_MAX_LIGHTS:
            putParami(params, type, 0, MAX_LIGHTS);
            break;
        case GL_MAX_CLIP_PLANES:
            putParami(params, type, 0, GLES2 ? 0 : 6);
            break;
        case GL_NUM_EXTENSIONS:
            if (params) {
                int numExtensions = 0;
                getGLExtensions(&numExtensions);
                putParami(params, type, 0, numExtensions);
            }
            break;
        case GL_STEREO:
        case GL_AUX_BUFFERS:
            putParami(params, type, 0, 0);
            break;
        case GL_MAX_TEXTURE_UNITS:
        case GL_MAX_TEXTURE_COORDS:
        case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS:
        case GL_MAX_TEXTURE_IMAGE_UNITS:
            putParami(params, type, 0, MAX_TEXTURES);
            break;
        case GL_MAX_RECTANGLE_TEXTURE_SIZE:
        case GL_MAX_TEXTURE_SIZE:
            putParami(params, type, 0, 4096);
            break;
        case GL_FOG:
            putParami(params, type, 0, renderer->state.fog.enabled);
            break;
        case GL_FOG_COLOR:
            count = 4;
            putParamsf(params, type, renderer->state.fog.color, count, true);
            break;
        case GL_FOG_DENSITY:
            putParamsf(params, type, &renderer->state.fog.density, 1, false);
            break;
        case GL_FOG_START:
            putParamsf(params, type, &renderer->state.fog.start, 1, false);
            break;
        case GL_FOG_END:
            putParamsf(params, type, &renderer->state.fog.end, 1, false);
            break;
        case GL_FOG_MODE:
            putParami(params, type, 0, renderer->state.fog.mode);
            break;
        case GL_CONTEXT_PROFILE_MASK:
            putParami(params, type, 0, GL_CONTEXT_CORE_PROFILE_BIT | GL_CONTEXT_COMPATIBILITY_PROFILE_BIT);
            break;
        case GL_SHADE_MODEL:
            putParami(params, type, 0, renderer->state.shadeModel);
            break;
        case GL_LIGHTING:
            putParami(params, type, 0, renderer->state.lighting);
            break;
        case GL_LIGHT_MODEL_AMBIENT:
            count = 4;
            putParamsf(params, type, renderer->state.lightModelAmbient, count, true);
            break;
        case GL_LIGHT_MODEL_TWO_SIDE:
            putParami(params, type, 0, renderer->state.lightModelTwoSide);
            break;
        case GL_COLOR_MATERIAL:
            putParami(params, type, 0, renderer->state.colorMaterial.enabled);
            break;
        case GL_COLOR_MATERIAL_FACE:
            putParami(params, type, 0, renderer->state.colorMaterial.face);
            break;
        case GL_COLOR_MATERIAL_PARAMETER:
            putParami(params, type, 0, renderer->state.colorMaterial.mode);
            break;
        case GL_POLYGON_MODE:
            count = 2;
            putParami(params, type, 0, renderer->state.polygonMode);
            putParami(params, type, 1, renderer->state.polygonMode);
            break;
        case GL_POINT_SIZE:
            putParamsf(params, type, &renderer->state.point.size, 1, false);
            break;
        case GL_POINT_SPRITE:
            putParami(params, type, 0, renderer->state.point.sprite);
            break;
        case GL_POINT_SMOOTH:
            putParami(params, type, 0, renderer->state.point.smooth);
            break;
        case GL_MAJOR_VERSION:
        case GL_MINOR_VERSION:
            putParami(params, type, 0, pname == GL_MAJOR_VERSION ? GLES_majorVersion() : GLES_minorVersion());
            break;
        case GL_LINE_SMOOTH:
        case GL_LINE_STIPPLE:
        case GL_POLYGON_STIPPLE:
        case GL_POLYGON_OFFSET_LINE:
        case GL_VERTEX_PROGRAM_POINT_SIZE:
        case GL_NORMALIZE:
        case GL_DOUBLEBUFFER:
            putParami(params, type, 0, 0);
            break;
        case GL_ALPHA_TEST:
            putParami(params, type, 0, renderer->state.alphaTest.enabled);
            break;
        case GL_ALPHA_TEST_REF:
            putParamsf(params, type, &renderer->state.alphaTest.ref, 1, true);
            break;
        case GL_ALPHA_TEST_FUNC:
            putParami(params, type, 0, renderer->state.alphaTest.func);
            break;
        case GL_CURRENT_COLOR:
            count = 4;
            putParamsf(params, type, renderer->state.color, count, true);
            break;
        case GL_CURRENT_NORMAL:
            count = 3;
            putParamsf(params, type, renderer->state.normal, count, true);
            break;
        case GL_CURRENT_TEXTURE_COORDS:
            count = 4;
            putParamsf(params, type, renderer->state.texCoords[renderer->clientState.activeTexCoord], count, false);
            break;
        case GL_CLAMP_READ_COLOR:
            putParami(params, type, 0, renderer->state.clampReadColor ? GL_TRUE : GL_FALSE);
            break;
        case GL_PROGRAM_ERROR_POSITION_ARB:
            putParami(params, type, 0, -1);
            break;
        case GL_VERTEX_PROGRAM_ARB:
        case GL_FRAGMENT_PROGRAM_ARB:
            putParami(params, type, 0, renderer->state.enabledARBPrograms[pname == GL_FRAGMENT_PROGRAM_ARB ? 1 : 0]);
            break;
        case GL_TEXTURE_BINDING_1D:
        case GL_TEXTURE_BINDING_2D:
        case GL_TEXTURE_BINDING_3D:
        case GL_TEXTURE_BINDING_CUBE_MAP:
        case GL_TEXTURE_BINDING_RECTANGLE:
        case GL_TEXTURE_BINDING_2D_ARRAY:
            /* Namen des Programms, nicht die des Treibers */
            if (params) putParami(params, type, 0, GLTexture_getBindingId(getTexTargetForBinding(pname)));
            break;
        case GL_TEXTURE_1D:
        case GL_TEXTURE_2D:
        case GL_TEXTURE_3D:
        case GL_TEXTURE_CUBE_MAP:
        case GL_TEXTURE_RECTANGLE: {
            uint8_t flags = renderer->state.enabledTextures[renderer->clientState.activeTexture];
            putParami(params, type, 0, (flags & getTexTargetFlag(pname)) ? GL_TRUE : GL_FALSE);
            break;
        }
        case GL_ACTIVE_TEXTURE:
            putParami(params, type, 0, GL_TEXTURE0 + renderer->clientState.activeTexture);
            break;
        case GL_CLIENT_ACTIVE_TEXTURE:
            putParami(params, type, 0, GL_TEXTURE0 + renderer->clientState.activeTexCoord);
            break;
        /* Pixelspeicher: GLU liest vor jedem gluBuild2DMipmaps den ganzen Zustand und stellt ihn danach
         * wieder her. Was ES nicht kennt, ließ der Treiber unbeantwortet, und zurück ging der
         * uninitialisierte Antwortpuffer – auf dem Mali-400 (ES 2.0) unbestimmte Zeilenlängen und
         * Versätze, mit denen GLU hinter die Bilddaten las (SKIDROW-Intro: schwarzes Bild, dann Ende). */
        case GL_UNPACK_SWAP_BYTES:
        case GL_UNPACK_LSB_FIRST:
        case GL_PACK_SWAP_BYTES:
        case GL_PACK_LSB_FIRST:
        case GL_PACK_IMAGE_HEIGHT:
        case GL_PACK_SKIP_IMAGES:
            putParami(params, type, 0, 0);
            break;
        case GL_UNPACK_ROW_LENGTH:
        case GL_UNPACK_SKIP_ROWS:
        case GL_UNPACK_SKIP_PIXELS:
        case GL_UNPACK_IMAGE_HEIGHT:
        case GL_UNPACK_SKIP_IMAGES:
        case GL_PACK_ROW_LENGTH:
        case GL_PACK_SKIP_ROWS:
        case GL_PACK_SKIP_PIXELS:
            if (GLES2) {
                /* ES 2.0: die Werte führt der Server selbst (gd_handle_glPixelStorei) */
                int value = 0;
                if (pname == GL_UNPACK_ROW_LENGTH) value = gles.unpackRowLength;
                else if (pname == GL_UNPACK_SKIP_ROWS) value = gles.unpackSkipRows;
                else if (pname == GL_UNPACK_SKIP_PIXELS) value = gles.unpackSkipPixels;
                putParami(params, type, 0, value);
            }
            else if (params) {
                GLint value = 0;
                glGetIntegerv(pname, &value);
                putParami(params, type, 0, value);
            }
            break;
        default: {
            if (pname == GL_POINT_SIZE_RANGE) pname = GL_ALIASED_POINT_SIZE_RANGE;
            else if (pname == GL_LINE_WIDTH_RANGE) pname = GL_ALIASED_LINE_WIDTH_RANGE;
            /* ES kennt nur die getrennten Faktoren (GL_BLEND_SRC war ungültig: glPopAttrib stellte GL_ZERO her) */
            else if (pname == GL_BLEND_SRC) pname = GL_BLEND_SRC_RGB;
            else if (pname == GL_BLEND_DST) pname = GL_BLEND_DST_RGB;

            if (GLES2) {
                /* Grenzen, die ES 2.0 anders oder gar nicht kennt */
                int value = -1;
                switch (pname) {
                    /* wined3d legt Konstantenfelder in dieser Größe an (vs_c[n]); etwas Platz für
                     * posFixup, Matrizen und die eigenen Uniforms des Servers lassen */
                    case GL_MAX_VERTEX_UNIFORM_COMPONENTS: value = MAX(gles.maxVertexUniformVectors - 32, 64) * 4; break;
                    case GL_MAX_FRAGMENT_UNIFORM_COMPONENTS: value = MAX(gles.maxFragmentUniformVectors - 32, 32) * 4; break;
                    case GL_MAX_VARYING_COMPONENTS: /* = GL_MAX_VARYING_FLOATS */
                        /* abzüglich der Varyings, die der Server selbst belegt (Farbe, Nebel) */
                        value = MAX(gles.maxVaryingVectors - 2, 4) * 4;
                        break;
                    case GL_MAX_FRAGMENT_INPUT_COMPONENTS: value = MAX(gles.maxVaryingVectors - 2, 4) * 4 + 4; break;
                    case GL_MAX_VERTEX_OUTPUT_COMPONENTS: value = MAX(gles.maxVaryingVectors - 2, 4) * 4 + 4; break;
                    case GL_MAX_DRAW_BUFFERS:
                    case GL_MAX_COLOR_ATTACHMENTS:
                        value = 1;
                        break;
                    case GL_MAX_SAMPLES:
                    case GL_MAX_3D_TEXTURE_SIZE:
                    case GL_MAX_ARRAY_TEXTURE_LAYERS:
                    case GL_MAX_UNIFORM_BUFFER_BINDINGS:
                    case GL_MAX_VERTEX_UNIFORM_BLOCKS:
                    case GL_MAX_FRAGMENT_UNIFORM_BLOCKS:
                    case GL_MAX_COMBINED_UNIFORM_BLOCKS:
                    case GL_MAX_UNIFORM_BLOCK_SIZE:
                    case GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS:
                    case GL_MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS:
                    case GL_MAX_ELEMENTS_VERTICES:
                    case GL_MAX_ELEMENTS_INDICES:
                    case GL_MAX_TEXTURE_LOD_BIAS:
                    case GL_MAX_TEXTURE_BUFFER_SIZE:
                    case GL_MAX_DUAL_SOURCE_DRAW_BUFFERS:
                    case GL_NUM_PROGRAM_BINARY_FORMATS:
                    case GL_NUM_SHADER_BINARY_FORMATS:
                        value = 0;
                        break;
                    case GL_PROGRAM_BINARY_FORMATS:
                    case GL_SHADER_BINARY_FORMATS:
                        /* passend zur gemeldeten Anzahl 0: keine Werte */
                        return 0;
                    case GL_MAX_TEXTURE_MAX_ANISOTROPY:
                        value = gles.anisotropic ? -1 : 1;
                        break;
                    case GL_MAX_CUBE_MAP_TEXTURE_SIZE: value = gles.maxCubeMapSize; break;
                    case GL_MAX_RENDERBUFFER_SIZE: value = gles.maxRenderbufferSize; break;
                    case GL_MAX_VERTEX_ATTRIBS: value = gles.maxVertexAttribs; break;
                    case GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS: value = gles.maxVertexTextureUnits; break;
                    case GL_READ_FRAMEBUFFER_BINDING:
                    case GL_DRAW_FRAMEBUFFER_BINDING:
                        pname = GL_FRAMEBUFFER_BINDING;
                        break;
                    case GL_SAMPLE_BUFFERS:
                    case GL_SAMPLES:
                        value = 0;
                        break;
                }
                if (value != -1) {
                    putParami(params, type, 0, value);
                    break;
                }
            }
            if (pname >= GL_LIGHT0 && pname <= GL_LIGHT7) {
                int index = pname - GL_LIGHT0;
                putParami(params, type, 0, index < MAX_LIGHTS ? renderer->lights[index].enabled : 0);
                break;
            }

            switch (pname) {
                case GL_COLOR_WRITEMASK:
                case GL_SCISSOR_BOX:
                case GL_VIEWPORT:
                case GL_BLEND_COLOR:
                case GL_COLOR_CLEAR_VALUE:
                    count = 4;
                    break;
                case GL_DEPTH_RANGE:
                case GL_ALIASED_POINT_SIZE_RANGE:
                case GL_ALIASED_LINE_WIDTH_RANGE:
                case GL_MAX_VIEWPORT_DIMS:
                    count = 2;
                    break;
                case GL_PRIMITIVE_BOUNDING_BOX:
                    count = 8;
                    break;
                case GL_COMPRESSED_TEXTURE_FORMATS:
                case GL_PROGRAM_BINARY_FORMATS:
                case GL_SHADER_BINARY_FORMATS: {
                    /* Felder in der Länge, die der Treiber für die zugehörige Anzahl meldet */
                    GLenum numPname = pname == GL_COMPRESSED_TEXTURE_FORMATS ? GL_NUM_COMPRESSED_TEXTURE_FORMATS :
                                      pname == GL_PROGRAM_BINARY_FORMATS ? GL_NUM_PROGRAM_BINARY_FORMATS : GL_NUM_SHADER_BINARY_FORMATS;
                    GLint numValues = 0;
                    glGetIntegerv(numPname, &numValues);
                    if (numValues <= 0 || numValues > 256) return 0;
                    count = numValues;
                    if (params) {
                        GLint values[256];
                        glGetIntegerv(pname, values);
                        for (int i = 0; i < count; i++) putParami(params, type, i, values[i]);
                    }
                    return count * paramElementSize(type);
                }
            }

            if (params) {
                /* über einen großzügigen Zwischenpuffer: Abfragen, deren Wertezahl der Server nicht kennt,
                 * schreiben nicht über die Antwort hinaus */
                union {
                    GLfloat f[16];
                    GLint i[16];
                    GLboolean b[16];
                } values;
                memset(&values, 0, sizeof(values));
                switch (type) {
                    case GL_FLOAT: glGetFloatv(pname, values.f); break;
                    case GL_BOOL: glGetBooleanv(pname, values.b); break;
                    default: glGetIntegerv(pname, values.i); break;
                }
                memcpy(params, &values, count * paramElementSize(type));

                if (pname == GL_FRAMEBUFFER_BINDING ||
                    pname == GL_DRAW_FRAMEBUFFER_BINDING ||
                    pname == GL_READ_FRAMEBUFFER_BINDING) {
                    GLint framebuffer = type == GL_FLOAT ? (GLint)values.f[0] : type == GL_BOOL ? values.b[0] : values.i[0];
                    if (framebuffer == (GLint)renderer->displayBuffer) putParami(params, type, 0, 0);
                }
            }
            break;
        }
    }

    return count * paramElementSize(type);
}

void GLRenderer_setPixelZoom(GLRenderer* renderer, GLfloat xfactor, GLfloat yfactor) {
    initRaster(renderer);

    renderer->raster->pixelZoom[0] = xfactor;
    renderer->raster->pixelZoom[1] = yfactor;
}

void GLRenderer_setRasterPos(GLRenderer* renderer, bool transform, GLfloat x, GLfloat y, GLfloat z, GLfloat w) {
    initRaster(renderer);

    float* projMatrix = GLRenderer_getMatrixFromStack(renderer, PROJECTION_MATRIX_INDEX);
    float* modelViewMatrix = GLRenderer_getMatrixFromStack(renderer, MODEL_VIEW_MATRIX_INDEX);
    float* rasterPos = renderer->raster->position;

    if (transform) {
        rasterPos[0] = x;
        rasterPos[1] = y;
        rasterPos[2] = z;
        rasterPos[3] = w;

        vec3_apply_mat4(rasterPos, modelViewMatrix);
        vec3_apply_mat4(rasterPos, projMatrix);

        float dist = (rasterPos[3] == 0.0F) ? 1.0f : 1.0f / rasterPos[3];
        rasterPos[0] *= dist;
        rasterPos[1] *= dist;
        rasterPos[2] *= dist;

        float halfWidth = renderer->displaySize[0] * 0.5f;
        float halfHeight = renderer->displaySize[1] * 0.5f;
        rasterPos[0] = rasterPos[0] * halfWidth + halfWidth;
        rasterPos[1] = rasterPos[1] * halfHeight + halfHeight;
    }
    else {
        rasterPos[0] = x;
        rasterPos[1] = y;
        rasterPos[2] = z;
        rasterPos[3] = w;
    }
}

void GLRenderer_drawPixels(GLRenderer* renderer, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels) {
    GLint internalformat = GL_NONE;
    bool ownsPixels = false;
    if (GLES2) {
        int imageSize = computeTexImageDataSize(format, type, width, height, 1);
        if (!GLES_convertTexImageFormat(GL_TEXTURE_2D, &internalformat, &format, &type, &pixels, &imageSize, &ownsPixels)) return;
    }
    else convertTexImageFormat(GL_TEXTURE_2D, &internalformat, &format, &type, NULL, 0);

    initRaster(renderer);

    GLboolean oldEnabled;
    GLRenderer_getParamsv(renderer, GL_TEXTURE_2D, GL_BOOL, &oldEnabled);
    GLRenderer_setCapabilityState(renderer, GL_TEXTURE_2D, true, -1);

    GLint oldActiveTexture = GL_TEXTURE0;
    GLRenderer_getParamsv(renderer, GL_ACTIVE_TEXTURE, GL_INT, &oldActiveTexture);
    GLTexture_setActiveUnit(GL_TEXTURE0);

    GLRaster* raster = renderer->raster;
    glBindTexture(GL_TEXTURE_2D, raster->textureId);
    glTexImage2D(GL_TEXTURE_2D, 0, internalformat, width, height, 0, format, type, pixels);
    if (ownsPixels) free(pixels);

    ArrayBuffer_rewind(&raster->quad.vertices);
    ArrayBuffer_rewind(&raster->quad.texCoords);

    const float* rasterPos = renderer->raster->position;
    const float x0 = rasterPos[0] / (float)renderer->displaySize[0] * 2.0f - 1.0f;
    const float y0 = rasterPos[1] / (float)renderer->displaySize[1] * 2.0f - 1.0f;
    const float x1 = (rasterPos[0] + width * raster->pixelZoom[0]) / (float)renderer->displaySize[0] * 2.0f - 1.0f;
    const float y1 = (rasterPos[1] + height * raster->pixelZoom[1]) / (float)renderer->displaySize[1] * 2.0f - 1.0f;

    ArrayBuffer_putFloat4(&raster->quad.vertices, x0, y0, 0.0f, 1.0f);
    ArrayBuffer_putFloat4(&raster->quad.texCoords, 0.0f, 0.0f, 0.0f, 1.0f);
    ArrayBuffer_putFloat4(&raster->quad.vertices, x0, y1, 0.0f, 1.0f);
    ArrayBuffer_putFloat4(&raster->quad.texCoords, 0.0f, 1.0f, 0.0f, 1.0f);
    ArrayBuffer_putFloat4(&raster->quad.vertices, x1, y0, 0.0f, 1.0f);
    ArrayBuffer_putFloat4(&raster->quad.texCoords, 1.0f, 0.0f, 0.0f, 1.0f);
    ArrayBuffer_putFloat4(&raster->quad.vertices, x1, y1, 0.0f, 1.0f);
    ArrayBuffer_putFloat4(&raster->quad.texCoords, 1.0f, 1.0f, 0.0f, 1.0f);

    MaterialOptions options = {renderer->state.lighting, false, false, false, false, 1, NULL, NULL};
    ShaderMaterial* material = setCurrentMaterial(renderer, &options);
    bindVertexBuffer(renderer, renderer->bufferIds[2], material->location.attributes[POSITION_ARRAY_INDEX], 4, raster->quad.vertices.size, raster->quad.vertices.buffer);
    bindVertexBuffer(renderer, renderer->bufferIds[4], material->location.attributes[TEXCOORD_ARRAY_INDEX], 4, raster->quad.texCoords.size, raster->quad.texCoords.buffer);

    glBindBuffer(GL_ARRAY_BUFFER, renderer->bufferIds[3]);
    GLRenderer_disableVertexAttribute(renderer, material->location.attributes[COLOR_ARRAY_INDEX]);
    glVertexAttrib4fv(material->location.attributes[COLOR_ARRAY_INDEX], renderer->state.color);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    GLRenderer_disableUnusedVertexAttributes(renderer);

    GLTexture* texture = GLTexture_getBound(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, texture ? texture->id : 0);

    GLTexture_setActiveUnit(oldActiveTexture);
    GLRenderer_setCapabilityState(renderer, GL_TEXTURE_2D, oldEnabled, -1);
}

void GLRenderer_setSamplerParameter(GLRenderer* renderer, GLuint sampler, GLenum pname, GLfloat* params) {
    if (GLES2) return;
    switch (pname) {
        case GL_TEXTURE_WRAP_S:
        case GL_TEXTURE_WRAP_T:
        case GL_TEXTURE_WRAP_R:
        case GL_TEXTURE_COMPARE_FUNC:
        case GL_TEXTURE_COMPARE_MODE:
        case GL_TEXTURE_MIN_FILTER:
        case GL_TEXTURE_MAG_FILTER:
            glSamplerParameteri(sampler, pname, (GLint)*params);
            break;
        case GL_TEXTURE_MAX_ANISOTROPY:
            glSamplerParameterf(sampler, pname, MAX(*params, 1.0f));
            break;
        case GL_TEXTURE_MIN_LOD:
        case GL_TEXTURE_MAX_LOD:
            glSamplerParameterf(sampler, pname, *params);
            break;
        case GL_TEXTURE_BORDER_COLOR:
            glSamplerParameterfv(sampler, pname, params);
            break;
    }
}

void GLRenderer_getSamplerParameter(GLRenderer* renderer, GLuint sampler, GLenum pname, GLfloat* params) {
    *params = 0;
    if (GLES2) return;
    switch (pname) {
        case GL_TEXTURE_WRAP_S:
        case GL_TEXTURE_WRAP_T:
        case GL_TEXTURE_WRAP_R:
        case GL_TEXTURE_COMPARE_FUNC:
        case GL_TEXTURE_COMPARE_MODE:
        case GL_TEXTURE_MIN_FILTER:
        case GL_TEXTURE_MAG_FILTER: {
            GLint paramsi;
            glGetSamplerParameteriv(sampler, pname, &paramsi);
            *params = paramsi;
            break;
        }
        case GL_TEXTURE_MAX_ANISOTROPY:
        case GL_TEXTURE_MIN_LOD:
        case GL_TEXTURE_MAX_LOD:
        case GL_TEXTURE_BORDER_COLOR:
            glGetSamplerParameterfv(sampler, pname, params);
            break;
        default:
            println("gladio:getSamplerParameter: unimplemented pname %x", pname);
            break;
    }
}

void GLRenderer_setTexParameter(GLRenderer* renderer, GLenum target, GLenum pname, GLfloat* params) {
    target = parseTexTarget(target);
    switch (pname) {
        case GL_GENERATE_MIPMAP: {
            GLint param = (GLint)*params;
            GLTexture* texture = GLTexture_getBound(target);
            if (texture) texture->generateMipmap = param ? true : false;
            break;
        }
        case GL_TEXTURE_WRAP_S:
        case GL_TEXTURE_WRAP_T:
        case GL_TEXTURE_WRAP_R: {
            GLint param = (GLint)*params;
            if (param == GL_CLAMP) param = GL_CLAMP_TO_EDGE;
            if (GLES2) {
                if (pname == GL_TEXTURE_WRAP_R) break;
                if (param == GL_CLAMP_TO_BORDER) param = GL_CLAMP_TO_EDGE;
            }
            glTexParameteri(target, pname, param);
            break;
        }
        case GL_TEXTURE_MIN_FILTER:
            if (GLES2) {
                GLTexture* texture = GLTexture_getBound(target);
                if (texture) {
                    texture->minFilter = (GLenum)*params;
                    GLES_applyMinFilter(target, texture);
                    break;
                }
            }
            glTexParameteri(target, pname, (GLint)*params);
            break;
        case GL_TEXTURE_MAG_FILTER:
            glTexParameteri(target, pname, (GLint)*params);
            break;
        case GL_TEXTURE_MAX_LEVEL:
            if (GLES2) {
                /* ES 2.0 kennt keine Stufenbegrenzung; eine Textur mit nur Stufe 0 wäre mit
                 * Mipmap-Filter unvollständig (schwarz) – dann ohne Mipmaps filtern */
                GLTexture* texture = GLTexture_getBound(target);
                if (texture) {
                    texture->maxLevel = (GLint)*params;
                    GLES_applyMinFilter(target, texture);
                }
                break;
            }
            glTexParameteri(target, pname, (GLint)*params);
            break;
        case GL_TEXTURE_SWIZZLE_RGBA:
            /* wined3d setzt die Swizzle als Vektor (glTexParameteriv), z. B. für L8 als GL_R8 */
            if (GLES2) break;
            for (int i = 0; i < 4; i++) glTexParameteri(target, GL_TEXTURE_SWIZZLE_R + i, (GLint)params[i]);
            break;
        case GL_DEPTH_STENCIL_TEXTURE_MODE:
        case GL_TEXTURE_BASE_LEVEL:
        case GL_TEXTURE_COMPARE_FUNC:
        case GL_TEXTURE_COMPARE_MODE:
        case GL_TEXTURE_SWIZZLE_R:
        case GL_TEXTURE_SWIZZLE_G:
        case GL_TEXTURE_SWIZZLE_B:
        case GL_TEXTURE_SWIZZLE_A:
            if (GLES2) break;
            glTexParameteri(target, pname, (GLint)*params);
            break;
        case GL_TEXTURE_MAX_ANISOTROPY:
            if (GLES2 && !gles.anisotropic) break;
            glTexParameterf(target, pname, MAX(*params, 1.0f));
            break;
        case GL_TEXTURE_MIN_LOD:
        case GL_TEXTURE_MAX_LOD:
            if (GLES2) break;
            glTexParameterf(target, pname, *params);
            break;
        case GL_TEXTURE_BORDER_COLOR:
            if (GLES2) break;
            glTexParameterfv(target, pname, params);
            break;
        case GL_DEPTH_TEXTURE_MODE: {
            /* Altes GL: wie eine Tiefentextur ohne Vergleich gelesen wird – LUMINANCE (D,D,D,1), INTENSITY
             * (D,D,D,D), ALPHA (0,0,0,D), RED (D,0,0,1). ES liest Tiefe immer als (D,0,0,1); nachgebildet
             * mit Swizzle, nur für Tiefenformate (sonst würden Farbtexturen umgestellt). Vorher war INTENSITY
             * (R,G,B,A) – in ES also (D,0,0,1) statt (D,D,D,D), ALPHA (0,0,0,1). */
            if (GLES2) break;
            GLTexture* texture = GLTexture_getBound(target);
            if (texture && !isDepthFormat(texture->originFormat)) break;
            GLint mode = (GLint)*params;
            GLint swizzle[4];
            switch (mode) {
                case GL_LUMINANCE: swizzle[0] = swizzle[1] = swizzle[2] = GL_RED; swizzle[3] = GL_ONE; break;
                case GL_INTENSITY: swizzle[0] = swizzle[1] = swizzle[2] = swizzle[3] = GL_RED; break;
                case GL_ALPHA: swizzle[0] = swizzle[1] = swizzle[2] = GL_ZERO; swizzle[3] = GL_RED; break;
                case GL_RED: swizzle[0] = GL_RED; swizzle[1] = swizzle[2] = GL_ZERO; swizzle[3] = GL_ONE; break;
                default: swizzle[0] = -1; break;
            }
            if (swizzle[0] == -1) break;
            glTexParameteri(target, GL_TEXTURE_SWIZZLE_R, swizzle[0]);
            glTexParameteri(target, GL_TEXTURE_SWIZZLE_G, swizzle[1]);
            glTexParameteri(target, GL_TEXTURE_SWIZZLE_B, swizzle[2]);
            glTexParameteri(target, GL_TEXTURE_SWIZZLE_A, swizzle[3]);
            break;
        }
    }
}

void GLRenderer_getTexParameter(GLRenderer* renderer, GLenum target, GLint level, GLenum pname, GLfloat* params) {
    GLTexture* texture = GLTexture_getBound(target);
    *params = 0;
    if (!texture) return;

    switch (pname) {
        case GL_TEXTURE_WIDTH:
            *params = TEXLEVEL(texture->width, level);
            break;
        case GL_TEXTURE_HEIGHT:
            *params = TEXLEVEL(texture->height, level);
            break;
        case GL_TEXTURE_DEPTH:
            *params = 0;
            break;
        case GL_TEXTURE_RED_TYPE:
        case GL_TEXTURE_GREEN_TYPE:
        case GL_TEXTURE_BLUE_TYPE:
        case GL_TEXTURE_ALPHA_TYPE:
        case GL_TEXTURE_DEPTH_TYPE:
            *params = GL_FLOAT;
            break;
        case GL_TEXTURE_RED_SIZE:
        case GL_TEXTURE_GREEN_SIZE:
        case GL_TEXTURE_BLUE_SIZE:
        case GL_TEXTURE_ALPHA_SIZE:
            *params = 8;
            break;
        case GL_TEXTURE_DEPTH_SIZE:
            *params = 0;
            break;
        case GL_TEXTURE_COMPRESSED:
            *params = isCompressedFormat(texture->originFormat);
            break;
        case GL_TEXTURE_COMPRESSED_IMAGE_SIZE:
            if (isCompressedFormat(texture->originFormat)) {
                *params = getCompressedImageSize(texture->originFormat, texture->width, texture->height, level);
            }
            else *params = texture->width * texture->height * 4;
            break;
        case GL_TEXTURE_INTERNAL_FORMAT:
            *params = texture->originFormat;
            break;
        case GL_TEXTURE_RESIDENT:
            *params = GL_TRUE;
            break;
        case GL_TEXTURE_MAG_FILTER:
        case GL_TEXTURE_MIN_FILTER: {
            GLint paramsi;
            glGetTexParameteriv(target, pname, &paramsi);
            *params = paramsi;
            break;
        }
        case GL_TEXTURE_WRAP_S:
        case GL_TEXTURE_WRAP_T:
            glGetTexParameterfv(target, pname, params);
            break;
        case GL_DEPTH_STENCIL_TEXTURE_MODE:
        case GL_IMAGE_FORMAT_COMPATIBILITY_TYPE:
        case GL_TEXTURE_BASE_LEVEL:
        case GL_TEXTURE_COMPARE_FUNC:
        case GL_TEXTURE_COMPARE_MODE:
        case GL_TEXTURE_IMMUTABLE_FORMAT:
        case GL_TEXTURE_IMMUTABLE_LEVELS:
        case GL_TEXTURE_MAX_LEVEL:
        case GL_TEXTURE_SWIZZLE_R:
        case GL_TEXTURE_SWIZZLE_G:
        case GL_TEXTURE_SWIZZLE_B:
        case GL_TEXTURE_SWIZZLE_A: {
            if (GLES2) {
                switch (pname) {
                    case GL_TEXTURE_MAX_LEVEL: *params = 1000; break;
                    case GL_TEXTURE_COMPARE_FUNC: *params = GL_LEQUAL; break;
                    case GL_TEXTURE_SWIZZLE_R: *params = GL_RED; break;
                    case GL_TEXTURE_SWIZZLE_G: *params = GL_GREEN; break;
                    case GL_TEXTURE_SWIZZLE_B: *params = GL_BLUE; break;
                    case GL_TEXTURE_SWIZZLE_A: *params = GL_ALPHA; break;
                    default: *params = 0; break;
                }
                break;
            }
            GLint paramsi;
            glGetTexParameteriv(target, pname, &paramsi);
            *params = paramsi;
            break;
        }
        case GL_TEXTURE_MAX_ANISOTROPY:
        case GL_TEXTURE_MIN_LOD:
        case GL_TEXTURE_MAX_LOD:
        case GL_TEXTURE_WRAP_R:
        case GL_TEXTURE_BORDER_COLOR:
            if (GLES2) {
                switch (pname) {
                    case GL_TEXTURE_MAX_ANISOTROPY: *params = 1.0f; break;
                    case GL_TEXTURE_MIN_LOD: *params = -1000.0f; break;
                    case GL_TEXTURE_MAX_LOD: *params = 1000.0f; break;
                    case GL_TEXTURE_WRAP_R: *params = GL_REPEAT; break;
                    default: params[0] = params[1] = params[2] = params[3] = 0.0f; break;
                }
                break;
            }
            glGetTexParameterfv(target, pname, params);
            break;
        default:
            println("gladio:getTexParameter: unimplemented pname %x", pname);
            break;
    }
}

static void freePixelReadCache(GLRenderer* renderer);

/* Stufe level der gebundenen Textur lesen. Vorher wurde immer Stufe 0 in deren Größe gelesen (glGetCompressedTexImage
 * einer kleineren Stufe schrieb dann über das Ende des Puffers), und der Lesecache von GLRenderer_readPixels
 * (Schlüssel: das Lese-FBO des Programms, nicht die Textur) lieferte bei gleicher Größe binnen drei Bildern das
 * Bild einer anderen Textur. wined3d lädt so Texturen herunter, z. B. bevor es sie in die sRGB-Fassung kopiert. */
void* GLRenderer_getTexImage(GLRenderer* renderer, GLenum target, GLint level, GLenum format, GLenum type, int* imageSize) {
    target = parseTexTarget(target);
    GLTexture* texture = GLTexture_getBound(target);
    *imageSize = 0;
    if (!texture || level < 0 || level > 15) return NULL;
    int width = TEXLEVEL(texture->width, level), height = TEXLEVEL(texture->height, level);
    *imageSize = computeTexImageDataSize(format, type, width, height, 1);
    if (*imageSize == 0) return NULL;
    if (GLES2 && level > 0) {
        /* ES 2.0 hängt nur Stufe 0 an ein FBO: schwarz in der richtigen Größe statt Speicherinhalt */
        return calloc(1, *imageSize);
    }

    GLuint framebuffer;
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);

    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, target, texture->id, level);

    void* pixels = malloc(*imageSize);
    freePixelReadCache(renderer);
    GLRenderer_readPixels(renderer, 0, 0, width, height, format, type, pixels);
    freePixelReadCache(renderer);

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, renderer->clientState.framebuffer[indexOfGLTarget(GL_DRAW_FRAMEBUFFER)]);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, renderer->clientState.framebuffer[indexOfGLTarget(GL_READ_FRAMEBUFFER)]);
    glDeleteFramebuffers(1, &framebuffer);
    return pixels;
}

void* GLRenderer_getCompressedTexImage(GLRenderer* renderer, GLenum target, GLint level, int* compressedSize) {
    target = parseTexTarget(target);
    GLTexture* texture = GLTexture_getBound(target);
    *compressedSize = texture && level >= 0 && level <= 15 ? getCompressedImageSize(texture->originFormat, texture->width, texture->height, level) : 0;
    if (*compressedSize == 0) return NULL;

    int imageSize;
    void* imageData = GLRenderer_getTexImage(renderer, target, level, GL_RGBA, GL_UNSIGNED_BYTE, &imageSize);
    if (!imageData) {
        *compressedSize = 0;
        return NULL;
    }

    void* compressedData = malloc(*compressedSize);
    compressTexImage2D(texture->originFormat, TEXLEVEL(texture->width, level), TEXLEVEL(texture->height, level), imageData, compressedData);
    free(imageData);
    return compressedData;
}

void GLRenderer_setDrawBuffer(GLRenderer* renderer, GLenum drawBuffer) {
    if (drawBuffer == GL_NONE || (drawBuffer >= GL_COLOR_ATTACHMENT0 && drawBuffer <= GL_COLOR_ATTACHMENT31)) {
        GLFramebuffer_setDrawBuffers(1, &drawBuffer);
    }
    else if (renderer->displayBuffer != renderer->clientState.framebuffer[indexOfGLTarget(GL_DRAW_FRAMEBUFFER)]) {
        /* GL_BACK/GL_FRONT gibt es nur am Fenster: dorthin zeichnen, die Lesebindung bleibt (wined3d merkt sich
         * Lese- und Zeichenbindung getrennt und bindet nicht neu, was es für gebunden hält) */
        GLFramebuffer_bind(GLES2 ? GL_FRAMEBUFFER : GL_DRAW_FRAMEBUFFER, renderer->displayBuffer);
    }
}

void GLRenderer_enableVertexAttribute(GLRenderer* renderer, int location) {
    if (location >= 0 && !renderer->enabledVertexAttribs[location]) {
        glEnableVertexAttribArray(location);
        renderer->enabledVertexAttribs[location] = true;
    }
}

void GLRenderer_disableVertexAttribute(GLRenderer* renderer, int location) {
    if (location >= 0 && renderer->enabledVertexAttribs[location]) {
        glDisableVertexAttribArray(location);
        renderer->enabledVertexAttribs[location] = false;
    }
}

void GLRenderer_disableUnusedVertexAttributes(GLRenderer* renderer) {
    for (int i = 0; i < ARRAY_SIZE(renderer->enabledVertexAttribs); i++) {
        if (renderer->enabledVertexAttribs[i]) {
            glDisableVertexAttribArray(i);
            renderer->enabledVertexAttribs[i] = false;
        }
    }
}

static void freePixelReadCache(GLRenderer* renderer) {
    if (renderer->pixelReadCache) {
        if (renderer->pixelReadCache->data) free(renderer->pixelReadCache->data);
        MEMFREE(renderer->pixelReadCache); /* auch die Struktur (lief vorher bei jedem ungecachten Lesen aus) */
    }
}

void GLRenderer_resetFrameCount(GLRenderer* renderer) {
    freePixelReadCache(renderer);
    renderer->frameCount = 0;
}

void GLRenderer_readPixels(GLRenderer* renderer, GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels) {
    int srcDataSize = width * height * 4;
    GLuint framebuffer = renderer->clientState.framebuffer[indexOfGLTarget(GL_READ_FRAMEBUFFER)];
    /* Mit gebundenem Pack-Puffer ist pixels ein Versatz in diesen Puffer, kein Zeiger: den Lesecache
     * dann weder hineinkopieren noch daraus füllen (vorher Absturz, memcpy an Adresse 0) */
    GLBuffer* pixelPackBuffer = GLBuffer_getBound(GL_PIXEL_PACK_BUFFER);
    PixelReadCache* pixelReadCache = renderer->pixelReadCache;
    if (pixelReadCache && !pixelPackBuffer && x == 0 && y == 0 &&
                          pixelReadCache->dataSize == srcDataSize &&
                          pixelReadCache->format == format &&
                          pixelReadCache->type == type &&
                          pixelReadCache->framebuffer == framebuffer &&
                         (renderer->frameCount-pixelReadCache->frameIndex) <= PIXEL_READ_CACHE_SKIP_FRAMES) {
        memcpy(pixels, pixelReadCache->data, srcDataSize);
        return;
    }

    bool convert = format != GL_RGBA && format != GL_BGRA;
    bool cacheResult = x == 0 && y == 0 && width <= 256 && height <= 256 && !convert && !pixelPackBuffer;
    if (GLES2 && pixelPackBuffer) {
        /* kein Pufferobjekt in ES 2.0: direkt in den geteilten Speicher des Clients lesen */
        if (!pixelPackBuffer->mappedData) return;
        pixels = pixelPackBuffer->mappedData + (uintptr_t)pixels;
        pixelPackBuffer = NULL;
    }
    if (pixelPackBuffer && convert) glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    char* srcData = convert ? malloc(srcDataSize) : pixels;
    bool rebound = GLFramebuffer_beginReadAccess();
    glReadPixels(x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, srcData);
    GLFramebuffer_endReadAccess(rebound);

    if (!cacheResult) freePixelReadCache(renderer);

    if (pixelPackBuffer) {
        if (!convert) {
            srcData = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, pixelPackBuffer->size, GL_MAP_READ_BIT);
            memcpy(pixelPackBuffer->mappedData, srcData, pixelPackBuffer->size);
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        }
        else glBindBuffer(GL_PIXEL_PACK_BUFFER, pixelPackBuffer->id);
    }

    if (type == GL_BYTE ||
        type == GL_UNSIGNED_BYTE ||
        type == GL_UNSIGNED_INT_8_8_8_8 ||
        type == GL_UNSIGNED_INT_8_8_8_8_REV ||
        type == GL_UNSIGNED_SHORT_5_6_5) {
        char* dstData = pixelPackBuffer ? pixelPackBuffer->mappedData : pixels;

        switch (format) {
            case GL_RED:
            case GL_GREEN:
            case GL_BLUE:
            case GL_LUMINANCE:
                for (int i = 0, j = format - GL_RED, k = 0; i < srcDataSize; i += 4, k++) dstData[k] = srcData[i+j];
                break;
            case GL_RGB:
                if (type == GL_UNSIGNED_SHORT_5_6_5) {
                    for (int i = 0, j = 0; i < srcDataSize; i += 4, j += 2) {
                        uint8_t r = srcData[i+0];
                        uint8_t g = srcData[i+1];
                        uint8_t b = srcData[i+2];

                        dstData[j+0] = ((g & 0x1c) << 3) | (b >> 3);
                        dstData[j+1] = (r & 0xf8) | (g >> 5);
                    }
                }
                else {
                    for (int i = 0, j = 0; i < srcDataSize; i += 4, j += 3) {
                        dstData[j+0] = srcData[i+0];
                        dstData[j+1] = srcData[i+1];
                        dstData[j+2] = srcData[i+2];
                    }
                }
                break;
            case GL_BGR:
                for (int i = 0, j = 0; i < srcDataSize; i += 4, j += 3) {
                    dstData[j+0] = srcData[i+2];
                    dstData[j+1] = srcData[i+1];
                    dstData[j+2] = srcData[i+0];
                }
                break;
            case GL_BGRA:
                /* dst und src sind hier derselbe Puffer, daher über eine Zwischenvariable tauschen */
                for (int i = 0; i < srcDataSize; i += 4) {
                    char red = srcData[i+0];
                    dstData[i+0] = srcData[i+2];
                    dstData[i+1] = srcData[i+1];
                    dstData[i+2] = red;
                    dstData[i+3] = srcData[i+3];
                }
                break;
            case GL_RGBA:
                break;
            default:
                println("gladio:readPixels: unimplemented format %x", format);
                break;
        }
    }
    else println("gladio:readPixels: unimplemented type %x", type);

    /* Lesecache: die Pixel so, wie der Client sie bekommt (nach der Umwandlung), nur für dasselbe
     * Format wiederverwenden (vorher kam ein zweites GL_BGRA-Lesen als RGBA aus dem Cache) */
    if (cacheResult) {
        if (!pixelReadCache) renderer->pixelReadCache = pixelReadCache = calloc(1, sizeof(PixelReadCache));
        if (srcDataSize != pixelReadCache->dataSize) {
            MEMFREE(pixelReadCache->data);
            pixelReadCache->data = malloc(srcDataSize);
            pixelReadCache->dataSize = srcDataSize;
        }
        pixelReadCache->framebuffer = framebuffer;
        pixelReadCache->format = format;
        pixelReadCache->type = type;
        pixelReadCache->frameIndex = renderer->frameCount;
        memcpy(pixelReadCache->data, pixels, srcDataSize);
    }

    if (convert) free(srcData);
}