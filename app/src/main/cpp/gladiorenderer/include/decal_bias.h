#ifndef DECAL_BIAS_H
#define DECAL_BIAS_H

#include <stdbool.h>
#include <stdint.h>
#include "gl_renderer.h"

/*
 * Tiefenversatz für „Aufkleber“: Zeichenaufrufe mit einem Fragment-Shader, der Pixel verwirft (discard – wined3d
 * setzt so Alphatest und Farbschlüssel um), bekommen beim Tiefentest einen kleinen Versatz zur Kamera hin
 * (glPolygonOffset). Alte Direct3D-Spiele legen Blumen, Schatten, Flecken als farbgeschlüsselte Vierecke genau auf
 * den Boden und verlassen sich mit ZFUNC LESSEQUAL darauf, dass gleiche Tiefe gewinnt; Mali rastert die Tiefe
 * zweier deckungsgleicher Flächen mit verschiedenen Eckpunkten nicht bitgleich, die Blume flackert (Tiny Tiger,
 * sobald sich die Kamera bewegt). Der Versatz ist kleiner als alles, was ein Spiel absichtlich staffelt.
 * GLADIO_DECAL_BIAS=0 schaltet ab, "faktor,einheiten" (z. B. -0.5,-2) stellt ein; setzt das Programm selbst
 * GL_POLYGON_OFFSET_FILL, bleibt sein Versatz unangetastet.
 */
extern void DecalBias_capability(GLRenderer* renderer, GLenum cap, bool state);
extern void DecalBias_polygonOffset(GLRenderer* renderer, float factor, float units);
extern bool DecalBias_begin(GLRenderer* renderer);
extern void DecalBias_end(GLRenderer* renderer, bool active);
extern void DecalBias_printStats(uint32_t frames);
extern bool ShaderSource_hasDiscard(const char* source, int size);

#endif
