/*
 * d3d9occl – Verdeckungsabfragen (D3DQUERYTYPE_OCCLUSION) über wined3d und Gladio, wie sie die Unreal
 * Engine 3 braucht (Outlast: CreateQuery(D3DQUERYTYPE_OCCLUSION) lieferte D3DERR_NOTAVAILABLE, für die
 * Engine ein fataler Fehler).
 *
 * Zeichnet eine bildfüllende Fläche in der Tiefe 0,5, dann in je einer Abfrage:
 *   A  eine Fläche dahinter (Tiefe 0,9)        -> 0 Abtastwerte
 *   B  ein 16x16-Quadrat davor (Tiefe 0,1)      -> 256 (Gladio auf ES 3: „irgendein Abtastwert“, > 0)
 *   C  nichts                                   -> 0
 * und liest die Ergebnisse mit GetData(D3DGETDATA_FLUSH) wie die Engine (abfragen, bis S_OK).
 *
 *   i686-w64-mingw32-gcc -O1 -o d3d9occl.exe d3d9occl.c -ld3d9 -luser32
 * Unter ES 2 (Mali-400) gibt es keine Verdeckungsabfragen; Gladio meldet dort alles als sichtbar
 * (UINT8_MAX), das prüft der zweite Aufrufparameter „es2“.
 *
 *   i686-w64-mingw32-gcc -O1 -o d3d9occl.exe d3d9occl.c -ld3d9 -luser32
 *   wine d3d9occl.exe [ausgabe.txt [es2]]
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

#define W 256
#define H 256

struct Vertex { float x, y, z, rhw; DWORD color; };
#define FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

static const char* hrName(HRESULT hr)
{
    switch (hr) {
    case D3D_OK: return "D3D_OK";
    case S_FALSE: return "S_FALSE";
    case D3DERR_NOTAVAILABLE: return "D3DERR_NOTAVAILABLE";
    case D3DERR_INVALIDCALL: return "D3DERR_INVALIDCALL";
    case D3DERR_DEVICELOST: return "D3DERR_DEVICELOST";
    default: return "?";
    }
}

static void rect(IDirect3DDevice9* dev, float x0, float y0, float x1, float y1, float z, DWORD color)
{
    struct Vertex v[4] = {
        {x0 - 0.5f, y0 - 0.5f, z, 1.0f, color},
        {x1 - 0.5f, y0 - 0.5f, z, 1.0f, color},
        {x0 - 0.5f, y1 - 0.5f, z, 1.0f, color},
        {x1 - 0.5f, y1 - 0.5f, z, 1.0f, color},
    };
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, v, sizeof(struct Vertex));
}

static DWORD result(IDirect3DQuery9* q, const char* name, int* polls)
{
    DWORD samples = 0xdeadbeef;
    HRESULT hr;
    *polls = 0;
    DWORD start = GetTickCount();
    while ((hr = IDirect3DQuery9_GetData(q, &samples, sizeof(samples), D3DGETDATA_FLUSH)) == S_FALSE) {
        (*polls)++;
        if (GetTickCount() - start > 5000) { printf("%s: nach 5 s noch kein Ergebnis\n", name); return samples; }
    }
    if (hr != D3D_OK) printf("%s: GetData %08lx %s\n", name, (unsigned long)hr, hrName(hr));
    return samples;
}

int main(int argc, char** argv)
{
    if (argc > 1) freopen(argv[1], "w", stdout);
    int es2 = argc > 2 && !strcmp(argv[2], "es2");
    setvbuf(stdout, NULL, _IONBF, 0);

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndProc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "d3d9occl";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("d3d9occl", "d3d9occl", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, W, H, NULL, NULL, wc.hInstance, NULL);

    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { printf("Direct3DCreate9 fehlgeschlagen\n"); return 1; }
    D3DPRESENT_PARAMETERS pp = {0};
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = W; pp.BackBufferHeight = H; pp.hDeviceWindow = hwnd;
    pp.EnableAutoDepthStencil = TRUE; pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9* dev = NULL;
    HRESULT hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr)) { printf("CreateDevice fehlgeschlagen: %08lx\n", (unsigned long)hr); return 1; }

    hr = IDirect3DDevice9_CreateQuery(dev, D3DQUERYTYPE_OCCLUSION, NULL);
    printf("CreateQuery(OCCLUSION, NULL) (Unterstützung): %s\n", hrName(hr));
    IDirect3DQuery9* q[3] = {NULL, NULL, NULL};
    for (int i = 0; i < 3; i++) {
        hr = IDirect3DDevice9_CreateQuery(dev, D3DQUERYTYPE_OCCLUSION, &q[i]);
        if (FAILED(hr)) { printf("CreateQuery(OCCLUSION) %d: %08lx %s\n", i, (unsigned long)hr, hrName(hr)); return 1; }
    }
    printf("CreateQuery(EVENT, NULL): %s\n", hrName(IDirect3DDevice9_CreateQuery(dev, D3DQUERYTYPE_EVENT, NULL)));
    printf("CreateQuery(TIMESTAMP, NULL): %s\n", hrName(IDirect3DDevice9_CreateQuery(dev, D3DQUERYTYPE_TIMESTAMP, NULL)));

    IDirect3DDevice9_SetFVF(dev, FVF);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, D3DZB_TRUE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZFUNC, D3DCMP_LESSEQUAL);

    int fails = 0;
    for (int frame = 0; frame < 3; frame++) {
        IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000, 1.0f, 0);
        IDirect3DDevice9_BeginScene(dev);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_ZWRITEENABLE, TRUE);
        rect(dev, 0, 0, W, H, 0.5f, 0xff404040);
        /* wie die Engine: Abfragen ohne Farb- und Tiefenschreiben */
        IDirect3DDevice9_SetRenderState(dev, D3DRS_ZWRITEENABLE, FALSE);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_COLORWRITEENABLE, 0);
        IDirect3DQuery9_Issue(q[0], D3DISSUE_BEGIN); rect(dev, 0, 0, W, H, 0.9f, 0xffff0000); IDirect3DQuery9_Issue(q[0], D3DISSUE_END);
        IDirect3DQuery9_Issue(q[1], D3DISSUE_BEGIN); rect(dev, 100, 100, 116, 116, 0.1f, 0xff00ff00); IDirect3DQuery9_Issue(q[1], D3DISSUE_END);
        IDirect3DQuery9_Issue(q[2], D3DISSUE_BEGIN); IDirect3DQuery9_Issue(q[2], D3DISSUE_END);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_COLORWRITEENABLE, 0xf);
        IDirect3DDevice9_EndScene(dev);
        IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);

        int pa, pb, pc;
        DWORD a = result(q[0], "A", &pa), b = result(q[1], "B", &pb), c = result(q[2], "C", &pc);
        printf("Bild %d: A dahinter %lu (erwartet %s), B davor %lu (erwartet > 0), C leer %lu (erwartet %s); S_FALSE-Runden %d/%d/%d\n",
               frame, (unsigned long)a, es2 ? "> 0" : "0", (unsigned long)b, (unsigned long)c, es2 ? "> 0" : "0", pa, pb, pc);
        if (b == 0 || b == 0xdeadbeef || (es2 ? (a == 0 || c == 0) : (a != 0 || c != 0))) fails++;
    }
    printf("%s\n", fails ? "FEHLER" : "ok");
    for (int i = 0; i < 3; i++) IDirect3DQuery9_Release(q[i]);
    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
    return fails ? 1 : 0;
}
