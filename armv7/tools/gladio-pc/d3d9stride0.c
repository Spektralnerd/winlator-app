/*
 * d3d9stride0 – konstante Vertexattribute über wined3d und Gladio: ein Vertexstrom mit Schrittweite 0
 * (D3DCOLOR), wie ihn die Unreal Engine 3 für Modelle ohne Vertexfarben bindet. wined3d setzt solche
 * Attribute mit glVertexAttrib4Nubv statt über ein Array.
 *
 * Zeichnet zwei Vierecke, jedes mit eigener konstanter Farbe aus einem Strom mit Schrittweite 0, und
 * liest den Hintergrundpuffer zurück. Erwartet: links (32,192,64), rechts (255,128,64).
 *
 * Bauen (MinGW):  i686-w64-mingw32-gcc -O1 -o d3d9stride0.exe d3d9stride0.c -ld3d9 -lgdi32 -luser32
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

#define W 256
#define H 128

typedef HRESULT (WINAPI *PFN_D3DXAssembleShader)(const char* src, UINT len, const void* defines, void* include, DWORD flags, void** shader, void** errors);
static void* bufferPointer(void* buf) { return ((void* (WINAPI **)(void*))(*(void***)buf))[3](buf); }
static void bufferRelease(void* buf) { ((ULONG (WINAPI **)(void*))(*(void***)buf))[2](buf); }
static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

static void* assemble(PFN_D3DXAssembleShader fn, const char* src) {
    void* code = NULL; void* err = NULL;
    if (FAILED(fn(src, strlen(src), NULL, NULL, 0, &code, &err))) { printf("Assemblierung fehlgeschlagen: %s\n", err ? (char*)bufferPointer(err) : "?"); return NULL; }
    return code;
}

int main(void) {
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndProc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "d3d9stride0";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("d3d9stride0", "d3d9stride0", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, W, H, NULL, NULL, wc.hInstance, NULL);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp = {0};
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = W; pp.BackBufferHeight = H; pp.hDeviceWindow = hwnd; pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9* dev = NULL;
    if (FAILED(IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev))) { printf("CreateDevice fehlgeschlagen\n"); return 1; }

    HMODULE d3dx = LoadLibraryA("d3dx9_36.dll");
    PFN_D3DXAssembleShader fn = d3dx ? (PFN_D3DXAssembleShader)GetProcAddress(d3dx, "D3DXAssembleShader") : NULL;
    if (!fn) { printf("D3DXAssembleShader fehlt\n"); return 1; }
    void* vsCode = assemble(fn, "vs_2_0\ndcl_position v0\ndcl_color v1\nmov oPos, v0\nmov oD0, v1\n");
    void* psCode = assemble(fn, "ps_2_0\ndcl v0\nmov oC0, v0\n");
    if (!vsCode || !psCode) return 1;
    IDirect3DVertexShader9* vs; IDirect3DPixelShader9* ps;
    IDirect3DDevice9_CreateVertexShader(dev, bufferPointer(vsCode), &vs); bufferRelease(vsCode);
    IDirect3DDevice9_CreatePixelShader(dev, bufferPointer(psCode), &ps); bufferRelease(psCode);

    static const D3DVERTEXELEMENT9 decl[] = {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {1, 0, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        D3DDECL_END()
    };
    IDirect3DVertexDeclaration9* vdecl;
    IDirect3DDevice9_CreateVertexDeclaration(dev, decl, &vdecl);

    /* Positionen in Clipkoordinaten: links und rechts je ein Viereck (Dreiecksstreifen) */
    float pos[2][4][4] = {
        {{-1, 1, 0.5f, 1}, {0, 1, 0.5f, 1}, {-1, -1, 0.5f, 1}, {0, -1, 0.5f, 1}},
        {{0, 1, 0.5f, 1}, {1, 1, 0.5f, 1}, {0, -1, 0.5f, 1}, {1, -1, 0.5f, 1}},
    };
    const DWORD colors[2] = {0xff20c040, 0xffff8040};
    IDirect3DVertexBuffer9 *vbPos, *vbColor[2];
    void* p;
    IDirect3DDevice9_CreateVertexBuffer(dev, sizeof(pos), 0, 0, D3DPOOL_MANAGED, &vbPos, NULL);
    IDirect3DVertexBuffer9_Lock(vbPos, 0, 0, &p, 0); memcpy(p, pos, sizeof(pos)); IDirect3DVertexBuffer9_Unlock(vbPos);
    for (int i = 0; i < 2; i++) {
        IDirect3DDevice9_CreateVertexBuffer(dev, 16, 0, 0, D3DPOOL_MANAGED, &vbColor[i], NULL);
        IDirect3DVertexBuffer9_Lock(vbColor[i], 0, 0, &p, 0); memcpy(p, &colors[i], 4); IDirect3DVertexBuffer9_Unlock(vbColor[i]);
    }

    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetVertexDeclaration(dev, vdecl);
    IDirect3DDevice9_SetVertexShader(dev, vs);
    IDirect3DDevice9_SetPixelShader(dev, ps);
    for (int frame = 0; frame < 3; frame++) {
        IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xff000000, 1.0f, 0);
        IDirect3DDevice9_BeginScene(dev);
        for (int i = 0; i < 2; i++) {
            IDirect3DDevice9_SetStreamSource(dev, 0, vbPos, i * 4 * 16, 16);
            IDirect3DDevice9_SetStreamSource(dev, 1, vbColor[i], 0, 0);   /* Schrittweite 0 */
            IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLESTRIP, 0, 2);
        }
        IDirect3DDevice9_EndScene(dev);
        if (frame < 2) IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
    }

    IDirect3DSurface9 *bb, *sys;
    IDirect3DDevice9_GetRenderTarget(dev, 0, &bb);
    IDirect3DDevice9_CreateOffscreenPlainSurface(dev, W, H, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL);
    IDirect3DDevice9_GetRenderTargetData(dev, bb, sys);
    D3DLOCKED_RECT lr;
    IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY);
    for (int i = 0; i < 2; i++) {
        DWORD px = ((DWORD*)((char*)lr.pBits + (H / 2) * lr.Pitch))[i == 0 ? W / 4 : 3 * W / 4];
        printf("%s: (%lu,%lu,%lu)  erwartet %s\n", i == 0 ? "links " : "rechts", (px >> 16) & 0xff, (px >> 8) & 0xff, px & 0xff,
               i == 0 ? "(32,192,64)" : "(255,128,64)");
    }
    IDirect3DSurface9_UnlockRect(sys);
    return 0;
}
