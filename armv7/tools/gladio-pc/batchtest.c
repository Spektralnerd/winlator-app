/*
 * Zeichenstapel des Servers im Vergleich: Der Gladio-Server sammelt aufeinanderfolgende
 * Zeichenaufrufe zu einem Stapel, solange dazwischen nur Anfragen kommen, die den Zustand der
 * Zeichnung nicht ändern (Client-Arrays, aktuelle Farbe/Normale/Texturkoordinate, glBegin/glEnd,
 * im ES-2.0-Pfad auch glActiveTexture/glClientActiveTexture). Jeder Fall zeichnet zwei Quadrate
 * mit genau solchen Anfragen dazwischen; das Ergebnis muss dem direkten Zeichnen mit Mesa
 * entsprechen. 4x4 Felder je 128x128, Ergebnis als PPM (ppmdiff.py zum Vergleich).
 *
 *   0  Farb-Array, dazwischen glActiveTexture/glClientActiveTexture über alle Einheiten
 *      (Muster von Worms 3D), zweites Quadrat mit anderem Farb-Array
 *   1  erstes Quadrat mit Farb-Array, dann Farb-Array aus und glColor (magenta)
 *   2  ohne Farb-Array: glColor rot, Quadrat, glColor blau, Quadrat
 *   3  erst ohne Farb-Array (cyan), dann mit Farb-Array
 *   4  glBegin/glEnd mit Farbe je Vertex, danach Array-Quadrat ohne Farb-Array (übernimmt
 *      die letzte Farbe), danach glBegin/glEnd ohne glColor
 *   5  Textur: erst mit Texturkoordinaten-Array, dann ohne (feste Koordinate per
 *      glMultiTexCoord2f trifft ein Feld des Schachbretts)
 *   6  Beleuchtung: erst mit Normalen-Array, dann feste Normale per glNormal3f
 *   7  zwei Textureinheiten: Koordinaten-Arrays über glClientActiveTexture, zweites Quadrat
 *      mit anderem Array für Einheit 1
 *
 * Zweite Hälfte: Zustandsänderungen zwischen zwei Zeichenaufrufen, die der Server im ES-2.0-Pfad
 * erst beim nächsten Zeichenaufruf vergleicht (gleicher Zustand: der Stapel läuft weiter):
 *   8  Modelview: LoadIdentity, Licht setzen, wieder dieselbe Matrix (gleich) – dann andere Matrix
 *   9  Beleuchtung an zwischen zwei Quadraten, Licht und Material gesetzt
 *  10  Texturumgebung MODULATE -> REPLACE zwischen zwei Quadraten, dazwischen gleiche Werte
 *  11  PushMatrix/Translate/PopMatrix: drittes Quadrat wieder an der Stelle des ersten
 *  12  Alphatest: Funktion zwischen zwei Quadraten geändert (Alpha 0.4 je Vertex)
 *  13  glEnable(GL_BLEND) bei schon eingeschaltetem Blending, dann glDisable
 *  14  dieselbe Textur noch einmal gebunden, dann eine andere
 *  15  Material zwischen zwei beleuchteten Quadraten geändert und zurückgesetzt
 */
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 512
#define H 512
#define CELL 128

static void cell(int index)
{
    int x = (index % 4) * CELL, y = (3 - index / 4) * CELL;
    glViewport(x, y, CELL, CELL);
    glScissor(x, y, CELL, CELL);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, 4, 0, 4, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
}

/* Quadrat bei (x, y) mit Kantenlänge 1.6 als zwei Dreiecke über Indizes */
static float quadVerts[2][8];
static const unsigned short quadIdx[6] = {0, 1, 2, 0, 2, 3};

static float* quadAt(int slot, float x, float y)
{
    float* v = quadVerts[slot];
    v[0] = x; v[1] = y; v[2] = x + 1.6f; v[3] = y; v[4] = x + 1.6f; v[5] = y + 1.6f; v[6] = x; v[7] = y + 1.6f;
    return v;
}

static void drawQuad(int slot, float x, float y)
{
    glVertexPointer(2, GL_FLOAT, 0, quadAt(slot, x, y));
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, quadIdx);
}

static void cycleUnits(void)
{
    for (int i = 1; i < 8; i++) glActiveTexture(GL_TEXTURE0 + i);
    glActiveTexture(GL_TEXTURE0);
    for (int i = 1; i < 8; i++) glClientActiveTexture(GL_TEXTURE0 + i);
    glClientActiveTexture(GL_TEXTURE0);
}

static GLuint makeTexture(const unsigned char a[3], const unsigned char b[3])
{
    unsigned char px[4 * 4 * 3];
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) memcpy(px + (y * 4 + x) * 3, ((x + y) & 1) ? b : a, 3);
    GLuint id;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 4, 4, 0, GL_RGB, GL_UNSIGNED_BYTE, px);
    return id;
}

int main(int argc, char** argv)
{
    const char* out = argc > 1 ? argv[1] : "batchtest.ppm";
    int attribs[] = {GLX_RGBA, GLX_DOUBLEBUFFER, GLX_DEPTH_SIZE, 16, None};
    Display* dpy = XOpenDisplay(NULL);
    if (!dpy) return 1;
    int screen = XDefaultScreen(dpy);
    XVisualInfo* vis = glXChooseVisual(dpy, screen, attribs);
    if (!vis) { puts("kein Visual"); return 2; }
    GLXContext ctx = glXCreateContext(dpy, vis, NULL, True);
    Window win = XCreateSimpleWindow(dpy, XRootWindow(dpy, screen), 0, 0, W, H, 0, 0, 0);
    XMapWindow(dpy, win); XSync(dpy, False);
    glXMakeCurrent(dpy, win, ctx);
    printf("%s\n", glGetString(GL_RENDERER));

    static const unsigned char red[3] = {255, 0, 0}, white[3] = {255, 255, 255}, green[3] = {0, 200, 0}, blue[3] = {0, 0, 255};
    GLuint checker = makeTexture(red, white);
    GLuint stripes = makeTexture(green, blue);
    glBindTexture(GL_TEXTURE_2D, 0);

    static const float colorsA[16] = {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1, 1, 1, 0, 1};
    static const float colorsB[16] = {0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1};
    static const float texA[8] = {0, 0, 1, 0, 1, 1, 0, 1};
    static const float texB[8] = {0, 0, 0.5f, 0, 0.5f, 0.5f, 0, 0.5f};
    static const float normals[12] = {0, 0, 1, 0.7f, 0, 0.7f, 0, 0.7f, 0.7f, -0.7f, 0, 0.7f};

    glViewport(0, 0, W, H);
    glClearColor(0.1f, 0.1f, 0.15f, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    glEnableClientState(GL_VERTEX_ARRAY);

    /* 0: Farb-Arrays, dazwischen Einheiten durchschalten */
    cell(0);
    glEnableClientState(GL_COLOR_ARRAY);
    glColorPointer(4, GL_FLOAT, 0, colorsA);
    drawQuad(0, 0.2f, 0.2f);
    cycleUnits();
    glColor4f(1, 1, 1, 1);
    glColorPointer(4, GL_FLOAT, 0, colorsB);
    cycleUnits();
    drawQuad(1, 2.2f, 2.2f);
    glDisableClientState(GL_COLOR_ARRAY);

    /* 1: Farb-Array, dann aus und feste Farbe */
    cell(1);
    glEnableClientState(GL_COLOR_ARRAY);
    glColorPointer(4, GL_FLOAT, 0, colorsA);
    drawQuad(0, 0.2f, 0.2f);
    glDisableClientState(GL_COLOR_ARRAY);
    glColor4f(1, 0, 1, 1);
    drawQuad(1, 2.2f, 2.2f);

    /* 2: feste Farbe wechselt zwischen zwei Quadraten */
    cell(2);
    glColor4f(1, 0, 0, 1);
    drawQuad(0, 0.2f, 0.2f);
    glColor4f(0, 0, 1, 1);
    drawQuad(1, 2.2f, 2.2f);

    /* 3: erst feste Farbe, dann Farb-Array */
    cell(3);
    glColor4f(0, 1, 1, 1);
    drawQuad(0, 0.2f, 0.2f);
    glEnableClientState(GL_COLOR_ARRAY);
    glColorPointer(4, GL_FLOAT, 0, colorsB);
    drawQuad(1, 2.2f, 2.2f);
    glDisableClientState(GL_COLOR_ARRAY);

    /* 4: glBegin/glEnd mit Farben, Array ohne Farben, glBegin/glEnd ohne Farben */
    cell(4);
    glBegin(GL_QUADS);
    glColor3f(1, 0, 0); glVertex2f(0.2f, 0.2f);
    glColor3f(0, 1, 0); glVertex2f(1.4f, 0.2f);
    glColor3f(0, 0, 1); glVertex2f(1.4f, 1.4f);
    glColor3f(1, 1, 0); glVertex2f(0.2f, 1.4f);
    glEnd();
    drawQuad(1, 1.6f, 1.6f);
    glBegin(GL_TRIANGLES);
    glVertex2f(2.6f, 0.2f); glVertex2f(3.8f, 0.2f); glVertex2f(3.8f, 1.4f);
    glEnd();

    /* 5: Texturkoordinaten-Array, dann feste Koordinate */
    cell(5);
    glColor4f(1, 1, 1, 1);
    glBindTexture(GL_TEXTURE_2D, checker);
    glEnable(GL_TEXTURE_2D);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(2, GL_FLOAT, 0, texA);
    drawQuad(0, 0.2f, 0.2f);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glMultiTexCoord2f(GL_TEXTURE0, 0.375f, 0.125f);
    drawQuad(1, 2.2f, 2.2f);
    glDisable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);

    /* 6: Beleuchtung, Normalen-Array, dann feste Normale */
    cell(6);
    {
        const float dir[] = {0.3f, 0.2f, 1, 0}, dif[] = {0.9f, 0.7f, 0.2f, 1};
        glLightfv(GL_LIGHT0, GL_POSITION, dir);
        glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, dif);
        glEnable(GL_LIGHTING);
        glEnable(GL_LIGHT0);
        glEnableClientState(GL_NORMAL_ARRAY);
        glNormalPointer(GL_FLOAT, 0, normals);
        drawQuad(0, 0.2f, 0.2f);
        glDisableClientState(GL_NORMAL_ARRAY);
        glNormal3f(-0.6f, 0.0f, 0.8f);
        drawQuad(1, 2.2f, 2.2f);
        glDisable(GL_LIGHTING);
    }

    /* 7: zwei Textureinheiten, Koordinaten-Arrays über glClientActiveTexture */
    cell(7);
    glColor4f(1, 1, 1, 1);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, checker);
    glEnable(GL_TEXTURE_2D);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, stripes);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glClientActiveTexture(GL_TEXTURE0);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(2, GL_FLOAT, 0, texA);
    glClientActiveTexture(GL_TEXTURE1);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(2, GL_FLOAT, 0, texA);
    drawQuad(0, 0.2f, 0.2f);
    glTexCoordPointer(2, GL_FLOAT, 0, texB);
    glClientActiveTexture(GL_TEXTURE0);
    drawQuad(1, 2.2f, 2.2f);
    glClientActiveTexture(GL_TEXTURE1);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisable(GL_TEXTURE_2D);
    glActiveTexture(GL_TEXTURE0);
    glClientActiveTexture(GL_TEXTURE0);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisable(GL_TEXTURE_2D);

    /* 8: Modelview zurück auf dieselbe Matrix, dann eine andere */
    cell(8);
    glColor4f(1, 0.5f, 0, 1);
    {
        float m[16];
        glTranslatef(0.2f, 0.2f, 0);
        glGetFloatv(GL_MODELVIEW_MATRIX, m);
        drawQuad(0, 0, 0);
        glLoadIdentity();
        const float dir[] = {0, 0, 1, 0};
        glLightfv(GL_LIGHT0, GL_POSITION, dir);
        glLoadMatrixf(m);
        drawQuad(1, 0, 2);
        glLoadIdentity();
        glTranslatef(2.2f, 1.2f, 0);
        drawQuad(0, 0, 0);
        glLoadIdentity();
    }

    /* 9: Beleuchtung zwischen zwei Quadraten an */
    cell(9);
    {
        const float dir[] = {0.4f, 0.3f, 1, 0}, dif[] = {0.2f, 0.9f, 0.4f, 1};
        glColor4f(0.8f, 0.2f, 0.9f, 1);
        drawQuad(0, 0.2f, 0.2f);
        glLightfv(GL_LIGHT0, GL_POSITION, dir);
        glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, dif);
        glNormal3f(0, 0, 1);
        glEnable(GL_LIGHTING);
        glEnable(GL_LIGHT0);
        drawQuad(1, 2.2f, 2.2f);
        glDisable(GL_LIGHTING);
    }

    /* 10: Texturumgebung MODULATE -> REPLACE */
    cell(10);
    glBindTexture(GL_TEXTURE_2D, checker);
    glEnable(GL_TEXTURE_2D);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(2, GL_FLOAT, 0, texA);
    glColor4f(0, 0.5f, 1, 1);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    drawQuad(0, 0.2f, 0.2f);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    drawQuad(1, 2.2f, 0.2f);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    drawQuad(0, 1.2f, 2.2f);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisable(GL_TEXTURE_2D);

    /* 11: Push/Translate/Pop */
    cell(11);
    glColor4f(0.3f, 0.9f, 0.9f, 1);
    glTranslatef(0.2f, 0.2f, 0);
    drawQuad(0, 0, 0);
    glPushMatrix();
    glTranslatef(2, 2, 0);
    glColor4f(0.9f, 0.9f, 0.3f, 1);
    drawQuad(1, 0, 0);
    glPopMatrix();
    glColor4f(0.9f, 0.3f, 0.3f, 1);
    drawQuad(0, 0.5f, 0.5f);
    glLoadIdentity();

    /* 12: Alphatest-Funktion geändert */
    cell(12);
    {
        static const float alphaColors[16] = {1, 1, 1, 0.4f, 1, 1, 1, 0.4f, 1, 1, 1, 0.4f, 1, 1, 1, 0.4f};
        glEnableClientState(GL_COLOR_ARRAY);
        glColorPointer(4, GL_FLOAT, 0, alphaColors);
        glEnable(GL_ALPHA_TEST);
        glAlphaFunc(GL_GREATER, 0.5f);
        drawQuad(0, 0.2f, 0.2f);
        glAlphaFunc(GL_LESS, 0.5f);
        drawQuad(1, 2.2f, 2.2f);
        glDisable(GL_ALPHA_TEST);
        glDisableClientState(GL_COLOR_ARRAY);
    }

    /* 13: Blending schon an, wieder an, dann aus */
    cell(13);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(1, 1, 1, 0.5f);
    drawQuad(0, 0.2f, 0.2f);
    glEnable(GL_BLEND);
    drawQuad(1, 1.2f, 1.2f);
    glDisable(GL_BLEND);
    drawQuad(0, 2.2f, 2.2f);

    /* 14: dieselbe Textur noch einmal, dann eine andere */
    cell(14);
    glColor4f(1, 1, 1, 1);
    glBindTexture(GL_TEXTURE_2D, checker);
    glEnable(GL_TEXTURE_2D);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(2, GL_FLOAT, 0, texA);
    drawQuad(0, 0.2f, 0.2f);
    glBindTexture(GL_TEXTURE_2D, checker);
    drawQuad(1, 2.2f, 0.2f);
    glBindTexture(GL_TEXTURE_2D, stripes);
    drawQuad(0, 1.2f, 2.2f);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);

    /* 15: Material geändert und zurückgesetzt */
    cell(15);
    {
        const float dir[] = {0, 0, 1, 0}, red[] = {1, 0.2f, 0.2f, 1}, blue[] = {0.2f, 0.3f, 1, 1};
        glLightfv(GL_LIGHT0, GL_POSITION, dir);
        glNormal3f(0, 0, 1);
        glEnable(GL_LIGHTING);
        glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, red);
        drawQuad(0, 0.2f, 0.2f);
        glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, blue);
        glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, red);
        drawQuad(1, 2.2f, 0.2f);
        glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, blue);
        drawQuad(0, 1.2f, 2.2f);
        glDisable(GL_LIGHTING);
    }

    glFinish();
    printf("glGetError %u\n", glGetError());

    unsigned char* px = malloc(W * H * 4);
    glViewport(0, 0, W, H);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    FILE* f = fopen(out, "wb");
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int y = H - 1; y >= 0; y--)
        for (int x = 0; x < W; x++) fwrite(px + (y * W + x) * 4, 1, 3, f);
    fclose(f);
    return 0;
}
