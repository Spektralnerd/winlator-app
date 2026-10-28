/*
 * d3d9hdr – Gleitkomma-Renderziel über wined3d und Gladio (wie die Szenenfarbe der Unreal Engine 3).
 *
 * Rendert in eine A16B16G16R16F-Textur Werte über 1 (links 4/2/0,5) und mischt rechts mit Alpha
 * (SRCALPHA/INVSRCALPHA: 0,5 * (0/0/8) + 0,5 * (4/2/0,5) = 2/1/4,25), zeichnet die Textur dann mit einem
 * Faktor 0,25 in den Hintergrundpuffer und liest ihn zurück. Erwartet: links etwa (255,128,32), rechts
 * etwa (128,64,255). Würde das Ziel auf [0,1] begrenzt, käme links (64,64,32) heraus.
 *
 * Bauen (MinGW):  i686-w64-mingw32-gcc -O1 -o d3d9hdr.exe d3d9hdr.c -ld3d9 -lgdi32 -luser32
 * Aufruf:         wine d3d9hdr.exe out.ppm
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

struct Vertex { float x, y, z, rhw; float u, v; };
#define FVF (D3DFVF_XYZRHW | D3DFVF_TEX1)

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

static void drawRect(IDirect3DDevice9* dev, float x0, float y0, float x1, float y1) {
    struct Vertex v[4] = {
        {x0 - 0.5f, y0 - 0.5f, 0.5f, 1.0f, 0.0f, 0.0f},
        {x1 - 0.5f, y0 - 0.5f, 0.5f, 1.0f, 1.0f, 0.0f},
        {x0 - 0.5f, y1 - 0.5f, 0.5f, 1.0f, 0.0f, 1.0f},
        {x1 - 0.5f, y1 - 0.5f, 0.5f, 1.0f, 1.0f, 1.0f},
    };
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, v, sizeof(struct Vertex));
}

static IDirect3DPixelShader9* assemblePS(IDirect3DDevice9* dev, PFN_D3DXAssembleShader assemble, const char* src) {
    void* code = NULL; void* err = NULL;
    IDirect3DPixelShader9* ps = NULL;
    if (SUCCEEDED(assemble(src, strlen(src), NULL, NULL, 0, &code, &err))) {
        IDirect3DDevice9_CreatePixelShader(dev, bufferPointer(code), &ps);
        bufferRelease(code);
    }
    else printf("PS-Assemblierung fehlgeschlagen: %s\n", err ? (char*)bufferPointer(err) : "?");
    return ps;
}

int main(int argc, char** argv) {
    const char* out = argc > 1 ? argv[1] : "hdr.ppm";
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndProc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "d3d9hdr";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("d3d9hdr", "d3d9hdr", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, W, H, NULL, NULL, wc.hInstance, NULL);

    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { printf("Direct3DCreate9 fehlgeschlagen\n"); return 1; }
    HRESULT check = IDirect3D9_CheckDeviceFormat(d3d, 0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8,
                                                 D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING, D3DRTYPE_TEXTURE, D3DFMT_A16B16G16R16F);
    printf("CheckDeviceFormat A16B16G16R16F RT+Blend: %s\n", check == D3D_OK ? "ja" : "nein");

    D3DPRESENT_PARAMETERS pp = {0};
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = W; pp.BackBufferHeight = H; pp.hDeviceWindow = hwnd; pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9* dev = NULL;
    HRESULT hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr)) { printf("CreateDevice fehlgeschlagen: %lx\n", (long)hr); return 1; }

    IDirect3DTexture9* hdr = NULL;
    hr = IDirect3DDevice9_CreateTexture(dev, W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &hdr, NULL);
    if (FAILED(hr)) { printf("CreateTexture A16B16G16R16F fehlgeschlagen: %lx\n", (long)hr); return 1; }

    HMODULE d3dx = LoadLibraryA("d3dx9_36.dll");
    PFN_D3DXAssembleShader assemble = d3dx ? (PFN_D3DXAssembleShader)GetProcAddress(d3dx, "D3DXAssembleShader") : NULL;
    if (!assemble) { printf("d3dx9_36 / D3DXAssembleShader nicht verfügbar\n"); return 1; }
    IDirect3DPixelShader9* psConst = assemblePS(dev, assemble, "ps_2_0\nmov oC0, c0\n");
    IDirect3DPixelShader9* psScale = assemblePS(dev, assemble,
        "ps_2_0\ndcl t0.xy\ndcl_2d s0\ndef c1, 0.25, 0.25, 0.25, 1.0\ntexld r0, t0, s0\nmul r0, r0, c1\nmov oC0, r0\n");
    if (!psConst || !psScale) return 1;

    IDirect3DSurface9 *backbuffer = NULL, *hdrSurface = NULL, *sysmem = NULL;
    IDirect3DDevice9_GetRenderTarget(dev, 0, &backbuffer);
    IDirect3DTexture9_GetSurfaceLevel(hdr, 0, &hdrSurface);
    IDirect3DDevice9_CreateOffscreenPlainSurface(dev, W, H, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sysmem, NULL);

    IDirect3DDevice9_SetFVF(dev, FVF);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);

    for (int frame = 0; frame < 3; frame++) {
        IDirect3DDevice9_BeginScene(dev);

        /* 1. in das Gleitkomma-Ziel: Werte über 1, rechts mit Mischen */
        IDirect3DDevice9_SetRenderTarget(dev, 0, hdrSurface);
        IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0x00000000, 1.0f, 0);
        IDirect3DDevice9_SetPixelShader(dev, psConst);
        const float bright[4] = {4.0f, 2.0f, 0.5f, 1.0f};
        const float blendColor[4] = {0.0f, 0.0f, 8.0f, 0.5f};
        IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);
        IDirect3DDevice9_SetPixelShaderConstantF(dev, 0, bright, 1);
        drawRect(dev, 0, 0, W, H);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        IDirect3DDevice9_SetPixelShaderConstantF(dev, 0, blendColor, 1);
        drawRect(dev, W / 2, 0, W, H);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);

        /* 2. Textur mal 0,25 in den Hintergrundpuffer */
        IDirect3DDevice9_SetRenderTarget(dev, 0, backbuffer);
        IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xff102030, 1.0f, 0);
        IDirect3DDevice9_SetPixelShader(dev, psScale);
        IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9*)hdr);
        IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        drawRect(dev, 0, 0, W, H);
        IDirect3DDevice9_SetTexture(dev, 0, NULL);

        IDirect3DDevice9_EndScene(dev);
        if (frame < 2) IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
    }

    hr = IDirect3DDevice9_GetRenderTargetData(dev, backbuffer, sysmem);
    if (FAILED(hr)) { printf("GetRenderTargetData fehlgeschlagen: %lx\n", (long)hr); return 1; }
    D3DLOCKED_RECT lr;
    IDirect3DSurface9_LockRect(sysmem, &lr, NULL, D3DLOCK_READONLY);
    FILE* f = fopen(out, "wb");
    if (f) fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            DWORD p = ((DWORD*)((char*)lr.pBits + y * lr.Pitch))[x];
            unsigned char rgb[3] = {(p >> 16) & 0xff, (p >> 8) & 0xff, p & 0xff};
            if (f) fwrite(rgb, 1, 3, f);
        }
    }
    if (f) fclose(f);
    for (int i = 0; i < 2; i++) {
        int x = i == 0 ? W / 4 : 3 * W / 4, y = H / 2;
        DWORD p = ((DWORD*)((char*)lr.pBits + y * lr.Pitch))[x];
        printf("%s: (%lu,%lu,%lu)  erwartet %s\n", i == 0 ? "links " : "rechts", (p >> 16) & 0xff, (p >> 8) & 0xff, p & 0xff,
               i == 0 ? "(255,128,32)" : "(128,64,255)");
    }
    IDirect3DSurface9_UnlockRect(sysmem);
    printf("Bild nach %s geschrieben\n", out);

    IDirect3DSurface9_Release(sysmem);
    IDirect3DSurface9_Release(hdrSurface);
    IDirect3DSurface9_Release(backbuffer);
    IDirect3DTexture9_Release(hdr);
    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
    return 0;
}
