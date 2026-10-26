/*
 * glReadPixels im Vergleich: Fenster auf eine bekannte Farbe löschen und zurücklesen, je Fall das erste
 * Pixel als Bytes ausgeben; direkt mit Mesa und über Gladio ausführen und die Ausgaben vergleichen.
 *   1  GL_BGRA in Client-Speicher
 *   2  derselbe Aufruf noch einmal (kommt beim Server aus dem Lesecache)
 *   3  GL_RGBA gleich danach (gleiche Größe und Stelle, anderes Format)
 *   4  GL_RGBA in einen Pack-Puffer, danach glMapBuffer (vorher: Server-Absturz bzw. alte Daten)
 *   5  wie 4 mit einem Rundlauf (glGetError) zwischen Lesen und Mappen
 *   6  Pack-Puffer gleich nach glXSwapBuffers (ausstehende Bildantworten des armv7-Clients)
 *   7  glGetBufferSubData auf den Pack-Puffer
 *   8  gewöhnliches Lesen danach (Antworten dürfen nicht verrutscht sein)
 * Bauen: gcc -O1 -o readtest readtest.c -lX11 -lGL
 */
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>

#define W 64
#define H 64

static void print(const char* name, const unsigned char* p) {
    printf("%-28s %3d %3d %3d %3d\n", name, p[0], p[1], p[2], p[3]);
    fflush(stdout);
}

int main(void) {
    Display* dpy = XOpenDisplay(NULL);
    if (!dpy) return 1;
    int attribs[] = {GLX_RGBA, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, GLX_ALPHA_SIZE, 8, GLX_DOUBLEBUFFER, None};
    XVisualInfo* vi = glXChooseVisual(dpy, DefaultScreen(dpy), attribs);
    if (!vi) return 2;
    XSetWindowAttributes swa = {0};
    swa.colormap = XCreateColormap(dpy, RootWindow(dpy, vi->screen), vi->visual, AllocNone);
    Window win = XCreateWindow(dpy, RootWindow(dpy, vi->screen), 0, 0, W, H, 0, vi->depth, InputOutput, vi->visual, CWColormap, &swa);
    XMapWindow(dpy, win);
    XSync(dpy, False);
    GLXContext ctx = glXCreateContext(dpy, vi, NULL, True);
    glXMakeCurrent(dpy, win, ctx);

    glViewport(0, 0, W, H);
    glClearColor(200 / 255.0f, 100 / 255.0f, 50 / 255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();

    unsigned char* buf = calloc(W * H, 4);
    glReadPixels(0, 0, W, H, GL_BGRA, GL_UNSIGNED_BYTE, buf);
    print("1 BGRA", buf);
    glReadPixels(0, 0, W, H, GL_BGRA, GL_UNSIGNED_BYTE, buf);
    print("2 BGRA again", buf);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, buf);
    print("3 RGBA after BGRA", buf);

    GLuint pbo;
    glGenBuffers(1, &pbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
    glBufferData(GL_PIXEL_PACK_BUFFER, W * H * 4, NULL, GL_STREAM_READ);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, (void*)0);
    const unsigned char* mapped = glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
    if (mapped) print("4 RGBA into pack buffer", mapped);
    else printf("4 RGBA into pack buffer      glMapBuffer failed\n");
    glUnmapBuffer(GL_PIXEL_PACK_BUFFER);

    /* 5: wie 4, mit einem Rundlauf (glGetError) zwischen Lesen und Mappen */
    glClearColor(10 / 255.0f, 20 / 255.0f, 30 / 255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, (void*)0);
    glGetError();
    mapped = glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
    if (mapped) print("5 pack buffer after sync", mapped);
    glUnmapBuffer(GL_PIXEL_PACK_BUFFER);

    /* 6: gleich nach Bildtauschen (der Client wartet evtl. noch auf die Antwort zum letzten) */
    for (int i = 0; i < 3; i++) {
        glClearColor(30 / 255.0f, 60 / 255.0f, 90 / 255.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glXSwapBuffers(dpy, win);
    }
    glClear(GL_COLOR_BUFFER_BIT);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, (void*)0);
    mapped = glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
    if (mapped) print("6 pack buffer after swaps", mapped);
    glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    unsigned char sub[4] = {0};
    glGetBufferSubData(GL_PIXEL_PACK_BUFFER, 0, 4, sub);
    print("7 glGetBufferSubData", sub);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    /* 8: ein gewöhnliches Lesen danach muss seine eigene Antwort bekommen */
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, buf);
    print("8 RGBA after that", buf);

    printf("glGetError 0x%x\n", glGetError());
    glXMakeCurrent(dpy, None, NULL);
    glXDestroyContext(dpy, ctx);
    XCloseDisplay(dpy);
    return 0;
}
