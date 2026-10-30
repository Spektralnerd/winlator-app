#include "gl_context.h"
#include "gl_es2.h"
#include "time_utils.h"
#include <unistd.h>

static GLuint maxQueryId = 1;
static GLQuery nullQuery = {0};

GLuint GLQuery_create() {
    GLX_CONTEXT_LOCK();
    GLuint id = maxQueryId++;
    SparseArray_put(currentRenderer->clientState.queries, id, &nullQuery);
    GLX_CONTEXT_UNLOCK();
    return id;
}

static GLQuery* findQueryWithTarget(GLenum target) {
    GLX_CONTEXT_LOCK();
    GLQuery* result = NULL;
    for (int i = 0; i < currentRenderer->clientState.queries->size; i++) {
        GLQuery* query = currentRenderer->clientState.queries->entries[i].value;
        if (query != &nullQuery && query->active && query->target == target) {
            result = query;
            break;
        }
    }
    GLX_CONTEXT_UNLOCK();
    return result;
}

static GLQuery* findQueryWithId(GLuint id) {
    GLX_CONTEXT_LOCK();
    GLQuery* query = SparseArray_get(currentRenderer->clientState.queries, id);
    if (query == &nullQuery) {
        query = calloc(1, sizeof(GLQuery));
        if (GLES2) query->id = 0; /* keine Abfrageobjekte in ES 2.0 */
        else glGenQueries(1, &query->id);
        query->ownerId = currentRenderer->contextId;
        query->clientId = id;
        SparseArray_put(currentRenderer->clientState.queries, id, query);
    }
    GLX_CONTEXT_UNLOCK();
    return query;
}

/* Beendete Verdeckungsabfragen in der Reihenfolge ihres Endes merken: der Client bekommt ihre Ergebnisse
 * gesammelt mit der nächsten Frage (GLADIO_QUERY_AVAILABLE_AND_RESULTS), statt je Abfrage einmal zu warten.
 * Höchstens MAX_ENDED_QUERIES; fällt eine heraus, fragt der Client sie wie bisher einzeln ab. */
#define MAX_ENDED_QUERIES 8192

static void rememberEndedQuery(GLRenderer* renderer, GLQuery* query) {
    GLEndedQueries* fifo = &renderer->endedQueries;
    if (fifo->count == fifo->capacity) {
        if (fifo->capacity >= MAX_ENDED_QUERIES) {
            /* voll: die älteste vergessen */
            fifo->head = (fifo->head + 1) % fifo->capacity;
            fifo->count--;
        }
        else {
            uint32_t capacity = fifo->capacity ? fifo->capacity * 2 : 256;
            GLuint* clientIds = malloc(capacity * sizeof(GLuint));
            uint32_t* serials = malloc(capacity * sizeof(uint32_t));
            if (!clientIds || !serials) {
                free(clientIds);
                free(serials);
                return;
            }
            for (uint32_t i = 0; i < fifo->count; i++) {
                clientIds[i] = fifo->clientIds[(fifo->head + i) % fifo->capacity];
                serials[i] = fifo->serials[(fifo->head + i) % fifo->capacity];
            }
            free(fifo->clientIds);
            free(fifo->serials);
            fifo->clientIds = clientIds;
            fifo->serials = serials;
            fifo->head = 0;
            fifo->capacity = capacity;
        }
    }
    if (++fifo->lastSerial == 0) fifo->lastSerial = 1;
    query->endSerial = fifo->lastSerial;
    uint32_t tail = (fifo->head + fifo->count) % fifo->capacity;
    fifo->clientIds[tail] = query->clientId;
    fifo->serials[tail] = query->endSerial;
    fifo->count++;
}

/* Mali-G615 (CMF Phone 1): meldet der Treiber eine Abfrage als fertig und der Server liest das Ergebnis sofort, kommt
 * ab und zu 0 statt des echten Werts – die Unreal Engine 3 lässt das Objekt dann ein Bild lang weg (Outlast: Torflügel,
 * Mauern, der Scheinwerfer des Wagens). Mit einer Rundreise Abstand zwischen „fertig?“ und dem Lesen (wie vor den
 * gesammelten Antworten) passierte es viel seltener, aber noch. Deshalb gilt ein Ergebnis erst diese Zeit, nachdem der
 * Treiber es zuerst als fertig meldete, als lesbar; bis dahin heißt es „noch nicht fertig“ (der Client fragt ~1 ms
 * später wieder). Das kostet je Bild etwa diese Zeit einmal, nicht je Abfrage: die Abfragen eines Durchgangs werden
 * zusammen fertig. */
#define OCCLUSION_RESULT_GRACE_NS 2000000ULL

static bool queryReadable(GLQuery* query) {
    return !query->active && query->id > 0 && query->ownerId == currentRenderer->contextId;
}

/* fertig und seit OCCLUSION_RESULT_GRACE_NS fertig gemeldet */
static bool occlusionAvailable(GLQuery* query, bool* driverAvailable) {
    GLuint available = GL_FALSE;
    glGetQueryObjectuiv(query->id, GL_QUERY_RESULT_AVAILABLE, &available);
    if (driverAvailable) *driverAvailable = available;
    if (!available) {
        query->availableSinceNs = 0;
        return false;
    }
    uint64_t now = nanoTime();
    if (!query->availableSinceNs) query->availableSinceNs = now;
    return now - query->availableSinceNs >= OCCLUSION_RESULT_GRACE_NS;
}

/* Ergebnis einer beendeten Verdeckungsabfrage: erst lesen, wenn es nach obiger Regel lesbar ist (höchstens ~50 ms
 * warten), und ein Ergebnis, das der Treiber nicht liefert (Fehler: Abfrage noch aktiv, fremder Kontext – params bleibt
 * unverändert), gilt als „sichtbar“: ein zu Unrecht sichtbares Objekt fällt nicht auf, ein zu Unrecht unsichtbares
 * verschwindet ein Bild lang. */
static uint32_t occlusionResult(GLQuery* query) {
    GLuint result = UINT32_MAX;
    if (queryReadable(query)) {
        for (int i = 0; i < 500; i++) {
            if (occlusionAvailable(query, NULL)) break;
            if (i == 0) glFlush();
            usleep(100);
        }
        glGetQueryObjectuiv(query->id, GL_QUERY_RESULT, &result);
        if (result == 0) {
            /* Null ist das einzige schädliche Ergebnis: gleich noch einmal lesen, falls der Treiber „fertig“ meldete,
             * bevor der Wert für den Prozessor sichtbar war (eine Pause dazwischen kostete auf dem PC 3 ms je Bild) */
            glGetQueryObjectuiv(query->id, GL_QUERY_RESULT, &result);
        }
    }
    if (result == UINT32_MAX) result = GL_TRUE;
    query->counter = result == GL_TRUE && query->target == GL_SAMPLES_PASSED ? UINT8_MAX : result;
    return query->counter;
}

/* Ergebnisse der fertigen beendeten Abfragen in der Reihenfolge ihres Endes (Paare Kennung, Ergebnis) bis zur
 * ersten noch nicht fertigen (deren Kennung in firstPendingId, sonst 0); Rückgabe: Zahl der Paare */
int GLQuery_collectResults(GLuint* pairs, int maxPairs, GLuint* firstPendingId) {
    *firstPendingId = 0;
    if (!currentRenderer || GLES2) return 0;
    GLEndedQueries* fifo = &currentRenderer->endedQueries;
    int n = 0;
    GLX_CONTEXT_LOCK();
    while (fifo->count > 0 && n < maxPairs) {
        GLuint clientId = fifo->clientIds[fifo->head];
        uint32_t serial = fifo->serials[fifo->head];
        GLQuery* query = SparseArray_get(currentRenderer->clientState.queries, clientId);
        bool current = query && query != &nullQuery && query->endSerial == serial && queryReadable(query);
        if (current) {
            bool driverAvailable = false;
            if (!occlusionAvailable(query, &driverAvailable)) {
                *firstPendingId = clientId;
                /* Die Wartezeit der folgenden Abfragen beginnt jetzt, nicht erst, wenn die Reihe an sie kommt (sonst
                 * käme je Abfrage eine Wartezeit hinzu: d3d9occlmany 41 statt 200 Bilder/s) */
                for (uint32_t i = 1; driverAvailable && i < fifo->count; i++) {
                    uint32_t pos = (fifo->head + i) % fifo->capacity;
                    GLQuery* later = SparseArray_get(currentRenderer->clientState.queries, fifo->clientIds[pos]);
                    if (!later || later == &nullQuery || later->endSerial != fifo->serials[pos] || !queryReadable(later)) continue;
                    occlusionAvailable(later, &driverAvailable);
                }
                break;
            }
            pairs[2 * n] = clientId;
            pairs[2 * n + 1] = occlusionResult(query);
            n++;
        }
        fifo->head = (fifo->head + 1) % fifo->capacity;
        fifo->count--;
    }
    GLX_CONTEXT_UNLOCK();
    return n;
}

void GLQuery_begin(GLenum target, GLuint id) {
    GLQuery* query = findQueryWithId(id);
    if (!query) return;
    query->target = target;
    query->active = true;
    query->counter = 0;
    query->endSerial = 0; /* ein gemerktes früheres Ende gilt nicht mehr */
    query->availableSinceNs = 0;
    currentRenderer->activeQuery = query;

    switch(target) {
        case GL_SAMPLES_PASSED:
            target = GL_ANY_SAMPLES_PASSED;
        case GL_ANY_SAMPLES_PASSED:
        case GL_ANY_SAMPLES_PASSED_CONSERVATIVE:
        case GL_PRIMITIVES_GENERATED:
        case GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN:
            if (!GLES2) glBeginQuery(target, query->id);
            break;
        case GL_TIME_ELAPSED:
            query->counter = nanoTime() - currentRenderer->queriesStartTime;
            break;
    }
}

void GLQuery_end(GLenum target) {
    bool useActiveQuery = currentRenderer->activeQuery && currentRenderer->activeQuery->target == target;
    GLQuery* query = useActiveQuery ? currentRenderer->activeQuery : findQueryWithTarget(target);
    if (!query) return;
    query->active = false;
    query->availableSinceNs = 0;

    if (query->target == GL_TIME_ELAPSED) {
        query->counter = (nanoTime() - currentRenderer->queriesStartTime) - query->counter;
    }
    else {
        query->counter = 0;
        if (target == GL_SAMPLES_PASSED) target = GL_ANY_SAMPLES_PASSED;
        if (!GLES2) glEndQuery(target);
        if (!GLES2 && query->id > 0 && (target == GL_ANY_SAMPLES_PASSED || target == GL_ANY_SAMPLES_PASSED_CONSERVATIVE))
            rememberEndedQuery(currentRenderer, query);
    }
}

void GLQuery_getParamsv(GLenum target, GLenum pname, GLint* params) {
    if (pname == GL_QUERY_COUNTER_BITS) {
        /* Größe des Zählers je Abfrageart, auch ohne laufende Abfrage: wined3d fragt sie beim Start
         * ab und schaltet Verdeckungsabfragen bei 0 ab (D3D9: CreateQuery(D3DQUERYTYPE_OCCLUSION)
         * -> D3DERR_NOTAVAILABLE, für die Unreal Engine 3 ein fataler Fehler). Die Zahl der
         * Abtastwerte wird nachgebildet: ES 3 kennt nur „irgendein Abtastwert“ (Ergebnis 0 oder
         * UINT8_MAX), unter ES 2 gilt alles als sichtbar. */
        switch (target) {
            case GL_SAMPLES_PASSED:
            case GL_ANY_SAMPLES_PASSED:
            case GL_ANY_SAMPLES_PASSED_CONSERVATIVE:
            case GL_TIME_ELAPSED:
                *params = 32;
                break;
            default:
                *params = 0;
                break;
        }
        return;
    }

    bool useActiveQuery = currentRenderer->activeQuery && currentRenderer->activeQuery->target == target;
    GLQuery* query = useActiveQuery ? currentRenderer->activeQuery : findQueryWithTarget(target);
    if (!query) return;

    if (pname == GL_CURRENT_QUERY) {
        *params = query->counter;
    }
}

void GLQuery_getObjectParamsv(GLuint id, GLenum pname, GLint* params) {
    GLQuery* query = findQueryWithId(id);
    if (!query) return;

    if (pname == GL_QUERY_RESULT_AVAILABLE) {
        /* ES 3: beim Treiber nachfragen, statt immer „fertig“ zu melden – sonst wartet das folgende
         * GL_QUERY_RESULT, bis die GPU alles bis zur Abfrage gezeichnet hat (je Abfrage ein
         * Stillstand; die Engine stellt viele Abfragen je Bild und holt sie Bilder später ab).
         * Eine noch nie begonnene oder gerade laufende Abfrage lässt glGetQueryObjectuiv
         * unverändert (GL_INVALID_OPERATION), dann bleibt es bei GL_TRUE. */
        GLuint available = GL_TRUE;
        if (!GLES2 && query->target != GL_TIME_ELAPSED && queryReadable(query)) available = occlusionAvailable(query, NULL);
        *params = available;
    }
    else if (pname == GL_QUERY_RESULT) {
        if (query->target == GL_TIME_ELAPSED) {
            *params = query->counter;
        }
        else if (GLES2) {
            /* ohne Verdeckungsabfragen gilt alles als sichtbar */
            query->counter = query->target == GL_SAMPLES_PASSED ? UINT8_MAX : GL_TRUE;
            *params = query->counter;
        }
        else {
            *params = occlusionResult(query);
        }
    }
}

void GLQuery_queryCounter(GLuint id, GLenum target) {
    GLQuery* query = findQueryWithId(id);
    if (!query || target != GL_TIMESTAMP) return;

    query->target = GL_TIME_ELAPSED;
    query->counter = nanoTime() - currentRenderer->queriesStartTime;
}

void GLQuery_delete(GLuint id) {
    GLX_CONTEXT_LOCK();
    GLQuery* query = SparseArray_get(currentRenderer->clientState.queries, id);
    if (query == &nullQuery) {
        SparseArray_remove(currentRenderer->clientState.queries, id);
    }
    else if (query) {
        if (query == currentRenderer->activeQuery) currentRenderer->activeQuery = NULL;
        if (query->ownerId == currentRenderer->contextId) {
            if (query->id > 0) glDeleteQueries(1, &query->id);
            SparseArray_remove(currentRenderer->clientState.queries, id);
            free(query);
        }
    }
    GLX_CONTEXT_UNLOCK();
}
bool GLQuery_exists(GLuint id) {
    GLX_CONTEXT_LOCK();
    bool result = SparseArray_get(currentRenderer->clientState.queries, id) != NULL;
    GLX_CONTEXT_UNLOCK();
    return result;
}
