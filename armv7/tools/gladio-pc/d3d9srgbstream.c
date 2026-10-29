/*
 * d3d9srgbstream – DXT-Texturen mit allen Mip-Stufen wie beim Textur-Streaming der Unreal Engine 3: in
 * D3DPOOL_DEFAULT über UpdateTexture hochgeladen und mit D3DSAMP_SRGBTEXTURE gelesen (wined3d kopiert dafür
 * jede Stufe über glGetCompressedTexImage in eine sRGB-Fassung), dann mit anderen Farben neu hochgeladen, zum
 * Teil freigegeben und neu angelegt. Jede Textur hat je Stufe und Durchgang eine eigene Farbe; gelesen wird
 * jede Stufe einzeln (D3DSAMP_MAXMIPLEVEL) mit und ohne sRGB und mit der erwarteten Farbe verglichen.
 *
 *   x86_64-w64-mingw32-gcc -O1 -o d3d9srgbstream64.exe d3d9srgbstream.c -ld3d9 -luser32 -lm
 *   wine d3d9srgbstream64.exe [ausgabe.txt]
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 64
#define H 64
#define TEXTURES 6
#define SIZE 32
#define LEVELS 6

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

struct Vertex { float x, y, z, rhw, u, v; };

static const D3DFORMAT formats[TEXTURES] = {D3DFMT_DXT1, D3DFMT_DXT3, D3DFMT_DXT5, D3DFMT_DXT1, D3DFMT_DXT5, D3DFMT_DXT3};
static const char* formatNames[TEXTURES] = {"DXT1", "DXT3", "DXT5", "DXT1", "DXT5", "DXT3"};

/* Farbe (5/6/5 Bit) je Textur, Stufe und Durchgang */
static void color565(int texture, int level, int pass, int* r5, int* g6, int* b5)
{
    *r5 = (texture * 7 + level * 3 + pass * 11 + 4) % 32;
    *g6 = (texture * 13 + level * 5 + pass * 17 + 9) % 64;
    *b5 = (texture * 3 + level * 11 + pass * 5 + 2) % 32;
}

static int expand5(int v) { return (v << 3) | (v >> 2); }
static int expand6(int v) { return (v << 2) | (v >> 4); }

static int srgbToLinear(int c)
{
    double v = c / 255.0;
    v = v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
    return (int)floor(v * 255.0 + 0.5);
}

/* ein einfarbiger Block (8 Byte Farbe, bei DXT3/5 davor 8 Byte Alpha = undurchsichtig) */
static int writeBlock(unsigned char* out, D3DFORMAT format, int r5, int g6, int b5)
{
    int n = 0;
    if (format == D3DFMT_DXT3) {
        memset(out, 0xff, 8);
        n = 8;
    }
    else if (format == D3DFMT_DXT5) {
        out[0] = 0xff; out[1] = 0xff;
        memset(out + 2, 0, 6);
        n = 8;
    }
    unsigned short c = (unsigned short)((r5 << 11) | (g6 << 5) | b5);
    /* c0 > c1 erzwingt bei DXT1 den Vierfarbmodus; Index 0 = c0 */
    unsigned short c0 = c, c1 = c > 0 ? c - 1 : 0;
    if (c == 0) { c0 = 1; c1 = 0; } /* nie gewählt: alle Indizes 1 = c1 = 0 */
    out[n + 0] = c0 & 0xff; out[n + 1] = c0 >> 8;
    out[n + 2] = c1 & 0xff; out[n + 3] = c1 >> 8;
    memset(out + n + 4, c == 0 ? 0x55 : 0x00, 4);
    return n + 8;
}

static IDirect3DTexture9* createTexture(IDirect3DDevice9* dev, int texture, int pass)
{
    IDirect3DTexture9 *sys = NULL, *tex = NULL;
    D3DFORMAT format = formats[texture];
    if (FAILED(IDirect3DDevice9_CreateTexture(dev, SIZE, SIZE, LEVELS, 0, format, D3DPOOL_DEFAULT, &tex, NULL))) return NULL;
    if (FAILED(IDirect3DDevice9_CreateTexture(dev, SIZE, SIZE, LEVELS, 0, format, D3DPOOL_SYSTEMMEM, &sys, NULL))) return NULL;
    for (int level = 0; level < LEVELS; level++) {
        int r5, g6, b5;
        color565(texture, level, pass, &r5, &g6, &b5);
        D3DLOCKED_RECT lr;
        if (FAILED(IDirect3DTexture9_LockRect(sys, level, &lr, NULL, 0))) return NULL;
        int size = SIZE >> level;
        int blocks = (size + 3) / 4;
        for (int by = 0; by < blocks; by++) {
            unsigned char* row = (unsigned char*)lr.pBits + by * lr.Pitch;
            for (int bx = 0; bx < blocks; bx++) row += writeBlock(row, format, r5, g6, b5);
        }
        IDirect3DTexture9_UnlockRect(sys, level);
    }
    IDirect3DDevice9_UpdateTexture(dev, (IDirect3DBaseTexture9*)sys, (IDirect3DBaseTexture9*)tex);
    IDirect3DTexture9_Release(sys);
    return tex;
}

/* neue Farben in eine bestehende Textur (Streaming) */
static void reupload(IDirect3DDevice9* dev, IDirect3DTexture9* tex, int texture, int pass)
{
    /* UpdateTexture verlangt SYSTEMMEM als Quelle: über eine Systemkopie gehen */
    IDirect3DTexture9* sys = NULL;
    D3DFORMAT format = formats[texture];
    IDirect3DDevice9_CreateTexture(dev, SIZE, SIZE, LEVELS, 0, format, D3DPOOL_SYSTEMMEM, &sys, NULL);
    for (int level = 0; level < LEVELS; level++) {
        int r5, g6, b5;
        color565(texture, level, pass, &r5, &g6, &b5);
        D3DLOCKED_RECT lr;
        IDirect3DTexture9_LockRect(sys, level, &lr, NULL, 0);
        int size = SIZE >> level, blocks = (size + 3) / 4;
        for (int by = 0; by < blocks; by++) {
            unsigned char* row = (unsigned char*)lr.pBits + by * lr.Pitch;
            for (int bx = 0; bx < blocks; bx++) row += writeBlock(row, format, r5, g6, b5);
        }
        IDirect3DTexture9_UnlockRect(sys, level);
    }
    IDirect3DDevice9_UpdateTexture(dev, (IDirect3DBaseTexture9*)sys, (IDirect3DBaseTexture9*)tex);
    IDirect3DTexture9_Release(sys);
}

static DWORD readCenter(IDirect3DDevice9* dev, IDirect3DSurface9* sysSurface)
{
    IDirect3DSurface9* back = NULL;
    IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &back);
    IDirect3DDevice9_GetRenderTargetData(dev, back, sysSurface);
    IDirect3DSurface9_Release(back);
    D3DLOCKED_RECT lr;
    DWORD pixel = 0;
    if (SUCCEEDED(IDirect3DSurface9_LockRect(sysSurface, &lr, NULL, D3DLOCK_READONLY))) {
        pixel = ((DWORD*)((char*)lr.pBits + (H / 2) * lr.Pitch))[W / 2];
        IDirect3DSurface9_UnlockRect(sysSurface);
    }
    return pixel;
}

static int errors = 0, checks = 0;

static void drawAndCheck(IDirect3DDevice9* dev, IDirect3DSurface9* sysSurface, IDirect3DTexture9* tex, int texture,
                         int pass, const char* label)
{
    struct Vertex quad[4] = {{0, 0, 0.5f, 1, 0, 0}, {W, 0, 0.5f, 1, 1, 0}, {0, H, 0.5f, 1, 0, 1}, {W, H, 0.5f, 1, 1, 1}};
    for (int srgb = 1; srgb >= 0; srgb--) {
        printf("%-10s %-5s Textur %d %s:", label, formatNames[texture], texture, srgb ? "sRGB  " : "linear");
        for (int level = 0; level < LEVELS; level++) {
            IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9*)tex);
            IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_SRGBTEXTURE, srgb);
            IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAXMIPLEVEL, level);
            IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xff000000, 1.0f, 0);
            IDirect3DDevice9_BeginScene(dev);
            IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, quad, sizeof(struct Vertex));
            IDirect3DDevice9_EndScene(dev);
            DWORD pixel = readCenter(dev, sysSurface);
            IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
            int r5, g6, b5;
            color565(texture, level, pass, &r5, &g6, &b5);
            int er = expand5(r5), eg = expand6(g6), eb = expand5(b5);
            if (srgb) { er = srgbToLinear(er); eg = srgbToLinear(eg); eb = srgbToLinear(eb); }
            int r = (pixel >> 16) & 0xff, g = (pixel >> 8) & 0xff, b = pixel & 0xff;
            int ok = abs(r - er) <= 2 && abs(g - eg) <= 2 && abs(b - eb) <= 2;
            checks++;
            if (!ok) errors++;
            printf(" %02x%02x%02x%s", r, g, b, ok ? "" : "!");
        }
        printf("\n");
    }
}

int main(int argc, char** argv)
{
    if (argc > 1) freopen(argv[1], "w", stdout);
    setvbuf(stdout, NULL, _IONBF, 0);

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndProc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "d3d9srgbstream";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("d3d9srgbstream", "d3d9srgbstream", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, W, H, NULL, NULL, wc.hInstance, NULL);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp = {0};
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = W; pp.BackBufferHeight = H; pp.hDeviceWindow = hwnd;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9* dev = NULL;
    if (!d3d || FAILED(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev))) {
        printf("CreateDevice fehlgeschlagen\n");
        return 1;
    }
    IDirect3DSurface9* sysSurface = NULL;
    IDirect3DDevice9_CreateOffscreenPlainSurface(dev, W, H, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sysSurface, NULL);

    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_TEX1);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MIPFILTER, D3DTEXF_POINT);

    IDirect3DTexture9* textures[TEXTURES];
    int passOf[TEXTURES];
    for (int t = 0; t < TEXTURES; t++) {
        textures[t] = createTexture(dev, t, 0);
        passOf[t] = 0;
        if (!textures[t]) { printf("CreateTexture %d fehlgeschlagen\n", t); return 1; }
    }
    /* 1: frisch hochgeladen */
    for (int t = 0; t < TEXTURES; t++) drawAndCheck(dev, sysSurface, textures[t], t, passOf[t], "neu");
    /* 2: neue Farben in dieselben Texturen (die sRGB-Fassung muss neu kopiert werden) */
    for (int t = 0; t < TEXTURES; t++) { reupload(dev, textures[t], t, 1); passOf[t] = 1; }
    for (int t = 0; t < TEXTURES; t++) drawAndCheck(dev, sysSurface, textures[t], t, passOf[t], "neu geladen");
    /* 3: jede zweite freigeben und neu anlegen (GL-Namen werden wiederverwendet) */
    for (int t = 0; t < TEXTURES; t += 2) {
        IDirect3DTexture9_Release(textures[t]);
        textures[t] = NULL;
    }
    for (int t = 0; t < TEXTURES; t += 2) { textures[t] = createTexture(dev, t, 2); passOf[t] = 2; }
    for (int t = 0; t < TEXTURES; t++) drawAndCheck(dev, sysSurface, textures[t], t, passOf[t], "ersetzt");

    printf("%d von %d Stufen richtig%s\n", checks - errors, checks, errors ? " – FEHLER" : "");
    for (int t = 0; t < TEXTURES; t++) IDirect3DTexture9_Release(textures[t]);
    IDirect3DSurface9_Release(sysSurface);
    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
    printf(errors ? "FEHLER\n" : "ok\n");
    return errors ? 1 : 0;
}
