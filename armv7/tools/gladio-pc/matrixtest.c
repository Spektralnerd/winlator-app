/*
 * Matrix- und Zustandsabfragen im Vergleich: Der armv7-Client führt Matrixstapel, Matrixmodus und
 * Tiefenmaske selbst mit und beantwortet glGetFloatv/glGetDoublev(GL_*_MATRIX),
 * glGetIntegerv(GL_MATRIX_MODE) und glGetBooleanv(GL_DEPTH_WRITEMASK) ohne den Server
 * (patch-gladio-client.py). Das Programm macht eine Folge von Matrixoperationen und gibt nach
 * jedem Schritt die abgefragten Werte aus; direkt mit Mesa und über Gladio ausführen und die
 * Ausgaben vergleichen (Abweichung der Matrizen höchstens Rundung).
 *
 *   ./matrixtest > ref.txt;  run.sh --es 2 -- ./matrixtest > es2.txt;  diff/Vergleich
 */
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <stdio.h>

static int step = 0;

static void dump(const char* what)
{
    float mv[16], pr[16], tx[16];
    double mvd[16];
    GLint mode = 0;
    GLboolean depthMask = 0;
    glGetFloatv(GL_MODELVIEW_MATRIX, mv);
    glGetFloatv(GL_PROJECTION_MATRIX, pr);
    glGetFloatv(GL_TEXTURE_MATRIX, tx);
    glGetDoublev(GL_MODELVIEW_MATRIX, mvd);
    glGetIntegerv(GL_MATRIX_MODE, &mode);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
    printf("%02d %-28s mode=%x depthMask=%d\n", step++, what, mode, depthMask);
    printf("   MV"); for (int i = 0; i < 16; i++) printf(" %.6g", mv[i]); printf("\n");
    printf("   MVd"); for (int i = 0; i < 16; i++) printf(" %.6g", (float)mvd[i]); printf("\n");
    printf("   PR"); for (int i = 0; i < 16; i++) printf(" %.6g", pr[i]); printf("\n");
    printf("   TX"); for (int i = 0; i < 16; i++) printf(" %.6g", tx[i]); printf("\n");
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

    const float m[16] = {1, 2, 0, 0, 0, 1, 3, 0, 0.5f, 0, 1, 0, 4, 5, 6, 1};

    dump("Anfang");
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-1, 1, -0.75, 0.75, 1, 100);
    dump("Frustum");
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, 640, 480, 0, -1, 1);
    dump("Ortho (Push)");
    glPopMatrix();
    dump("Pop Projektion");
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(1.5f, -2, -10);
    glRotatef(33, 0.2f, 1, 0.1f);
    glScalef(2, 0.5f, 1.25f);
    dump("Translate/Rotate/Scale");
    glPushMatrix();
    glMultMatrixf(m);
    dump("MultMatrix (Push)");
    glPushMatrix();
    glLoadTransposeMatrixf(m);
    dump("LoadTranspose (Push)");
    glMultTransposeMatrixf(m);
    dump("MultTranspose");
    glPopMatrix();
    glPopMatrix();
    dump("2x Pop Modelview");
    glMatrixMode(GL_TEXTURE);
    glTranslatef(0.5f, 0.25f, 0);
    glScalef(2, 2, 1);
    dump("Texturmatrix");
    glMatrixMode(GL_MODELVIEW);
    glRotated(-15, 1, 0, 0);
    glTranslated(0.25, 0.5, 0.75);
    dump("Rotated/Translated");
    glDepthMask(GL_FALSE);
    dump("DepthMask aus");
    glPushAttrib(GL_DEPTH_BUFFER_BIT);
    glDepthMask(GL_TRUE);
    dump("DepthMask an (PushAttrib)");
    glPopAttrib();
    dump("PopAttrib");
    /* tiefe Stapel bis über die Grenze (Server: 32 Einträge) */
    for (int i = 0; i < 40; i++) {
        glPushMatrix();
        glTranslatef(1, 0, 0);
    }
    dump("40x Push+Translate");
    for (int i = 0; i < 45; i++) glPopMatrix();
    dump("45x Pop");
    glXSwapBuffers(dpy, win);
    dump("nach SwapBuffers");
    glFinish();
    return 0;
}
