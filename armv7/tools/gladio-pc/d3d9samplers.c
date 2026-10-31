/*
 * d3d9samplers – Pixel-Shader (ps_3_0) mit Samplern jenseits von s7. Direct3D 9 kennt 16 Pixel-Sampler
 * (s0..s15); wined3d bindet sie auf die GL-Textureinheiten 0..15, soweit der Treiber sie meldet
 * (GL_MAX_TEXTURE_IMAGE_UNITS, GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS). Meldete der Gladio-Server 8, blieben
 * s8..s15 ungebunden („Trying to load sampler ps_sampler8 on unsupported unit 8“) und lasen die Textur von
 * Einheit 0 – die Unreal Engine 3 legt die Eingänge ihrer Nachbearbeitung (Rauschen, Farbtabellen) dort ab
 * (Outlast: Nachtsicht ohne Rauschen, überstrahlt).
 *
 * Fünf Streifen, je ein Shader, der genau einen Sampler liest: s0 rot, s7 grün, s8 blau, s12 gelb,
 * s15 cyan; auf allen anderen Einheiten liegt eine weiße Textur. Erwartet die fünf Farben.
 *
 *   x86_64-w64-mingw32-gcc -O1 -o d3d9samplers64.exe d3d9samplers.c -ld3d9 -luser32
 *   i686-w64-mingw32-gcc -O1 -o d3d9samplers.exe d3d9samplers.c -ld3d9 -luser32
 *   wine d3d9samplers64.exe [ausgabe.txt]
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define W 200
#define H 64

typedef HRESULT (WINAPI *PFN_D3DXAssembleShader)(const char* src, UINT len, const void* defines, void* include, DWORD flags, void** shader, void** errors);
static void* bufferPointer(void* buf) { return ((void* (WINAPI **)(void*))(*(void***)buf))[3](buf); }

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

static void* assemble(PFN_D3DXAssembleShader fn, const char* src) {
    void* code = NULL; void* err = NULL;
    if (FAILED(fn(src, strlen(src), NULL, NULL, 0, &code, &err))) {
        printf("Shader: %s\n", err ? (char*)bufferPointer(err) : "?");
        exit(1);
    }
    return bufferPointer(code);
}

struct Vertex { float x, y, z, w, u, v; };

static void quad(IDirect3DDevice9* dev, float x0, float y0, float x1, float y1)
{
    float ax = x0 / W * 2 - 1, bx = x1 / W * 2 - 1, ay = 1 - y0 / H * 2, by = 1 - y1 / H * 2;
    struct Vertex v[4] = {{ax, ay, 0.5f, 1, 0, 0}, {bx, ay, 0.5f, 1, 1, 0}, {ax, by, 0.5f, 1, 0, 1}, {bx, by, 0.5f, 1, 1, 1}};
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, v, sizeof(struct Vertex));
}

static IDirect3DTexture9* solidTexture(IDirect3DDevice9* dev, DWORD argb)
{
    IDirect3DTexture9* tex = NULL;
    if (FAILED(IDirect3DDevice9_CreateTexture(dev, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, NULL))) { printf("CreateTexture fehlgeschlagen\n"); exit(1); }
    D3DLOCKED_RECT lr;
    IDirect3DTexture9_LockRect(tex, 0, &lr, NULL, 0);
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) ((DWORD*)((char*)lr.pBits + y * lr.Pitch))[x] = argb;
    IDirect3DTexture9_UnlockRect(tex, 0);
    return tex;
}

int main(int argc, char** argv)
{
    if (argc > 1) freopen(argv[1], "w", stdout);
    setvbuf(stdout, NULL, _IONBF, 0);

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndProc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "d3d9samplers";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("d3d9samplers", "d3d9samplers", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, W, H, NULL, NULL, wc.hInstance, NULL);

    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp = {0};
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = W; pp.BackBufferHeight = H; pp.hDeviceWindow = hwnd;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9* dev = NULL;
    HRESULT hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr)) { printf("CreateDevice fehlgeschlagen: %08lx\n", (unsigned long)hr); return 1; }

    D3DCAPS9 caps;
    IDirect3DDevice9_GetDeviceCaps(dev, &caps);
    printf("MaxSimultaneousTextures %lu, PixelShaderVersion %lx, VertexShaderVersion %lx\n",
           (unsigned long)caps.MaxSimultaneousTextures, (unsigned long)caps.PixelShaderVersion, (unsigned long)caps.VertexShaderVersion);

    HMODULE d3dx = LoadLibraryA("d3dx9_36.dll");
    PFN_D3DXAssembleShader asmFn = d3dx ? (PFN_D3DXAssembleShader)GetProcAddress(d3dx, "D3DXAssembleShader") : NULL;
    if (!asmFn) { printf("d3dx9_36 fehlt\n"); return 1; }

    const char* vsSrc =
        "vs_3_0\n"
        "dcl_position v0\n"
        "dcl_texcoord0 v1\n"
        "dcl_position o0\n"
        "dcl_texcoord0 o1\n"
        "mov o0, v0\n"
        "mov o1, v1\n";
    IDirect3DVertexShader9* vs = NULL;
    IDirect3DDevice9_CreateVertexShader(dev, assemble(asmFn, vsSrc), &vs);

    static const struct { int sampler; DWORD color; const char* name; } cases[] = {
        {0, 0xffff0000, "s0 rot"}, {7, 0xff00ff00, "s7 grün"}, {8, 0xff0000ff, "s8 blau"},
        {12, 0xffffff00, "s12 gelb"}, {15, 0xff00ffff, "s15 cyan"},
    };
    const int n = sizeof cases / sizeof cases[0];
    IDirect3DPixelShader9* ps[5];
    for (int i = 0; i < n; i++) {
        char src[256];
        snprintf(src, sizeof src, "ps_3_0\ndcl_2d s%d\ndcl_texcoord0 v0.xy\ntexld r0, v0, s%d\nmov oC0, r0\n", cases[i].sampler, cases[i].sampler);
        IDirect3DDevice9_CreatePixelShader(dev, assemble(asmFn, src), &ps[i]);
    }

    IDirect3DTexture9* white = solidTexture(dev, 0xffffffff);
    for (int s = 0; s < 16; s++) {
        IDirect3DDevice9_SetTexture(dev, s, (IDirect3DBaseTexture9*)white);
        IDirect3DDevice9_SetSamplerState(dev, s, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        IDirect3DDevice9_SetSamplerState(dev, s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    }
    for (int i = 0; i < n; i++) IDirect3DDevice9_SetTexture(dev, cases[i].sampler, (IDirect3DBaseTexture9*)solidTexture(dev, cases[i].color));

    D3DVERTEXELEMENT9 elements[] = {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()};
    IDirect3DVertexDeclaration9* decl = NULL;
    IDirect3DDevice9_CreateVertexDeclaration(dev, elements, &decl);

    IDirect3DSurface9 *backbuffer = NULL, *sysmem = NULL;
    IDirect3DDevice9_GetRenderTarget(dev, 0, &backbuffer);
    IDirect3DDevice9_CreateOffscreenPlainSurface(dev, W, H, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sysmem, NULL);

    IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, D3DZB_FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetVertexDeclaration(dev, decl);
    IDirect3DDevice9_SetVertexShader(dev, vs);
    for (int frame = 0; frame < 2; frame++) {
        IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xff808080, 1.0f, 0);
        IDirect3DDevice9_BeginScene(dev);
        for (int i = 0; i < n; i++) {
            IDirect3DDevice9_SetPixelShader(dev, ps[i]);
            quad(dev, i * (W / n), 0, (i + 1) * (W / n), H);
        }
        IDirect3DDevice9_EndScene(dev);
        if (frame == 0) IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
    }

    IDirect3DDevice9_GetRenderTargetData(dev, backbuffer, sysmem);
    D3DLOCKED_RECT lr;
    IDirect3DSurface9_LockRect(sysmem, &lr, NULL, D3DLOCK_READONLY);
    int fails = 0;
    for (int i = 0; i < n; i++) {
        int x = i * (W / n) + W / n / 2, y = H / 2;
        DWORD c = ((DWORD*)((char*)lr.pBits + y * lr.Pitch))[x] & 0xffffff;
        DWORD expect = cases[i].color & 0xffffff;
        int ok = c == expect;
        printf("%-10s %06lx (erwartet %06lx)%s\n", cases[i].name, (unsigned long)c, (unsigned long)expect, ok ? "" : "  <-- FALSCH");
        if (!ok) fails++;
    }
    IDirect3DSurface9_UnlockRect(sysmem);
    printf("%s\n", fails ? "FEHLER" : "ok");
    return fails ? 1 : 0;
}
