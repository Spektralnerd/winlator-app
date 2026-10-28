/*
 * Zustandsabfragen über Gladio: Werte, Typen und Antwortgrößen von glGetIntegerv/Floatv/Booleanv.
 *
 * Jede Antwort landet in einem mit 0x5a vorbelegten Feld; danach steht, wie viele Bytes die Antwort
 * belegt hat (Server-Antworten mit falscher Größe überschreiben das Feld nur teilweise oder zu weit).
 * Geprüft werden außerdem der Pixelspeicher in der Reihenfolge von GLU (retrieveStoreModes vor
 * gluBuild2DMipmaps: unter ES 2.0 lieferte der Server dort den uninitialisierten Antwortpuffer),
 * Proxy-Texturen (closestFit in GLU) und glIsEnabled für Fähigkeiten, die ES nicht kennt.
 *
 *   ./gettest > ref.txt;  run.sh --es 2 -- ./gettest > es2.txt
 */
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <stdio.h>
#include <string.h>

#define FILL 0x5a

/* Zahl der Bytes ab dem Feldanfang bis zum letzten veränderten Byte */
static int usedBytes(const unsigned char* data, int size)
{
    int used = 0;
    for (int i = 0; i < size; i++) if (data[i] != FILL) used = i + 1;
    return used;
}

static void geti(const char* name, GLenum pname, int count)
{
    union { GLint v[64]; unsigned char b[256]; } u;
    memset(&u, FILL, sizeof(u));
    glGetIntegerv(pname, u.v);
    printf("I %-28s", name);
    for (int i = 0; i < count; i++) printf(" %d", u.v[i]);
    printf("   [%d Byte]\n", usedBytes(u.b, sizeof(u)));
}

static void getf(const char* name, GLenum pname, int count)
{
    union { GLfloat v[64]; unsigned char b[256]; } u;
    memset(&u, FILL, sizeof(u));
    glGetFloatv(pname, u.v);
    printf("F %-28s", name);
    for (int i = 0; i < count; i++) printf(" %.6g", u.v[i]);
    printf("   [%d Byte]\n", usedBytes(u.b, sizeof(u)));
}

static void getb(const char* name, GLenum pname, int count)
{
    union { GLboolean v[256]; unsigned char b[256]; } u;
    memset(&u, FILL, sizeof(u));
    glGetBooleanv(pname, u.v);
    printf("B %-28s", name);
    for (int i = 0; i < count; i++) printf(" %d", u.v[i]);
    printf("   [%d Byte]\n", usedBytes(u.b, sizeof(u)));
}

/* wie retrieveStoreModes in GLU (mipmap.c) */
static void storeModes(const char* what)
{
    static const struct { const char* name; GLenum pname; } modes[] = {
        {"UNPACK_ALIGNMENT", GL_UNPACK_ALIGNMENT}, {"UNPACK_ROW_LENGTH", GL_UNPACK_ROW_LENGTH},
        {"UNPACK_SKIP_ROWS", GL_UNPACK_SKIP_ROWS}, {"UNPACK_SKIP_PIXELS", GL_UNPACK_SKIP_PIXELS},
        {"UNPACK_LSB_FIRST", GL_UNPACK_LSB_FIRST}, {"UNPACK_SWAP_BYTES", GL_UNPACK_SWAP_BYTES},
        {"PACK_ALIGNMENT", GL_PACK_ALIGNMENT}, {"PACK_ROW_LENGTH", GL_PACK_ROW_LENGTH},
        {"PACK_SKIP_ROWS", GL_PACK_SKIP_ROWS}, {"PACK_SKIP_PIXELS", GL_PACK_SKIP_PIXELS},
        {"PACK_LSB_FIRST", GL_PACK_LSB_FIRST}, {"PACK_SWAP_BYTES", GL_PACK_SWAP_BYTES},
    };
    printf("-- Pixelspeicher %s\n", what);
    for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) geti(modes[i].name, modes[i].pname, 1);
}

int main(void)
{
    int attribs[] = {GLX_RGBA, GLX_DOUBLEBUFFER, GLX_DEPTH_SIZE, 16, None};
    Display* dpy = XOpenDisplay(NULL);
    if (!dpy) return 1;
    int screen = XDefaultScreen(dpy);
    XVisualInfo* vis = glXChooseVisual(dpy, screen, attribs);
    if (!vis) return 2;
    GLXContext ctx = glXCreateContext(dpy, vis, NULL, True);
    Window win = XCreateSimpleWindow(dpy, XRootWindow(dpy, screen), 0, 0, 64, 64, 0, 0, 0);
    XMapWindow(dpy, win); XSync(dpy, False);
    glXMakeCurrent(dpy, win, ctx);

    storeModes("Anfang");
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 7);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 2);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 3);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, -5); /* ungültig: bleibt 7 */
    storeModes("nach ROW_LENGTH 7, SKIP_ROWS 2, SKIP_PIXELS 3");
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);

    printf("-- Typen und Größen\n");
    glEnable(GL_LIGHTING);
    geti("LIGHTING", GL_LIGHTING, 1);
    getf("LIGHTING", GL_LIGHTING, 1);
    getb("LIGHTING", GL_LIGHTING, 1);
    glDisable(GL_LIGHTING);
    glFogf(GL_FOG_DENSITY, 2.6f);
    glFogi(GL_FOG_MODE, GL_LINEAR);
    geti("FOG_DENSITY", GL_FOG_DENSITY, 1);
    getf("FOG_DENSITY", GL_FOG_DENSITY, 1);
    geti("FOG_MODE", GL_FOG_MODE, 1);
    getf("FOG_MODE", GL_FOG_MODE, 1);
    getb("MAX_LIGHTS", GL_MAX_LIGHTS, 1);
    geti("MAX_LIGHTS", GL_MAX_LIGHTS, 1);
    geti("MAX_VIEWPORT_DIMS", GL_MAX_VIEWPORT_DIMS, 2);
    geti("MAX_TEXTURE_SIZE", GL_MAX_TEXTURE_SIZE, 1);
    glColor4f(1.0f, 0.5f, 0.0f, -1.0f);
    getf("CURRENT_COLOR", GL_CURRENT_COLOR, 4);
    geti("CURRENT_COLOR", GL_CURRENT_COLOR, 4);
    getb("CURRENT_COLOR", GL_CURRENT_COLOR, 4);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    geti("BLEND_SRC", GL_BLEND_SRC, 1);
    geti("BLEND_DST", GL_BLEND_DST, 1);
    geti("VIEWPORT", GL_VIEWPORT, 4);
    getb("COLOR_WRITEMASK", GL_COLOR_WRITEMASK, 4);
    geti("COLOR_WRITEMASK", GL_COLOR_WRITEMASK, 4);
    getb("MODELVIEW_MATRIX", GL_MODELVIEW_MATRIX, 16);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    geti("MODELVIEW_STACK_DEPTH", GL_MODELVIEW_STACK_DEPTH, 1);
    glPopMatrix();
    geti("MODELVIEW_STACK_DEPTH", GL_MODELVIEW_STACK_DEPTH, 1);
    glActiveTexture(GL_TEXTURE1);
    geti("ACTIVE_TEXTURE", GL_ACTIVE_TEXTURE, 1);
    glActiveTexture(GL_TEXTURE0);
    printf("B IsEnabled(CLIP_PLANE0)       %d\n", glIsEnabled(GL_CLIP_PLANE0));
    printf("B IsEnabled(LIGHT0)            %d\n", glIsEnabled(GL_LIGHT0));

    printf("-- Proxy-Texturen (GLU closestFit)\n");
    GLint w = -1, h = -1;
    glTexImage2D(GL_PROXY_TEXTURE_2D, 1, GL_RGB, 128, 64, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
    glGetTexLevelParameteriv(GL_PROXY_TEXTURE_2D, 1, GL_TEXTURE_WIDTH, &w);
    glGetTexLevelParameteriv(GL_PROXY_TEXTURE_2D, 1, GL_TEXTURE_HEIGHT, &h);
    printf("P 2D Stufe 1 128x64              %d x %d\n", w, h);
    w = h = -1;
    glTexImage2D(GL_PROXY_TEXTURE_2D, 1, GL_RGBA, 65536, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGetTexLevelParameteriv(GL_PROXY_TEXTURE_2D, 1, GL_TEXTURE_WIDTH, &w);
    printf("P 2D Stufe 1 65536x64 (zu groß)  %d\n", w);
    w = -1;
    glGetTexLevelParameteriv(GL_PROXY_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
    printf("P 2D Stufe 0 (nie angefragt)     %d\n", w);

    /* eine echte Textur danach: gebunden und abfragbar */
    GLuint tex;
    unsigned char pixels[16 * 8 * 3];
    memset(pixels, 0x80, sizeof(pixels));
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 16, 8, 0, GL_RGB, GL_UNSIGNED_BYTE, pixels);
    w = h = -1;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
    printf("T 2D Stufe 0                     %d x %d\n", w, h);
    geti("TEXTURE_BINDING_2D", GL_TEXTURE_BINDING_2D, 1);
    printf("E glGetError                     0x%x\n", glGetError());

    glXMakeCurrent(dpy, None, NULL);
    glXDestroyContext(dpy, ctx);
    XCloseDisplay(dpy);
    return 0;
}
