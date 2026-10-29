/*
 * d3d9occlmany – viele Verdeckungsabfragen je Bild wie die Unreal Engine 3: je Bild 64 Abfragen um kleine
 * Vierecke, Ergebnisse zwei Bilder später mit GetData(D3DGETDATA_FLUSH) abgeholt (Schleife bis fertig).
 * Gibt Bilder/s und die Summe der sichtbaren Abfragen aus; die Rundreisen je Bild stehen in der Statistik
 * des Servers (GetQueryObjectuiv, ClientWaitSync).
 *
 *   i686-w64-mingw32-gcc -O1 -o d3d9occlmany.exe d3d9occlmany.c -ld3d9 -luser32
 *   wine d3d9occlmany.exe [sekunden] [ausgabe.txt]
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>

#define W 256
#define H 256
#define QUERIES 64
#define LAG 3

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

struct Vertex { float x, y, z, rhw; DWORD color; };

static void rect(IDirect3DDevice9* dev, float x0, float y0, float x1, float y1, float z, DWORD color)
{
    struct Vertex v[4] = {{x0, y0, z, 1, color}, {x1, y0, z, 1, color}, {x0, y1, z, 1, color}, {x1, y1, z, 1, color}};
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, v, sizeof(struct Vertex));
}

int main(int argc, char** argv)
{
    int seconds = argc > 1 ? atoi(argv[1]) : 8;
    if (argc > 2) freopen(argv[2], "w", stdout);
    setvbuf(stdout, NULL, _IONBF, 0);

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndProc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "d3d9occlmany";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("d3d9occlmany", "d3d9occlmany", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, W, H, NULL, NULL, wc.hInstance, NULL);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp = {0};
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = W; pp.BackBufferHeight = H; pp.hDeviceWindow = hwnd;
    pp.EnableAutoDepthStencil = TRUE; pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9* dev = NULL;
    if (FAILED(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev))) {
        printf("CreateDevice fehlgeschlagen\n");
        return 1;
    }
    IDirect3DQuery9* queries[LAG][QUERIES];
    for (int f = 0; f < LAG; f++)
        for (int i = 0; i < QUERIES; i++)
            if (FAILED(IDirect3DDevice9_CreateQuery(dev, D3DQUERYTYPE_OCCLUSION, &queries[f][i]))) { printf("CreateQuery fehlgeschlagen\n"); return 1; }

    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    DWORD start = GetTickCount();
    int frames = 0;
    long visibleSum = 0, readCount = 0;
    while (GetTickCount() - start < (DWORD)seconds * 1000) {
        int slot = frames % LAG;
        if (frames >= LAG) {
            /* Ergebnisse von vor LAG Bildern abholen */
            for (int i = 0; i < QUERIES; i++) {
                DWORD pixels = 0;
                while (IDirect3DQuery9_GetData(queries[slot][i], &pixels, sizeof(pixels), D3DGETDATA_FLUSH) == S_FALSE) ;
                if (pixels) visibleSum++;
                readCount++;
            }
        }
        IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff202040, 1.0f, 0);
        IDirect3DDevice9_BeginScene(dev);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, D3DZB_TRUE);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_ZWRITEENABLE, TRUE);
        rect(dev, 0, 0, W / 2, H, 0.2f, 0xff808080); /* linke Hälfte verdeckt */
        IDirect3DDevice9_SetRenderState(dev, D3DRS_ZWRITEENABLE, FALSE);
        for (int i = 0; i < QUERIES; i++) {
            float x = (float)((i * 37 + frames * 3) % (W - 8)), y = (float)((i * 53) % (H - 8));
            IDirect3DQuery9_Issue(queries[slot][i], D3DISSUE_BEGIN);
            rect(dev, x, y, x + 8, y + 8, 0.5f, 0xffffff00);
            IDirect3DQuery9_Issue(queries[slot][i], D3DISSUE_END);
        }
        IDirect3DDevice9_EndScene(dev);
        IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
        frames++;
    }
    double secs = (GetTickCount() - start) / 1000.0;
    printf("%d Bilder in %.1f s (%.1f/s), %ld Abfragen gelesen, davon %ld sichtbar (%.0f %%)\n", frames, secs, frames / secs,
           readCount, visibleSum, readCount ? 100.0 * visibleSum / readCount : 0.0);
    return 0;
}
