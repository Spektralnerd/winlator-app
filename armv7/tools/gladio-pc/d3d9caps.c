/*
 * Direct3D-9-Formatabfragen, wie Spiele sie beim Start stellen (Unreal Engine 3 / Outlast:
 * D3DFMT_A16B16G16R16F mit D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING, sonst Abbruch).
 * Gibt je Format und Verwendung aus, ob CheckDeviceFormat zustimmt; dazu Grundangaben des Geräts.
 *
 *   i686-w64-mingw32-gcc -O1 -o d3d9caps.exe d3d9caps.c -ld3d9
 *   wine d3d9caps.exe > caps.txt
 */
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

static const struct { D3DFORMAT format; const char* name; } formats[] = {
    {D3DFMT_A8R8G8B8, "A8R8G8B8"}, {D3DFMT_X8R8G8B8, "X8R8G8B8"}, {D3DFMT_R5G6B5, "R5G6B5"},
    {D3DFMT_A2B10G10R10, "A2B10G10R10"}, {D3DFMT_A2R10G10B10, "A2R10G10B10"},
    {D3DFMT_A16B16G16R16, "A16B16G16R16"}, {D3DFMT_G16R16, "G16R16"},
    {D3DFMT_R16F, "R16F"}, {D3DFMT_G16R16F, "G16R16F"}, {D3DFMT_A16B16G16R16F, "A16B16G16R16F"},
    {D3DFMT_R32F, "R32F"}, {D3DFMT_G32R32F, "G32R32F"}, {D3DFMT_A32B32G32R32F, "A32B32G32R32F"},
    {(D3DFORMAT)MAKEFOURCC('I','N','T','Z'), "INTZ"}, {(D3DFORMAT)MAKEFOURCC('D','F','2','4'), "DF24"},
    {D3DFMT_D24S8, "D24S8"}, {D3DFMT_D16, "D16"},
};

static const struct { DWORD usage; D3DRESOURCETYPE type; const char* name; } uses[] = {
    {0, D3DRTYPE_TEXTURE, "Textur"},
    {D3DUSAGE_QUERY_FILTER, D3DRTYPE_TEXTURE, "Filter"},
    {D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, "RT"},
    {D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING, D3DRTYPE_TEXTURE, "Blend"},
    {D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING, D3DRTYPE_TEXTURE, "RT+Blend"},
    {D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING, D3DRTYPE_SURFACE, "RT+Blend(Fl.)"},
    {D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, "Tiefe"},
};

int main(void)
{
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { printf("Direct3DCreate9 fehlgeschlagen\n"); return 1; }

    D3DADAPTER_IDENTIFIER9 id;
    if (SUCCEEDED(IDirect3D9_GetAdapterIdentifier(d3d, 0, 0, &id)))
        printf("Adapter: %s / %s (Vendor %04lx Device %04lx)\n", id.Description, id.Driver, id.VendorId, id.DeviceId);
    D3DCAPS9 caps;
    if (SUCCEEDED(IDirect3D9_GetDeviceCaps(d3d, 0, D3DDEVTYPE_HAL, &caps)))
        printf("VS %08lx PS %08lx MaxTexture %lux%lu NumSimultaneousRTs %lu\n", caps.VertexShaderVersion, caps.PixelShaderVersion,
               caps.MaxTextureWidth, caps.MaxTextureHeight, caps.NumSimultaneousRTs);

    printf("%-16s", "Format");
    for (unsigned u = 0; u < sizeof(uses) / sizeof(uses[0]); u++) printf(" %-13s", uses[u].name);
    printf("\n");
    for (unsigned f = 0; f < sizeof(formats) / sizeof(formats[0]); f++) {
        printf("%-16s", formats[f].name);
        for (unsigned u = 0; u < sizeof(uses) / sizeof(uses[0]); u++) {
            HRESULT hr = IDirect3D9_CheckDeviceFormat(d3d, 0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, uses[u].usage, uses[u].type, formats[f].format);
            printf(" %-13s", hr == D3D_OK ? "ja" : hr == D3DOK_NOAUTOGEN ? "ja(noautogen)" : "-");
        }
        printf("\n");
    }
    IDirect3D9_Release(d3d);
    return 0;
}
