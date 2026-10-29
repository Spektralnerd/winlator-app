/*
 * d3d9link – ein Vertex-Shader (vs_3_0) mit zwei Pixel-Shadern (ps_3_0), die verschiedene Ausgaben
 * des Vertex-Shaders lesen. wined3d übersetzt den Vertex-Shader einmal und hängt je Programm einen
 * eigenen kleinen Vertex-Shader mit setup_vs_output() an, der die Ausgaben nach den Eingängen des
 * Pixel-Shaders ordnet (in GL dürfen mehrere Shader derselben Stufe in einem Programm stecken).
 * Gladio fügt beides für OpenGL ES zu einem Shader zusammen – das darf nicht im gemeinsamen
 * Vertex-Shader geschehen, sonst bekommt das zweite Programm die Zuordnung des ersten (Outlast: die
 * letzte Nachbearbeitung las statt der Pixelposition die Clip-Koordinaten, das ganze Bild bekam die
 * Farbe eines Eckpixels).
 *
 * Links: Pixel-Shader A liest TEXCOORD0 (rot), rechts: B liest TEXCOORD1 (grün), Mitte unten: C liest
 * TEXCOORD2 (blau) – erwartet rot/grün/blau.
 *
 *   i686-w64-mingw32-gcc -O1 -o d3d9link.exe d3d9link.c -ld3d9 -luser32
 *   wine d3d9link.exe [ausgabe.txt]
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define W 192
#define H 128

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

struct Vertex { float x, y, z, w; };

static void quad(IDirect3DDevice9* dev, float x0, float y0, float x1, float y1)
{
    /* Clip-Koordinaten aus Pixeln */
    float ax = x0 / W * 2 - 1, bx = x1 / W * 2 - 1, ay = 1 - y0 / H * 2, by = 1 - y1 / H * 2;
    struct Vertex v[4] = {{ax, ay, 0.5f, 1}, {bx, ay, 0.5f, 1}, {ax, by, 0.5f, 1}, {bx, by, 0.5f, 1}};
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, v, sizeof(struct Vertex));
}

int main(int argc, char** argv)
{
    if (argc > 1) freopen(argv[1], "w", stdout);
    setvbuf(stdout, NULL, _IONBF, 0);

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndProc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "d3d9link";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("d3d9link", "d3d9link", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, W, H, NULL, NULL, wc.hInstance, NULL);

    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp = {0};
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = W; pp.BackBufferHeight = H; pp.hDeviceWindow = hwnd;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9* dev = NULL;
    HRESULT hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr)) { printf("CreateDevice fehlgeschlagen: %08lx\n", (unsigned long)hr); return 1; }

    HMODULE d3dx = LoadLibraryA("d3dx9_36.dll");
    PFN_D3DXAssembleShader asmFn = d3dx ? (PFN_D3DXAssembleShader)GetProcAddress(d3dx, "D3DXAssembleShader") : NULL;
    if (!asmFn) { printf("d3dx9_36 fehlt\n"); return 1; }

    /* drei Ausgaben in drei Farben, die Position zuletzt (wie bei der Unreal Engine 3 nicht in o0) */
    const char* vsSrc =
        "vs_3_0\n"
        "def c0, 1, 0, 0, 1\n"
        "def c1, 0, 1, 0, 1\n"
        "def c2, 0, 0, 1, 1\n"
        "dcl_position v0\n"
        "dcl_texcoord0 o0\n"
        "dcl_texcoord1 o1\n"
        "dcl_texcoord2 o2\n"
        "dcl_position o3\n"
        "mov o0, c0\n"
        "mov o1, c1\n"
        "mov o2, c2\n"
        "mov o3, v0\n";
    const char* psA = "ps_3_0\ndcl_texcoord0 v0\nmov oC0, v0\n";
    const char* psB = "ps_3_0\ndcl_texcoord1 v0\nmov oC0, v0\n";
    const char* psC = "ps_3_0\ndcl_texcoord2 v0\nmov oC0, v0\n";
    IDirect3DVertexShader9* vs = NULL;
    IDirect3DPixelShader9 *pa = NULL, *pb = NULL, *pc = NULL;
    IDirect3DDevice9_CreateVertexShader(dev, assemble(asmFn, vsSrc), &vs);
    IDirect3DDevice9_CreatePixelShader(dev, assemble(asmFn, psA), &pa);
    IDirect3DDevice9_CreatePixelShader(dev, assemble(asmFn, psB), &pb);
    IDirect3DDevice9_CreatePixelShader(dev, assemble(asmFn, psC), &pc);

    D3DVERTEXELEMENT9 elements[] = {{0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0}, D3DDECL_END()};
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
        IDirect3DDevice9_SetPixelShader(dev, pa);
        quad(dev, 0, 0, W / 2, H / 2);
        IDirect3DDevice9_SetPixelShader(dev, pb);
        quad(dev, W / 2, 0, W, H / 2);
        IDirect3DDevice9_SetPixelShader(dev, pc);
        quad(dev, W / 4, H / 2, 3 * W / 4, H);
        IDirect3DDevice9_EndScene(dev);
        if (frame == 0) IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
    }

    IDirect3DDevice9_GetRenderTargetData(dev, backbuffer, sysmem);
    D3DLOCKED_RECT lr;
    IDirect3DSurface9_LockRect(sysmem, &lr, NULL, D3DLOCK_READONLY);
    static const struct { int x, y; const char* name; DWORD expect; } probes[] = {
        {W / 4, H / 4, "A liest TEXCOORD0 (rot)", 0xff0000}, {3 * W / 4, H / 4, "B liest TEXCOORD1 (grün)", 0x00ff00},
        {W / 2, 3 * H / 4, "C liest TEXCOORD2 (blau)", 0x0000ff},
    };
    int fails = 0;
    for (int i = 0; i < 3; i++) {
        DWORD c = ((DWORD*)((char*)lr.pBits + probes[i].y * lr.Pitch))[probes[i].x] & 0xffffff;
        int ok = c == probes[i].expect;
        printf("%-28s %06lx (erwartet %06lx)%s\n", probes[i].name, (unsigned long)c, (unsigned long)probes[i].expect, ok ? "" : "  <-- FALSCH");
        if (!ok) fails++;
    }
    IDirect3DSurface9_UnlockRect(sysmem);
    printf("%s\n", fails ? "FEHLER" : "ok");
    return fails ? 1 : 0;
}
