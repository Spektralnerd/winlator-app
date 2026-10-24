#ifndef GLADIO_SHADER_CONVERTER_ES2_H
#define GLADIO_SHADER_CONVERTER_ES2_H

#include "shader_converter.h"

/* Zeile nach GLSL ES 1.00 umschreiben (nach den gl_*-Ersetzungen aufzurufen); gibt line frei. */
extern char* ShaderConverterES2_convertLine(ShaderObject* shader, char* line, int scopeBefore);
/* Ersatz für eingebaute gl_*-Namen, die ES 1.00 nicht kennt (NULL = nichts Besonderes). */
extern const char* ShaderConverterES2_replaceBuiltin(ShaderObject* shader, const char* name, int nameLen);
/* Größe des varying-Felds gd_TexCoord[] für alle Shader eines Programms. */
extern int ShaderConverterES2_texCoordArraySize(ShaderProgram* program);
/* Kopf (#version 100, Erweiterungen, Präzision, Ersatzfunktionen). */
extern void ShaderConverterES2_injectHeader(ShaderObject* shader, void (*insertLine)(ShaderObject*, int, char*), int* head);
/* gl_FragColor aus einem Ausgabefeld (out vec4 x[n]) am Ende von main. */
extern void ShaderConverterES2_finishFragmentOutput(ShaderObject* shader, void (*insertLine)(ShaderObject*, int, char*), ShaderFunction** mainFunctions, int numMainFunctions);

#endif
