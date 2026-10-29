/*
 * d3d9srgb – sRGB-Lesen (D3DSAMP_SRGBTEXTURE) mit DXT- und A8R8G8B8-Texturen. Die Unreal Engine 3 liest
 * Farbtexturen so; wined3d legt dafür ohne GL_EXT_texture_sRGB_decode eine zweite GL-Textur mit sRGB-Format an
 * (DXT: GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT usw.). Gladio packt DXT auf dem Server aus – auch die
 * sRGB-Formate müssen ausgepackt und als sRGB hochgeladen werden, sonst liest der Shader Müll oder zu helle Werte.
 *
 * Gibt die Fähigkeiten (CheckDeviceFormat mit D3DUSAGE_QUERY_SRGBREAD) und für jede Textur den gelesenen Wert
 * ohne und mit sRGB aus. Erwartet: Grau 0x84 (0.52) bleibt ohne sRGB ~132, mit sRGB ~59 (linear 0.23).
 *
 *   i686-w64-mingw32-gcc -O1 -o d3d9srgb.exe d3d9srgb.c -ld3d9 -luser32
 *   wine d3d9srgb.exe [ausgabe.txt]
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#define W 64
#define H 64

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

struct Vertex { float x, y, z, rhw; float u, v; };

static const char* formatName(D3DFORMAT f)
{
    switch (f) {
        case D3DFMT_DXT1: return "DXT1";
        case D3DFMT_DXT3: return "DXT3";
        case D3DFMT_DXT5: return "DXT5";
        case D3DFMT_A8R8G8B8: return "A8R8G8B8";
        case D3DFMT_X8R8G8B8: return "X8R8G8B8";
        default: return "?";
    }
}

/* 4x4-Textur in einer Farbe: DXT-Blöcke mit c0 = 0x8410 (R 16/31, G 32/63, B 16/31 = 0x84 0x82 0x84), alle
 * Indizes 0; DXT3/5 mit Alpha 255 */
static IDirect3DTexture9* makeTexture(IDirect3DDevice9* dev, D3DFORMAT format)
{
    IDirect3DTexture9* tex = NULL;
    if (FAILED(IDirect3DDevice9_CreateTexture(dev, 4, 4, 1, 0, format, D3DPOOL_MANAGED, &tex, NULL))) return NULL;
    D3DLOCKED_RECT lr;
    if (FAILED(IDirect3DTexture9_LockRect(tex, 0, &lr, NULL, 0))) return tex;
    uint8_t* p = lr.pBits;
    uint8_t color[8] = {0x10, 0x84, 0x00, 0x00, 0, 0, 0, 0}; /* c0 = 0x8410, c1 = 0, Indizes 0 */
    switch (format) {
        case D3DFMT_DXT1:
            memcpy(p, color, 8);
            break;
        case D3DFMT_DXT3:
            memset(p, 0xff, 8);
            memcpy(p + 8, color, 8);
            break;
        case D3DFMT_DXT5: {
            uint8_t alpha[8] = {255, 255, 0, 0, 0, 0, 0, 0};
            memcpy(p, alpha, 8);
            memcpy(p + 8, color, 8);
            break;
        }
        default:
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++) ((DWORD*)(p + y * lr.Pitch))[x] = 0xff848284;
            break;
    }
    IDirect3DTexture9_UnlockRect(tex, 0);
    return tex;
}

int main(int argc, char** argv)
{
    if (argc > 1) freopen(argv[1], "w", stdout);
    setvbuf(stdout, NULL, _IONBF, 0);

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndProc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "d3d9srgb";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("d3d9srgb", "d3d9srgb", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, W, H, NULL, NULL, wc.hInstance, NULL);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);

    static const D3DFORMAT formats[] = {D3DFMT_DXT1, D3DFMT_DXT3, D3DFMT_DXT5, D3DFMT_A8R8G8B8, D3DFMT_X8R8G8B8};
    printf("CheckDeviceFormat (Textur):          normal  SRGBREAD\n");
    for (int i = 0; i < 5; i++) {
        HRESULT plain = IDirect3D9_CheckDeviceFormat(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0, D3DRTYPE_TEXTURE, formats[i]);
        HRESULT srgb = IDirect3D9_CheckDeviceFormat(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_QUERY_SRGBREAD, D3DRTYPE_TEXTURE, formats[i]);
        printf("  %-10s                         %-6s  %s\n", formatName(formats[i]), SUCCEEDED(plain) ? "ja" : "nein", SUCCEEDED(srgb) ? "ja" : "nein");
    }
    HRESULT srgbWrite = IDirect3D9_CheckDeviceFormat(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_SRGBWRITE, D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8);
    printf("  A8R8G8B8 als Ziel mit SRGBWRITE:     %s\n", SUCCEEDED(srgbWrite) ? "ja" : "nein");

    D3DPRESENT_PARAMETERS pp = {0};
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = W; pp.BackBufferHeight = H; pp.hDeviceWindow = hwnd;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9* dev = NULL;
    if (FAILED(IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev))) {
        printf("CreateDevice fehlgeschlagen\n");
        return 1;
    }
    IDirect3DSurface9 *backbuffer = NULL, *sysmem = NULL;
    IDirect3DDevice9_GetRenderTarget(dev, 0, &backbuffer);
    IDirect3DDevice9_CreateOffscreenPlainSurface(dev, W, H, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sysmem, NULL);

    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_TEX1);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, D3DZB_FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);

    int fails = 0;
    printf("\nGelesen (R G B):                      ohne sRGB        mit sRGB\n");
    for (int i = 0; i < 5; i++) {
        IDirect3DTexture9* tex = makeTexture(dev, formats[i]);
        if (!tex) { printf("  %-10s CreateTexture fehlgeschlagen\n", formatName(formats[i])); continue; }
        DWORD values[2];
        for (int srgb = 0; srgb < 2; srgb++) {
            IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_SRGBTEXTURE, srgb);
            IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xff00ff00, 1.0f, 0);
            IDirect3DDevice9_BeginScene(dev);
            IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9*)tex);
            struct Vertex v[4] = {{0, 0, 0.5f, 1, 0, 0}, {W, 0, 0.5f, 1, 1, 0}, {0, H, 0.5f, 1, 0, 1}, {W, H, 0.5f, 1, 1, 1}};
            IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, v, sizeof(struct Vertex));
            IDirect3DDevice9_EndScene(dev);
            IDirect3DDevice9_GetRenderTargetData(dev, backbuffer, sysmem);
            D3DLOCKED_RECT lr;
            IDirect3DSurface9_LockRect(sysmem, &lr, NULL, D3DLOCK_READONLY);
            values[srgb] = ((DWORD*)((char*)lr.pBits + (H / 2) * lr.Pitch))[W / 2] & 0xffffff;
            IDirect3DSurface9_UnlockRect(sysmem);
            IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL); /* ein Bild je Lesen (Aufzeichnung am PC-Server) */
        }
        IDirect3DDevice9_SetTexture(dev, 0, NULL);
        IDirect3DTexture9_Release(tex);
        int r0 = (values[0] >> 16) & 0xff, g0 = (values[0] >> 8) & 0xff, b0 = values[0] & 0xff;
        int r1 = (values[1] >> 16) & 0xff, g1 = (values[1] >> 8) & 0xff, b1 = values[1] & 0xff;
        /* ohne sRGB ~132, mit sRGB ~59 (wenn das Format sRGB-Lesen kann, sonst wie ohne) */
        HRESULT canSrgb = IDirect3D9_CheckDeviceFormat(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_QUERY_SRGBREAD, D3DRTYPE_TEXTURE, formats[i]);
        int okPlain = abs(r0 - 132) <= 3 && abs(g0 - 130) <= 3 && abs(b0 - 132) <= 3;
        int okSrgb = SUCCEEDED(canSrgb) ? (abs(r1 - 59) <= 4 && abs(g1 - 58) <= 4 && abs(b1 - 59) <= 4) : (r1 == r0 && g1 == g0 && b1 == b0);
        printf("  %-10s                         %3d %3d %3d%s    %3d %3d %3d%s\n", formatName(formats[i]),
               r0, g0, b0, okPlain ? "  " : " !", r1, g1, b1, okSrgb ? "" : "  <-- FALSCH");
        if (!okPlain || !okSrgb) fails++;
    }

    /* Nur mit sRGB benutzt, danach gelesen (LockRect): wined3d hat den Systemspeicher nach dem Hochladen verworfen und
     * lädt die sRGB-Fassung herunter – die Bytes müssen die ursprünglichen sein (roh, nicht entschlüsselt). */
    printf("\nNur sRGB benutzt, dann gelesen (LockRect, Pixel 0):\n");
    for (int i = 0; i < 5; i++) {
        IDirect3DTexture9* tex = makeTexture(dev, formats[i]);
        if (!tex) continue;
        IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_SRGBTEXTURE, TRUE);
        IDirect3DDevice9_BeginScene(dev);
        IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9*)tex);
        struct Vertex v[4] = {{0, 0, 0.5f, 1, 0, 0}, {W, 0, 0.5f, 1, 1, 0}, {0, H, 0.5f, 1, 0, 1}, {W, H, 0.5f, 1, 1, 1}};
        IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, v, sizeof(struct Vertex));
        IDirect3DDevice9_EndScene(dev);
        IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
        IDirect3DDevice9_SetTexture(dev, 0, NULL);
        D3DLOCKED_RECT lr;
        int r = -1, g = -1, b = -1;
        if (SUCCEEDED(IDirect3DTexture9_LockRect(tex, 0, &lr, NULL, D3DLOCK_READONLY))) {
            const uint8_t* p = lr.pBits;
            if (formats[i] == D3DFMT_A8R8G8B8 || formats[i] == D3DFMT_X8R8G8B8) {
                b = p[0]; g = p[1]; r = p[2];
            }
            else {
                /* Farbblock (DXT1 am Anfang, DXT3/5 nach 8 Byte Alpha): Farbe von Pixel 0 */
                const uint8_t* c = formats[i] == D3DFMT_DXT1 ? p : p + 8;
                unsigned c0 = c[0] | (c[1] << 8), c1 = c[2] | (c[3] << 8), idx = c[4] & 3;
                int r0 = ((c0 >> 11) & 31) * 255 / 31, g0 = ((c0 >> 5) & 63) * 255 / 63, b0 = (c0 & 31) * 255 / 31;
                int r1 = ((c1 >> 11) & 31) * 255 / 31, g1 = ((c1 >> 5) & 63) * 255 / 63, b1 = (c1 & 31) * 255 / 31;
                switch (idx) {
                    case 0: r = r0; g = g0; b = b0; break;
                    case 1: r = r1; g = g1; b = b1; break;
                    case 2: r = (2 * r0 + r1) / 3; g = (2 * g0 + g1) / 3; b = (2 * b0 + b1) / 3; break;
                    default: r = (r0 + 2 * r1) / 3; g = (g0 + 2 * g1) / 3; b = (b0 + 2 * b1) / 3; break;
                }
            }
            IDirect3DTexture9_UnlockRect(tex, 0);
        }
        IDirect3DTexture9_Release(tex);
        int ok = abs(r - 132) <= 4 && abs(g - 130) <= 4 && abs(b - 132) <= 4;
        printf("  %-10s                         %3d %3d %3d%s\n", formatName(formats[i]), r, g, b, ok ? "" : "  <-- FALSCH (erwartet 132 130 132)");
        if (!ok) fails++;
    }
    printf("%s\n", fails ? "FEHLER" : "ok");
    return fails ? 1 : 0;
}
