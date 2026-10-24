/* Rendertest wie das Logo im SKIDROW-Intro: RGBA-Textur 1024x128 mit Mipmaps
 * (LINEAR_MIPMAP_LINEAR), Alphatest GL_LEQUAL 0.2 (Buchstaben haben Alpha 0, der Rest 1),
 * optional Licht und Nebel. Gibt einige Pixel aus und schreibt ein PPM. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct _XDisplay Display;
typedef unsigned long XID, Window, VisualID, Colormap;
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
void glXSwapBuffers(Display *, Window);
const unsigned char *glGetString(unsigned);

typedef unsigned int GLenum; typedef float GLfloat; typedef int GLint; typedef unsigned int GLuint;
void glViewport(int, int, int, int);
void glClearColor(float, float, float, float);
void glClear(unsigned);
void glEnable(GLenum); void glDisable(GLenum);
void glAlphaFunc(GLenum, float);
void glGenTextures(int, GLuint *); void glBindTexture(GLenum, GLuint);
void glTexImage2D(GLenum, GLint, GLint, int, int, GLint, GLenum, GLenum, const void *);
void glTexParameteri(GLenum, GLenum, GLint);
void glMatrixMode(GLenum); void glLoadIdentity(void);
void glOrtho(double, double, double, double, double, double);
void glBegin(GLenum); void glEnd(void);
void glTexCoord2f(float, float); void glVertex3f(float, float, float); void glNormal3f(float, float, float);
void glColor4f(float, float, float, float);
void glFogf(GLenum, float);
void glLightfv(GLenum, GLenum, const float *);
void glFinish(void);
void glBlendFunc(GLenum, GLenum);
void glReadPixels(int, int, int, int, GLenum, GLenum, void *);
GLenum glGetError(void);

#define TW 1024
#define TH 128
#define W 512
#define H 128

static unsigned char *level(int w, int h, const unsigned char *src, int sw, int sh)
{
    unsigned char *d = malloc((size_t)w * h * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            for (int c = 0; c < 4; c++) {
                int fx = sw / w, fy = sh / h, sum = 0;
                for (int j = 0; j < fy; j++)
                    for (int i = 0; i < fx; i++)
                        sum += src[((y * fy + j) * sw + (x * fx + i)) * 4 + c];
                d[(y * w + x) * 4 + c] = sum / (fx * fy);
            }
    return d;
}

int main(int argc, char **argv)
{
    int mode = argc > 1 ? atoi(argv[1]) : 0;   /* 0 nur Textur, 1 + Alphatest, 2 + Licht/Nebel */
    const char *out = argc > 2 ? argv[2] : "out.ppm";
    int filt = argc > 3 ? atoi(argv[3]) : 2;   /* 0 NEAREST ohne Mipmaps, 1 LINEAR ohne Mipmaps, 2 Mipmaps */
    int attribs[] = { 4, 5, 12, 16, 0 };       /* RGBA, DOUBLEBUFFER, DEPTH_SIZE 16 */
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) return 1;
    int screen = XDefaultScreen(dpy);
    XVisualInfo *vis = glXChooseVisual(dpy, screen, attribs);
    if (!vis) { puts("kein Visual"); return 2; }
    GLXContext ctx = glXCreateContext(dpy, vis, NULL, 1);
    Window win = XCreateSimpleWindow(dpy, XRootWindow(dpy, screen), 0, 0, W, H, 0, 0, 0);
    XMapWindow(dpy, win); XSync(dpy, 0);
    glXMakeCurrent(dpy, win, ctx);
    printf("%s, Modus %d, Filter %d\n", glGetString(0x1F01), mode, filt);

    /* Textur: Streifen von 64 Pixeln abwechselnd "Buchstabe" (orange, Alpha 0) und Hintergrund
     * (schwarz, Alpha 255) */
    unsigned char *tex = malloc(TW * TH * 4);
    for (int y = 0; y < TH; y++)
        for (int x = 0; x < TW; x++) {
            unsigned char *p = tex + (y * TW + x) * 4;
            int letter = ((x / 64) % 2) == 0 && y > 16 && y < TH - 16;
            p[0] = letter ? 240 : 0; p[1] = letter ? 120 : 0; p[2] = letter ? 20 : 0; p[3] = letter ? 0 : 255;
        }
    GLuint t; glGenTextures(1, &t); glBindTexture(0x0DE1, t);
    int w = TW, h = TH, lv = 0; unsigned char *cur = tex;
    for (;;) {
        glTexImage2D(0x0DE1, lv, 4, w, h, 0, 0x1908, 0x1401, cur);
        if (filt < 2 || (w == 1 && h == 1)) break;
        int nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
        unsigned char *n = level(nw, nh, cur, w, h);
        cur = n; w = nw; h = nh; lv++;
    }
    glTexParameteri(0x0DE1, 0x2801, filt == 0 ? 0x2600 : filt == 1 ? 0x2601 : 0x2703);
    glTexParameteri(0x0DE1, 0x2800, filt == 0 ? 0x2600 : 0x2601);

    glViewport(0, 0, W, H);
    glMatrixMode(0x1701); glLoadIdentity(); glOrtho(0, W, 0, H, -10, 10);
    glMatrixMode(0x1700); glLoadIdentity();
    glClearColor(0.0f, 0.6f, 0.0f, 1.0f);
    glClear(0x4100);
    if (mode >= 2) {
        float amb[] = { 0.3f, 0.3f, 0.3f, 1 }, dif[] = { 1, 1, 1, 1 }, pos[] = { 0, 0, 1, 0 };
        glLightfv(0x4000, 0x1200, amb); glLightfv(0x4000, 0x1201, dif); glLightfv(0x4000, 0x1203, pos);
        glEnable(0x0B50); glEnable(0x4000);          /* LIGHTING, LIGHT0 */
        if (!getenv("GLRENDER_NOFOG")) {
        glFogf(0x0B65, 0x2601); glFogf(0x0B62, 0.025f); glFogf(0x0B63, 100); glFogf(0x0B64, 400);
        glEnable(0x0B60);                            /* FOG */
        }
    }
    if (mode >= 1) { glEnable(0x0BC0); glAlphaFunc(0x0203, 0.2f); }   /* ALPHA_TEST, LEQUAL */
    glEnable(0x0DE1);
    if (getenv("GLRENDER_BLEND")) { glEnable(0x0BE2); glBlendFunc(0x0302, 0x0303); }   /* BLEND, SRC_ALPHA, ONE_MINUS_SRC_ALPHA */
    glColor4f(1, 1, 1, 1);
    if (mode == 3) {                                 /* viele Varianten: Tiefenfunktion wechseln */
        void glDepthFunc(GLenum);
        glEnable(0x0B71);                            /* DEPTH_TEST */
        for (int i = 0; i < 10; i++) {
            glDepthFunc(0x0200 + (i % 8));
            glBegin(7);
            glTexCoord2f(0, 0); glVertex3f(0, 0, 0);
            glTexCoord2f(1, 0); glVertex3f(W, 0, 0);
            glTexCoord2f(1, 1); glVertex3f(W, H, 0);
            glTexCoord2f(0, 1); glVertex3f(0, H, 0);
            glEnd();
            glFinish();
        }
    }
    /* GLRENDER_SECONDS=n: n Sekunden lang Bilder zeichnen (für das Stichproben-Profil) */
    const char *secs = getenv("GLRENDER_SECONDS");
    if (secs && atoi(secs) > 0) {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        int frames = 0;
        do {
            glClear(0x4100);
            glBegin(7);
            glTexCoord2f(0, 0); glVertex3f(0, 0, 0);
            glTexCoord2f(1, 0); glVertex3f(W, 0, 0);
            glTexCoord2f(1, 1); glVertex3f(W, H, 0);
            glTexCoord2f(0, 1); glVertex3f(0, H, 0);
            glEnd();
            glXSwapBuffers(dpy, win);
            frames++;
            clock_gettime(CLOCK_MONOTONIC, &t1);
        } while (t1.tv_sec - t0.tv_sec < atoi(secs));
        printf("%d Bilder in %s s\n", frames, secs);
    }
    glBegin(7);                                      /* QUADS */
    glNormal3f(0, 0, 1);
    glTexCoord2f(0, 0); glVertex3f(0, 0, 0);
    glTexCoord2f(1, 0); glVertex3f(W, 0, 0);
    glTexCoord2f(1, 1); glVertex3f(W, H, 0);
    glTexCoord2f(0, 1); glVertex3f(0, H, 0);
    glEnd();
    glFinish();
    printf("glGetError %u\n", glGetError());

    unsigned char *px = malloc(W * H * 4);
    glReadPixels(0, 0, W, H, 0x1908, 0x1401, px);
    int xs[] = { 16, 48, 80, 112, 300 };
    for (unsigned i = 0; i < sizeof(xs) / sizeof(*xs); i++) {
        unsigned char *p = px + ((H / 2) * W + xs[i]) * 4;
        printf("x=%d: %3d %3d %3d %3d\n", xs[i], p[0], p[1], p[2], p[3]);
    }
    FILE *f = fopen(out, "wb");
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int y = H - 1; y >= 0; y--)
        for (int x = 0; x < W; x++) fwrite(px + (y * W + x) * 4, 1, 3, f);
    fclose(f);
    return 0;
}
