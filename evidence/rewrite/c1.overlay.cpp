// overlay.cpp — transparent click-through D3D11 overlay
// Build (standalone smoke test):
//   cl /std:c++17 /DOVERLAY_STANDALONE /W3 /O2 overlay.cpp /link d3d11.lib dxgi.lib d3dcompiler.lib user32.lib gdi32.lib

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <DirectXMath.h>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <vector>
#include <string>
#include <unordered_map>

using namespace DirectX;

// ---------------------------------------------------------------------------
// Internal constants
// ---------------------------------------------------------------------------
static constexpr int   FONT_ATLAS_W   = 512;
static constexpr int   FONT_ATLAS_H   = 512;
static constexpr int   FONT_FIRST     = 32;   // first printable ASCII
static constexpr int   FONT_LAST      = 126;  // last printable ASCII
static constexpr int   FONT_CELL_W    = 10;   // pixels per glyph cell
static constexpr int   FONT_CELL_H    = 16;
static constexpr int   FONT_COLS      = FONT_ATLAS_W / FONT_CELL_W;
static constexpr DWORD WS_OVERLAY     = WS_POPUP;
static constexpr DWORD WS_EX_OVERLAY  = WS_EX_TOPMOST | WS_EX_TRANSPARENT |
                                         WS_EX_LAYERED  | WS_EX_NOACTIVATE;

// ---------------------------------------------------------------------------
// Vertex layout
// ---------------------------------------------------------------------------
struct Vertex {
    float x, y;        // NDC
    float u, v;        // tex coord (0,0,0,0 for solid shapes)
    uint32_t color;    // ABGR
};

// ---------------------------------------------------------------------------
// Constant buffer (per-frame)
// ---------------------------------------------------------------------------
struct alignas(16) CB {
    float screenW;
    float screenH;
    float pad[2];
};

// ---------------------------------------------------------------------------
// Glyph descriptor (screen-space source rect in the atlas)
// ---------------------------------------------------------------------------
struct Glyph {
    float u0, v0, u1, v1;  // atlas UV
    int   advance;          // pixels to advance cursor
    int   w, h;             // glyph dimensions in pixels
};

// ---------------------------------------------------------------------------
// Overlay state
// ---------------------------------------------------------------------------
struct Overlay {
    HWND                        hwnd          = nullptr;
    int                         width         = 0;
    int                         height        = 0;
    bool                        visible       = true;

    // D3D11
    ID3D11Device*               device        = nullptr;
    ID3D11DeviceContext*        ctx           = nullptr;
    IDXGISwapChain*             swapChain     = nullptr;
    ID3D11RenderTargetView*     rtv           = nullptr;

    // Pipeline
    ID3D11VertexShader*         vs            = nullptr;
    ID3D11PixelShader*          ps            = nullptr;
    ID3D11InputLayout*          layout        = nullptr;
    ID3D11Buffer*               vb            = nullptr;
    ID3D11Buffer*               cb            = nullptr;
    ID3D11BlendState*           blendState    = nullptr;
    ID3D11RasterizerState*      rsState       = nullptr;
    ID3D11SamplerState*         sampler       = nullptr;

    // Font atlas
    ID3D11Texture2D*            fontTex       = nullptr;
    ID3D11ShaderResourceView*   fontSrv       = nullptr;
    Glyph                       glyphs[128]   = {};

    // Geometry batch
    std::vector<Vertex>         verts;
    static constexpr size_t     VB_MAX        = 65536;

    // White pixel SRV (used for solid draws)
    ID3D11Texture2D*            whiteTex      = nullptr;
    ID3D11ShaderResourceView*   whiteSrv      = nullptr;
};

// ---------------------------------------------------------------------------
// Shader source
// ---------------------------------------------------------------------------
static const char* s_shaderSrc = R"(
cbuffer CB : register(b0)
{
    float screenW;
    float screenH;
    float2 pad;
};

struct VS_IN  { float2 pos : POSITION; float2 uv : TEXCOORD; float4 col : COLOR; };
struct VS_OUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD; float4 col : COLOR; };

Texture2D    tex : register(t0);
SamplerState smp : register(s0);

VS_OUT VS(VS_IN v)
{
    VS_OUT o;
    // Convert pixel-space to NDC (we store pre-NDC, nothing to do here;
    // but accept either: if |x| > 2 treat as pixel coords)
    o.pos = float4(v.pos, 0.0, 1.0);
    o.uv  = v.uv;
    o.col = v.col;
    return o;
}

float4 PS(VS_OUT p) : SV_Target
{
    float4 t = tex.Sample(smp, p.uv);
    return float4(p.col.rgb, p.col.a * t.a);
}
)";

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static uint32_t PackColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
}

static void ColorToFloat4(uint32_t c, float out[4])
{
    out[0] = ((c      ) & 0xFF) / 255.f; // r
    out[1] = ((c >>  8) & 0xFF) / 255.f; // g
    out[2] = ((c >> 16) & 0xFF) / 255.f; // b
    out[3] = ((c >> 24) & 0xFF) / 255.f; // a
}

static XMFLOAT2 PixelToNDC(float px, float py, float w, float h)
{
    return { 2.f * px / w - 1.f, 1.f - 2.f * py / h };
}

// ---------------------------------------------------------------------------
// Font atlas — 8×8 pixel font baked into code (IBM CP437 subset)
// We use GDI to rasterise into a system bitmap if available, else a minimal
// 1-bit hand-coded glyph sheet for printable ASCII 32-126.
// ---------------------------------------------------------------------------

// 5×7 bitmask font (each uint8 = one column, 5 columns per char)
// Source: a minimal public-domain 5×7 pixel font.
// Each character is 5 columns × 7 rows, stored column-major.
static const uint8_t s_font5x7[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // 32 ' '
    {0x00,0x00,0x5F,0x00,0x00}, // 33 '!'
    {0x00,0x07,0x00,0x07,0x00}, // 34 '"'
    {0x14,0x7F,0x14,0x7F,0x14}, // 35 '#'
    {0x24,0x2A,0x7F,0x2A,0x12}, // 36 '$'
    {0x23,0x13,0x08,0x64,0x62}, // 37 '%'
    {0x36,0x49,0x55,0x22,0x50}, // 38 '&'
    {0x00,0x05,0x03,0x00,0x00}, // 39 '''
    {0x00,0x1C,0x22,0x41,0x00}, // 40 '('
    {0x00,0x41,0x22,0x1C,0x00}, // 41 ')'
    {0x14,0x08,0x3E,0x08,0x14}, // 42 '*'
    {0x08,0x08,0x3E,0x08,0x08}, // 43 '+'
    {0x00,0x50,0x30,0x00,0x00}, // 44 ','
    {0x08,0x08,0x08,0x08,0x08}, // 45 '-'
    {0x00,0x60,0x60,0x00,0x00}, // 46 '.'
    {0x20,0x10,0x08,0x04,0x02}, // 47 '/'
    {0x3E,0x51,0x49,0x45,0x3E}, // 48 '0'
    {0x00,0x42,0x7F,0x40,0x00}, // 49 '1'
    {0x42,0x61,0x51,0x49,0x46}, // 50 '2'
    {0x21,0x41,0x45,0x4B,0x31}, // 51 '3'
    {0x18,0x14,0x12,0x7F,0x10}, // 52 '4'
    {0x27,0x45,0x45,0x45,0x39}, // 53 '5'
    {0x3C,0x4A,0x49,0x49,0x30}, // 54 '6'
    {0x01,0x71,0x09,0x05,0x03}, // 55 '7'
    {0x36,0x49,0x49,0x49,0x36}, // 56 '8'
    {0x06,0x49,0x49,0x29,0x1E}, // 57 '9'
    {0x00,0x36,0x36,0x00,0x00}, // 58 ':'
    {0x00,0x56,0x36,0x00,0x00}, // 59 ';'
    {0x08,0x14,0x22,0x41,0x00}, // 60 '<'
    {0x14,0x14,0x14,0x14,0x14}, // 61 '='
    {0x00,0x41,0x22,0x14,0x08}, // 62 '>'
    {0x02,0x01,0x51,0x09,0x06}, // 63 '?'
    {0x32,0x49,0x79,0x41,0x3E}, // 64 '@'
    {0x7E,0x11,0x11,0x11,0x7E}, // 65 'A'
    {0x7F,0x49,0x49,0x49,0x36}, // 66 'B'
    {0x3E,0x41,0x41,0x41,0x22}, // 67 'C'
    {0x7F,0x41,0x41,0x22,0x1C}, // 68 'D'
    {0x7F,0x49,0x49,0x49,0x41}, // 69 'E'
    {0x7F,0x09,0x09,0x09,0x01}, // 70 'F'
    {0x3E,0x41,0x49,0x49,0x7A}, // 71 'G'
    {0x7F,0x08,0x08,0x08,0x7F}, // 72 'H'
    {0x00,0x41,0x7F,0x41,0x00}, // 73 'I'
    {0x20,0x40,0x41,0x3F,0x01}, // 74 'J'
    {0x7F,0x08,0x14,0x22,0x41}, // 75 'K'
    {0x7F,0x40,0x40,0x40,0x40}, // 76 'L'
    {0x7F,0x02,0x0C,0x02,0x7F}, // 77 'M'
    {0x7F,0x04,0x08,0x10,0x7F}, // 78 'N'
    {0x3E,0x41,0x41,0x41,0x3E}, // 79 'O'
    {0x7F,0x09,0x09,0x09,0x06}, // 80 'P'
    {0x3E,0x41,0x51,0x21,0x5E}, // 81 'Q'
    {0x7F,0x09,0x19,0x29,0x46}, // 82 'R'
    {0x46,0x49,0x49,0x49,0x31}, // 83 'S'
    {0x01,0x01,0x7F,0x01,0x01}, // 84 'T'
    {0x3F,0x40,0x40,0x40,0x3F}, // 85 'U'
    {0x1F,0x20,0x40,0x20,0x1F}, // 86 'V'
    {0x3F,0x40,0x38,0x40,0x3F}, // 87 'W'
    {0x63,0x14,0x08,0x14,0x63}, // 88 'X'
    {0x07,0x08,0x70,0x08,0x07}, // 89 'Y'
    {0x61,0x51,0x49,0x45,0x43}, // 90 'Z'
    {0x00,0x7F,0x41,0x41,0x00}, // 91 '['
    {0x02,0x04,0x08,0x10,0x20}, // 92 '\'
    {0x00,0x41,0x41,0x7F,0x00}, // 93 ']'
    {0x04,0x02,0x01,0x02,0x04}, // 94 '^'
    {0x40,0x40,0x40,0x40,0x40}, // 95 '_'
    {0x00,0x01,0x02,0x04,0x00}, // 96 '`'
    {0x20,0x54,0x54,0x54,0x78}, // 97 'a'
    {0x7F,0x48,0x44,0x44,0x38}, // 98 'b'
    {0x38,0x44,0x44,0x44,0x20}, // 99 'c'
    {0x38,0x44,0x44,0x48,0x7F}, // 100 'd'
    {0x38,0x54,0x54,0x54,0x18}, // 101 'e'
    {0x08,0x7E,0x09,0x01,0x02}, // 102 'f'
    {0x0C,0x52,0x52,0x52,0x3E}, // 103 'g'
    {0x7F,0x08,0x04,0x04,0x78}, // 104 'h'
    {0x00,0x44,0x7D,0x40,0x00}, // 105 'i'
    {0x20,0x40,0x44,0x3D,0x00}, // 106 'j'
    {0x7F,0x10,0x28,0x44,0x00}, // 107 'k'
    {0x00,0x41,0x7F,0x40,0x00}, // 108 'l'
    {0x7C,0x04,0x18,0x04,0x78}, // 109 'm'
    {0x7C,0x08,0x04,0x04,0x78}, // 110 'n'
    {0x38,0x44,0x44,0x44,0x38}, // 111 'o'
    {0x7C,0x14,0x14,0x14,0x08}, // 112 'p'
    {0x08,0x14,0x14,0x18,0x7C}, // 113 'q'
    {0x7C,0x08,0x04,0x04,0x08}, // 114 'r'
    {0x48,0x54,0x54,0x54,0x20}, // 115 's'
    {0x04,0x3F,0x44,0x40,0x20}, // 116 't'
    {0x3C,0x40,0x40,0x20,0x7C}, // 117 'u'
    {0x1C,0x20,0x40,0x20,0x1C}, // 118 'v'
    {0x3C,0x40,0x30,0x40,0x3C}, // 119 'w'
    {0x44,0x28,0x10,0x28,0x44}, // 120 'x'
    {0x0C,0x50,0x50,0x50,0x3C}, // 121 'y'
    {0x44,0x64,0x54,0x4C,0x44}, // 122 'z'
    {0x00,0x08,0x36,0x41,0x00}, // 123 '{'
    {0x00,0x00,0x7F,0x00,0x00}, // 124 '|'
    {0x00,0x41,0x36,0x08,0x00}, // 125 '}'
    {0x10,0x08,0x08,0x10,0x08}, // 126 '~'
};

// ---------------------------------------------------------------------------
// Build the font atlas into an RGBA8 buffer
// Glyph cell is FONT_CELL_W × FONT_CELL_H, font is 5×7 centred in the cell.
// ---------------------------------------------------------------------------
static std::vector<uint8_t> BuildFontAtlas(Glyph glyphs[128])
{
    std::vector<uint8_t> pixels(FONT_ATLAS_W * FONT_ATLAS_H * 4, 0);

    const int offX = 1; // left pad inside cell
    const int offY = (FONT_CELL_H - 7) / 2;

    for (int ch = FONT_FIRST; ch <= FONT_LAST; ch++) {
        int idx    = ch - FONT_FIRST;
        int cellX  = (idx % FONT_COLS) * FONT_CELL_W;
        int cellY  = (idx / FONT_COLS) * FONT_CELL_H;

        const uint8_t* col5 = s_font5x7[ch - 32];

        for (int col = 0; col < 5; col++) {
            for (int row = 0; row < 7; row++) {
                if (col5[col] & (1 << row)) {
                    int px = cellX + offX + col;
                    int py = cellY + offY + row;
                    if (px < FONT_ATLAS_W && py < FONT_ATLAS_H) {
                        uint8_t* p = &pixels[(py * FONT_ATLAS_W + px) * 4];
                        p[0] = p[1] = p[2] = p[3] = 0xFF;
                    }
                }
            }
        }

        glyphs[ch].u0      = (float)cellX           / FONT_ATLAS_W;
        glyphs[ch].v0      = (float)cellY           / FONT_ATLAS_H;
        glyphs[ch].u1      = (float)(cellX + FONT_CELL_W) / FONT_ATLAS_W;
        glyphs[ch].v1      = (float)(cellY + FONT_CELL_H) / FONT_ATLAS_H;
        glyphs[ch].advance = FONT_CELL_W;
        glyphs[ch].w       = FONT_CELL_W;
        glyphs[ch].h       = FONT_CELL_H;
    }

    // space glyph
    glyphs[32].advance = FONT_CELL_W;
    glyphs[32].w       = FONT_CELL_W;
    glyphs[32].h       = FONT_CELL_H;

    return pixels;
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------
static LRESULT CALLBACK OverlayWndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(hw, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/// Create the overlay window and initialise D3D11.
/// @param width/height  client area in pixels (0 → full primary monitor)
/// @param title         window class/title
Overlay* Overlay_Create(int width, int height, const wchar_t* title)
{
    if (width  <= 0) width  = GetSystemMetrics(SM_CXSCREEN);
    if (height <= 0) height = GetSystemMetrics(SM_CYSCREEN);

    // --- window ---
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = OverlayWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = title;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_OVERLAY, title, title, WS_OVERLAY,
        0, 0, width, height, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return nullptr;

    SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
    ShowWindow(hwnd, SW_SHOW);

    // --- D3D11 ---
    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount                = 2;
    scd.BufferDesc.Width           = (UINT)width;
    scd.BufferDesc.Height          = (UINT)height;
    scd.BufferDesc.Format          = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.RefreshRate     = {60, 1};
    scd.BufferUsage                = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow               = hwnd;
    scd.SampleDesc.Count           = 1;
    scd.Windowed                   = TRUE;
    scd.SwapEffect                 = DXGI_SWAP_EFFECT_DISCARD;
    scd.Flags                      = 0;

    D3D_FEATURE_LEVEL fl;
    ID3D11Device*        dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain*      sc  = nullptr;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION,
        &scd, &sc, &dev, &fl, &ctx);
    if (FAILED(hr)) { DestroyWindow(hwnd); return nullptr; }

    // back-buffer RTV
    ID3D11Texture2D* bb = nullptr;
    sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    ID3D11RenderTargetView* rtv = nullptr;
    dev->CreateRenderTargetView(bb, nullptr, &rtv);
    bb->Release();

    // --- shaders ---
    ID3DBlob *vsBlob = nullptr, *psBlob = nullptr, *errBlob = nullptr;
    D3DCompile(s_shaderSrc, strlen(s_shaderSrc), nullptr, nullptr, nullptr,
               "VS", "vs_4_0", 0, 0, &vsBlob, &errBlob);
    if (errBlob) errBlob->Release();
    D3DCompile(s_shaderSrc, strlen(s_shaderSrc), nullptr, nullptr, nullptr,
               "PS", "ps_4_0", 0, 0, &psBlob, &errBlob);
    if (errBlob) errBlob->Release();

    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader*  ps = nullptr;
    dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs);
    dev->CreatePixelShader (psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps);

    // input layout
    D3D11_INPUT_ELEMENT_DESC ied[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,   0, offsetof(Vertex,x), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,   0, offsetof(Vertex,u), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM,  0, offsetof(Vertex,color), D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    ID3D11InputLayout* layout = nullptr;
    dev->CreateInputLayout(ied, 3, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &layout);
    vsBlob->Release();
    psBlob->Release();

    // vertex buffer (dynamic)
    D3D11_BUFFER_DESC vbd = {};
    vbd.ByteWidth      = (UINT)(Overlay::VB_MAX * sizeof(Vertex));
    vbd.Usage          = D3D11_USAGE_DYNAMIC;
    vbd.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
    vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer* vb = nullptr;
    dev->CreateBuffer(&vbd, nullptr, &vb);

    // constant buffer
    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth      = sizeof(CB);
    cbd.Usage          = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer* cb = nullptr;
    dev->CreateBuffer(&cbd, nullptr, &cb);

    // blend state (pre-multiplied alpha)
    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable           = TRUE;
    bd.RenderTarget[0].SrcBlend              = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ID3D11BlendState* blendState = nullptr;
    dev->CreateBlendState(&bd, &blendState);

    // rasteriser (scissor off, cull none)
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode        = D3D11_FILL_SOLID;
    rd.CullMode        = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    ID3D11RasterizerState* rs = nullptr;
    dev->CreateRasterizerState(&rd, &rs);

    // sampler
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    ID3D11SamplerState* sampler = nullptr;
    dev->CreateSamplerState(&sd, &sampler);

    // --- font atlas ---
    Overlay* ov = new Overlay();
    std::vector<uint8_t> atlasData = BuildFontAtlas(ov->glyphs);

    D3D11_TEXTURE2D_DESC td = {};
    td.Width            = FONT_ATLAS_W;
    td.Height           = FONT_ATLAS_H;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initData = { atlasData.data(), FONT_ATLAS_W * 4, 0 };
    dev->CreateTexture2D(&td, &initData, &ov->fontTex);
    dev->CreateShaderResourceView(ov->fontTex, nullptr, &ov->fontSrv);

    // 1×1 white texture
    uint8_t white[4] = {255,255,255,255};
    D3D11_TEXTURE2D_DESC wd = {};
    wd.Width = wd.Height = 1;
    wd.MipLevels = wd.ArraySize = 1;
    wd.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    wd.SampleDesc.Count = 1;
    wd.Usage            = D3D11_USAGE_DEFAULT;
    wd.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA wd2 = { white, 4, 0 };
    dev->CreateTexture2D(&wd, &wd2, &ov->whiteTex);
    dev->CreateShaderResourceView(ov->whiteTex, nullptr, &ov->whiteSrv);

    ov->hwnd       = hwnd;
    ov->width      = width;
    ov->height     = height;
    ov->device     = dev;
    ov->ctx        = ctx;
    ov->swapChain  = sc;
    ov->rtv        = rtv;
    ov->vs         = vs;
    ov->ps         = ps;
    ov->layout     = layout;
    ov->vb         = vb;
    ov->cb         = cb;
    ov->blendState = blendState;
    ov->rsState    = rs;
    ov->sampler    = sampler;
    ov->verts.reserve(Overlay::VB_MAX);

    return ov;
}

/// Destroy all resources and the window.
void Overlay_Destroy(Overlay* ov)
{
    if (!ov) return;
    if (ov->sampler)   ov->sampler->Release();
    if (ov->rsState)   ov->rsState->Release();
    if (ov->blendState)ov->blendState->Release();
    if (ov->cb)        ov->cb->Release();
    if (ov->vb)        ov->vb->Release();
    if (ov->layout)    ov->layout->Release();
    if (ov->ps)        ov->ps->Release();
    if (ov->vs)        ov->vs->Release();
    if (ov->fontSrv)   ov->fontSrv->Release();
    if (ov->fontTex)   ov->fontTex->Release();
    if (ov->whiteSrv)  ov->whiteSrv->Release();
    if (ov->whiteTex)  ov->whiteTex->Release();
    if (ov->rtv)       ov->rtv->Release();
    if (ov->swapChain) ov->swapChain->Release();
    if (ov->ctx)       ov->ctx->Release();
    if (ov->device)    ov->device->Release();
    if (ov->hwnd)      DestroyWindow(ov->hwnd);
    delete ov;
}

/// Pump window messages; returns false when WM_QUIT received.
bool Overlay_PumpMessages(Overlay* /*ov*/)
{
    MSG msg = {};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) return false;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return true;
}

/// Clear the back-buffer and prepare for draw calls.
void Overlay_BeginFrame(Overlay* ov)
{
    ov->verts.clear();

    // Update constant buffer
    D3D11_MAPPED_SUBRESOURCE ms = {};
    ov->ctx->Map(ov->cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
    CB* cbData   = (CB*)ms.pData;
    cbData->screenW = (float)ov->width;
    cbData->screenH = (float)ov->height;
    ov->ctx->Unmap(ov->cb, 0);

    // Set up pipeline
    const float cc[4] = {0,0,0,0};
    ov->ctx->ClearRenderTargetView(ov->rtv, cc);
    ov->ctx->OMSetRenderTargets(1, &ov->rtv, nullptr);

    D3D11_VIEWPORT vp = { 0,0,(float)ov->width,(float)ov->height,0,1 };
    ov->ctx->RSSetViewports(1, &vp);
    ov->ctx->RSSetState(ov->rsState);

    float bf[4] = {0,0,0,0};
    ov->ctx->OMSetBlendState(ov->blendState, bf, 0xFFFFFFFF);

    ov->ctx->VSSetShader(ov->vs, nullptr, 0);
    ov->ctx->PSSetShader(ov->ps, nullptr, 0);
    ov->ctx->IASetInputLayout(ov->layout);
    ov->ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ov->ctx->VSSetConstantBuffers(0, 1, &ov->cb);
    ov->ctx->PSSetSamplers(0, 1, &ov->sampler);

    UINT stride = sizeof(Vertex), offset = 0;
    ov->ctx->IASetVertexBuffers(0, 1, &ov->vb, &stride, &offset);
}

// Internal: flush the current vertex batch using the given SRV
static void FlushBatch(Overlay* ov, ID3D11ShaderResourceView* srv)
{
    if (ov->verts.empty()) return;

    D3D11_MAPPED_SUBRESOURCE ms = {};
    ov->ctx->Map(ov->vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
    memcpy(ms.pData, ov->verts.data(), ov->verts.size() * sizeof(Vertex));
    ov->ctx->Unmap(ov->vb, 0);

    ov->ctx->PSSetShaderResources(0, 1, &srv);
    ov->ctx->Draw((UINT)ov->verts.size(), 0);
    ov->verts.clear();
}

/// Submit geometry and present.
void Overlay_EndFrame(Overlay* ov)
{
    // Flush remaining solid geometry
    FlushBatch(ov, ov->whiteSrv);
    ov->swapChain->Present(1, 0);
}

// Internal: convert pixel coords to NDC float2
static XMFLOAT2 P2N(float px, float py, int w, int h)
{
    return { 2.f*px/(float)w - 1.f, 1.f - 2.f*py/(float)h };
}

// Internal: push a solid-colour triangle (pre-multiplied alpha)
static void PushTri(Overlay* ov,
    float x0, float y0, float x1, float y1, float x2, float y2,
    uint32_t color)
{
    if (ov->verts.size() + 3 > Overlay::VB_MAX) FlushBatch(ov, ov->whiteSrv);

    // pre-multiply
    uint8_t a = (color >> 24) & 0xFF;
    uint8_t r = (uint8_t)(((color      ) & 0xFF) * a / 255);
    uint8_t g = (uint8_t)(((color >>  8) & 0xFF) * a / 255);
    uint8_t b = (uint8_t)(((color >> 16) & 0xFF) * a / 255);
    uint32_t c = ((uint32_t)a<<24)|((uint32_t)b<<16)|((uint32_t)g<<8)|r;

    int W = ov->width, H = ov->height;
    auto n0 = P2N(x0,y0,W,H), n1 = P2N(x1,y1,W,H), n2 = P2N(x2,y2,W,H);
    ov->verts.push_back({n0.x,n0.y,0,0,c});
    ov->verts.push_back({n1.x,n1.y,0,0,c});
    ov->verts.push_back({n2.x,n2.y,0,0,c});
}

// Internal: push a solid quad (two triangles)
static void PushQuad(Overlay* ov,
    float x0, float y0, float x1, float y1, uint32_t color)
{
    PushTri(ov, x0,y0, x1,y0, x1,y1, color);
    PushTri(ov, x0,y0, x1,y1, x0,y1, color);
}

/// Draw a filled or outlined rectangle in screen-space pixels.
/// @param x,y       top-left corner
/// @param w,h       width and height
/// @param color     RGBA8 (0xAABBGGRR)
/// @param filled    true = filled, false = 1-pixel outline
void Overlay_DrawRect(Overlay* ov,
    float x, float y, float w, float h,
    uint32_t color, bool filled)
{
    if (filled) {
        PushQuad(ov, x, y, x+w, y+h, color);
    } else {
        // Top, Bottom, Left, Right (1-px border)
        PushQuad(ov, x,       y,       x+w,   y+1,   color);
        PushQuad(ov, x,       y+h-1,   x+w,   y+h,   color);
        PushQuad(ov, x,       y+1,     x+1,   y+h-1, color);
        PushQuad(ov, x+w-1,   y+1,     x+w,   y+h-1, color);
    }
}

/// Draw a 1-pixel-wide line using a thin quad.
void Overlay_DrawLine(Overlay* ov,
    float x0, float y0, float x1, float y1,
    uint32_t color)
{
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx*dx + dy*dy);
    if (len < 0.5f) return;

    // Perpendicular unit vector, scaled to 0.5px each side
    float nx = -dy / len * 0.5f;
    float ny =  dx / len * 0.5f;

    // Build a thin quad around the line
    int W = ov->width, H = ov->height;

    uint8_t a = (color >> 24) & 0xFF;
    uint8_t r = (uint8_t)(((color      ) & 0xFF) * a / 255);
    uint8_t g = (uint8_t)(((color >>  8) & 0xFF) * a / 255);
    uint8_t b = (uint8_t)(((color >> 16) & 0xFF) * a / 255);
    uint32_t c = ((uint32_t)a<<24)|((uint32_t)b<<16)|((uint32_t)g<<8)|r;

    if (ov->verts.size() + 6 > Overlay::VB_MAX) FlushBatch(ov, ov->whiteSrv);

    auto N = [&](float px, float py) { return P2N(px, py, W, H); };
    XMFLOAT2 p0a = N(x0+nx, y0+ny), p0b = N(x0-nx, y0-ny);
    XMFLOAT2 p1a = N(x1+nx, y1+ny), p1b = N(x1-nx, y1-ny);

    ov->verts.push_back({p0a.x, p0a.y, 0,0, c});
    ov->verts.push_back({p1a.x, p1a.y, 0,0, c});
    ov->verts.push_back({p1b.x, p1b.y, 0,0, c});
    ov->verts.push_back({p0a.x, p0a.y, 0,0, c});
    ov->verts.push_back({p1b.x, p1b.y, 0,0, c});
    ov->verts.push_back({p0b.x, p0b.y, 0,0, c});
}

/// Draw a string using the built-in font atlas.
/// @param x,y    top-left of first character, screen pixels
/// @param scale  integer scale factor (1 = native 5×7 in a 10×16 cell)
/// @param color  RGBA8
void Overlay_DrawText(Overlay* ov,
    float x, float y,
    const char* text,
    uint32_t color,
    int scale)
{
    if (!text || scale < 1) return;

    // We need to flush solid geometry before switching to the font SRV,
    // then flush font geometry before returning.
    FlushBatch(ov, ov->whiteSrv);

    int W = ov->width, H = ov->height;

    uint8_t a = (color >> 24) & 0xFF;
    uint8_t r = (uint8_t)(((color      ) & 0xFF) * a / 255);
    uint8_t g = (uint8_t)(((color >>  8) & 0xFF) * a / 255);
    uint8_t b = (uint8_t)(((color >> 16) & 0xFF) * a / 255);
    uint32_t c = ((uint32_t)a<<24)|((uint32_t)b<<16)|((uint32_t)g<<8)|r;

    float cx = x;
    for (const char* p = text; *p; ++p) {
        int ch = (unsigned char)*p;
        if (ch < 32 || ch > 126) { cx += (float)(FONT_CELL_W * scale); continue; }
        const Glyph& g2 = ov->glyphs[ch];

        float px0 = cx;
        float py0 = y;
        float px1 = cx + (float)(g2.w * scale);
        float py1 = y  + (float)(g2.h * scale);

        if (ov->verts.size() + 6 > Overlay::VB_MAX) FlushBatch(ov, ov->fontSrv);

        auto N = [&](float qx, float qy) { return P2N(qx, qy, W, H); };
        XMFLOAT2 n00 = N(px0,py0), n10 = N(px1,py0);
        XMFLOAT2 n01 = N(px0,py1), n11 = N(px1,py1);

        ov->verts.push_back({n00.x,n00.y, g2.u0,g2.v0, c});
        ov->verts.push_back({n10.x,n10.y, g2.u1,g2.v0, c});
        ov->verts.push_back({n11.x,n11.y, g2.u1,g2.v1, c});
        ov->verts.push_back({n00.x,n00.y, g2.u0,g2.v0, c});
        ov->verts.push_back({n11.x,n11.y, g2.u1,g2.v1, c});
        ov->verts.push_back({n01.x,n01.y, g2.u0,g2.v1, c});

        cx += (float)(g2.advance * scale);
    }

    FlushBatch(ov, ov->fontSrv);
}

// ---------------------------------------------------------------------------
// Smoke test (define OVERLAY_STANDALONE to link standalone)
// ---------------------------------------------------------------------------
#ifdef OVERLAY_STANDALONE

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    Overlay* ov = Overlay_Create(1280, 720, L"OverlayTest");
    if (!ov) return 1;

    while (Overlay_PumpMessages(ov)) {
        Overlay_BeginFrame(ov);

        // Filled box (semi-transparent green)
        Overlay_DrawRect(ov, 100, 100, 200, 100, 0xAA00FF00, true);
        // Outlined box (red)
        Overlay_DrawRect(ov, 320, 100, 200, 100, 0xFFFF0000, false /*outline*/);
        // Line (white)
        Overlay_DrawLine(ov, 100, 250, 520, 250, 0xFFFFFFFF);
        // Text
        Overlay_DrawText(ov, 100, 280, "Hello, Overlay!", 0xFFFFFFFF, 2);

        Overlay_EndFrame(ov);
    }

    Overlay_Destroy(ov);
    return 0;
}

#endif // OVERLAY_STANDALONE
