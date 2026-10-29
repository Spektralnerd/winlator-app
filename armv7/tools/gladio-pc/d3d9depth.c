/*
 * d3d9depth – Tiefe aus einer INTZ-Textur lesen, wie die Unreal Engine 3 die Szenentiefe für
 * Tiefenunschärfe, Nebel und Bewegungsunschärfe liest (D3D9-Erweiterung „INTZ“: Tiefen-Stencil-Textur,
 * die sich auch als Textur abtasten lässt).
 *
 * 1. Zeichnet in ein Farbziel mit INTZ als Tiefenpuffer: links Tiefe 0,25, rechts 0,75.
 * 2. Liest die INTZ-Textur in einem Pixel-Shader und schreibt die Tiefe als Grauwert in den
 *    Hintergrundpuffer, einmal mit Punktfilter (oben), einmal bilinear (unten).
 * Erwartet: oben links/rechts etwa 64/191, unten ebenso. Unter OpenGL ES ist eine Tiefentextur mit
 * linearem Filter und ohne Vergleichsmodus unvollständig (liefert 0) – auf dem PC unter GL egal.
 *
 *   i686-w64-mingw32-gcc -O1 -o d3d9depth.exe d3d9depth.c -ld3d9 -luser32
 *   wine d3d9depth.exe [ausgabe.txt]
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define W 256
#define H 256
#define FOURCC_INTZ ((D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z'))

typedef HRESULT (WINAPI *PFN_D3DXAssembleShader)(const char* src, UINT len, const void* defines, void* include, DWORD flags, void** shader, void** errors);
static void* bufferPointer(void* buf) { return ((void* (WINAPI **)(void*))(*(void***)buf))[3](buf); }

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

struct Vertex { float x, y, z, rhw; float u, v; };

static void rect(IDirect3DDevice9* dev, float x0, float y0, float x1, float y1, float z)
{
    struct Vertex v[4] = {
        {x0 - 0.5f, y0 - 0.5f, z, 1.0f, x0 / W, y0 / H}, {x1 - 0.5f, y0 - 0.5f, z, 1.0f, x1 / W, y0 / H},
        {x0 - 0.5f, y1 - 0.5f, z, 1.0f, x0 / W, y1 / H}, {x1 - 0.5f, y1 - 0.5f, z, 1.0f, x1 / W, y1 / H},
    };
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, v, sizeof(struct Vertex));
}

int main(int argc, char** argv)
{
    if (argc > 1) freopen(argv[1], "w", stdout);
    setvbuf(stdout, NULL, _IONBF, 0);

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndProc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "d3d9depth";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("d3d9depth", "d3d9depth", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, W, H, NULL, NULL, wc.hInstance, NULL);

    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    HRESULT hr = IDirect3D9_CheckDeviceFormat(d3d, 0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, FOURCC_INTZ);
    printf("INTZ als Tiefen-Textur: %s\n", hr == D3D_OK ? "ja" : "nein");
    D3DPRESENT_PARAMETERS pp = {0};
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = W; pp.BackBufferHeight = H; pp.hDeviceWindow = hwnd;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9* dev = NULL;
    hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr)) { printf("CreateDevice fehlgeschlagen: %08lx\n", (unsigned long)hr); return 1; }

    IDirect3DTexture9 *depthTex = NULL, *colorTex = NULL;
    hr = IDirect3DDevice9_CreateTexture(dev, W, H, 1, D3DUSAGE_DEPTHSTENCIL, FOURCC_INTZ, D3DPOOL_DEFAULT, &depthTex, NULL);
    if (FAILED(hr)) { printf("CreateTexture INTZ fehlgeschlagen: %08lx\n", (unsigned long)hr); return 1; }
    IDirect3DDevice9_CreateTexture(dev, W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &colorTex, NULL);
    IDirect3DSurface9 *depthSurf = NULL, *colorSurf = NULL, *backbuffer = NULL, *sysmem = NULL;
    IDirect3DTexture9_GetSurfaceLevel(depthTex, 0, &depthSurf);
    IDirect3DTexture9_GetSurfaceLevel(colorTex, 0, &colorSurf);
    IDirect3DDevice9_GetRenderTarget(dev, 0, &backbuffer);
    IDirect3DDevice9_CreateOffscreenPlainSurface(dev, W, H, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sysmem, NULL);

    HMODULE d3dx = LoadLibraryA("d3dx9_36.dll");
    PFN_D3DXAssembleShader asmFn = d3dx ? (PFN_D3DXAssembleShader)GetProcAddress(d3dx, "D3DXAssembleShader") : NULL;
    if (!asmFn) { printf("d3dx9_36 fehlt\n"); return 1; }
    /* Tiefe (.r der INTZ-Textur) als Grauwert; .a zur Kontrolle in Alpha */
    const char* psSrc = "ps_3_0\ndcl_texcoord0 v0.xy\ndcl_2d s0\ntexld r0, v0, s0\nmov oC0, r0.xxxw\n";
    void* code = NULL; void* err = NULL;
    if (FAILED(asmFn(psSrc, strlen(psSrc), NULL, NULL, 0, &code, &err))) { printf("PS: %s\n", err ? (char*)bufferPointer(err) : "?"); return 1; }
    IDirect3DPixelShader9* ps = NULL;
    IDirect3DDevice9_CreatePixelShader(dev, bufferPointer(code), &ps);

    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_TEX1);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    for (int frame = 0; frame < 2; frame++) {
        IDirect3DDevice9_BeginScene(dev);
        /* 1. Tiefe schreiben */
        IDirect3DDevice9_SetRenderTarget(dev, 0, colorSurf);
        IDirect3DDevice9_SetDepthStencilSurface(dev, depthSurf);
        IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0xff000000, 1.0f, 0);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, D3DZB_TRUE);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_ZWRITEENABLE, TRUE);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_ZFUNC, D3DCMP_ALWAYS);
        IDirect3DDevice9_SetPixelShader(dev, NULL);
        rect(dev, 0, 0, W / 2, H, 0.25f);
        rect(dev, W / 2, 0, W, H, 0.75f);
        /* 2. Tiefe lesen (Tiefenpuffer lösen, wie die Engine vor dem Lesen) */
        IDirect3DDevice9_SetRenderTarget(dev, 0, backbuffer);
        IDirect3DDevice9_SetDepthStencilSurface(dev, NULL);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, D3DZB_FALSE);
        IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xffff00ff, 1.0f, 0);
        IDirect3DDevice9_SetPixelShader(dev, ps);
        IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9*)depthTex);
        IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        rect(dev, 0, 0, W, H / 2, 0.5f);
        IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        rect(dev, 0, H / 2, W, H, 0.5f);
        IDirect3DDevice9_SetTexture(dev, 0, NULL);
        IDirect3DDevice9_EndScene(dev);
        if (frame == 0) IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
    }

    IDirect3DDevice9_GetRenderTargetData(dev, backbuffer, sysmem);
    D3DLOCKED_RECT lr;
    IDirect3DSurface9_LockRect(sysmem, &lr, NULL, D3DLOCK_READONLY);
    static const struct { int x, y; const char* name; int expect; } probes[] = {
        {W / 4, H / 4, "Punktfilter, links (0,25)", 64}, {3 * W / 4, H / 4, "Punktfilter, rechts (0,75)", 191},
        {W / 4, 3 * H / 4, "bilinear, links (0,25)", 64}, {3 * W / 4, 3 * H / 4, "bilinear, rechts (0,75)", 191},
    };
    int fails = 0;
    for (int i = 0; i < 4; i++) {
        DWORD c = ((DWORD*)((char*)lr.pBits + probes[i].y * lr.Pitch))[probes[i].x];
        int r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff;
        int ok = abs(r - probes[i].expect) <= 3 && r == g && g == b;
        printf("%-28s %3d/%3d/%3d (erwartet %d)%s\n", probes[i].name, r, g, b, probes[i].expect, ok ? "" : "  <-- FALSCH");
        if (!ok) fails++;
    }
    IDirect3DSurface9_UnlockRect(sysmem);
    printf("%s\n", fails ? "FEHLER" : "ok");
    return fails ? 1 : 0;
}
