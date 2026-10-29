#ifndef GLADIO_GL_DIAG_H
#define GLADIO_GL_DIAG_H

#include "gladio.h"

/*
 * Diagnose des Gladio-Servers (gl_diag.c)
 *
 * Immer an, auch im Freigabe-Bau (kostet fast nichts):
 *  - Shader, die der Treiber nicht übersetzt, und Programme, die er nicht bindet: die ersten Fälle mit
 *    Fehlertext ins Protokoll, der Quelltext in eine Datei im Diagnoseordner (Documents/Winlator)
 *  - GL-Fehler: nach jedem Zeichenaufruf und beim Bildwechsel geprüft. Findet sich einer, prüft der
 *    Server zwei Bilder lang nach jeder Anfrage, um den Auslöser zu benennen
 *  - unvollständige Zeichenziele: geprüft beim ersten Zeichenaufruf nach einer Änderung am FBO
 *  - Anzahl dieser Fehler in der 5-s-Statistik
 *
 * Auf Anforderung (App-Menü „Bild aufzeichnen (Gladio)“, am PC GLADIO_PC_CAPTURE_AT): das nächste
 * vollständige Bild nach <Diagnoseordner>/gladio-JJJJMMTT-HHMMSS/ – jede Anfrage mit Argumenten und
 * GL-Fehlern, zu jedem Zeichenaufruf Programm, Sampler mit Texturen, Vertexattribute und Zustand, das
 * Ergebnis jedes Durchgangs als PNG (Farbe und Alpha, Wertebereiche je Kanal) und die Quelltexte der
 * benutzten Programme.
 */

struct GLContext;
struct ShaderObject;
struct ShaderProgram;

typedef struct GladioDiagStats {
    uint32_t glErrors;
    uint32_t incompleteFramebuffers;
    uint32_t compileErrors;
    uint32_t linkErrors;
} GladioDiagStats;

/* Zähler über alle Verbindungen (für die 5-s-Statistik) */
extern GladioDiagStats gladio_diagStats;

/* Ordner für Diagnosedateien (Documents/Winlator); ohne nur Protokoll */
extern void GladioDiag_setDir(const char* dir);
/* das nächste vollständige Bild aufzeichnen (aus einem beliebigen Thread) */
extern void GladioDiag_requestCapture(const char* dir);

/* aus der Anfrageschleife (gl_context.c): vor und nach der Bearbeitung jeder Anfrage */
extern void GladioDiag_beforeRequest(struct GLContext* context, short requestCode, bool drawing);
extern void GladioDiag_afterRequest(struct GLContext* context, short requestCode, bool drawing);
/* glGetError des Programms: zuerst ein Fehler, den die Diagnose dem Treiber schon abgenommen hat */
extern GLenum GladioDiag_takePendingError(struct GLContext* context);
/* Ende einer Verbindung (eine laufende Aufzeichnung abschließen) */
extern void GladioDiag_endContext(struct GLContext* context);

extern void GladioDiag_shaderCompileFailed(struct ShaderObject* shader, GLuint programId, const char* source);
extern void GladioDiag_programLinkFailed(struct ShaderProgram* program);

/* Zeile für die 5-s-Statistik, wenn es seit der letzten Fehler gab (sonst nichts) */
extern void GladioDiag_printStats(void);

#endif
