/*
 * GL_BGRA-Eckpunktfarben aus Puffern (ARB_vertex_array_bgra), wie wined3d sie ab GL 3.2 für
 * D3DCOLOR angibt, und das Hochladen über abgebildete Puffer. Acht Felder (4 x 2), je ein Quadrat
 * mit Diffus- und Glanzfarbe (zwei GL_BGRA-Attribute, Abstand 32 Byte wie bei wined3d):
 *
 *   0  glDrawArrays mit first > 0 aus einem festen Puffer
 *   1  glDrawElements mit Indexpuffer
 *   2  glDrawElementsBaseVertex
 *   3  glDrawElements mit Indizes im Client-Speicher (Eckpunkte im Puffer)
 *   4  Teilbereich abgebildet (GL_MAP_FLUSH_EXPLICIT_BIT | GL_MAP_UNSYNCHRONIZED_BIT), Übertragung
 *      mit Versatz ab dem Anfang des abgebildeten Bereichs, Zeichnen mit first > 0
 *   5  zwei Zeichenaufrufe hintereinander aus demselben Streaming-Puffer (wachsender Versatz)
 *   6  Diffusfarbe abgeschaltet (glVertexAttrib4f), danach wieder an ohne neues glVertexAttribPointer
 *      (rechte Hälfte des Felds)
 *   7  ganzer Puffer mit glMapBuffer(GL_WRITE_ONLY) neu beschrieben, danach gezeichnet
 *
 *   gcc -O1 -o bgratest bgratest.c -lX11 -lGL -ldl
 *   ./bgratest out.ppm          (mit Mesa direkt vergleichen: ppmdiff.py)
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
    uint8_t diffuse[4];  /* B G R A */
    uint8_t specular[4]; /* B G R A */
    float pad[2];
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
    "in vec4 vs_in6;\n"
    "out vec4 color;\n"
    "void main()\n"
    "{\n"
    "    gl_Position = vs_in0;\n"
    "    color = vs_in5 * 0.75 + vs_in6 * 0.25;\n"
    "}\n";
static const char* fs =
    "#version 130\n"
    "in vec4 color;\n"
    "out vec4 ps_out;\n"
    "void main()\n"
    "{\n"
    "    ps_out = color;\n"
    "}\n";

/* Quadrat (Dreiecksstreifen) im Feld 0..7; Farben als R,G,B */
static void quad(Vertex* v, int cell, const uint8_t d[3], const uint8_t s[3]) {
    float x0 = -1.0f + (cell % 4) * 0.5f + 0.05f, y0 = (cell < 4 ? 0.0f : -1.0f) + 0.05f;
    float x1 = x0 + 0.4f, y1 = y0 + 0.9f;
    float xy[4][2] = { {x0, y0}, {x1, y0}, {x0, y1}, {x1, y1} };
    for (int i = 0; i < 4; i++) {
        memset(&v[i], 0, sizeof(Vertex));
        v[i].pos[0] = xy[i][0];
        v[i].pos[1] = xy[i][1];
        v[i].pos[3] = 1.0f;
        /* je Ecke etwas anders, damit vertauschte Eckpunkte auffallen */
        v[i].diffuse[0] = d[2]; v[i].diffuse[1] = (uint8_t)(d[1] + i * 20); v[i].diffuse[2] = d[0]; v[i].diffuse[3] = 255;
        v[i].specular[0] = s[2]; v[i].specular[1] = s[1]; v[i].specular[2] = s[0]; v[i].specular[3] = 255;
    }
}

static void pointers(GLintptr base) {
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(base + 0));
    glVertexAttribPointer(5, GL_BGRA, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), (void*)(base + 16));
    glVertexAttribPointer(6, GL_BGRA, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), (void*)(base + 20));
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
    PFNGLMAPBUFFERRANGEPROC pMapBufferRange = proc("glMapBufferRange");
    PFNGLFLUSHMAPPEDBUFFERRANGEPROC pFlushMappedBufferRange = proc("glFlushMappedBufferRange");

    GLuint program = glCreateProgram();
    glAttachShader(program, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(program, compile(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(program, 0, "vs_in0");
    glBindAttribLocation(program, 5, "vs_in5");
    glBindAttribLocation(program, 6, "vs_in6");
    glLinkProgram(program);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) { puts("Link fehlgeschlagen"); return 5; }
    glUseProgram(program);

    glViewport(0, 0, W, H);
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    static const uint8_t colors[8][2][3] = {
        { {255, 64, 0}, {0, 0, 255} }, { {0, 200, 255}, {255, 0, 0} }, { {200, 0, 100}, {0, 255, 0} }, { {50, 100, 250}, {250, 50, 0} },
        { {255, 255, 0}, {0, 0, 255} }, { {0, 64, 255}, {255, 128, 0} }, { {128, 0, 255}, {255, 255, 255} }, { {255, 0, 0}, {0, 0, 0} },
    };

    /* fester Puffer: Platzhalter, dann Felder 0..3 ab Eckpunkt 4, 8, 12, 16 */
    Vertex fixed[24];
    memset(fixed, 0, sizeof(fixed));
    for (int c = 0; c < 4; c++) quad(&fixed[4 + c * 4], c, colors[c][0], colors[c][1]);
    GLuint vbo, ibo;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(fixed), fixed, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(5);
    glEnableVertexAttribArray(6);
    pointers(0);

    /* 0: glDrawArrays, first 4 */
    glDrawArrays(GL_TRIANGLE_STRIP, 4, 4);

    /* 1: Indexpuffer (Eckpunkte 8..11); 2: dieselben Indizes 0..3 mit basevertex 12 */
    GLushort idx[12] = { 0, 0, 0, 8, 9, 10, 9, 11, 10, 0, 1, 2 };
    GLushort idxBase[6] = { 0, 1, 2, 1, 3, 2 };
    glGenBuffers(1, &ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    GLushort ibuf[18];
    memcpy(ibuf, idx, sizeof(idx));
    memcpy(ibuf + 12, idxBase, sizeof(idxBase));
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(ibuf), ibuf, GL_STATIC_DRAW);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (void*)(3 * sizeof(GLushort)));
    pDrawElementsBaseVertex(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (void*)(12 * sizeof(GLushort)), 12);

    /* 3: Indizes im Client-Speicher (Eckpunkte 16..19) */
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    GLushort clientIdx[6] = { 16, 17, 18, 17, 19, 18 };
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, clientIdx);

    /* Streaming-Puffer */
    GLuint stream;
    glGenBuffers(1, &stream);
    glBindBuffer(GL_ARRAY_BUFFER, stream);
    glBufferData(GL_ARRAY_BUFFER, 65536, NULL, GL_STREAM_DRAW);
    pointers(0);

    /* 4: Teilbereich ab 1024 abbilden, Quadrat bei +128 schreiben, nur diese 128 Byte übertragen */
    Vertex v4[4];
    quad(v4, 4, colors[4][0], colors[4][1]);
    uint8_t* map = pMapBufferRange(GL_ARRAY_BUFFER, 1024, 512, GL_MAP_WRITE_BIT | GL_MAP_FLUSH_EXPLICIT_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
    if (!map) { puts("glMapBufferRange fehlgeschlagen"); return 6; }
    memcpy(map + 128, v4, sizeof(v4));
    pFlushMappedBufferRange(GL_ARRAY_BUFFER, 128, sizeof(v4));
    glUnmapBuffer(GL_ARRAY_BUFFER);
    glDrawArrays(GL_TRIANGLE_STRIP, (1024 + 128) / sizeof(Vertex), 4);

    /* 5: zwei Aufrufe hintereinander, wachsender Versatz (unsynchronisiert, wie wined3d) */
    for (int k = 0; k < 2; k++) {
        Vertex v5[4];
        uint8_t d[3] = { colors[5][0][0], (uint8_t)(colors[5][0][1] + k * 100), colors[5][0][2] };
        quad(v5, 5, d, colors[5][1]);
        if (k == 0) for (int i = 0; i < 4; i++) v5[i].pos[0] -= 0.1f; /* links halb verdeckt */
        GLintptr offset = 4096 + k * sizeof(v5);
        map = pMapBufferRange(GL_ARRAY_BUFFER, 0, 65536, GL_MAP_WRITE_BIT | GL_MAP_FLUSH_EXPLICIT_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
        memcpy(map + offset, v5, sizeof(v5));
        pFlushMappedBufferRange(GL_ARRAY_BUFFER, offset, sizeof(v5));
        glUnmapBuffer(GL_ARRAY_BUFFER);
        glDrawArrays(GL_TRIANGLE_STRIP, offset / sizeof(Vertex), 4);
    }

    /* 6: Diffusfarbe aus (feste Farbe), zeichnen; wieder an ohne neuen Zeiger, zeichnen (rechte Hälfte) */
    Vertex v6[8];
    quad(v6, 6, colors[6][0], colors[6][1]);
    quad(v6 + 4, 6, colors[6][1], colors[6][0]);
    v6[4].pos[0] += 0.2f; /* zweites Quadrat nur in der rechten Hälfte des Felds */
    v6[6].pos[0] += 0.2f;
    map = pMapBufferRange(GL_ARRAY_BUFFER, 0, 65536, GL_MAP_WRITE_BIT | GL_MAP_FLUSH_EXPLICIT_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
    memcpy(map + 8192, v6, sizeof(v6));
    pFlushMappedBufferRange(GL_ARRAY_BUFFER, 8192, sizeof(v6));
    glUnmapBuffer(GL_ARRAY_BUFFER);
    glDisableVertexAttribArray(5);
    glVertexAttrib4f(5, 0.0f, 1.0f, 0.5f, 1.0f);
    glDrawArrays(GL_TRIANGLE_STRIP, 8192 / sizeof(Vertex), 4);
    glEnableVertexAttribArray(5);
    glDrawArrays(GL_TRIANGLE_STRIP, 8192 / sizeof(Vertex) + 4, 4);

    /* 7: glMapBuffer(GL_WRITE_ONLY), ganzer Puffer neu */
    Vertex v7[4];
    quad(v7, 7, colors[7][0], colors[7][1]);
    map = glMapBuffer(GL_ARRAY_BUFFER, GL_WRITE_ONLY);
    memset(map, 0, 65536);
    memcpy(map + 2048, v7, sizeof(v7));
    glUnmapBuffer(GL_ARRAY_BUFFER);
    glDrawArrays(GL_TRIANGLE_STRIP, 2048 / sizeof(Vertex), 4);

    glFinish();
    unsigned char* px = malloc(W * H * 4);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glXSwapBuffers(dpy, win);
    printf("GL-Fehler: 0x%x\n", glGetError());
    for (int c = 0; c < 8; c++) {
        int x = (c % 4) * 64 + 20, y = (c < 4 ? 64 : 0) + 20;
        unsigned char* p = px + (y * W + x) * 4;
        printf("Feld %d: %3d %3d %3d\n", c, p[0], p[1], p[2]);
    }

    FILE* f = fopen(out, "wb");
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int y = H - 1; y >= 0; y--) for (int x = 0; x < W; x++) fwrite(px + (y * W + x) * 4, 1, 3, f);
    fclose(f);
    return 0;
}
