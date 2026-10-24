/*
 * Shader-Test für den Gladio-Server, angelehnt an das, was wined3d auf einem
 * OpenGL-2.1/GLSL-1.20-Treiber erzeugt: alter GLSL-Stil mit gl_Vertex, gl_TexCoord,
 * gl_FrontColor, gl_FragData, gl_Fog, attribute/varying, VBO + IBO, Framebuffer-Objekt
 * (EXT), Alphatest über glAlphaFunc, Blending, glReadPixels.
 *
 *   gcc -O1 -o glsltest glsltest.c -lX11 -lGL -ldl
 *   ./glsltest out.ppm
 *
 * Pass 1 zeichnet mit Shadern im GLSL-1.30-Stil (in/out, texture()) einen Farbverlauf in
 * eine 64x64-Textur (FBO). Pass 2 zeichnet damit ein Rechteck im wined3d-Stil auf den
 * Bildschirm: Textur * Farbe, Nebel über gl_Fog, Alphatest GL_GREATER 0.5, zweites
 * Rechteck mit Blending. Ausgabe: einige Pixelwerte und ein PPM.
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

#define W 256
#define H 128

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

static GLuint link(const char* vs, const char* fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(p, 0, "vs_in0");
    glBindAttribLocation(p, 1, "vs_in1");
    glBindAttribLocation(p, 2, "vs_in2");
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

/* Pass 1: GLSL 1.30 (in/out, texture()) */
static const char* vs130 =
    "#version 130\n"
    "in vec4 vs_in0;\n"
    "in vec2 vs_in2;\n"
    "out vec2 uv;\n"
    "void main() { gl_Position = vs_in0; uv = vs_in2; }\n";
static const char* fs130 =
    "#version 130\n"
    "in vec2 uv;\n"
    "out vec4 ps_out;\n"
    "uniform sampler2D ps_sampler0;\n"
    "void main() {\n"
    "    vec4 t = texture(ps_sampler0, uv);\n"
    "    ps_out = vec4(uv.x, uv.y, t.r, uv.x > 0.5 ? 1.0 : 0.25);\n"
    "}\n";

/* Pass 2: wined3d-Stil (GLSL 1.20, vs_1_1 / ps_1_1 Übersetzung) */
static const char* vs120 =
    "#version 120\n"
    "uniform vec4 vs_c[8];\n"
    "uniform vec4 posFixup;\n"
    "attribute vec4 vs_in0;\n"
    "attribute vec4 vs_in1;\n"
    "attribute vec4 vs_in2;\n"
    "void main() {\n"
    "    vec4 R0;\n"
    "    R0.x = dot(vs_in0, vs_c[0]);\n"
    "    R0.y = dot(vs_in0, vs_c[1]);\n"
    "    R0.z = dot(vs_in0, vs_c[2]);\n"
    "    R0.w = dot(vs_in0, vs_c[3]);\n"
    "    gl_Position = R0;\n"
    "    gl_FrontColor = vs_in1;\n"
    "    gl_TexCoord[0] = vs_in2;\n"
    "    gl_FogFragCoord = clamp(vs_in0.z * 100.0 + 50.0, 0.0, 1000.0);\n"
    "    gl_Position.y = gl_Position.y * posFixup.y;\n"
    "    gl_Position.xy += posFixup.zw * gl_Position.ww;\n"
    "}\n";
static const char* fs120 =
    "#version 120\n"
    "uniform vec4 ps_c[8];\n"
    "uniform sampler2D ps_sampler0;\n"
    "void main() {\n"
    "    vec4 R0 = texture2D(ps_sampler0, gl_TexCoord[0].xy);\n"
    "    R0 = R0 * gl_Color;\n"
    "    R0 = R0 + ps_c[0];\n"
    "    gl_FragData[0] = R0;\n"
    "    float fogFactor = clamp((gl_Fog.end - gl_FogFragCoord) * gl_Fog.scale, 0.0, 1.0);\n"
    "    gl_FragData[0].xyz = mix(gl_Fog.color.xyz, gl_FragData[0].xyz, fogFactor);\n"
    "}\n";

int main(int argc, char** argv) {
    const char* out = argc > 1 ? argv[1] : "out.ppm";
    Display* dpy = XOpenDisplay(NULL);
    if (!dpy) return 1;
    int screen = DefaultScreen(dpy);
    int attribs[] = { GLX_RGBA, GLX_DOUBLEBUFFER, GLX_DEPTH_SIZE, 16, None };
    XVisualInfo* vis = glXChooseVisual(dpy, screen, attribs);
    if (!vis) { puts("kein Visual"); return 2; }
    GLXContext ctx = glXCreateContext(dpy, vis, NULL, True);
    Window win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 0, 0, W, H, 0, 0, 0);
    XMapWindow(dpy, win);
    XSync(dpy, False);
    glXMakeCurrent(dpy, win, ctx);
    printf("%s / GL %s / GLSL %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION), glGetString(GL_SHADING_LANGUAGE_VERSION));

    PFNGLGENFRAMEBUFFERSEXTPROC pGenFramebuffers = proc("glGenFramebuffersEXT");
    PFNGLBINDFRAMEBUFFEREXTPROC pBindFramebuffer = proc("glBindFramebufferEXT");
    PFNGLFRAMEBUFFERTEXTURE2DEXTPROC pFramebufferTexture2D = proc("glFramebufferTexture2DEXT");
    PFNGLCHECKFRAMEBUFFERSTATUSEXTPROC pCheckFramebufferStatus = proc("glCheckFramebufferStatusEXT");
    PFNGLGENRENDERBUFFERSEXTPROC pGenRenderbuffers = proc("glGenRenderbuffersEXT");
    PFNGLBINDRENDERBUFFEREXTPROC pBindRenderbuffer = proc("glBindRenderbufferEXT");
    PFNGLRENDERBUFFERSTORAGEEXTPROC pRenderbufferStorage = proc("glRenderbufferStorageEXT");
    PFNGLFRAMEBUFFERRENDERBUFFEREXTPROC pFramebufferRenderbuffer = proc("glFramebufferRenderbufferEXT");

    /* Quelltextur 4x4 (Rotkanal = Schachbrett) */
    GLuint srcTex;
    glGenTextures(1, &srcTex);
    glBindTexture(GL_TEXTURE_2D, srcTex);
    unsigned char checker[4 * 4 * 4];
    for (int i = 0; i < 16; i++) {
        int on = ((i & 1) ^ ((i >> 2) & 1));
        checker[i*4+0] = on ? 255 : 0; checker[i*4+1] = 128; checker[i*4+2] = 64; checker[i*4+3] = 255;
    }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, checker);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    /* Zieltextur 64x64 + Tiefe als Renderbuffer, FBO */
    GLuint fboTex, fbo, rb;
    glGenTextures(1, &fboTex);
    glBindTexture(GL_TEXTURE_2D, fboTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    pGenFramebuffers(1, &fbo);
    pBindFramebuffer(GL_FRAMEBUFFER_EXT, fbo);
    pFramebufferTexture2D(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, fboTex, 0);
    pGenRenderbuffers(1, &rb);
    pBindRenderbuffer(GL_RENDERBUFFER_EXT, rb);
    pRenderbufferStorage(GL_RENDERBUFFER_EXT, GL_DEPTH24_STENCIL8_EXT, 64, 64);
    pFramebufferRenderbuffer(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, rb);
    pFramebufferRenderbuffer(GL_FRAMEBUFFER_EXT, GL_STENCIL_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, rb);
    GLenum status = pCheckFramebufferStatus(GL_FRAMEBUFFER_EXT);
    printf("FBO-Status %x\n", status);

    /* Geometrie in VBO/IBO: Position (xyzw), Farbe (D3DCOLOR als BGRA-Bytes), Texturkoordinate */
    struct Vertex { float x, y, z, w; unsigned char c[4]; float u, v, s, t; } verts[8] = {
        { -1, -1, 0.5f, 1, {255, 255, 255, 255}, 0, 0, 0, 1 },
        {  1, -1, 0.5f, 1, {255, 255, 255, 255}, 1, 0, 0, 1 },
        {  1,  1, 0.5f, 1, {255, 255, 255, 255}, 1, 1, 0, 1 },
        { -1,  1, 0.5f, 1, {255, 255, 255, 255}, 0, 1, 0, 1 },
        /* zweites Rechteck (Blending), halbtransparent grün */
        { -0.5f, -0.5f, 0.2f, 1, {0, 255, 0, 128}, 0, 0, 0, 1 },
        {  0.5f, -0.5f, 0.2f, 1, {0, 255, 0, 128}, 1, 0, 0, 1 },
        {  0.5f,  0.5f, 0.2f, 1, {0, 255, 0, 128}, 1, 1, 0, 1 },
        { -0.5f,  0.5f, 0.2f, 1, {0, 255, 0, 128}, 0, 1, 0, 1 },
    };
    unsigned short indices[12] = { 0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7 };
    GLuint vbo, ibo;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glGenBuffers(1, &ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(struct Vertex), (void*)0);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(struct Vertex), (void*)16);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(struct Vertex), (void*)20);

    /* Pass 1: in die Textur */
    GLuint prog130 = link(vs130, fs130);
    glUseProgram(prog130);
    glUniform1i(glGetUniformLocation(prog130, "ps_sampler0"), 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, srcTex);
    glViewport(0, 0, 64, 64);
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (void*)0);
    pBindFramebuffer(GL_FRAMEBUFFER_EXT, 0);

    /* Pass 2: auf den Bildschirm */
    GLuint prog120 = link(vs120, fs120);
    glUseProgram(prog120);
    float vsc[32] = { 1, 0, 0, 0,   0, 1, 0, 0,   0, 0, 1, 0,   0, 0, 0, 1 };
    glUniform4fv(glGetUniformLocation(prog120, "vs_c"), 4, vsc);
    glUniform4f(glGetUniformLocation(prog120, "posFixup"), 1, 1, 0, 0);
    float psc[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    glUniform4fv(glGetUniformLocation(prog120, "ps_c"), 1, psc);
    glUniform1i(glGetUniformLocation(prog120, "ps_sampler0"), 0);
    glBindTexture(GL_TEXTURE_2D, fboTex);

    GLfloat fogColor[] = { 0.5f, 0.5f, 0.5f, 1.0f };
    glFogfv(GL_FOG_COLOR, fogColor);
    glFogf(GL_FOG_MODE, GL_LINEAR);
    glFogf(GL_FOG_START, 0.0f);
    glFogf(GL_FOG_END, 200.0f);

    glViewport(0, 0, W, H);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.5f);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (void*)0);

    glDisable(GL_ALPHA_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (void*)(6 * sizeof(unsigned short)));
    glDisable(GL_BLEND);
    glFinish();
    printf("glGetError %x\n", glGetError());

    unsigned char* px = malloc(W * H * 4);
    glReadPixels(0, 0, W, H, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, px);
    int xs[] = { 10, 60, 128, 200, 250 };
    for (unsigned i = 0; i < sizeof(xs) / sizeof(*xs); i++) {
        unsigned char* p = px + ((H / 2) * W + xs[i]) * 4;
        printf("x=%d: B%3d G%3d R%3d A%3d\n", xs[i], p[0], p[1], p[2], p[3]);
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
    return 0;
}
