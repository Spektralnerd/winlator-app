/*
 * Test der Zustandsfilter und des Uniform-Wertevergleichs im Gladio-Client (patch-gladio-client.py):
 * Jede Zelle des Bilds entsteht nach einer Folge von Aufrufen, in der ein unveränderter Zustand noch
 * einmal gesetzt wird oder sich der Zustand hinter dem Rücken des Filters ändern könnte (glPopAttrib,
 * gelöschte und neu angelegte Objekte mit derselben Nummer, Werte hinter dem Ende eines Arrays, neu
 * gebundene Programme, Arrays des festen Funktionsmodells, Direktmodus, zweiter Kontext).
 *
 *   gcc -O1 -o statetest statetest.c -lX11 -lGL -ldl
 *   ./statetest out.ppm
 *
 * Das Bild muss mit GLADIO_NO_STATE_FILTER=1 GLADIO_NO_UNIFORM_DIFF=1 dasselbe sein wie ohne.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <X11/Xlib.h>

#define CELL 32
#define COLS 10
#define ROWS 5
#define W (CELL * COLS)
#define H (CELL * ROWS)

static Display* dpy;
static Window win;
static GLXContext ctx, ctx2;
static int cell = 0;

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

static const char* vsrc =
    "#version 120\n"
    "uniform mat4 m;\n"
    "attribute vec4 pos;\n"
    "attribute vec4 col;\n"
    "varying vec4 vcol;\n"
    "varying vec2 uv;\n"
    "void main() { gl_Position = m * pos; vcol = col; uv = pos.xy * 0.5 + 0.5; }\n";
static const char* fsrc =
    "#version 120\n"
    "uniform vec4 c[8];\n"
    "uniform vec4 d;\n"
    "uniform float f[4];\n"
    "uniform int sel;\n"
    "uniform int useTex;\n"
    "uniform sampler2D tex;\n"
    "varying vec4 vcol;\n"
    "varying vec2 uv;\n"
    "void main() {\n"
    "    vec4 color = c[sel] + d + vec4(f[0], f[1], f[2], f[3]) + vcol;\n"
    "    if (useTex == 1) color = texture2D(tex, uv);\n"
    "    gl_FragColor = color;\n"
    "}\n";

static GLuint makeProgram(void) {
    GLuint p = glCreateProgram();
    glAttachShader(p, compile(GL_VERTEX_SHADER, vsrc));
    glAttachShader(p, compile(GL_FRAGMENT_SHADER, fsrc));
    glBindAttribLocation(p, 0, "pos");
    glBindAttribLocation(p, 1, "col");
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p, sizeof(log), NULL, log);
        fprintf(stderr, "Link: %s\n", log);
        exit(5);
    }
    return p;
}

typedef struct Locs { GLint m, c, d, f, sel, useTex, tex; } Locs;
static Locs getLocs(GLuint p) {
    Locs l;
    l.m = glGetUniformLocation(p, "m");
    l.c = glGetUniformLocation(p, "c");
    l.d = glGetUniformLocation(p, "d");
    l.f = glGetUniformLocation(p, "f");
    l.sel = glGetUniformLocation(p, "sel");
    l.useTex = glGetUniformLocation(p, "useTex");
    l.tex = glGetUniformLocation(p, "tex");
    return l;
}

static const float identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
static const float half[16] = { 0.5f, 0, 0, 0, 0, 0.5f, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
static const float quad[] = { -1, -1, 0, 1,  1, -1, 0, 1,  1, 1, 0, 1,  -1, -1, 0, 1,  1, 1, 0, 1,  -1, 1, 0, 1 };
static const float quadHalf[] = { -1, -1, 0, 1,  0, -1, 0, 1,  0, 1, 0, 1,  -1, -1, 0, 1,  0, 1, 0, 1,  -1, 1, 0, 1 };

/* nächste Zelle: Viewport setzen (Scissor für glClear) */
static void nextCell(void) {
    int x = (cell % COLS) * CELL, y = (cell / COLS) * CELL;
    glViewport(x, y, CELL, CELL);
    glScissor(x, y, CELL, CELL);
    cell++;
}

static void draw(void) { glDrawArrays(GL_TRIANGLES, 0, 6); }

static GLuint makeTexture(unsigned char r, unsigned char g, unsigned char b) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    unsigned char px[2 * 2 * 4];
    for (int i = 0; i < 4; i++) {
        px[i * 4] = i == 0 ? r : r / 4; px[i * 4 + 1] = i == 0 ? g : g / 4; px[i * 4 + 2] = i == 0 ? b : b / 4; px[i * 4 + 3] = 255;
    }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return t;
}

int main(int argc, char** argv) {
    const char* out = argc > 1 ? argv[1] : "out.ppm";
    dpy = XOpenDisplay(NULL);
    if (!dpy) return 1;
    int screen = DefaultScreen(dpy);
    int attribs[] = { GLX_RGBA, GLX_DOUBLEBUFFER, GLX_DEPTH_SIZE, 16, None };
    XVisualInfo* vis = glXChooseVisual(dpy, screen, attribs);
    if (!vis) { puts("kein Visual"); return 2; }
    ctx = glXCreateContext(dpy, vis, NULL, True);
    ctx2 = glXCreateContext(dpy, vis, ctx, True);
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 0, 0, W, H, 0, 0, 0);
    XMapWindow(dpy, win);
    XSync(dpy, False);
    glXMakeCurrent(dpy, win, ctx);
    printf("%s / GL %s / GLSL %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION), glGetString(GL_SHADING_LANGUAGE_VERSION));
    glXSwapBuffers(dpy, win); /* leeres erstes Bild: die Aufzeichnung des Servers (GLADIO_PC_CAPTURE_AT=1) trifft dann das eigentliche */

    PFNGLBINDMULTITEXTUREEXTPROC pBindMultiTexture = proc("glBindMultiTextureEXT");
    PFNGLGENSAMPLERSPROC pGenSamplers = proc("glGenSamplers");
    PFNGLDELETESAMPLERSPROC pDeleteSamplers = proc("glDeleteSamplers");
    PFNGLBINDSAMPLERPROC pBindSampler = proc("glBindSampler");
    PFNGLSAMPLERPARAMETERIPROC pSamplerParameteri = proc("glSamplerParameteri");
    PFNGLGENVERTEXARRAYSPROC pGenVertexArrays = proc("glGenVertexArrays");
    PFNGLBINDVERTEXARRAYPROC pBindVertexArray = proc("glBindVertexArray");
    PFNGLENABLEIPROC pEnablei = proc("glEnablei");
    PFNGLDISABLEIPROC pDisablei = proc("glDisablei");
    PFNGLCOLORMASKIPROC pColorMaski = proc("glColorMaski");

    GLuint vboFull, vboHalf;
    glGenBuffers(1, &vboFull);
    glBindBuffer(GL_ARRAY_BUFFER, vboFull);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glGenBuffers(1, &vboHalf);
    glBindBuffer(GL_ARRAY_BUFFER, vboHalf);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadHalf), quadHalf, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, vboFull);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, (void*)0);
    glDisableVertexAttribArray(1);
    glVertexAttrib4f(1, 0, 0, 0, 0);

    GLuint prog = makeProgram();
    Locs l = getLocs(prog);
    glUseProgram(prog);
    glUniformMatrix4fv(l.m, 1, GL_FALSE, identity);
    float c[32];
    for (int i = 0; i < 8; i++) { c[i * 4] = i / 8.0f; c[i * 4 + 1] = 1.0f - i / 8.0f; c[i * 4 + 2] = (i & 1) ? 1.0f : 0.0f; c[i * 4 + 3] = 1.0f; }
    glUniform4fv(l.c, 8, c);
    float zero4[4] = { 0, 0, 0, 0 };
    glUniform4fv(l.d, 1, zero4);
    glUniform1fv(l.f, 4, zero4);
    glUniform1i(l.sel, 0);
    glUniform1i(l.useTex, 0);
    glUniform1i(l.tex, 0);

    glEnable(GL_SCISSOR_TEST);
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glViewport(0, 0, W, H);
    glScissor(0, 0, W, H);
    glClear(GL_COLOR_BUFFER_BIT);

    /* 0: Grundzustand c[0] */
    nextCell(); draw();
    /* 1: ganzes Array noch einmal mit geändertem c[3], sel 3 */
    nextCell();
    c[3 * 4] = 0.9f; c[3 * 4 + 1] = 0.2f;
    glUniform4fv(l.c, 8, c);
    glUniform1i(l.sel, 3);
    draw();
    /* 2: unverändertes Array noch einmal, sel noch einmal */
    nextCell();
    glUniform4fv(l.c, 8, c);
    glUniform1i(l.sel, 3);
    draw();
    /* 3: Werte hinter dem Ende des Arrays (c[6..9]): GL ignoriert sie, d bleibt, dann d wirklich setzen */
    nextCell();
    float d1[4] = { 0.0f, 0.0f, 0.5f, 0.0f }, d2[4] = { 0.5f, 0.0f, 0.0f, 0.0f };
    glUniform4fv(l.d, 1, d1);
    float spill[16] = { 0.2f, 0.2f, 0.2f, 1,  0.3f, 0.3f, 0.3f, 1,  d2[0], d2[1], d2[2], d2[3],  0.7f, 0.7f, 0.7f, 1 };
    glUniform4fv(l.c + 6, 4, spill);
    glUniform1i(l.sel, 6);
    draw();
    nextCell();
    glUniform4fv(l.d, 1, d2); /* muss ankommen, obwohl d2 schon hinter c[7] „geschickt“ wurde */
    glUniform1i(l.sel, 7);
    draw();
    glUniform4fv(l.d, 1, zero4);
    /* 5: Skalarform nach Vektorform und zurück */
    nextCell();
    glUniform4f(l.c + 2, 0.1f, 0.9f, 0.9f, 1.0f);
    glUniform1i(l.sel, 2);
    draw();
    nextCell();
    glUniform4fv(l.c, 8, c); /* c[2] alt: muss gesendet werden */
    draw();
    /* 7: f als einzelne Werte, dann als Array mit denselben Werten, dann anders */
    nextCell();
    glUniform1f(l.f + 1, 0.4f);
    draw();
    nextCell();
    float f4[4] = { 0.0f, 0.4f, 0.0f, 0.0f };
    glUniform1fv(l.f, 4, f4);
    f4[2] = 0.3f;
    glUniform1fv(l.f, 4, f4);
    draw();
    glUniform1fv(l.f, 4, zero4);
    /* 9: Matrix ändern und zurück */
    nextCell();
    glUniformMatrix4fv(l.m, 1, GL_FALSE, half);
    draw();
    nextCell();
    glUniformMatrix4fv(l.m, 1, GL_FALSE, identity);
    draw();
    /* 11: neu binden setzt alle Uniforms zurück: dieselben Werte müssen wieder hinaus */
    nextCell();
    glLinkProgram(prog);
    glUseProgram(prog);
    l = getLocs(prog);
    glUniformMatrix4fv(l.m, 1, GL_FALSE, identity);
    glUniform4fv(l.c, 8, c);
    glUniform4fv(l.d, 1, zero4);
    glUniform1fv(l.f, 4, zero4);
    glUniform1i(l.sel, 2);
    glUniform1i(l.useTex, 0);
    glUniform1i(l.tex, 0);
    draw();
    /* 12: zweites Programm mit denselben Namen, Werte je Programm */
    nextCell();
    GLuint prog2 = makeProgram();
    Locs l2 = getLocs(prog2);
    glUseProgram(prog2);
    glUniformMatrix4fv(l2.m, 1, GL_FALSE, identity);
    float c2[32];
    for (int i = 0; i < 32; i++) c2[i] = 1.0f - c[i];
    glUniform4fv(l2.c, 8, c2);
    glUniform4fv(l2.d, 1, zero4);
    glUniform1fv(l2.f, 4, zero4);
    glUniform1i(l2.sel, 1);
    glUniform1i(l2.useTex, 0);
    glUniform1i(l2.tex, 0);
    draw();
    nextCell();
    glUseProgram(prog);
    glUniform4fv(l.c, 8, c);
    glUniform1i(l.sel, 1);
    draw();
    /* 14: Programm löschen, neues (meist dieselbe Nummer) bekommt frische Uniforms */
    nextCell();
    glUseProgram(0);
    glDeleteProgram(prog2);
    GLuint prog3 = makeProgram();
    printf("Programme %u %u %u\n", prog, prog2, prog3);
    Locs l3 = getLocs(prog3);
    glUseProgram(prog3);
    glUniformMatrix4fv(l3.m, 1, GL_FALSE, identity);
    glUniform4fv(l3.c, 8, c2);
    glUniform4fv(l3.d, 1, zero4);
    glUniform1fv(l3.f, 4, zero4);
    glUniform1i(l3.sel, 5);
    glUniform1i(l3.useTex, 0);
    glUniform1i(l3.tex, 0);
    draw();
    glUseProgram(prog);

    /* 15: Blending: an, doppelt an, aus, Enablei/Disablei dazwischen */
    nextCell();
    glClearColor(0, 0, 0.5f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glVertexAttrib4f(1, 0, 0, 0, -0.5f); /* Alpha 0.5 */
    glUniform1i(l.sel, 0);
    draw();
    nextCell();
    glClear(GL_COLOR_BUFFER_BIT);
    pDisablei(GL_BLEND, 0);
    glEnable(GL_BLEND); /* nach Disablei muss das wieder hinaus */
    draw();
    nextCell();
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_BLEND);
    pEnablei(GL_BLEND, 0);
    pEnablei(GL_BLEND, 0);
    draw();
    nextCell();
    glClear(GL_COLOR_BUFFER_BIT);
    pDisablei(GL_BLEND, 0);
    draw();
    glDisable(GL_BLEND);
    glVertexAttrib4f(1, 0, 0, 0, 0);
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    /* 19: Farbmasken: ColorMask, ColorMaski, ColorMask (muss hinaus), ColorMaski wie zuvor (muss hinaus) */
    nextCell();
    glClear(GL_COLOR_BUFFER_BIT);
    glColorMask(1, 1, 1, 1);
    pColorMaski(0, 1, 0, 0, 1);
    draw();
    nextCell();
    glClear(GL_COLOR_BUFFER_BIT);
    glColorMask(1, 1, 1, 1);
    draw();
    nextCell();
    glClear(GL_COLOR_BUFFER_BIT);
    pColorMaski(0, 1, 0, 0, 1);
    draw();
    nextCell();
    glClear(GL_COLOR_BUFFER_BIT);
    pColorMaski(0, 0, 1, 0, 1);
    glColorMask(1, 1, 1, 1);
    glColorMask(1, 1, 1, 1);
    draw();
    /* 23: feste Attributwerte: 4f, 4Nubv, 4f wie zuvor (anderer Typ: muss hinaus), 1f dann 4f */
    nextCell();
    glVertexAttrib4f(1, 0.5f, 0, 0, 0);
    draw();
    nextCell();
    GLubyte nub[4] = { 0, 128, 0, 0 };
    glVertexAttrib4Nubv(1, nub);
    draw();
    nextCell();
    glVertexAttrib4f(1, 0.5f, 0, 0, 0);
    draw();
    nextCell();
    glVertexAttrib1f(1, 0.2f);
    glVertexAttrib4f(1, 0.5f, 0, 0, 0);
    draw();
    /* 27: glPushAttrib/glPopAttrib: Wert vor dem Push, anderer Wert, Pop (zurück), anderer Wert noch einmal (muss hinaus) */
    nextCell();
    glVertexAttrib4f(1, 0, 0, 0.5f, 0);
    glPushAttrib(GL_CURRENT_BIT | GL_COLOR_BUFFER_BIT | GL_ENABLE_BIT);
    glVertexAttrib4f(1, 0.5f, 0.5f, 0, 0);
    glPopAttrib();
    glVertexAttrib4f(1, 0.5f, 0.5f, 0, 0);
    draw();
    glVertexAttrib4f(1, 0, 0, 0, 0);
    nextCell();
    glPushAttrib(GL_COLOR_BUFFER_BIT | GL_ENABLE_BIT);
    glEnable(GL_BLEND);
    glColorMask(0, 1, 1, 1);
    glPopAttrib();
    glColorMask(0, 1, 1, 1);
    glEnable(GL_BLEND);
    glVertexAttrib4f(1, 0.5f, 0, 0, -0.5f);
    glClear(GL_COLOR_BUFFER_BIT);
    draw();
    glDisable(GL_BLEND);
    glColorMask(1, 1, 1, 1);
    glVertexAttrib4f(1, 0, 0, 0, 0);

    /* 29: Texturen: binden, doppelt binden, löschen, neue mit derselben Nummer, wieder binden */
    nextCell();
    glUniform1i(l.useTex, 1);
    GLuint t1 = makeTexture(255, 0, 0), t2 = makeTexture(0, 255, 0);
    glBindTexture(GL_TEXTURE_2D, t1);
    glBindTexture(GL_TEXTURE_2D, t1);
    draw();
    nextCell();
    glBindTexture(GL_TEXTURE_2D, t2);
    glBindTexture(GL_TEXTURE_2D, t2);
    draw();
    nextCell();
    glDeleteTextures(1, &t2);
    GLuint t3 = makeTexture(0, 0, 255); /* makeTexture bindet */
    printf("Texturen %u %u %u\n", t1, t2, t3);
    glBindTexture(GL_TEXTURE_2D, t3);
    draw();
    /* 32: Einheit 1 über glBindMultiTextureEXT, Sampler zeigt auf Einheit 1 */
    nextCell();
    pBindMultiTexture(GL_TEXTURE1, GL_TEXTURE_2D, t1);
    pBindMultiTexture(GL_TEXTURE1, GL_TEXTURE_2D, t1);
    glUniform1i(l.tex, 1);
    draw();
    nextCell();
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, t3);
    glActiveTexture(GL_TEXTURE0);
    draw();
    glUniform1i(l.tex, 0);
    /* 34: glPopAttrib(GL_TEXTURE_BIT) stellt die Bindung zurück; danach dieselbe Textur noch einmal binden */
    nextCell();
    glBindTexture(GL_TEXTURE_2D, t1);
    glPushAttrib(GL_TEXTURE_BIT);
    glBindTexture(GL_TEXTURE_2D, t3);
    glPopAttrib();
    glBindTexture(GL_TEXTURE_2D, t3);
    draw();
    /* 36: Sampler-Objekte: NEAREST, löschen, neuer (dieselbe Nummer) mit LINEAR, binden */
    nextCell();
    GLuint s1;
    pGenSamplers(1, &s1);
    pSamplerParameteri(s1, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    pSamplerParameteri(s1, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    pBindSampler(0, s1);
    pBindSampler(0, s1);
    draw();
    nextCell();
    pDeleteSamplers(1, &s1);
    GLuint s2;
    pGenSamplers(1, &s2);
    printf("Sampler %u %u\n", s1, s2);
    pSamplerParameteri(s2, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    pSamplerParameteri(s2, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    pBindSampler(0, s2);
    draw();
    pBindSampler(0, 0);
    glUniform1i(l.useTex, 0);

    /* 38: Zeiger: halbes Viereck, ganzes, ganzes noch einmal; Puffer löschen und neu anlegen (dieselbe Nummer) */
    nextCell();
    glBindBuffer(GL_ARRAY_BUFFER, vboHalf);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, (void*)0);
    draw();
    nextCell();
    glBindBuffer(GL_ARRAY_BUFFER, vboFull);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, (void*)0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, (void*)0);
    draw();
    nextCell();
    glDeleteBuffers(1, &vboHalf);
    GLuint vboNew;
    glGenBuffers(1, &vboNew);
    printf("Puffer %u %u %u\n", vboFull, vboHalf, vboNew);
    glBindBuffer(GL_ARRAY_BUFFER, vboNew);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadHalf), quadHalf, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, (void*)0);
    draw();
    nextCell();
    glDeleteBuffers(1, &vboNew);
    glGenBuffers(1, &vboNew);
    glBindBuffer(GL_ARRAY_BUFFER, vboNew);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, (void*)0); /* gleiche Nummer, gleiche Angaben, anderer Puffer */
    draw();
    /* 42: Vertex-Array-Objekte: A ganz, B halb, A, B, A noch einmal mit demselben Zeiger */
    nextCell();
    GLuint vaoA, vaoB;
    pGenVertexArrays(1, &vaoA);
    pGenVertexArrays(1, &vaoB);
    pBindVertexArray(vaoA);
    glEnableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, vboFull);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, (void*)0);
    pBindVertexArray(vaoB);
    glEnableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, vboNew);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadHalf), quadHalf, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, (void*)0);
    pBindVertexArray(vaoA);
    draw();
    nextCell();
    pBindVertexArray(vaoB);
    draw();
    nextCell();
    pBindVertexArray(vaoA);
    glBindBuffer(GL_ARRAY_BUFFER, vboFull);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, (void*)0);
    draw();
    pBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, vboFull);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, (void*)0);
    /* 45: Array des festen Funktionsmodells dazwischen, dann derselbe Zeiger noch einmal */
    nextCell();
    glEnableClientState(GL_VERTEX_ARRAY);
    glBindBuffer(GL_ARRAY_BUFFER, vboNew);
    glVertexPointer(4, GL_FLOAT, 0, (void*)0);
    glDisableClientState(GL_VERTEX_ARRAY);
    glBindBuffer(GL_ARRAY_BUFFER, vboFull);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, (void*)0);
    draw();
    /* 46: Direktmodus dazwischen */
    nextCell();
    glBegin(GL_TRIANGLES);
    glVertex3f(-0.5f, -0.5f, 0);
    glVertex3f(0.5f, -0.5f, 0);
    glVertex3f(0, 0.5f, 0);
    glEnd();
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, (void*)0);
    draw();
    /* 47: glPopClientAttrib */
    nextCell();
    glPushClientAttrib(GL_CLIENT_VERTEX_ARRAY_BIT);
    glBindBuffer(GL_ARRAY_BUFFER, vboNew);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, (void*)0);
    glPopClientAttrib();
    glBindBuffer(GL_ARRAY_BUFFER, vboNew);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, (void*)0);
    draw();
    glBindBuffer(GL_ARRAY_BUFFER, vboFull);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, (void*)0);
    /* 48: sel per glUniform1iv doppelt, dann anders */
    nextCell();
    GLint sel = 4;
    glUniform1iv(l.sel, 1, &sel);
    glUniform1iv(l.sel, 1, &sel);
    sel = 5;
    glUniform1iv(l.sel, 1, &sel);
    draw();
    /* 48: Zellenende */
    nextCell();
    glUniform1i(l.sel, 7);
    draw();
    glFinish();
    printf("glGetError %x, Zellen %d\n", glGetError(), cell);

    unsigned char* px = malloc(W * H * 4);
    glReadPixels(0, 0, W, H, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, px);
    for (int i = 0; i < cell; i++) {
        int x = (i % COLS) * CELL + CELL / 4, y = (i / COLS) * CELL + CELL / 2;
        unsigned char* p = px + (y * W + x) * 4;
        unsigned char* q = px + (y * W + x + CELL / 2) * 4;
        printf("Zelle %2d: links R%3d G%3d B%3d A%3d  rechts R%3d G%3d B%3d A%3d\n", i, p[2], p[1], p[0], p[3], q[2], q[1], q[0], q[3]);
    }
    FILE* f = fopen(out, "wb");
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int y = H - 1; y >= 0; y--)
        for (int x = 0; x < W; x++) {
            unsigned char* p = px + (y * W + x) * 4;
            fputc(p[2], f); fputc(p[1], f); fputc(p[0], f);
        }
    fclose(f);
    glXSwapBuffers(dpy, win);

    /* zweites Bild: Kontextwechsel (der Server legt den Anzeigepuffer neu an und verliert den Inhalt, darum getrennt) */
    glViewport(0, 0, W, H);
    glScissor(0, 0, W, H);
    glClear(GL_COLOR_BUFFER_BIT);
    /* 49: Löschen im zweiten Kontext (gemeinsame Objekte), neue Textur mit derselben Nummer, im ersten binden – zuletzt,
     * weil der Server beim Kontextwechsel den Fensterinhalt verliert (Anzeigepuffer wird neu angelegt) */
    nextCell();
    glXMakeCurrent(dpy, win, ctx2);
    glDeleteTextures(1, &t1);
    glXMakeCurrent(dpy, win, ctx);
    glUseProgram(prog);
    glUniform1i(l.useTex, 1);
    GLuint t4 = makeTexture(255, 255, 0);
    printf("Textur nach Löschen im zweiten Kontext %u (vorher %u)\n", t4, t1);
    glBindTexture(GL_TEXTURE_2D, t4);
    draw();
    glUniform1i(l.useTex, 0);

    glFinish();
    glReadPixels(0, 0, W, H, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, px);
    {
        int i = cell - 1;
        int x = (i % COLS) * CELL + CELL / 4, y = (i / COLS) * CELL + CELL / 2;
        unsigned char* p = px + (y * W + x) * 4;
        unsigned char* q = px + (y * W + x + CELL / 2) * 4;
        printf("Zelle %2d: links R%3d G%3d B%3d A%3d  rechts R%3d G%3d B%3d A%3d (zweites Bild)\n", i, p[2], p[1], p[0], p[3], q[2], q[1], q[0], q[3]);
    }
    glXSwapBuffers(dpy, win);
    return 0;
}
