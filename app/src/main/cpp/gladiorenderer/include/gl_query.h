#ifndef GLADIO_GL_QUERY_H
#define GLADIO_GL_QUERY_H

#include "gladio.h"

typedef struct GLQuery {
    GLuint ownerId;
    GLuint id;
    bool active;
    GLenum target;
    uint32_t counter;
    GLuint clientId;    /* Kennung beim Client */
    uint32_t endSerial; /* Nummer des letzten Endes (endedQueries im Renderer), 0: läuft oder nie beendet */
    uint64_t availableSinceNs; /* wann der Treiber das Ergebnis zuerst als fertig meldete (0: noch nicht) */
    uint32_t resultSerial;     /* endSerial, für das counter schon vom Treiber gelesen wurde (0: noch nicht) */
} GLQuery;

extern GLuint GLQuery_create();
extern void GLQuery_begin(GLenum target, GLuint id);
extern void GLQuery_end(GLenum target);
extern void GLQuery_getParamsv(GLenum target, GLenum pname, GLint* params);
extern void GLQuery_getObjectParamsv(GLuint id, GLenum pname, GLint* params);
extern void GLQuery_queryCounter(GLuint id, GLenum target);
extern void GLQuery_delete(GLuint id);
extern bool GLQuery_exists(GLuint id);
extern int GLQuery_collectResults(GLuint* pairs, int maxPairs, GLuint* firstPendingId);

#endif
