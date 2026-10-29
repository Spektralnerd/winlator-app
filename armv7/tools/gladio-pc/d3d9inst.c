/*
 * d3d9inst – Instanzen-Zeichnen in Direct3D 9 (SetStreamSourceFreq) über wined3d und Gladio.
 * wined3d macht daraus glDrawElementsInstancedBaseVertex mit Attributteilern (glVertexAttribDivisor).
 *
 * Ein Viereck (Strom 0, ab Vertex 4 im Puffer, BaseVertexIndex 4) wird viermal gezeichnet; je Instanz
 * kommen aus Strom 1 eine Verschiebung (FLOAT4) und eine Farbe (D3DCOLOR, wie Vertexfarben der
 * Unreal Engine 3). Danach ein gewöhnliches (nicht instanziertes) Viereck in der Mitte (weiß).
 * Erwartet: links oben rot, rechts oben grün, links unten blau, rechts unten gelb, Mitte weiß.
 *
 *   i686-w64-mingw32-gcc -O1 -o d3d9inst.exe d3d9inst.c -ld3d9 -luser32
 *   wine d3d9inst.exe [ausgabe.txt [bild.ppm]]
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

#define W 256
#define H 256

typedef HRESULT (WINAPI *PFN_D3DXAssembleShader)(const char* src, UINT len, const void* defines, void* include, DWORD flags, void** shader, void** errors);
static void* bufferPointer(void* buf) { return ((void* (WINAPI **)(void*))(*(void***)buf))[3](buf); }
static void bufferRelease(void* buf) { ((ULONG (WINAPI **)(void*))(*(void***)buf))[2](buf); }

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

static void* assemble(PFN_D3DXAssembleShader fn, const char* src)
{
    void* code = NULL; void* err = NULL;
    if (FAILED(fn(src, strlen(src), NULL, NULL, 0, &code, &err))) {
        printf("Assemblierung fehlgeschlagen: %s\n", err ? (char*)bufferPointer(err) : "?");
        return NULL;
    }
    return code;
}

int main(int argc, char** argv)
{
    if (argc > 1) freopen(argv[1], "w", stdout);
    setvbuf(stdout, NULL, _IONBF, 0);
    const char* ppm = argc > 2 ? argv[2] : NULL;

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndProc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "d3d9inst";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("d3d9inst", "d3d9inst", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, W, H, NULL, NULL, wc.hInstance, NULL);

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
    if (!asmFn) { printf("d3dx9_36 / D3DXAssembleShader fehlt\n"); return 1; }
    /* v0 Position (Strom 0), v1 Verschiebung und Größe (Strom 1), v2 Farbe (Strom 1, D3DCOLOR) */
    void* vsCode = assemble(asmFn,
        "vs_3_0\n"
        "dcl_position v0\n"
        "dcl_texcoord0 v1\n"
        "dcl_color0 v2\n"
        "dcl_position o0\n"
        "dcl_color0 o1\n"
        "def c0, 0.5, 1.0, 0.0, 0.0\n"
        "mad o0.xy, v0.xy, v1.zw, v1.xy\n"
        "mov o0.zw, c0.xy\n"
        "mov o1, v2\n");
    void* psCode = assemble(asmFn, "ps_3_0\ndcl_color0 v0\nmov oC0, v0\n");
    if (!vsCode || !psCode) return 1;
    IDirect3DVertexShader9* vs = NULL; IDirect3DPixelShader9* ps = NULL;
    IDirect3DDevice9_CreateVertexShader(dev, bufferPointer(vsCode), &vs);
    IDirect3DDevice9_CreatePixelShader(dev, bufferPointer(psCode), &ps);

    D3DVERTEXELEMENT9 decl[] = {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {1, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        {1, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        D3DDECL_END()
    };
    IDirect3DVertexDeclaration9* vdecl = NULL;
    IDirect3DDevice9_CreateVertexDeclaration(dev, decl, &vdecl);

    /* Viereck 0..1 ab Vertex 4 (davor Müll, der bei falschem BaseVertex sichtbar würde) */
    float verts[8][3] = {
        {-9, -9, 0}, {-9, 9, 0}, {9, -9, 0}, {9, 9, 0},
        {0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0},
    };
    WORD idx[6] = {0, 1, 2, 2, 1, 3};
    struct { float x, y, sx, sy; DWORD color; } inst[5] = {
        {-0.9f, 0.1f, 0.8f, 0.8f, 0xffff0000},   /* links oben, rot */
        {0.1f, 0.1f, 0.8f, 0.8f, 0xff00ff00},    /* rechts oben, grün */
        {-0.9f, -0.9f, 0.8f, 0.8f, 0xff0000ff},  /* links unten, blau */
        {0.1f, -0.9f, 0.8f, 0.8f, 0xffffff00},   /* rechts unten, gelb */
        {-0.15f, -0.15f, 0.3f, 0.3f, 0xffffffff}, /* Mitte, weiß, ohne Instanzen */
    };
    IDirect3DVertexBuffer9 *vb = NULL, *ib1 = NULL;
    IDirect3DIndexBuffer9* ib = NULL;
    void* p;
    IDirect3DDevice9_CreateVertexBuffer(dev, sizeof(verts), D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &vb, NULL);
    IDirect3DVertexBuffer9_Lock(vb, 0, 0, &p, 0); memcpy(p, verts, sizeof(verts)); IDirect3DVertexBuffer9_Unlock(vb);
    IDirect3DDevice9_CreateVertexBuffer(dev, sizeof(inst), D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &ib1, NULL);
    IDirect3DVertexBuffer9_Lock(ib1, 0, 0, &p, 0); memcpy(p, inst, sizeof(inst)); IDirect3DVertexBuffer9_Unlock(ib1);
    IDirect3DDevice9_CreateIndexBuffer(dev, sizeof(idx), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &ib, NULL);
    IDirect3DIndexBuffer9_Lock(ib, 0, 0, &p, 0); memcpy(p, idx, sizeof(idx)); IDirect3DIndexBuffer9_Unlock(ib);

    IDirect3DSurface9 *backbuffer = NULL, *sysmem = NULL;
    IDirect3DDevice9_GetRenderTarget(dev, 0, &backbuffer);
    IDirect3DDevice9_CreateOffscreenPlainSurface(dev, W, H, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sysmem, NULL);

    for (int frame = 0; frame < 3; frame++) {
        IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xff202020, 1.0f, 0);
        IDirect3DDevice9_BeginScene(dev);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE);
        IDirect3DDevice9_SetVertexDeclaration(dev, vdecl);
        IDirect3DDevice9_SetVertexShader(dev, vs);
        IDirect3DDevice9_SetPixelShader(dev, ps);
        IDirect3DDevice9_SetIndices(dev, ib);
        IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, 12);
        IDirect3DDevice9_SetStreamSource(dev, 1, ib1, 0, 20);
        IDirect3DDevice9_SetStreamSourceFreq(dev, 0, D3DSTREAMSOURCE_INDEXEDDATA | 4);
        IDirect3DDevice9_SetStreamSourceFreq(dev, 1, D3DSTREAMSOURCE_INSTANCEDATA | 1);
        hr = IDirect3DDevice9_DrawIndexedPrimitive(dev, D3DPT_TRIANGLELIST, 4, 0, 4, 0, 2);
        if (frame == 0) printf("DrawIndexedPrimitive mit 4 Instanzen: %08lx\n", (unsigned long)hr);
        /* ohne Instanzen: Strom 1 ab Eintrag 4 (weiß), Frequenz zurück auf 1 */
        IDirect3DDevice9_SetStreamSourceFreq(dev, 0, 1);
        IDirect3DDevice9_SetStreamSourceFreq(dev, 1, 1);
        IDirect3DDevice9_SetStreamSource(dev, 1, ib1, 4 * 20, 0);
        hr = IDirect3DDevice9_DrawIndexedPrimitive(dev, D3DPT_TRIANGLELIST, 4, 0, 4, 0, 2);
        if (frame == 0) printf("DrawIndexedPrimitive ohne Instanzen: %08lx\n", (unsigned long)hr);
        IDirect3DDevice9_EndScene(dev);
        if (frame < 2) IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
    }

    IDirect3DDevice9_GetRenderTargetData(dev, backbuffer, sysmem);
    D3DLOCKED_RECT lr;
    IDirect3DSurface9_LockRect(sysmem, &lr, NULL, D3DLOCK_READONLY);
    static const struct { int x, y; const char* name; DWORD expect; } probes[] = {
        {W / 4, H / 4, "links oben (rot)", 0xff0000}, {3 * W / 4, H / 4, "rechts oben (grün)", 0x00ff00},
        {W / 4, 3 * H / 4, "links unten (blau)", 0x0000ff}, {3 * W / 4, 3 * H / 4, "rechts unten (gelb)", 0xffff00},
        {W / 2, H / 2, "Mitte (weiß)", 0xffffff},
    };
    int fails = 0;
    for (unsigned i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        DWORD c = ((DWORD*)((char*)lr.pBits + probes[i].y * lr.Pitch))[probes[i].x] & 0xffffff;
        printf("%-22s %06lx (erwartet %06lx)%s\n", probes[i].name, (unsigned long)c, (unsigned long)probes[i].expect, c == probes[i].expect ? "" : "  <-- FALSCH");
        if (c != probes[i].expect) fails++;
    }
    if (ppm) {
        FILE* f = fopen(ppm, "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", W, H);
            for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
                DWORD c = ((DWORD*)((char*)lr.pBits + y * lr.Pitch))[x];
                unsigned char rgb[3] = {(c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff};
                fwrite(rgb, 1, 3, f);
            }
            fclose(f);
        }
    }
    IDirect3DSurface9_UnlockRect(sysmem);
    printf("%s\n", fails ? "FEHLER" : "ok");
    bufferRelease(vsCode); bufferRelease(psCode);
    return fails ? 1 : 0;
}
