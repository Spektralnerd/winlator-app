/*
 * d3d9test – kleines Direct3D-9-Testprogramm für wined3d über Gladio (PC-Testgeschirr).
 *
 * Zeichnet in ein 320x240-Fenster:
 *   - Hintergrund blau (Clear)
 *   - festes Funktionsmodell: beleuchtetes, texturiertes Viereck (A8R8G8B8-Textur mit Mipmaps),
 *     Dreieck mit R5G6B5-Textur, DXT1-Textur (wined3d dekodiert ohne S3TC selbst),
 *     halbtransparentes Viereck (Alpha-Blending) und Alphatest
 *   - vs_2_0/ps_2_0-Shader (über D3DXAssembleShader aus d3dx9_36) auf ein Viereck
 *   - Render-to-Texture (D3DUSAGE_RENDERTARGET) und Verwendung als Textur
 * und schreibt das fertige Bild über GetRenderTargetData als PPM (Vergleich Gladio gegen Mesa).
 *
 * Bauen (MinGW):  i686-w64-mingw32-gcc -O1 -o d3d9test.exe d3d9test.c -ld3d9 -lgdi32 -luser32
 * Aufruf:         wine d3d9test.exe out.ppm [BILDER]     (BILDER: Anzahl Present-Aufrufe, Vorgabe 60)
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define W 320
#define H 240

typedef HRESULT (WINAPI *PFN_D3DXAssembleShader)(const char* src, UINT len, const void* defines, void* include, DWORD flags, void** shader, void** errors);
typedef struct { void* lpVtbl; } ID3DXBufferLike;
/* ID3DXBuffer: QueryInterface, AddRef, Release, GetBufferPointer, GetBufferSize */
static void* bufferPointer(void* buf) { return ((void* (WINAPI **)(void*))(*(void***)buf))[3](buf); }
static void bufferRelease(void* buf) { ((ULONG (WINAPI **)(void*))(*(void***)buf))[2](buf); }

struct Vertex { float x, y, z; float nx, ny, nz; DWORD color; float u, v; };
#define FVF (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1)

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}

static void matIdentity(D3DMATRIX* m) { memset(m, 0, sizeof(*m)); m->m[0][0] = m->m[1][1] = m->m[2][2] = m->m[3][3] = 1.0f; }
static void matOrtho(D3DMATRIX* m, float w, float h, float zn, float zf) {
    matIdentity(m);
    m->m[0][0] = 2.0f / w; m->m[1][1] = 2.0f / h; m->m[2][2] = 1.0f / (zf - zn); m->m[3][2] = -zn / (zf - zn);
}

static IDirect3DTexture9* makeTexture(IDirect3DDevice9* dev, int size, D3DFORMAT fmt, int pattern, int mips) {
    IDirect3DTexture9* tex = NULL;
    if (FAILED(IDirect3DDevice9_CreateTexture(dev, size, size, mips, 0, fmt, D3DPOOL_MANAGED, &tex, NULL))) { printf("CreateTexture %x fehlgeschlagen\n", fmt); return NULL; }
    int levels = IDirect3DTexture9_GetLevelCount(tex);
    for (int lvl = 0; lvl < levels; lvl++) {
        D3DLOCKED_RECT lr;
        int ls = size >> lvl; if (ls < 1) ls = 1;
        if (FAILED(IDirect3DTexture9_LockRect(tex, lvl, &lr, NULL, 0))) { printf("LockRect fehlgeschlagen\n"); break; }
        for (int y = 0; y < ls; y++) {
            for (int x = 0; x < ls; x++) {
                int cx = (x * 8 / ls), cy = (y * 8 / ls);
                int checker = ((cx + cy) & 1);
                unsigned r = checker ? 255 : (x * 255 / (ls > 1 ? ls - 1 : 1));
                unsigned g = checker ? (y * 255 / (ls > 1 ? ls - 1 : 1)) : 64;
                unsigned b = pattern ? 200 : 32;
                unsigned a = (x < ls / 2) ? 255 : 96;            /* linke Hälfte deckend, rechte halb durchsichtig */
                if (lvl > 0) { r = 255 - r; }                     /* Mipmap-Stufen sichtbar anders */
                if (fmt == D3DFMT_A8R8G8B8 || fmt == D3DFMT_X8R8G8B8) {
                    ((DWORD*)((char*)lr.pBits + y * lr.Pitch))[x] = (a << 24) | (r << 16) | (g << 8) | b;
                }
                else if (fmt == D3DFMT_R5G6B5) {
                    ((WORD*)((char*)lr.pBits + y * lr.Pitch))[x] = (WORD)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
                }
                else if (fmt == D3DFMT_A1R5G5B5) {
                    ((WORD*)((char*)lr.pBits + y * lr.Pitch))[x] = (WORD)(((a >> 7) << 15) | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
                }
                else if (fmt == D3DFMT_A4R4G4B4) {
                    ((WORD*)((char*)lr.pBits + y * lr.Pitch))[x] = (WORD)(((a >> 4) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4));
                }
                else if (fmt == D3DFMT_A8) {
                    ((BYTE*)((char*)lr.pBits + y * lr.Pitch))[x] = (BYTE)r;
                }
                else if (fmt == D3DFMT_L8) {
                    ((BYTE*)((char*)lr.pBits + y * lr.Pitch))[x] = (BYTE)g;
                }
            }
        }
        IDirect3DTexture9_UnlockRect(tex, lvl);
    }
    return tex;
}

/* DXT1-Blöcke von Hand: jeder 4x4-Block mit zwei Farben, Muster diagonal */
static IDirect3DTexture9* makeDXT1(IDirect3DDevice9* dev, int size) {
    IDirect3DTexture9* tex = NULL;
    if (FAILED(IDirect3DDevice9_CreateTexture(dev, size, size, 1, 0, D3DFMT_DXT1, D3DPOOL_MANAGED, &tex, NULL))) { printf("CreateTexture DXT1 fehlgeschlagen\n"); return NULL; }
    D3DLOCKED_RECT lr;
    if (FAILED(IDirect3DTexture9_LockRect(tex, 0, &lr, NULL, 0))) return tex;
    for (int by = 0; by < size / 4; by++) {
        for (int bx = 0; bx < size / 4; bx++) {
            BYTE* blk = (BYTE*)lr.pBits + by * lr.Pitch + bx * 8;
            WORD c0 = (WORD)(((bx * 31 / (size / 4 - 1)) << 11) | (0 << 5) | 0);           /* Rotverlauf */
            WORD c1 = (WORD)((0 << 11) | ((by * 63 / (size / 4 - 1)) << 5) | 31);          /* Grünverlauf + Blau */
            if (c0 < c1) { WORD t = c0; c0 = c1; c1 = t; }                                   /* c0 > c1: 4-Farben-Modus */
            blk[0] = c0 & 0xff; blk[1] = c0 >> 8; blk[2] = c1 & 0xff; blk[3] = c1 >> 8;
            blk[4] = 0x00; blk[5] = 0x55; blk[6] = 0xaa; blk[7] = 0xff;                      /* Zeilen: c0, c1, c2, c3 */
        }
    }
    IDirect3DTexture9_UnlockRect(tex, 0);
    return tex;
}

static void quad(struct Vertex* v, float x, float y, float w, float h, float z, DWORD color) {
    struct Vertex q[4] = {
        { x,     y,     z, 0, 0, -1, color, 0, 0 },
        { x + w, y,     z, 0, 0, -1, color, 1, 0 },
        { x,     y + h, z, 0, 0, -1, color, 0, 1 },
        { x + w, y + h, z, 0, 0, -1, color, 1, 1 },
    };
    memcpy(v, q, sizeof(q));
}

static void drawQuad(IDirect3DDevice9* dev, float x, float y, float w, float h, float z, DWORD color) {
    struct Vertex v[4];
    quad(v, x, y, w, h, z, color);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, v, sizeof(struct Vertex));
}

static int writePPM(IDirect3DDevice9* dev, const char* path) {
    IDirect3DSurface9* back = NULL; IDirect3DSurface9* sys = NULL;
    if (FAILED(IDirect3DDevice9_GetRenderTarget(dev, 0, &back))) return 0;
    if (FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, W, H, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL))) return 0;
    HRESULT hr = IDirect3DDevice9_GetRenderTargetData(dev, back, sys);
    if (FAILED(hr)) { printf("GetRenderTargetData fehlgeschlagen: %lx\n", (long)hr); return 0; }
    D3DLOCKED_RECT lr;
    if (FAILED(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) return 0;
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int y = 0; y < H; y++) {
        const DWORD* row = (const DWORD*)((const char*)lr.pBits + y * lr.Pitch);
        for (int x = 0; x < W; x++) {
            DWORD p = row[x];
            fputc((p >> 16) & 0xff, f); fputc((p >> 8) & 0xff, f); fputc(p & 0xff, f);
        }
    }
    fclose(f);
    IDirect3DSurface9_UnlockRect(sys);
    IDirect3DSurface9_Release(sys);
    IDirect3DSurface9_Release(back);
    printf("Bild nach %s geschrieben\n", path);
    return 1;
}

static const char* vsSrc =
    "vs_2_0\n"
    "dcl_position v0\n"
    "dcl_color v1\n"
    "dcl_texcoord0 v2\n"
    "m4x4 oPos, v0, c0\n"
    "mov oD0, v1\n"
    "mov oT0, v2\n";

static const char* psSrc =
    "ps_2_0\n"
    "dcl t0.xy\n"
    "dcl v0\n"
    "dcl_2d s0\n"
    "def c0, 0.5, 0.25, 0.0, 1.0\n"
    "texld r0, t0, s0\n"
    "mul r0, r0, v0\n"
    "add r0.xyz, r0, c0\n"
    "mov oC0, r0\n";

int main(int argc, char** argv) {
    const char* out = argc > 1 ? argv[1] : "out.ppm";
    int frames = argc > 2 ? atoi(argv[2]) : 60;

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndProc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "d3d9test";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("d3d9test", "d3d9test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, W, H, NULL, NULL, wc.hInstance, NULL);

    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { printf("Direct3DCreate9 fehlgeschlagen\n"); return 1; }
    D3DADAPTER_IDENTIFIER9 adapter;
    if (SUCCEEDED(IDirect3D9_GetAdapterIdentifier(d3d, 0, 0, &adapter))) printf("Adapter: %s / %s\n", adapter.Description, adapter.Driver);

    D3DPRESENT_PARAMETERS pp = {0};
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = W; pp.BackBufferHeight = H; pp.EnableAutoDepthStencil = TRUE; pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.hDeviceWindow = hwnd; pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9* dev = NULL;
    HRESULT hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr)) { printf("CreateDevice fehlgeschlagen: %lx\n", (long)hr); return 1; }
    D3DCAPS9 caps;
    IDirect3DDevice9_GetDeviceCaps(dev, &caps);
    printf("Caps: VS %lx PS %lx MaxTextureWidth %lu MaxSimultaneousTextures %lu\n", (unsigned long)caps.VertexShaderVersion, (unsigned long)caps.PixelShaderVersion, (unsigned long)caps.MaxTextureWidth, (unsigned long)caps.MaxSimultaneousTextures);

    IDirect3DTexture9* texA = makeTexture(dev, 64, D3DFMT_A8R8G8B8, 0, 0);
    IDirect3DTexture9* tex565 = makeTexture(dev, 32, D3DFMT_R5G6B5, 1, 1);
    IDirect3DTexture9* tex1555 = makeTexture(dev, 32, D3DFMT_A1R5G5B5, 1, 1);
    IDirect3DTexture9* tex4444 = makeTexture(dev, 32, D3DFMT_A4R4G4B4, 0, 1);
    IDirect3DTexture9* texL8 = makeTexture(dev, 32, D3DFMT_L8, 0, 1);
    IDirect3DTexture9* texDXT = makeDXT1(dev, 32);
    IDirect3DTexture9* texRT = NULL;
    if (FAILED(IDirect3DDevice9_CreateTexture(dev, 64, 64, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &texRT, NULL))) printf("Render-Target-Textur fehlgeschlagen\n");

    /* Shader über d3dx9_36 (Wine-eigene Implementierung) */
    IDirect3DVertexShader9* vs = NULL; IDirect3DPixelShader9* ps = NULL;
    HMODULE d3dx = LoadLibraryA("d3dx9_36.dll");
    PFN_D3DXAssembleShader assemble = d3dx ? (PFN_D3DXAssembleShader)GetProcAddress(d3dx, "D3DXAssembleShader") : NULL;
    if (assemble) {
        void* code = NULL; void* err = NULL;
        if (SUCCEEDED(assemble(vsSrc, strlen(vsSrc), NULL, NULL, 0, &code, &err))) {
            IDirect3DDevice9_CreateVertexShader(dev, bufferPointer(code), &vs); bufferRelease(code);
        } else printf("VS-Assemblierung fehlgeschlagen: %s\n", err ? (char*)bufferPointer(err) : "?");
        code = NULL; err = NULL;
        if (SUCCEEDED(assemble(psSrc, strlen(psSrc), NULL, NULL, 0, &code, &err))) {
            IDirect3DDevice9_CreatePixelShader(dev, bufferPointer(code), &ps); bufferRelease(code);
        } else printf("PS-Assemblierung fehlgeschlagen: %s\n", err ? (char*)bufferPointer(err) : "?");
    } else printf("d3dx9_36 / D3DXAssembleShader nicht verfügbar, Shader-Teil entfällt\n");
    printf("Shader: VS %p PS %p\n", (void*)vs, (void*)ps);

    D3DMATRIX ident, proj;
    matIdentity(&ident);
    matOrtho(&proj, W, H, 0.0f, 10.0f);
    /* Pixelkoordinaten: Ursprung links oben, y nach unten */
    D3DMATRIX view; matIdentity(&view); view.m[3][0] = -W / 2.0f; view.m[3][1] = -H / 2.0f; view.m[1][1] = -1.0f; view.m[3][1] = H / 2.0f;

    D3DLIGHT9 light = {0};
    light.Type = D3DLIGHT_DIRECTIONAL; light.Diffuse.r = 1.0f; light.Diffuse.g = 0.9f; light.Diffuse.b = 0.8f;
    light.Ambient.r = light.Ambient.g = light.Ambient.b = 0.2f;
    light.Direction.x = 0.3f; light.Direction.y = 0.3f; light.Direction.z = 1.0f;
    D3DMATERIAL9 mat = {0};
    mat.Diffuse.r = mat.Diffuse.g = mat.Diffuse.b = mat.Diffuse.a = 1.0f;
    mat.Ambient.r = mat.Ambient.g = mat.Ambient.b = 1.0f;

    MSG msg;
    int frame = 0;
    for (;;) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
        float t = (float)(frame % 60) / 60.0f;

        /* 1. Render-to-Texture: Verlauf in die 64x64-Textur */
        if (texRT) {
            IDirect3DSurface9* rtSurf = NULL; IDirect3DSurface9* backSurf = NULL;
            IDirect3DTexture9_GetSurfaceLevel(texRT, 0, &rtSurf);
            IDirect3DDevice9_GetRenderTarget(dev, 0, &backSurf);
            IDirect3DDevice9_SetRenderTarget(dev, 0, rtSurf);
            IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(255, 255, 0), 1.0f, 0);
            IDirect3DDevice9_BeginScene(dev);
            D3DMATRIX projRT; matOrtho(&projRT, 64, 64, 0, 10);
            D3DMATRIX viewRT; matIdentity(&viewRT); viewRT.m[3][0] = -32; viewRT.m[3][1] = 32; viewRT.m[1][1] = -1;
            IDirect3DDevice9_SetTransform(dev, D3DTS_PROJECTION, &projRT);
            IDirect3DDevice9_SetTransform(dev, D3DTS_VIEW, &viewRT);
            IDirect3DDevice9_SetTransform(dev, D3DTS_WORLD, &ident);
            IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
            IDirect3DDevice9_SetFVF(dev, FVF);
            IDirect3DDevice9_SetTexture(dev, 0, NULL);
            drawQuad(dev, 8, 8, 48, 24, 1, D3DCOLOR_XRGB(255, 0, 255));
            drawQuad(dev, 8, 36, 48, 20, 1, D3DCOLOR_XRGB(0, 128, 255));
            IDirect3DDevice9_EndScene(dev);
            IDirect3DDevice9_SetRenderTarget(dev, 0, backSurf);
            IDirect3DSurface9_Release(rtSurf); IDirect3DSurface9_Release(backSurf);
        }

        IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(30, 40, 160), 1.0f, 0);
        IDirect3DDevice9_BeginScene(dev);
        IDirect3DDevice9_SetTransform(dev, D3DTS_PROJECTION, &proj);
        IDirect3DDevice9_SetTransform(dev, D3DTS_VIEW, &view);
        IDirect3DDevice9_SetTransform(dev, D3DTS_WORLD, &ident);
        IDirect3DDevice9_SetFVF(dev, FVF);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, TRUE);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, TRUE);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_AMBIENT, D3DCOLOR_XRGB(40, 40, 60));
        IDirect3DDevice9_SetLight(dev, 0, &light);
        IDirect3DDevice9_LightEnable(dev, 0, TRUE);
        IDirect3DDevice9_SetMaterial(dev, &mat);
        IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
        IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);

        /* beleuchtetes, texturiertes Viereck (Mipmaps: Textur 64 auf 40 Pixel) */
        IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9*)texA);
        drawQuad(dev, 10, 10, 40, 40, 2, D3DCOLOR_XRGB(255, 255, 255));
        /* dasselbe ohne Licht, mit Vertexfarbe */
        IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
        drawQuad(dev, 60, 10, 64, 64, 2, D3DCOLOR_XRGB(255, 128, 128));
        /* 16-Bit-Formate */
        IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9*)tex565);
        drawQuad(dev, 134, 10, 32, 32, 2, D3DCOLOR_XRGB(255, 255, 255));
        IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9*)tex1555);
        drawQuad(dev, 170, 10, 32, 32, 2, D3DCOLOR_XRGB(255, 255, 255));
        IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9*)tex4444);
        drawQuad(dev, 206, 10, 32, 32, 2, D3DCOLOR_XRGB(255, 255, 255));
        IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9*)texL8);
        drawQuad(dev, 242, 10, 32, 32, 2, D3DCOLOR_XRGB(255, 255, 255));
        IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9*)texDXT);
        drawQuad(dev, 278, 10, 32, 32, 2, D3DCOLOR_XRGB(255, 255, 255));
        /* Render-Target-Textur */
        IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9*)texRT);
        drawQuad(dev, 134, 50, 64, 64, 2, D3DCOLOR_XRGB(255, 255, 255));

        /* Alpha-Blending über dem blauen Hintergrund und über dem Viereck */
        IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9*)texA);
        drawQuad(dev, 40, 60, 64, 64, 1, D3DCOLOR_XRGB(255, 255, 255));
        /* Alphatest: rechte Hälfte (Alpha 96) verschwindet */
        IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHATESTENABLE, TRUE);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHAFUNC, D3DCMP_GREATER);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHAREF, 128);
        drawQuad(dev, 210, 50, 64, 64, 1, D3DCOLOR_XRGB(255, 255, 255));
        IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHATESTENABLE, FALSE);

        /* Nebel (Vertex-Nebel, linear) über ein Viereck in der Tiefe */
        IDirect3DDevice9_SetRenderState(dev, D3DRS_FOGENABLE, TRUE);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_FOGCOLOR, D3DCOLOR_XRGB(200, 200, 200));
        IDirect3DDevice9_SetRenderState(dev, D3DRS_FOGVERTEXMODE, D3DFOG_LINEAR);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_FOGTABLEMODE, D3DFOG_NONE);
        float fogStart = 0.0f, fogEnd = 8.0f;
        IDirect3DDevice9_SetRenderState(dev, D3DRS_FOGSTART, *(DWORD*)&fogStart);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_FOGEND, *(DWORD*)&fogEnd);
        IDirect3DDevice9_SetTexture(dev, 0, NULL);
        drawQuad(dev, 10, 130, 60, 40, 4, D3DCOLOR_XRGB(255, 0, 0));
        IDirect3DDevice9_SetRenderState(dev, D3DRS_FOGENABLE, FALSE);

        /* Shader-Viereck (bewegt sich, damit die Bildfolge lebt) */
        if (vs && ps) {
            float m[16]; D3DMATRIX mvp;
            /* Welt*View*Proj in Spaltenreihenfolge für m4x4 (c0..c3 = Zeilen der transponierten Matrix) */
            D3DMATRIX wv; memset(&wv, 0, sizeof(wv));
            for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { float s = 0; for (int k = 0; k < 4; k++) s += view.m[i][k] * proj.m[k][j]; wv.m[i][j] = s; }
            for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) m[i * 4 + j] = wv.m[j][i];
            memcpy(&mvp, m, sizeof(m));
            IDirect3DDevice9_SetVertexShader(dev, vs);
            IDirect3DDevice9_SetPixelShader(dev, ps);
            IDirect3DDevice9_SetVertexShaderConstantF(dev, 0, (const float*)&mvp, 4);
            IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9*)tex565);
            drawQuad(dev, 100 + t * 40, 140, 64, 64, 3, D3DCOLOR_XRGB(128, 255, 128));
            IDirect3DDevice9_SetVertexShader(dev, NULL);
            IDirect3DDevice9_SetPixelShader(dev, NULL);
        }

        /* Linien und Punkte */
        IDirect3DDevice9_SetTexture(dev, 0, NULL);
        struct Vertex lines[4] = { {200, 150, 1, 0,0,-1, D3DCOLOR_XRGB(255,255,0), 0,0}, {300, 220, 1, 0,0,-1, D3DCOLOR_XRGB(0,255,255), 0,0},
                                   {300, 150, 1, 0,0,-1, D3DCOLOR_XRGB(255,255,255), 0,0}, {200, 220, 1, 0,0,-1, D3DCOLOR_XRGB(255,255,255), 0,0} };
        IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_LINELIST, 2, lines, sizeof(struct Vertex));

        IDirect3DDevice9_EndScene(dev);
        if (frame == frames - 1) writePPM(dev, out);
        IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
        frame++;
        if (frame >= frames) break;
    }
    printf("%d Bilder\n", frame);
    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
    return 0;
}
