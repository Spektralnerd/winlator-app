/*
 * Streaming-Puffer des Servers (ES-2.0-Pfad, gl_es2.c): Puffer mit GL_STREAM_DRAW und Puffer, die in einem
 * Bild mehrfach teilweise neu beschrieben werden, zeichnet der Server aus einer eigenen Kopie als
 * Client-Arrays und stellt die Attribute nach jedem Zeichenaufruf wieder auf die GL-Puffer. Acht Felder
 * (4 x 2), Farben als GL_UNSIGNED_BYTE (RGBA, normalisiert) wie bei wined3d ohne ARB_vertex_array_bgra:
 *
 *   0  GL_STREAM_DRAW, sechsmal hochgeladen (wachsender Versatz) und jeweils gezeichnet (sechs Streifen)
 *   1  GL_STATIC_DRAW, im selben Bild fünfmal teilweise neu beschrieben (ab dem vierten Mal Kopie), nach
 *      jedem Mal gezeichnet (fünf Streifen)
 *   2  Eckpunkte und Indizes aus GL_STREAM_DRAW-Puffern, glDrawElements
 *   3  aus einem GL_STREAM_DRAW-Puffer gezeichnet, Puffer gelöscht, neuer GL_STATIC_DRAW-Puffer an denselben
 *      Attributen, gezeichnet (rechte Hälfte)
 *   4  GL_STREAM_DRAW-Puffer mit glBufferData vergrößert, neu hochgeladen, gezeichnet
 *   5  vorhandener Puffer mit glBufferData (gleiche Größe, GL_STREAM_DRAW) neu befüllt, gezeichnet
 *   6  Position aus einem GL_STREAM_DRAW-Puffer, Farbe aus einem GL_STATIC_DRAW-Puffer
 *   7  glDrawElementsBaseVertex aus GL_STREAM_DRAW-Puffern (ES 2: Indizes umgerechnet)
 *
 *   gcc -O1 -o streamtest streamtest.c -lX11 -lGL -ldl
 *   ./streamtest out.ppm          (mit Mesa direkt vergleichen: ppmdiff.py)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <X11/Xlib.h>

#define W 256
#define H 128

typedef struct Vertex {
    float pos[4];
    uint8_t color[4];
    float pad[3];
} Vertex;

static void* proc(const char* name) {
    void* p = (void*)glXGetProcAddressARB((const GLubyte*)name);
    if (!p) p = dlsym(RTLD_DEFAULT, name);
    if (!p) { fprintf(stderr, "%s fehlt\n", name); exit(3); }
    return p;
}

static GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "Shader %x: %s\n", type, log);
        exit(4);
    }
    return s;
}

static const char* vs =
    "#version 130\n"
    "in vec4 vs_in0;\n"
    "in vec4 vs_in5;\n"
    "out vec4 color;\n"
    "void main()\n"
    "{\n"
    "    gl_Position = vs_in0;\n"
    "    color = vs_in5;\n"
    "}\n";
static const char* fs =
    "#version 130\n"
    "in vec4 color;\n"
    "out vec4 ps_out;\n"
    "void main()\n"
    "{\n"
    "    ps_out = color;\n"
    "}\n";

/* Streifen [part/parts] eines Felds (0..7, Zeile 0 oben) als Dreiecksstreifen; Farbe R,G,B */
static void quad(Vertex* v, int cell, int part, int parts, uint8_t r, uint8_t g, uint8_t b) {
    float top = 1.0f - (cell / 4) * 1.0f;
    float x0 = -1.0f + (cell % 4) * 0.5f + 0.05f, y0 = top - 1.0f + 0.05f;
    float width = 0.4f / parts;
    x0 += part * width;
    float x1 = x0 + width, y1 = top - 0.05f;
    float xy[4][2] = { {x0, y0}, {x1, y0}, {x0, y1}, {x1, y1} };
    for (int i = 0; i < 4; i++) {
        memset(&v[i], 0, sizeof(Vertex));
        v[i].pos[0] = xy[i][0];
        v[i].pos[1] = xy[i][1];
        v[i].pos[3] = 1.0f;
        /* je Ecke etwas anders, damit vertauschte Eckpunkte auffallen */
        v[i].color[0] = r;
        v[i].color[1] = (uint8_t)(g + i * 16);
        v[i].color[2] = b;
        v[i].color[3] = 255;
    }
}

static void pointers(GLintptr base) {
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(base + 0));
    glVertexAttribPointer(5, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), (void*)(base + 16));
}

int main(int argc, char** argv) {
    const char* out = argc > 1 ? argv[1] : "out.ppm";
    Display* dpy = XOpenDisplay(NULL);
    if (!dpy) return 1;
    int screen = DefaultScreen(dpy);
    int attribs[] = { GLX_RGBA, GLX_DOUBLEBUFFER, None };
    XVisualInfo* vis = glXChooseVisual(dpy, screen, attribs);
    if (!vis) { puts("kein Visual"); return 2; }
    GLXContext ctx = glXCreateContext(dpy, vis, NULL, True);
    Window win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 0, 0, W, H, 0, 0, 0);
    XMapWindow(dpy, win);
    XSync(dpy, False);
    glXMakeCurrent(dpy, win, ctx);
    printf("%s / GL %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

    PFNGLDRAWELEMENTSBASEVERTEXPROC pDrawElementsBaseVertex = proc("glDrawElementsBaseVertex");

    GLuint program = glCreateProgram();
    glAttachShader(program, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(program, compile(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(program, 0, "vs_in0");
    glBindAttribLocation(program, 5, "vs_in5");
    glLinkProgram(program);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) { puts("Link fehlgeschlagen"); return 5; }
    glUseProgram(program);

    glViewport(0, 0, W, H);
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(5);

    /* 0: GL_STREAM_DRAW, sechs Streifen mit wachsendem Versatz */
    GLuint stream;
    glGenBuffers(1, &stream);
    glBindBuffer(GL_ARRAY_BUFFER, stream);
    glBufferData(GL_ARRAY_BUFFER, 65536, NULL, GL_STREAM_DRAW);
    pointers(0);
    for (int k = 0; k < 6; k++) {
        Vertex v[4];
        quad(v, 0, k, 6, (uint8_t)(255 - k * 40), (uint8_t)(k * 30), 64);
        GLintptr offset = 1024 + k * sizeof(v);
        glBufferSubData(GL_ARRAY_BUFFER, offset, sizeof(v), v);
        glDrawArrays(GL_TRIANGLE_STRIP, offset / sizeof(Vertex), 4);
    }

    /* 1: GL_STATIC_DRAW, derselbe Bereich fünfmal neu beschrieben (ab dem vierten Mal Kopie des Servers) */
    GLuint stat;
    glGenBuffers(1, &stat);
    glBindBuffer(GL_ARRAY_BUFFER, stat);
    glBufferData(GL_ARRAY_BUFFER, 8 * sizeof(Vertex), NULL, GL_STATIC_DRAW);
    pointers(0);
    for (int k = 0; k < 5; k++) {
        Vertex v[4];
        quad(v, 1, k, 5, 32, (uint8_t)(200 - k * 30), (uint8_t)(100 + k * 30));
        glBufferSubData(GL_ARRAY_BUFFER, (k & 1) * sizeof(v), sizeof(v), v);
        glDrawArrays(GL_TRIANGLE_STRIP, (k & 1) * 4, 4);
    }

    /* 2: Eckpunkte und Indizes aus GL_STREAM_DRAW-Puffern */
    GLuint ibo;
    glGenBuffers(1, &ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, 4096, NULL, GL_STREAM_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, stream);
    pointers(0);
    {
        Vertex v[4];
        quad(v, 2, 0, 1, 255, 200, 0);
        glBufferSubData(GL_ARRAY_BUFFER, 8192, sizeof(v), v);
        GLushort idx[6] = { 0, 1, 2, 1, 3, 2 };
        for (int i = 0; i < 6; i++) idx[i] += 8192 / sizeof(Vertex);
        glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 64, sizeof(idx), idx);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (void*)64);
    }

    /* 3: aus einem Streaming-Puffer zeichnen, Puffer löschen, neuer fester Puffer, zeichnen */
    {
        GLuint tmp;
        glGenBuffers(1, &tmp);
        glBindBuffer(GL_ARRAY_BUFFER, tmp);
        glBufferData(GL_ARRAY_BUFFER, 4096, NULL, GL_STREAM_DRAW);
        pointers(0);
        Vertex v[4];
        quad(v, 3, 0, 2, 0, 255, 255);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(v), v);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDeleteBuffers(1, &tmp);
        GLuint fixed;
        glGenBuffers(1, &fixed);
        glBindBuffer(GL_ARRAY_BUFFER, fixed);
        quad(v, 3, 1, 2, 255, 0, 255);
        glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
        pointers(0);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    /* 4: GL_STREAM_DRAW-Puffer vergrößern, neu hochladen, zeichnen */
    {
        GLuint grow;
        glGenBuffers(1, &grow);
        glBindBuffer(GL_ARRAY_BUFFER, grow);
        glBufferData(GL_ARRAY_BUFFER, 256, NULL, GL_STREAM_DRAW);
        pointers(0);
        Vertex v[4];
        quad(v, 4, 0, 1, 255, 255, 255);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(v), v);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBufferData(GL_ARRAY_BUFFER, 32768, NULL, GL_STREAM_DRAW);
        quad(v, 4, 0, 1, 64, 128, 255);
        glBufferSubData(GL_ARRAY_BUFFER, 16384, sizeof(v), v);
        glDrawArrays(GL_TRIANGLE_STRIP, 16384 / sizeof(Vertex), 4);
    }

    /* 5: vorhandenen Puffer mit glBufferData (gleiche Größe, GL_STREAM_DRAW) neu befüllen */
    {
        GLuint same;
        glGenBuffers(1, &same);
        glBindBuffer(GL_ARRAY_BUFFER, same);
        Vertex v[4];
        quad(v, 5, 0, 1, 255, 0, 0);
        glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
        quad(v, 5, 0, 1, 0, 160, 64);
        glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STREAM_DRAW);
        pointers(0);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    /* 6: Position aus GL_STREAM_DRAW, Farbe aus GL_STATIC_DRAW */
    {
        Vertex v[4];
        quad(v, 6, 0, 1, 0, 0, 0);
        glBindBuffer(GL_ARRAY_BUFFER, stream);
        glBufferSubData(GL_ARRAY_BUFFER, 12288, sizeof(v), v);
        glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)12288);
        uint8_t colors[4][4] = { {255, 128, 0, 255}, {255, 160, 0, 255}, {200, 128, 64, 255}, {255, 200, 100, 255} };
        GLuint col;
        glGenBuffers(1, &col);
        glBindBuffer(GL_ARRAY_BUFFER, col);
        glBufferData(GL_ARRAY_BUFFER, sizeof(colors), colors, GL_STATIC_DRAW);
        glVertexAttribPointer(5, 4, GL_UNSIGNED_BYTE, GL_TRUE, 4, (void*)0);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    /* 7: glDrawElementsBaseVertex aus GL_STREAM_DRAW-Puffern */
    {
        Vertex v[4];
        quad(v, 7, 0, 1, 128, 64, 255);
        glBindBuffer(GL_ARRAY_BUFFER, stream);
        glBufferSubData(GL_ARRAY_BUFFER, 16384, sizeof(v), v);
        pointers(0);
        GLushort idx[6] = { 0, 1, 2, 1, 3, 2 };
        glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 128, sizeof(idx), idx);
        pDrawElementsBaseVertex(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (void*)128, 16384 / sizeof(Vertex));
    }

    glFinish();
    unsigned char* px = malloc(W * H * 4);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glXSwapBuffers(dpy, win);
    printf("GL-Fehler: 0x%x\n", glGetError());
    for (int c = 0; c < 8; c++) {
        int x = (c % 4) * 64 + 40, y = (1 - c / 4) * 64 + 20;
        unsigned char* p = px + (y * W + x) * 4;
        printf("Feld %d: %3d %3d %3d\n", c, p[0], p[1], p[2]);
    }

    FILE* f = fopen(out, "wb");
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int y = H - 1; y >= 0; y--) for (int x = 0; x < W; x++) fwrite(px + (y * W + x) * 4, 1, 3, f);
    fclose(f);
    return 0;
}
