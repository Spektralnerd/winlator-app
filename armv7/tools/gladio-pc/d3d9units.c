/*
 * d3d9units – drei Sampler wie der YUV-Shader der Bink-Videos (Outlast-Intro): s0, s1, s2 bekommen jedes Bild dieselben
 * Texturen (Rot, Grün, Blau), das Bild zeigt nur s0 (Rot). wined3d lässt nach dem Zeichnen die zuletzt benutzte
 * Textureinheit (2) aktiv; bei der Bildausgabe bindet die Anzeige-Seite ihre Fenstertextur (Texture.java). Legt sie die
 * Textur erst an (nach glXMakeCurrent: erstes Bild, Fenstergröße geändert), schaltet sie dabei auf Einheit 0 um – der
 * Server stellte danach die Textur der vermeintlich aktiven Einheit 2 (Blau) auf Einheit 0 her. Mit dem Bindungsfilter
 * des Clients (patch-gladio-client.py) wurde Rot nicht neu gebunden: das Bild blieb Blau (Intro: Y-Ebene durch
 * Chroma ersetzt, graues Bild). Erwartung: alle Bilder Rot (ff0000).
 *
 *   x86_64-w64-mingw32-gcc -O1 -o d3d9units64.exe d3d9units.c -ld3d9 -luser32
 *   wine d3d9units64.exe [bilder] [ausgabe.txt]
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 128
#define H 128

typedef HRESULT (WINAPI *PFN_D3DXAssembleShader)(const char* src, UINT len, const void* defines, void* include, DWORD flags, void** shader, void** errors);
static void* bufferPointer(void* buf) { return ((void* (WINAPI **)(void*))(*(void***)buf))[3](buf); }
static void bufferRelease(void* buf) { ((ULONG (WINAPI **)(void*))(*(void***)buf))[2](buf); }

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

struct Vertex { float x, y, z, rhw, u, v; };

static const char* vsSrc = NULL; /* feste Funktion (XYZRHW) */

/* ps_2_0: Farbe = s0 + 0 * (s1 + s2); s1 und s2 bleiben aktiv, damit wined3d sie bindet */
static const char* psSrc =
    "ps_2_0\n"
    "dcl t0.xy\n"
    "dcl_2d s0\n"
    "dcl_2d s1\n"
    "dcl_2d s2\n"
    "def c0, 0, 0, 0, 0\n"
    "texld r0, t0, s0\n"
    "texld r1, t0, s1\n"
    "texld r2, t0, s2\n"
    "add r1, r1, r2\n"
    "mul r1, r1, c0\n"
    "add r0, r0, r1\n"
    "mov oC0, r0\n";

static IDirect3DTexture9* solidTexture(IDirect3DDevice9* dev, DWORD argb) {
    IDirect3DTexture9* tex = NULL;
    if (FAILED(IDirect3DDevice9_CreateTexture(dev, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, NULL))) return NULL;
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(IDirect3DTexture9_LockRect(tex, 0, &lr, NULL, 0))) {
        for (int y = 0; y < 4; y++) {
            DWORD* row = (DWORD*)((char*)lr.pBits + y * lr.Pitch);
            for (int x = 0; x < 4; x++) row[x] = argb;
        }
        IDirect3DTexture9_UnlockRect(tex, 0);
    }
    return tex;
}

int main(int argc, char** argv)
{
    int frames = argc > 1 ? atoi(argv[1]) : 8;
    if (argc > 2) freopen(argv[2], "w", stdout);
    setvbuf(stdout, NULL, _IONBF, 0);

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndProc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "d3d9units";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("d3d9units", "d3d9units", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, W, H, NULL, NULL, wc.hInstance, NULL);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp = {0};
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = W; pp.BackBufferHeight = H; pp.hDeviceWindow = hwnd;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9* dev = NULL;
    if (FAILED(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev))) {
        printf("CreateDevice fehlgeschlagen\n");
        return 1;
    }
    HMODULE d3dx = LoadLibraryA("d3dx9_36.dll");
    PFN_D3DXAssembleShader assemble = d3dx ? (PFN_D3DXAssembleShader)GetProcAddress(d3dx, "D3DXAssembleShader") : NULL;
    if (!assemble) { printf("d3dx9_36 fehlt\n"); return 1; }
    IDirect3DPixelShader9* ps = NULL;
    void* code = NULL; void* err = NULL;
    if (FAILED(assemble(psSrc, strlen(psSrc), NULL, NULL, 0, &code, &err))) { printf("PS: %s\n", err ? (char*)bufferPointer(err) : "?"); return 1; }
    IDirect3DDevice9_CreatePixelShader(dev, bufferPointer(code), &ps); bufferRelease(code);
    if (!ps) { printf("Shader fehlgeschlagen\n"); return 1; }
    (void)vsSrc;

    IDirect3DTexture9* red = solidTexture(dev, 0xffff0000);
    IDirect3DTexture9* green = solidTexture(dev, 0xff00ff00);
    IDirect3DTexture9* blue = solidTexture(dev, 0xff0000ff);
    if (!red || !green || !blue) { printf("Texturen fehlgeschlagen\n"); return 1; }

    IDirect3DDevice9_SetPixelShader(dev, ps);
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_TEX1);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, D3DZB_FALSE);
    for (int i = 0; i < 3; i++) {
        IDirect3DDevice9_SetSamplerState(dev, i, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        IDirect3DDevice9_SetSamplerState(dev, i, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    }

    IDirect3DSurface9* sysmem = NULL;
    IDirect3DDevice9_CreateOffscreenPlainSurface(dev, W, H, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sysmem, NULL);

    int bad = 0;
    for (int frame = 0; frame < frames; frame++) {
        IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xff202040, 1.0f, 0);
        IDirect3DDevice9_BeginScene(dev);
        /* jedes Bild dieselben Texturen in derselben Reihenfolge: Einheit 2 bleibt danach aktiv */
        IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9*)red);
        IDirect3DDevice9_SetTexture(dev, 1, (IDirect3DBaseTexture9*)green);
        IDirect3DDevice9_SetTexture(dev, 2, (IDirect3DBaseTexture9*)blue);
        struct Vertex v[4] = {{0, 0, 0.5f, 1, 0, 0}, {W, 0, 0.5f, 1, 1, 0}, {0, H, 0.5f, 1, 0, 1}, {W, H, 0.5f, 1, 1, 1}};
        IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, v, sizeof(struct Vertex));
        IDirect3DDevice9_EndScene(dev);

        DWORD pixel = 0;
        IDirect3DSurface9* bb = NULL;
        if (sysmem && SUCCEEDED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) {
            if (SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(dev, bb, sysmem))) {
                D3DLOCKED_RECT lr;
                if (SUCCEEDED(IDirect3DSurface9_LockRect(sysmem, &lr, NULL, D3DLOCK_READONLY))) {
                    pixel = ((DWORD*)((char*)lr.pBits + (H / 2) * lr.Pitch))[W / 2] & 0xffffff;
                    IDirect3DSurface9_UnlockRect(sysmem);
                }
            }
            IDirect3DSurface9_Release(bb);
        }
        if (pixel != 0xff0000) bad++;
        printf("Bild %d: Pixel %06lx%s\n", frame, (unsigned long)pixel, pixel == 0xff0000 ? "" : " FALSCH (erwartet ff0000)");
        IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
    }
    printf("%d Bilder, %d falsch\n", frames, bad);
    return bad ? 2 : 0;
}
