/*
 * d3d9const – Shader-Konstanten wie die Unreal Engine 3: je Zeichenaufruf setzt das Programm 118 Pixelshader- und
 * 96 Vertexshader-Konstanten neu, von denen sich nur wenige ändern. wined3d lädt bei jedem Zeichenaufruf den ganzen
 * Bereich (glUniform4fv mit 118 bzw. 96 vec4); der Gladio-Client schickt nur die geänderten Läufe
 * (patch-gladio-client.py, Wertevergleich). Gibt Bilder/s aus und schreibt das letzte Bild als PPM – zum Vergleich
 * mit GLADIO_NO_UNIFORM_DIFF=1 müssen die Bilder gleich sein.
 *
 *   x86_64-w64-mingw32-gcc -O1 -o d3d9const64.exe d3d9const.c -ld3d9 -luser32
 *   wine d3d9const64.exe [sekunden] [bild.ppm] [ausgabe.txt]
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 256
#define H 256
#define DRAWS 100
#define PS_CONSTS 118
#define VS_CONSTS 96

typedef HRESULT (WINAPI *PFN_D3DXAssembleShader)(const char* src, UINT len, const void* defines, void* include, DWORD flags, void** shader, void** errors);
static void* bufferPointer(void* buf) { return ((void* (WINAPI **)(void*))(*(void***)buf))[3](buf); }
static void bufferRelease(void* buf) { ((ULONG (WINAPI **)(void*))(*(void***)buf))[2](buf); }

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

struct Vertex { float x, y, z; };

/* vs_3_0: Position = v0 * c0..c3 (Orthoprojektion) + Versatz c4 + Summe einiger weiterer Konstanten (damit sie aktiv sind) */
static const char* vsSrc =
    "vs_3_0\n"
    "dcl_position v0\n"
    "dcl_position o0\n"
    "dcl_texcoord0 o1\n"
    "m4x4 r0, v0, c0\n"
    "add r0.xy, r0, c4\n"
    "mov r1, c5\n"
    "add r1, r1, c40\n"
    "add r1, r1, c70\n"
    "add r1, r1, c99\n"
    "mov o0, r0\n"
    "mov o1, r1\n";

/* ps_3_0: Farbe = c0 + c5 + c60 + c117 + Vertexfarbe aus o1 */
static const char* psSrc =
    "ps_3_0\n"
    "dcl_texcoord0 v0\n"
    "mov r0, c0\n"
    "add r0, r0, c5\n"
    "add r0, r0, c60\n"
    "add r0, r0, c117\n"
    "add r0, r0, v0\n"
    "mov oC0, r0\n";

static void matOrtho(float* m, float w, float h) {
    memset(m, 0, 16 * sizeof(float));
    /* Spaltenweise für m4x4 (c0..c3 = Zeilen der transponierten Matrix): x' = x*2/w-1, y' = 1-y*2/h */
    m[0] = 2.0f / w; m[3] = -1.0f;
    m[5] = -2.0f / h; m[7] = 1.0f;
    m[10] = 1.0f;
    m[15] = 1.0f;
}

int main(int argc, char** argv)
{
    int seconds = argc > 1 ? atoi(argv[1]) : 6;
    const char* ppm = argc > 2 ? argv[2] : "const.ppm";
    if (argc > 3) freopen(argv[3], "w", stdout);
    setvbuf(stdout, NULL, _IONBF, 0);

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndProc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "d3d9const";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("d3d9const", "d3d9const", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, W, H, NULL, NULL, wc.hInstance, NULL);
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
    IDirect3DVertexShader9* vs = NULL; IDirect3DPixelShader9* ps = NULL;
    void* code = NULL; void* err = NULL;
    if (FAILED(assemble(vsSrc, strlen(vsSrc), NULL, NULL, 0, &code, &err))) { printf("VS: %s\n", err ? (char*)bufferPointer(err) : "?"); return 1; }
    IDirect3DDevice9_CreateVertexShader(dev, bufferPointer(code), &vs); bufferRelease(code);
    code = NULL; err = NULL;
    if (FAILED(assemble(psSrc, strlen(psSrc), NULL, NULL, 0, &code, &err))) { printf("PS: %s\n", err ? (char*)bufferPointer(err) : "?"); return 1; }
    IDirect3DDevice9_CreatePixelShader(dev, bufferPointer(code), &ps); bufferRelease(code);
    if (!vs || !ps) { printf("Shader fehlgeschlagen\n"); return 1; }

    IDirect3DDevice9_SetVertexShader(dev, vs);
    IDirect3DDevice9_SetPixelShader(dev, ps);
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZ);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, D3DZB_FALSE);

    float psConst[PS_CONSTS * 4], vsConst[VS_CONSTS * 4];
    for (int i = 0; i < PS_CONSTS * 4; i++) psConst[i] = ((i * 7) % 13) / 100.0f;
    for (int i = 0; i < VS_CONSTS * 4; i++) vsConst[i] = ((i * 5) % 11) / 1000.0f;
    /* Grundfarbe über c0 (die anderen aktiven Konstanten sind klein) */
    psConst[0] = 0.1f; psConst[1] = 0.1f; psConst[2] = 0.1f; psConst[3] = 1.0f;
    float proj[16];
    matOrtho(proj, W, H);

    IDirect3DSurface9* sysmem = NULL;
    IDirect3DDevice9_CreateOffscreenPlainSurface(dev, W, H, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sysmem, NULL);

    DWORD start = GetTickCount();
    int frames = 0;
    while (GetTickCount() - start < (DWORD)seconds * 1000) {
        IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xff202040, 1.0f, 0);
        IDirect3DDevice9_BeginScene(dev);
        IDirect3DDevice9_SetVertexShaderConstantF(dev, 0, proj, 4);
        for (int i = 0; i < DRAWS; i++) {
            /* je Zeichenaufruf: alle Konstanten setzen, nur c5 (Pixel) und c4 (Vertex: Versatz) ändern sich */
            int phase = (frames / 30) % 2; /* alle 30 Bilder andere Farbe (Änderung über die Bilder) */
            psConst[5 * 4 + 0] = (i % 10) / 10.0f;
            psConst[5 * 4 + 1] = (i / 10) / 10.0f;
            psConst[5 * 4 + 2] = phase ? 0.5f : 0.0f;
            psConst[5 * 4 + 3] = 0.0f;
            IDirect3DDevice9_SetPixelShaderConstantF(dev, 0, psConst, PS_CONSTS);
            vsConst[0] = (i % 10) * 2.0f / W * 24.0f; /* Versatz in Clipkoordinaten */
            vsConst[1] = -(i / 10) * 2.0f / H * 24.0f;
            vsConst[2] = 0.0f; vsConst[3] = 0.0f;
            IDirect3DDevice9_SetVertexShaderConstantF(dev, 4, vsConst, VS_CONSTS);
            struct Vertex v[4] = {{4, 4, 0.5f}, {24, 4, 0.5f}, {4, 24, 0.5f}, {24, 24, 0.5f}};
            IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, v, sizeof(struct Vertex));
        }
        IDirect3DDevice9_EndScene(dev);
        if (frames == 45 && sysmem) {
            /* ein Bild der zweiten Phase lesen (vor Present) */
            IDirect3DSurface9* bb = NULL;
            if (SUCCEEDED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) {
                if (SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(dev, bb, sysmem))) {
                    D3DLOCKED_RECT lr;
                    if (SUCCEEDED(IDirect3DSurface9_LockRect(sysmem, &lr, NULL, D3DLOCK_READONLY))) {
                        FILE* f = fopen(ppm, "wb");
                        if (f) {
                            fprintf(f, "P6\n%d %d\n255\n", W, H);
                            for (int y = 0; y < H; y++) {
                                DWORD* row = (DWORD*)((char*)lr.pBits + y * lr.Pitch);
                                for (int x = 0; x < W; x++) { fputc((row[x] >> 16) & 255, f); fputc((row[x] >> 8) & 255, f); fputc(row[x] & 255, f); }
                            }
                            fclose(f);
                        }
                        DWORD* row = (DWORD*)((char*)lr.pBits + 14 * lr.Pitch);
                        printf("Bild 45: Pixel (14,14) %06lx, (38,14) %06lx, (14,38) %06lx\n", (unsigned long)(row[14] & 0xffffff),
                               (unsigned long)(row[38] & 0xffffff), (unsigned long)(((DWORD*)((char*)lr.pBits + 38 * lr.Pitch))[14] & 0xffffff));
                        IDirect3DSurface9_UnlockRect(sysmem);
                    }
                }
                IDirect3DSurface9_Release(bb);
            }
        }
        IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
        frames++;
    }
    double secs = (GetTickCount() - start) / 1000.0;
    printf("%d Bilder in %.1f s (%.1f/s), %d Zeichenaufrufe je Bild mit je %d+%d Konstanten\n", frames, secs, frames / secs, DRAWS, PS_CONSTS, VS_CONSTS);
    return 0;
}
