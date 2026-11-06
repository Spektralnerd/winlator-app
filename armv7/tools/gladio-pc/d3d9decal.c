/*
 * d3d9decal – Aufkleber auf geneigtem Boden mit Alphatest (D3DTLVERTEX, ZFUNC LESSEQUAL), wie Tiny Tigers Blumen:
 * der Aufkleber liegt rechnerisch 1,5 Tiefeneinheiten (24 Bit) HINTER dem Boden – so weit daneben, wie ein Rasterer
 * bei deckungsgleichen Flächen rundet. Ohne Tiefenversatz für verwerfende Shader (decal_bias.c) gewinnt der Boden
 * (grün), mit Versatz der Aufkleber (rot). Erwartet: Mitte rot, Rand grün.
 *
 * Bauen:  x86_64-w64-mingw32-gcc -O1 -o d3d9decal64.exe d3d9decal.c -ld3d9 -lgdi32 -luser32
 * Aufruf: wine d3d9decal64.exe [ausgabe.txt]
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

#define SIZE 128

struct Vertex { float x, y, z, rhw; DWORD color; };
#define FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

/* Tiefe des Bodens: geneigt, 0.3 oben bis 0.7 unten */
static float groundZ(float y) { return 0.3f + 0.4f * (y / SIZE); }

static void quad(IDirect3DDevice9* dev, float x0, float y0, float x1, float y1, float dz, DWORD color) {
    struct Vertex v[4] = {
        {x0 - 0.5f, y0 - 0.5f, groundZ(y0) + dz, 1.0f, color},
        {x1 - 0.5f, y0 - 0.5f, groundZ(y0) + dz, 1.0f, color},
        {x0 - 0.5f, y1 - 0.5f, groundZ(y1) + dz, 1.0f, color},
        {x1 - 0.5f, y1 - 0.5f, groundZ(y1) + dz, 1.0f, color},
    };
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, v, sizeof(struct Vertex));
}

int main(int argc, char** argv) {
    if (argc > 1) freopen(argv[1], "w", stdout);
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndProc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "d3d9decal";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("d3d9decal", "d3d9decal", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, SIZE, SIZE, NULL, NULL, wc.hInstance, NULL);

    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { printf("Direct3DCreate9 fehlgeschlagen\n"); return 1; }
    D3DPRESENT_PARAMETERS pp = {0};
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = SIZE; pp.BackBufferHeight = SIZE; pp.hDeviceWindow = hwnd;
    pp.EnableAutoDepthStencil = TRUE; pp.AutoDepthStencilFormat = D3DFMT_D24X8;
    IDirect3DDevice9* dev = NULL;
    HRESULT hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr)) { printf("CreateDevice fehlgeschlagen: %lx\n", (long)hr); return 1; }

    IDirect3DTexture9* tex = NULL;
    IDirect3DDevice9_CreateTexture(dev, SIZE, SIZE, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex, NULL);
    IDirect3DSurface9* rt = NULL;
    IDirect3DTexture9_GetSurfaceLevel(tex, 0, &rt);
    IDirect3DDevice9_SetRenderTarget(dev, 0, rt);

    IDirect3DDevice9_SetFVF(dev, FVF);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, D3DZB_TRUE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZWRITEENABLE, TRUE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);

    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff0000ff, 1.0f, 0);
    /* Boden: grün, ohne Alphatest */
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHATESTENABLE, FALSE);
    quad(dev, 0, 0, SIZE, SIZE, 0.0f, 0xff00ff00);
    /* Aufkleber: rot, Alphatest an (wined3d: discard im Shader), 1,5 Einheiten hinter dem Boden */
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHATESTENABLE, TRUE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHAFUNC, D3DCMP_GREATER);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHAREF, 128);
    quad(dev, 32, 32, 96, 96, 1.5f / 16777216.0f, 0xffff0000);
    IDirect3DDevice9_EndScene(dev);

    IDirect3DSurface9* sysmem = NULL;
    IDirect3DDevice9_CreateOffscreenPlainSurface(dev, SIZE, SIZE, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sysmem, NULL);
    hr = IDirect3DDevice9_GetRenderTargetData(dev, rt, sysmem);
    if (FAILED(hr)) { printf("GetRenderTargetData fehlgeschlagen: %lx\n", (long)hr); return 1; }
    D3DLOCKED_RECT lr;
    IDirect3DSurface9_LockRect(sysmem, &lr, NULL, D3DLOCK_READONLY);
    int bad = 0;
    const struct { int x, y; const char* name; DWORD want; } probes[] = {
        {64, 64, "Mitte (Aufkleber)", 0xff0000}, {40, 40, "Aufkleber links oben", 0xff0000}, {90, 90, "Aufkleber rechts unten", 0xff0000},
        {8, 8, "Rand oben", 0x00ff00}, {120, 120, "Rand unten", 0x00ff00},
    };
    for (int i = 0; i < 5; i++) {
        DWORD p = ((DWORD*)((char*)lr.pBits + probes[i].y * lr.Pitch))[probes[i].x] & 0xffffff;
        int ok = p == probes[i].want;
        if (!ok) bad++;
        printf("%s: %06lx %s\n", probes[i].name, (unsigned long)p, ok ? "ok" : "FALSCH");
    }
    IDirect3DSurface9_UnlockRect(sysmem);
    printf("%s\n", bad ? "FEHLER" : "alles ok");
    fflush(stdout);
    return bad ? 1 : 0;
}
