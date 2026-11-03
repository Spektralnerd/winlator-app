/*
 * d3d9mips – Mipmap-Kette eines Render-Targets Stufe für Stufe zeichnen, wie Outlast (Whistleblower) für den
 * Laptop-Bildschirm: Stufe 0 wird gerendert, danach ist je Stufe N die Stufe N als Renderziel gesetzt und dieselbe
 * Textur mit D3DSAMP_MAXMIPLEVEL = N−1 als Quelle gebunden (DrawPrimitiveUP eines Vierecks). wined3d lässt solche
 * Zeichenaufrufe weg ("Sampling attached render targets is not supported, skipping draw"), wenn der GL-Treiber
 * GL_ARB_texture_barrier nicht anbietet – die Stufen bleiben dann schwarz, und aus der Ferne (kleine Mipmap-Stufen)
 * ist die Textur schwarz. Liest alle Stufen zurück; erwartet überall (255,128,64).
 *
 * Bauen (MinGW):  x86_64-w64-mingw32-gcc -O1 -o d3d9mips64.exe d3d9mips.c -ld3d9 -lgdi32 -luser32
 * Aufruf:         wine d3d9mips64.exe [ausgabe.txt]
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

#define SIZE 256
#define LEVELS 9

typedef HRESULT (WINAPI *PFN_D3DXAssembleShader)(const char* src, UINT len, const void* defines, void* include, DWORD flags, void** shader, void** errors);
static void* bufferPointer(void* buf) { return ((void* (WINAPI **)(void*))(*(void***)buf))[3](buf); }
static void bufferRelease(void* buf) { ((ULONG (WINAPI **)(void*))(*(void***)buf))[2](buf); }

struct Vertex { float x, y, z, rhw; float u, v; };
#define FVF (D3DFVF_XYZRHW | D3DFVF_TEX1)

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

static void drawRect(IDirect3DDevice9* dev, float x0, float y0, float x1, float y1) {
    struct Vertex v[4] = {
        {x0 - 0.5f, y0 - 0.5f, 0.5f, 1.0f, 0.0f, 0.0f},
        {x1 - 0.5f, y0 - 0.5f, 0.5f, 1.0f, 1.0f, 0.0f},
        {x0 - 0.5f, y1 - 0.5f, 0.5f, 1.0f, 0.0f, 1.0f},
        {x1 - 0.5f, y1 - 0.5f, 0.5f, 1.0f, 1.0f, 1.0f},
    };
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, v, sizeof(struct Vertex));
}

static IDirect3DPixelShader9* assemblePS(IDirect3DDevice9* dev, PFN_D3DXAssembleShader assemble, const char* src) {
    void* code = NULL; void* err = NULL;
    IDirect3DPixelShader9* ps = NULL;
    if (SUCCEEDED(assemble(src, strlen(src), NULL, NULL, 0, &code, &err))) {
        IDirect3DDevice9_CreatePixelShader(dev, bufferPointer(code), &ps);
        bufferRelease(code);
    }
    else printf("PS-Assemblierung fehlgeschlagen: %s\n", err ? (char*)bufferPointer(err) : "?");
    return ps;
}

int main(int argc, char** argv) {
    if (argc > 1) freopen(argv[1], "w", stdout);
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndProc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "d3d9mips";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("d3d9mips", "d3d9mips", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, SIZE, SIZE, NULL, NULL, wc.hInstance, NULL);

    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { printf("Direct3DCreate9 fehlgeschlagen\n"); return 1; }
    D3DPRESENT_PARAMETERS pp = {0};
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = SIZE; pp.BackBufferHeight = SIZE; pp.hDeviceWindow = hwnd; pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9* dev = NULL;
    HRESULT hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr)) { printf("CreateDevice fehlgeschlagen: %lx\n", (long)hr); return 1; }

    IDirect3DTexture9* tex = NULL;
    hr = IDirect3DDevice9_CreateTexture(dev, SIZE, SIZE, LEVELS, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex, NULL);
    if (FAILED(hr)) { printf("CreateTexture fehlgeschlagen: %lx\n", (long)hr); return 1; }
    printf("Stufen: %lu\n", (unsigned long)IDirect3DTexture9_GetLevelCount(tex));

    HMODULE d3dx = LoadLibraryA("d3dx9_36.dll");
    PFN_D3DXAssembleShader assemble = d3dx ? (PFN_D3DXAssembleShader)GetProcAddress(d3dx, "D3DXAssembleShader") : NULL;
    if (!assemble) { printf("d3dx9_36 / D3DXAssembleShader nicht verfügbar\n"); return 1; }
    IDirect3DPixelShader9* psConst = assemblePS(dev, assemble, "ps_2_0\nmov oC0, c0\n");
    IDirect3DPixelShader9* psCopy = assemblePS(dev, assemble, "ps_2_0\ndcl t0.xy\ndcl_2d s0\ntexld r0, t0, s0\nmov oC0, r0\n");
    if (!psConst || !psCopy) return 1;

    IDirect3DSurface9* backbuffer = NULL;
    IDirect3DDevice9_GetRenderTarget(dev, 0, &backbuffer);
    IDirect3DDevice9_SetFVF(dev, FVF);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

    IDirect3DDevice9_BeginScene(dev);
    IDirect3DSurface9* level0 = NULL;
    IDirect3DTexture9_GetSurfaceLevel(tex, 0, &level0);
    IDirect3DDevice9_SetRenderTarget(dev, 0, level0);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0x00000000, 1.0f, 0);
    IDirect3DDevice9_SetPixelShader(dev, psConst);
    const float color[4] = {1.0f, 0.5f, 0.25f, 1.0f};
    IDirect3DDevice9_SetPixelShaderConstantF(dev, 0, color, 1);
    drawRect(dev, 0, 0, SIZE, SIZE);
    IDirect3DSurface9_Release(level0);

    /* Stufe N aus Stufe N−1: Textur ist zugleich Quelle (MAXMIPLEVEL = N−1) und Ziel (Stufe N) */
    IDirect3DDevice9_SetPixelShader(dev, psCopy);
    for (int n = 1; n < LEVELS; n++) {
        IDirect3DSurface9* level = NULL;
        IDirect3DTexture9_GetSurfaceLevel(tex, n, &level);
        IDirect3DDevice9_SetRenderTarget(dev, 0, level);
        IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9*)tex);
        IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAXMIPLEVEL, n - 1);
        int size = SIZE >> n;
        drawRect(dev, 0, 0, (float)size, (float)size);
        IDirect3DDevice9_SetTexture(dev, 0, NULL);
        IDirect3DSurface9_Release(level);
    }
    IDirect3DDevice9_SetRenderTarget(dev, 0, backbuffer);
    IDirect3DDevice9_EndScene(dev);
    IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);

    int bad = 0;
    for (int n = 0; n < LEVELS; n++) {
        int size = SIZE >> n;
        IDirect3DSurface9 *level = NULL, *sysmem = NULL;
        IDirect3DTexture9_GetSurfaceLevel(tex, n, &level);
        IDirect3DDevice9_CreateOffscreenPlainSurface(dev, size, size, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sysmem, NULL);
        hr = IDirect3DDevice9_GetRenderTargetData(dev, level, sysmem);
        if (FAILED(hr)) { printf("Stufe %d: GetRenderTargetData fehlgeschlagen: %lx\n", n, (long)hr); bad++; continue; }
        D3DLOCKED_RECT lr;
        IDirect3DSurface9_LockRect(sysmem, &lr, NULL, D3DLOCK_READONLY);
        DWORD p = ((DWORD*)((char*)lr.pBits + (size / 2) * lr.Pitch))[size / 2];
        int r = (p >> 16) & 0xff, g = (p >> 8) & 0xff, b = p & 0xff;
        int ok = r > 240 && g > 118 && g < 138 && b > 54 && b < 74;
        if (!ok) bad++;
        printf("Stufe %d (%dx%d): (%d,%d,%d) %s\n", n, size, size, r, g, b, ok ? "ok" : "FALSCH (erwartet 255,128,64)");
        IDirect3DSurface9_UnlockRect(sysmem);
        IDirect3DSurface9_Release(sysmem);
        IDirect3DSurface9_Release(level);
    }
    printf("%s\n", bad ? "FEHLER" : "alle Stufen ok");
    fflush(stdout);

    IDirect3DSurface9_Release(backbuffer);
    IDirect3DTexture9_Release(tex);
    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
    return bad ? 1 : 0;
}
