/*
 * Komprimierte Textur ohne Daten anlegen (glCompressedTexImage2D mit data = NULL, wie wined3d beim Anlegen einer
 * DXT-Textur ohne Startinhalt), danach per glCompressedTexSubImage2D füllen und ein Pixel zurücklesen.
 * Vor dem Gladio-Commit 3861b0a (GL_SEND_TEXIMAGE) schrieb der Client imageSize Byte ab Adresse 0 in den Ring: Absturz.
 *
 *   gcc -o ctexnull ctexnull.c -lGL -lX11;  run.sh -- ./ctexnull     (Erwartung: Pixel 255 0 0)
 */
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <stdio.h>
#include <string.h>

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

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    /* 8x8 DXT1 = 4 Blöcke zu 8 Byte; erst nur Speicher anlegen (data = NULL) */
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGB_S3TC_DXT1_EXT, 8, 8, 0, 32, NULL);
    printf("anlegen ohne Daten: Fehler %x\n", glGetError());
    fflush(stdout);
    unsigned char data[32];
    for (int b = 0; b < 4; b++) {
        /* color0 = color1 = 0xF800 (rot, RGB565), alle Indizes 0 */
        unsigned char block[8] = {0x00, 0xF8, 0x00, 0xF8, 0, 0, 0, 0};
        memcpy(data + b * 8, block, 8);
    }
    glCompressedTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 8, 8, GL_COMPRESSED_RGB_S3TC_DXT1_EXT, 32, data);
    printf("füllen: Fehler %x\n", glGetError());

    glViewport(0, 0, 64, 64);
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_TEXTURE_2D);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(-1, -1);
    glTexCoord2f(1, 0); glVertex2f(1, -1);
    glTexCoord2f(1, 1); glVertex2f(1, 1);
    glTexCoord2f(0, 1); glVertex2f(-1, 1);
    glEnd();
    unsigned char px[4] = {9, 9, 9, 9};
    glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    printf("Pixel: %d %d %d %d (erwartet 255 0 0)\n", px[0], px[1], px[2], px[3]);
    glXSwapBuffers(dpy, win);
    glXMakeCurrent(dpy, None, NULL);
    glXDestroyContext(dpy, ctx);
    XCloseDisplay(dpy);
    return 0;
}
