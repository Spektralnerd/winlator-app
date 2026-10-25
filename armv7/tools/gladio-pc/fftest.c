/*
 * Festfunktions-Beleuchtung im Vergleich: acht Fälle in einem 4x2-Raster (je 128x128), mit
 * Client-Arrays und glDrawElements gezeichnet. Ergebnis als PPM; direkt mit Mesa (Referenz) und
 * über Gladio (run.sh --es 2|3) ausführen und mit ppmdiff.py vergleichen.
 *
 *   0  Richtungslicht unter gedrehter Modelview gesetzt, aktuelle Farbe rot (ohne
 *      GL_COLOR_MATERIAL bedeutungslos)
 *   1  wie 0 mit GL_COLOR_MATERIAL (Vorgabe GL_AMBIENT_AND_DIFFUSE): rot
 *   2  GL_COLOR_MATERIAL nur GL_DIFFUSE (grün), Material-Umgebung blau
 *   3  Spot (Punktlicht, Öffnung 20°, Exponent 0) mit linearer Abschwächung
 *   4  zweiseitige Beleuchtung: Vorder- und Rückseiten mit abgewandter Normale, Vorderseite
 *      cyan, Rückseite gelb (GL wählt nach der Orientierung des Dreiecks)
 *   5  GL_NORMALIZE mit glScalef(0.6)
 *   6  Material-Alpha 0.5 mit Blending über weißem Hintergrund
 *   7  Farb-Array je Vertex bei Beleuchtung ohne GL_COLOR_MATERIAL (bedeutungslos)
 *   Glanzlichter mit GL_LIGHT_MODEL_LOCAL_VIEWER (so rechnet auch Gladio).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct _XDisplay Display;
typedef unsigned long XID, Window, VisualID;
typedef struct { void *visual; VisualID visualid; int screen; int depth; int c_class;
                 unsigned long red_mask, green_mask, blue_mask; int colormap_size; int bits_per_rgb; } XVisualInfo;
typedef void *GLXContext;
Display *XOpenDisplay(const char *);
int XDefaultScreen(Display *);
Window XRootWindow(Display *, int);
Window XCreateSimpleWindow(Display *, Window, int, int, unsigned, unsigned, unsigned, unsigned long, unsigned long);
int XMapWindow(Display *, Window);
int XSync(Display *, int);
XVisualInfo *glXChooseVisual(Display *, int, int *);
GLXContext glXCreateContext(Display *, XVisualInfo *, GLXContext, int);
int glXMakeCurrent(Display *, Window, GLXContext);
const unsigned char *glGetString(unsigned);

typedef unsigned int GLenum; typedef float GLfloat; typedef int GLint; typedef unsigned int GLuint;
void glViewport(int, int, int, int);
void glScissor(int, int, int, int);
void glClearColor(float, float, float, float);
void glClear(unsigned);
void glEnable(GLenum); void glDisable(GLenum);
void glMatrixMode(GLenum); void glLoadIdentity(void);
void glFrustum(double, double, double, double, double, double);
void glOrtho(double, double, double, double, double, double);
void glTranslatef(float, float, float); void glRotatef(float, float, float, float); void glScalef(float, float, float);
void glLightfv(GLenum, GLenum, const float *); void glLightf(GLenum, GLenum, float);
void glLightModeli(GLenum, GLint); void glLightModelfv(GLenum, const float *);
void glMaterialfv(GLenum, GLenum, const float *); void glMaterialf(GLenum, GLenum, float);
void glColorMaterial(GLenum, GLenum);
void glColor4f(float, float, float, float);
void glBlendFunc(GLenum, GLenum);
void glEnableClientState(GLenum); void glDisableClientState(GLenum);
void glVertexPointer(int, GLenum, int, const void *);
void glNormalPointer(GLenum, int, const void *);
void glColorPointer(int, GLenum, int, const void *);
void glDrawElements(GLenum, int, GLenum, const void *);
void glFinish(void);
void glReadPixels(int, int, int, int, GLenum, GLenum, void *);
GLenum glGetError(void);

#define GL_LIGHTING 0x0B50
#define GL_LIGHT0 0x4000
#define GL_AMBIENT 0x1200
#define GL_DIFFUSE 0x1201
#define GL_SPECULAR 0x1202
#define GL_POSITION 0x1203
#define GL_SPOT_DIRECTION 0x1204
#define GL_SPOT_EXPONENT 0x1205
#define GL_SPOT_CUTOFF 0x1206
#define GL_LINEAR_ATTENUATION 0x1208
#define GL_EMISSION 0x1600
#define GL_SHININESS 0x1601
#define GL_AMBIENT_AND_DIFFUSE 0x1602
#define GL_FRONT 0x0404
#define GL_BACK 0x0405
#define GL_FRONT_AND_BACK 0x0408
#define GL_COLOR_MATERIAL 0x0B57
#define GL_LIGHT_MODEL_TWO_SIDE 0x0B52
#define GL_LIGHT_MODEL_LOCAL_VIEWER 0x0B51
#define GL_LIGHT_MODEL_AMBIENT 0x0B53
#define GL_NORMALIZE 0x0BA1
#define GL_DEPTH_TEST 0x0B71
#define GL_BLEND 0x0BE2
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_SCISSOR_TEST 0x0C11
#define GL_MODELVIEW 0x1700
#define GL_PROJECTION 0x1701
#define GL_VERTEX_ARRAY 0x8074
#define GL_NORMAL_ARRAY 0x8075
#define GL_COLOR_ARRAY 0x8076
#define GL_FLOAT 0x1406
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_SHORT 0x1403
#define GL_TRIANGLES 0x0004
#define GL_RGBA 0x1908

#define W 512
#define H 256
#define CELL 128
#define SLICES 24
#define STACKS 16

static float sphereVertices[(SLICES + 1) * (STACKS + 1) * 3];
static float sphereNormals[(SLICES + 1) * (STACKS + 1) * 3];
static unsigned char sphereColors[(SLICES + 1) * (STACKS + 1) * 4];
static unsigned short sphereIndices[SLICES * STACKS * 6];

static void buildSphere(void)
{
    int v = 0;
    for (int st = 0; st <= STACKS; st++) {
        float phi = (float)M_PI * st / STACKS;
        for (int sl = 0; sl <= SLICES; sl++, v++) {
            float theta = 2.0f * (float)M_PI * sl / SLICES;
            float x = sinf(phi) * cosf(theta), y = cosf(phi), z = sinf(phi) * sinf(theta);
            sphereVertices[v * 3] = x; sphereVertices[v * 3 + 1] = y; sphereVertices[v * 3 + 2] = z;
            sphereNormals[v * 3] = x; sphereNormals[v * 3 + 1] = y; sphereNormals[v * 3 + 2] = z;
            sphereColors[v * 4] = (unsigned char)(255 * sl / SLICES);
            sphereColors[v * 4 + 1] = (unsigned char)(255 * st / STACKS);
            sphereColors[v * 4 + 2] = 40;
            sphereColors[v * 4 + 3] = 255;
        }
    }
    int i = 0;
    for (int st = 0; st < STACKS; st++)
        for (int sl = 0; sl < SLICES; sl++) {
            unsigned short a = st * (SLICES + 1) + sl, b = a + SLICES + 1;
            sphereIndices[i++] = a; sphereIndices[i++] = b; sphereIndices[i++] = a + 1;
            sphereIndices[i++] = a + 1; sphereIndices[i++] = b; sphereIndices[i++] = b + 1;
        }
}

static void drawSphere(void)
{
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_NORMAL_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, sphereVertices);
    glNormalPointer(GL_FLOAT, 0, sphereNormals);
    glDrawElements(GL_TRIANGLES, SLICES * STACKS * 6, GL_UNSIGNED_SHORT, sphereIndices);
    glDisableClientState(GL_NORMAL_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
}

static void cell(int index)
{
    int x = (index % 4) * CELL, y = (1 - index / 4) * CELL;
    glViewport(x, y, CELL, CELL);
    glScissor(x, y, CELL, CELL);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glFrustum(-0.5, 0.5, -0.5, 0.5, 1.0, 10.0);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
}

static void defaults(void)
{
    const float white[] = {1, 1, 1, 1}, black[] = {0, 0, 0, 1}, amb[] = {0.2f, 0.2f, 0.2f, 1}, dif[] = {0.8f, 0.8f, 0.8f, 1};
    glDisable(GL_COLOR_MATERIAL); glDisable(GL_BLEND); glDisable(GL_NORMALIZE);
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, 0);
    glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, amb);
    glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, dif);
    glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, black);
    glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, black);
    glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, 0);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, white);
    glLightfv(GL_LIGHT0, GL_SPECULAR, white);
    glLightf(GL_LIGHT0, GL_SPOT_CUTOFF, 180);
    glLightf(GL_LIGHT0, GL_SPOT_EXPONENT, 0);
    glLightf(GL_LIGHT0, GL_LINEAR_ATTENUATION, 0);
    glColor4f(1, 1, 1, 1);
}

static void directionalLightRotated(void)
{
    /* Licht aus (1,1,1)-Richtung, gesetzt unter einer um 60° um Y gedrehten Modelview */
    const float dir[] = {1, 1, 1, 0};
    glLoadIdentity(); glRotatef(60, 0, 1, 0);
    glLightfv(GL_LIGHT0, GL_POSITION, dir);
    glLoadIdentity();
}

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : "fftest.ppm";
    int attribs[] = { 4, 5, 12, 16, 0 };
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) return 1;
    int screen = XDefaultScreen(dpy);
    XVisualInfo *vis = glXChooseVisual(dpy, screen, attribs);
    if (!vis) { puts("kein Visual"); return 2; }
    GLXContext ctx = glXCreateContext(dpy, vis, NULL, 1);
    Window win = XCreateSimpleWindow(dpy, XRootWindow(dpy, screen), 0, 0, W, H, 0, 0, 0);
    XMapWindow(dpy, win); XSync(dpy, 0);
    glXMakeCurrent(dpy, win, ctx);
    printf("%s\n", glGetString(0x1F01));
    buildSphere();

    glViewport(0, 0, W, H);
    glClearColor(0.1f, 0.1f, 0.15f, 1);
    glClear(0x4100);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_SCISSOR_TEST);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glLightModeli(GL_LIGHT_MODEL_LOCAL_VIEWER, 1);

    /* 0: Richtungslicht unter gedrehter Modelview, Farbe rot ohne Bedeutung */
    cell(0); defaults(); directionalLightRotated();
    glColor4f(1, 0, 0, 1);
    glTranslatef(0, 0, -3); drawSphere();

    /* 1: GL_COLOR_MATERIAL (GL_AMBIENT_AND_DIFFUSE): rot */
    cell(1); defaults(); directionalLightRotated();
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
    glEnable(GL_COLOR_MATERIAL);
    glColor4f(1, 0, 0, 1);
    glTranslatef(0, 0, -3); drawSphere();
    glDisable(GL_COLOR_MATERIAL);

    /* 2: GL_COLOR_MATERIAL nur diffus (grün), Umgebung des Materials blau, Glanzlicht */
    cell(2); defaults(); directionalLightRotated();
    {
        const float blue[] = {0, 0, 1, 1}, spec[] = {1, 1, 1, 1}, lamb[] = {0.4f, 0.4f, 0.4f, 1};
        glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, blue);
        glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, spec);
        glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, 20);
        glLightModelfv(GL_LIGHT_MODEL_AMBIENT, lamb);
        glColorMaterial(GL_FRONT_AND_BACK, GL_DIFFUSE);
        glEnable(GL_COLOR_MATERIAL);
        glColor4f(0, 1, 0, 1);
        glTranslatef(0, 0, -3); drawSphere();
        glDisable(GL_COLOR_MATERIAL);
        const float lamb0[] = {0.2f, 0.2f, 0.2f, 1};
        glLightModelfv(GL_LIGHT_MODEL_AMBIENT, lamb0);
    }

    /* 3: Spot mit Abschwächung */
    cell(3); defaults();
    {
        const float pos[] = {0, 0, 0, 1}, dir[] = {0, 0, -1};
        glLoadIdentity();
        glLightfv(GL_LIGHT0, GL_POSITION, pos);
        glLightfv(GL_LIGHT0, GL_SPOT_DIRECTION, dir);
        glLightf(GL_LIGHT0, GL_SPOT_CUTOFF, 12);
        glLightf(GL_LIGHT0, GL_LINEAR_ATTENUATION, 0.2f);
        glTranslatef(0, 0, -3); drawSphere();
    }

    /* 4: zweiseitig, Fläche von hinten (Normalen zeigen vom Betrachter weg) */
    cell(4); defaults();
    {
        const float dir[] = {0, 0, 1, 0}, cyan[] = {0, 1, 1, 1}, yellow[] = {1, 1, 0, 1};
        /* links: Vorderseite (gegen den Uhrzeigersinn) mit abgewandter Normale -> Vordermaterial, dunkel;
         * rechts: Rückseite (im Uhrzeigersinn) mit abgewandter Normale -> Rückmaterial, beleuchtet */
        static float quad[] = {-1, -1, 0, -0.05f, -1, 0, -0.05f, 1, 0, -1, 1, 0,
                               0.05f, -1, 0, 1, -1, 0, 1, 1, 0, 0.05f, 1, 0};
        static float normals[] = {0, 0, -1, 0, 0, -1, 0, 0, -1, 0, 0, -1, 0, 0, -1, 0, 0, -1, 0, 0, -1, 0, 0, -1};
        static unsigned short idx[] = {0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6};
        glLoadIdentity(); glLightfv(GL_LIGHT0, GL_POSITION, dir);
        glMaterialfv(GL_FRONT, GL_DIFFUSE, cyan);
        glMaterialfv(GL_BACK, GL_DIFFUSE, yellow);
        glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, 1);
        glTranslatef(0, 0, -3); glScalef(0.8f, 0.8f, 0.8f);
        glEnableClientState(GL_VERTEX_ARRAY); glEnableClientState(GL_NORMAL_ARRAY);
        glVertexPointer(3, GL_FLOAT, 0, quad); glNormalPointer(GL_FLOAT, 0, normals);
        glDrawElements(GL_TRIANGLES, 12, GL_UNSIGNED_SHORT, idx);
        glDisableClientState(GL_NORMAL_ARRAY); glDisableClientState(GL_VERTEX_ARRAY);
        glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, 0);
    }

    /* 5: GL_NORMALIZE mit Skalierung */
    cell(5); defaults(); directionalLightRotated();
    glEnable(GL_NORMALIZE);
    glTranslatef(0, 0, -3); glScalef(0.6f, 0.6f, 0.6f); drawSphere();
    glDisable(GL_NORMALIZE);

    /* 6: Material-Alpha 0.5 mit Blending über weißem Hintergrund */
    cell(6); defaults(); directionalLightRotated();
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_SCISSOR_TEST);
    glClearColor(1, 1, 1, 1); glClear(0x4100); glClearColor(0.1f, 0.1f, 0.15f, 1);
    {
        const float dif[] = {0.8f, 0.3f, 0.3f, 0.5f};
        glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, dif);
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glTranslatef(0, 0, -3); drawSphere();
        glDisable(GL_BLEND);
    }

    /* 7: Farb-Array bei Beleuchtung ohne GL_COLOR_MATERIAL */
    cell(7); defaults(); directionalLightRotated();
    glEnableClientState(GL_COLOR_ARRAY);
    glColorPointer(4, GL_UNSIGNED_BYTE, 0, sphereColors);
    glTranslatef(0, 0, -3); drawSphere();
    glDisableClientState(GL_COLOR_ARRAY);

    glFinish();
    printf("glGetError %u\n", glGetError());

    unsigned char *px = malloc(W * H * 4);
    glViewport(0, 0, W, H);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (int c = 0; c < 8; c++) {
        int x = (c % 4) * CELL + CELL / 2, y = (1 - c / 4) * CELL + CELL / 2;
        unsigned char *p = px + (y * W + x) * 4;
        printf("Fall %d Mitte: %3d %3d %3d\n", c, p[0], p[1], p[2]);
    }
    FILE *f = fopen(out, "wb");
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int y = H - 1; y >= 0; y--)
        for (int x = 0; x < W; x++) fwrite(px + (y * W + x) * 4, 1, 3, f);
    fclose(f);
    return 0;
}
