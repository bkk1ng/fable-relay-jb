// overlay.cpp
// Transparent, click-through, always-on-top Win32 window backed by a D3D11 swap chain.
// Provides orthographic projection and screen-space drawing primitives.
//
// Build (MSVC):
//   cl /std:c++17 /O2 /W3 /DUNICODE /D_UNICODE overlay.cpp /link d3d11.lib dxgi.lib d3dcompiler.lib
//
// Exported API (call after Overlay::Init):
//   Overlay::Init(hInstance)      – create window + D3D11 device/swap chain
//   Overlay::Shutdown()           – release all resources
//   Overlay::BeginFrame()         – clear render target, begin drawing
//   Overlay::EndFrame()           – present (honours target frame rate)
//   Overlay::SetTargetFps(float)  – default 60
//   Overlay::DrawRect(...)        – filled or outline rectangle in screen space
//   Overlay::DrawLine(...)        – line segment in screen space
//   Overlay::DrawText(...)        – GDI-rendered text via dynamic texture

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

// ---------------------------------------------------------------------------
// Colour helpers
// ---------------------------------------------------------------------------
struct Color {
    float r, g, b, a;
    constexpr Color(float r, float g, float b, float a = 1.f) : r(r), g(g), b(b), a(a) {}
    // convenience: 0xAARRGGBB
    static constexpr Color fromARGB(uint32_t c) {
        return Color(
            ((c >> 16) & 0xFF) / 255.f,
            ((c >>  8) & 0xFF) / 255.f,
            ((c      ) & 0xFF) / 255.f,
            ((c >> 24) & 0xFF) / 255.f);
    }
};

// ---------------------------------------------------------------------------
// Vertex layout
// ---------------------------------------------------------------------------
struct Vertex {
    float x, y;        // screen-space pixel position
    float u, v;        // tex-coords (0 when not textured)
    float r, g, b, a;  // per-vertex colour
};

// ---------------------------------------------------------------------------
// HLSL shaders (compiled at runtime)
// ---------------------------------------------------------------------------
static const char* s_shaderSrc = R"HLSL(
cbuffer OrthoConstants : register(b0)
{
    float4x4 Proj;
};

struct VS_IN  { float2 pos : POSITION; float2 uv : TEXCOORD0; float4 col : COLOR; };
struct VS_OUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 col : COLOR; };

VS_OUT VS(VS_IN v)
{
    VS_OUT o;
    o.pos = mul(Proj, float4(v.pos, 0.f, 1.f));
    o.uv  = v.uv;
    o.col = v.col;
    return o;
}

Texture2D    gTex  : register(t0);
SamplerState gSamp : register(s0);

float4 PS(VS_OUT p) : SV_TARGET
{
    float4 t = gTex.Sample(gSamp, p.uv);
    // When UVs are (0,0) the texture is a 1x1 white pixel, so colour passes through.
    return t * p.col;
}
)HLSL";

// ---------------------------------------------------------------------------
// Overlay state (module-private)
// ---------------------------------------------------------------------------
namespace {

HWND                     g_hwnd            = nullptr;
int                      g_width           = 0;
int                      g_height          = 0;
float                    g_targetFps       = 60.f;
LARGE_INTEGER            g_qpcFreq         = {};
LARGE_INTEGER            g_lastPresent     = {};

// D3D11 objects
ID3D11Device*            g_device          = nullptr;
ID3D11DeviceContext*     g_ctx             = nullptr;
IDXGISwapChain1*         g_swapChain       = nullptr;
ID3D11RenderTargetView*  g_rtv             = nullptr;

ID3D11VertexShader*      g_vs              = nullptr;
ID3D11PixelShader*       g_ps              = nullptr;
ID3D11InputLayout*       g_inputLayout     = nullptr;
ID3D11Buffer*            g_vb              = nullptr;
ID3D11Buffer*            g_cbOrtho         = nullptr;
ID3D11BlendState*        g_blendState      = nullptr;
ID3D11RasterizerState*   g_rasterState     = nullptr;
ID3D11DepthStencilState* g_dsState         = nullptr;
ID3D11SamplerState*      g_sampler         = nullptr;

// 1×1 white texture used for colour-only primitives
ID3D11ShaderResourceView* g_whiteSRV       = nullptr;

static constexpr UINT    kVBCapacity       = 65536; // vertices
std::vector<Vertex>      g_verts;

// text texture cache (single-use per frame, re-created on demand)
struct TextEntry {
    ID3D11ShaderResourceView* srv;
    int tw, th;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static bool HR(HRESULT hr) { return SUCCEEDED(hr); }

static void SafeRelease(IUnknown** pp) {
    if (*pp) { (*pp)->Release(); *pp = nullptr; }
}

static void BuildOrthoMatrix(float* m44, float w, float h)
{
    // Column-major ortho projection: maps (0,0)-(w,h) to NDC (-1,1) Y-up
    // We flip Y so that pixel (0,0) is top-left.
    //  |  2/w   0     0  -1  |
    //  |  0    -2/h   0   1  |
    //  |  0     0     1   0  |
    //  |  0     0     0   1  |
    memset(m44, 0, 16 * sizeof(float));
    m44[ 0] =  2.f / w;
    m44[ 5] = -2.f / h;
    m44[10] =  1.f;
    m44[12] = -1.f;
    m44[13] =  1.f;
    m44[15] =  1.f;
}

static bool CreateWhiteTexture()
{
    D3D11_TEXTURE2D_DESC td{};
    td.Width  = 1; td.Height = 1; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    uint32_t white = 0xFFFFFFFFu;
    D3D11_SUBRESOURCE_DATA init{ &white, sizeof(white), 0 };

    ID3D11Texture2D* tex = nullptr;
    if (!HR(g_device->CreateTexture2D(&td, &init, &tex))) return false;
    HRESULT hr = g_device->CreateShaderResourceView(tex, nullptr, &g_whiteSRV);
    tex->Release();
    return HR(hr);
}

static bool CompileAndCreateShaders()
{
    HRESULT hr;
    ID3DBlob* vsBlob = nullptr; ID3DBlob* psBlob = nullptr; ID3DBlob* err = nullptr;

    hr = D3DCompile(s_shaderSrc, strlen(s_shaderSrc), nullptr, nullptr, nullptr,
                    "VS", "vs_4_0", 0, 0, &vsBlob, &err);
    if (!HR(hr)) {
        if (err) { OutputDebugStringA((char*)err->GetBufferPointer()); err->Release(); }
        return false;
    }
    hr = D3DCompile(s_shaderSrc, strlen(s_shaderSrc), nullptr, nullptr, nullptr,
                    "PS", "ps_4_0", 0, 0, &psBlob, &err);
    if (!HR(hr)) {
        if (err) { OutputDebugStringA((char*)err->GetBufferPointer()); err->Release(); }
        vsBlob->Release(); return false;
    }

    hr = g_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_vs);
    if (!HR(hr)) { vsBlob->Release(); psBlob->Release(); return false; }

    hr = g_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_ps);
    if (!HR(hr)) { vsBlob->Release(); psBlob->Release(); return false; }

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0, offsetof(Vertex,x), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, offsetof(Vertex,u), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT,  0, offsetof(Vertex,r), D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    hr = g_device->CreateInputLayout(layout, 3, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &g_inputLayout);
    vsBlob->Release(); psBlob->Release();
    return HR(hr);
}

// Push two triangles (quad) for a rectangle [x0,y0]-[x1,y1] with given uvs
static void PushQuad(float x0, float y0, float x1, float y1,
                     float u0, float v0, float u1, float v1,
                     const Color& c)
{
    Vertex tl{ x0,y0, u0,v0, c.r,c.g,c.b,c.a };
    Vertex tr{ x1,y0, u1,v0, c.r,c.g,c.b,c.a };
    Vertex bl{ x0,y1, u0,v1, c.r,c.g,c.b,c.a };
    Vertex br{ x1,y1, u1,v1, c.r,c.g,c.b,c.a };
    g_verts.push_back(tl); g_verts.push_back(tr); g_verts.push_back(bl);
    g_verts.push_back(tr); g_verts.push_back(br); g_verts.push_back(bl);
}

// Flush accumulated vertices to the GPU and draw them with the given SRV
static void Flush(ID3D11ShaderResourceView* srv)
{
    if (g_verts.empty()) return;

    // Orphan-and-upload
    D3D11_MAPPED_SUBRESOURCE ms{};
    if (HR(g_ctx->Map(g_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        size_t bytes = g_verts.size() * sizeof(Vertex);
        memcpy(ms.pData, g_verts.data(), bytes);
        g_ctx->Unmap(g_vb, 0);
    }

    g_ctx->PSSetShaderResources(0, 1, &srv);
    g_ctx->Draw(static_cast<UINT>(g_verts.size()), 0);
    g_verts.clear();
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------
static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ===========================================================================
// Public API
// ===========================================================================
namespace Overlay {

bool Init(HINSTANCE hInstance, float targetFps)
{
    QueryPerformanceFrequency(&g_qpcFreq);
    QueryPerformanceCounter(&g_lastPresent);
    g_targetFps = targetFps > 0.f ? targetFps : 60.f;

    // ---- window ----
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = OverlayWndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = L"D3D11Overlay";
    RegisterClassExW(&wc);

    g_width  = GetSystemMetrics(SM_CXSCREEN);
    g_height = GetSystemMetrics(SM_CYSCREEN);

    g_hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_NOACTIVATE,
        L"D3D11Overlay", L"Overlay",
        WS_POPUP,
        0, 0, g_width, g_height,
        nullptr, nullptr, hInstance, nullptr);
    if (!g_hwnd) return false;

    // Layered window: colour-key 0x00000000 as fully transparent (we use alpha blending instead)
    SetLayeredWindowAttributes(g_hwnd, 0, 0, LWA_ALPHA); // start fully transparent
    // We'll rely on D3D11 alpha blending; set window alpha to 255 so the compositor passes through
    SetLayeredWindowAttributes(g_hwnd, 0, 255, LWA_ALPHA);

    ShowWindow(g_hwnd, SW_SHOW);

    // ---- DXGI / D3D11 ----
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    UINT flags = 0;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    if (!HR(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                               &fl, 1, D3D11_SDK_VERSION, &g_device, nullptr, &g_ctx)))
        return false;

    IDXGIDevice1* dxgiDev = nullptr; g_device->QueryInterface(__uuidof(IDXGIDevice1), (void**)&dxgiDev);
    IDXGIAdapter* adapter = nullptr; dxgiDev->GetAdapter(&adapter);
    IDXGIFactory2* factory = nullptr; adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory);

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width  = static_cast<UINT>(g_width);
    scd.Height = static_cast<UINT>(g_height);
    scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    scd.AlphaMode   = DXGI_ALPHA_MODE_PREMULTIPLIED;

    HRESULT hr = factory->CreateSwapChainForComposition(g_device, &scd, nullptr, &g_swapChain);
    if (!HR(hr)) {
        // Fall back: composition may not be available (e.g. DWM off)
        scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        hr = factory->CreateSwapChainForHwnd(g_device, g_hwnd, &scd, nullptr, nullptr,
                                              (IDXGISwapChain1**)&g_swapChain);
        if (!HR(hr)) { factory->Release(); adapter->Release(); dxgiDev->Release(); return false; }
    } else {
        // Bind swap chain to the window via DComposition (simple path without dcomp.lib dependency)
        // For the composition swap chain we still associate it with the HWND via legacy method
        IDXGISwapChain* sc = nullptr;
        g_swapChain->QueryInterface(__uuidof(IDXGISwapChain), (void**)&sc);
        factory->MakeWindowAssociation(g_hwnd, DXGI_MWA_NO_ALT_ENTER);
        if (sc) sc->Release();
    }
    factory->Release(); adapter->Release(); dxgiDev->Release();

    // ---- RTV ----
    ID3D11Texture2D* back = nullptr;
    g_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back);
    g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
    back->Release();

    // ---- Shaders + layout ----
    if (!CompileAndCreateShaders()) return false;

    // ---- Constant buffer (ortho projection) ----
    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = 64; // 4x4 float
    cbd.Usage     = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (!HR(g_device->CreateBuffer(&cbd, nullptr, &g_cbOrtho))) return false;

    // ---- Vertex buffer ----
    D3D11_BUFFER_DESC vbd{};
    vbd.ByteWidth = kVBCapacity * sizeof(Vertex);
    vbd.Usage     = D3D11_USAGE_DYNAMIC;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (!HR(g_device->CreateBuffer(&vbd, nullptr, &g_vb))) return false;

    // ---- Blend state: standard premultiplied alpha ----
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable           = TRUE;
    bd.RenderTarget[0].SrcBlend              = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (!HR(g_device->CreateBlendState(&bd, &g_blendState))) return false;

    // ---- Rasteriser ----
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode        = D3D11_FILL_SOLID;
    rd.CullMode        = D3D11_CULL_NONE;
    rd.ScissorEnable   = FALSE;
    rd.DepthClipEnable = TRUE;
    if (!HR(g_device->CreateRasterizerState(&rd, &g_rasterState))) return false;

    // ---- Depth-stencil: disabled ----
    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable   = FALSE;
    dsd.StencilEnable = FALSE;
    if (!HR(g_device->CreateDepthStencilState(&dsd, &g_dsState))) return false;

    // ---- Sampler ----
    D3D11_SAMPLER_DESC sd{};
    sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxAnisotropy  = 1;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD         = D3D11_FLOAT32_MAX;
    if (!HR(g_device->CreateSamplerState(&sd, &g_sampler))) return false;

    // ---- 1×1 white texture ----
    if (!CreateWhiteTexture()) return false;

    g_verts.reserve(4096);
    return true;
}

void Shutdown()
{
    if (g_ctx) g_ctx->ClearState();
    auto R = [](IUnknown** p){ if (*p){ (*p)->Release(); *p=nullptr; } };
    R((IUnknown**)&g_whiteSRV);
    R((IUnknown**)&g_sampler);
    R((IUnknown**)&g_dsState);
    R((IUnknown**)&g_rasterState);
    R((IUnknown**)&g_blendState);
    R((IUnknown**)&g_cbOrtho);
    R((IUnknown**)&g_vb);
    R((IUnknown**)&g_inputLayout);
    R((IUnknown**)&g_ps);
    R((IUnknown**)&g_vs);
    R((IUnknown**)&g_rtv);
    R((IUnknown**)&g_swapChain);
    R((IUnknown**)&g_ctx);
    R((IUnknown**)&g_device);
    if (g_hwnd) { DestroyWindow(g_hwnd); g_hwnd = nullptr; }
}

void SetTargetFps(float fps) { g_targetFps = fps > 0.f ? fps : 60.f; }

// ---------------------------------------------------------------------------
// Frame management
// ---------------------------------------------------------------------------
void BeginFrame()
{
    // Clear to fully transparent black
    const float clear[4] = { 0.f, 0.f, 0.f, 0.f };
    g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
    g_ctx->ClearRenderTargetView(g_rtv, clear);

    // Viewport
    D3D11_VIEWPORT vp{ 0.f, 0.f, (float)g_width, (float)g_height, 0.f, 1.f };
    g_ctx->RSSetViewports(1, &vp);

    // Pipeline state
    g_ctx->VSSetShader(g_vs, nullptr, 0);
    g_ctx->PSSetShader(g_ps, nullptr, 0);
    g_ctx->IASetInputLayout(g_inputLayout);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    UINT stride = sizeof(Vertex), offset = 0;
    g_ctx->IASetVertexBuffers(0, 1, &g_vb, &stride, &offset);
    g_ctx->VSSetConstantBuffers(0, 1, &g_cbOrtho);

    float blend[4] = {};
    g_ctx->OMSetBlendState(g_blendState, blend, 0xFFFFFFFF);
    g_ctx->RSSetState(g_rasterState);
    g_ctx->OMSetDepthStencilState(g_dsState, 0);
    g_ctx->PSSetSamplers(0, 1, &g_sampler);

    // Upload ortho matrix
    float m[16]; BuildOrthoMatrix(m, (float)g_width, (float)g_height);
    D3D11_MAPPED_SUBRESOURCE ms{};
    if (HR(g_ctx->Map(g_cbOrtho, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        memcpy(ms.pData, m, sizeof(m));
        g_ctx->Unmap(g_cbOrtho, 0);
    }
}

void EndFrame()
{
    // Flush any remaining geometry
    Flush(g_whiteSRV);

    g_swapChain->Present(0, 0);

    // Busy-wait to cap frame rate
    if (g_targetFps > 0.f) {
        double targetTicks = (double)g_qpcFreq.QuadPart / g_targetFps;
        LARGE_INTEGER now;
        do { QueryPerformanceCounter(&now); }
        while ((double)(now.QuadPart - g_lastPresent.QuadPart) < targetTicks);
        g_lastPresent = now;
    }
}

// ---------------------------------------------------------------------------
// Drawing primitives – all coordinates in screen pixels, origin top-left
// ---------------------------------------------------------------------------

// Filled rectangle
void DrawRect(float x, float y, float w, float h, Color color, bool filled)
{
    if (filled) {
        // Premultiply alpha for the blend state
        Color pre{ color.r * color.a, color.g * color.a, color.b * color.a, color.a };
        PushQuad(x, y, x + w, y + h, 0.f, 0.f, 0.f, 0.f, pre);
    } else {
        // Four edge quads, 1 px thick each
        const float T = 1.f; // thickness
        DrawRect(x,         y,         w, T,  color, true); // top
        DrawRect(x,         y + h - T, w, T,  color, true); // bottom
        DrawRect(x,         y + T,     T, h - 2*T, color, true); // left
        DrawRect(x + w - T, y + T,     T, h - 2*T, color, true); // right
    }
}

// Overload with explicit outline thickness
void DrawRectEx(float x, float y, float w, float h, Color color, float thickness)
{
    if (thickness <= 0.f) return;
    Color pre{ color.r * color.a, color.g * color.a, color.b * color.a, color.a };
    float T = thickness;
    PushQuad(x,         y,         x + w,     y + T,     0,0,0,0, pre); // top
    PushQuad(x,         y + h - T, x + w,     y + h,     0,0,0,0, pre); // bottom
    PushQuad(x,         y + T,     x + T,     y + h - T, 0,0,0,0, pre); // left
    PushQuad(x + w - T, y + T,     x + w,     y + h - T, 0,0,0,0, pre); // right
}

// Line segment using a screen-aligned quad
void DrawLine(float x0, float y0, float x1, float y1, Color color, float thickness)
{
    if (thickness <= 0.f) thickness = 1.f;

    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx*dx + dy*dy);
    if (len < 1e-4f) return;

    // Perpendicular unit vector
    float nx = -dy / len * (thickness * 0.5f);
    float ny =  dx / len * (thickness * 0.5f);

    Color pre{ color.r * color.a, color.g * color.a, color.b * color.a, color.a };

    Vertex v0{ x0 + nx, y0 + ny, 0,0, pre.r, pre.g, pre.b, pre.a };
    Vertex v1{ x0 - nx, y0 - ny, 0,0, pre.r, pre.g, pre.b, pre.a };
    Vertex v2{ x1 + nx, y1 + ny, 0,0, pre.r, pre.g, pre.b, pre.a };
    Vertex v3{ x1 - nx, y1 - ny, 0,0, pre.r, pre.g, pre.b, pre.a };

    // tri 1
    g_verts.push_back(v0); g_verts.push_back(v2); g_verts.push_back(v1);
    // tri 2
    g_verts.push_back(v2); g_verts.push_back(v3); g_verts.push_back(v1);
}

// Text rendered via GDI into a dynamic texture.
// Font name and size are accepted; a new texture is created each call.
// For high-frequency text, cache the returned SRV yourself and release after use.
void DrawText(float x, float y, const wchar_t* text, Color color,
              const wchar_t* fontName, int fontSize)
{
    if (!text || !text[0]) return;

    // Measure text with a temporary GDI DC
    HDC hdcMem = CreateCompatibleDC(nullptr);
    HFONT hfont = CreateFontW(fontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, fontName);
    SelectObject(hdcMem, hfont);

    RECT rc{};
    DrawTextW(hdcMem, text, -1, &rc, DT_CALCRECT | DT_SINGLELINE);
    int tw = rc.right  - rc.left + 2;
    int th = rc.bottom - rc.top  + 2;
    if (tw <= 0 || th <= 0) { DeleteObject(hfont); DeleteDC(hdcMem); return; }

    // Create DIB
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = tw;
    bmi.bmiHeader.biHeight      = -th; // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP hbm = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    SelectObject(hdcMem, hbm);
    SetBkMode(hdcMem, TRANSPARENT);
    SetTextColor(hdcMem, RGB(255, 255, 255));

    RECT rcFull{ 0, 0, tw, th };
    DrawTextW(hdcMem, text, -1, &rcFull, DT_SINGLELINE | DT_NOCLIP);
    GdiFlush();

    // Convert BGRA→RGBA and apply colour tint + premultiplied alpha
    std::vector<uint32_t> rgba(tw * th);
    uint8_t* src = (uint8_t*)bits;
    for (int i = 0; i < tw * th; i++) {
        uint8_t b = src[i*4+0], g = src[i*4+1], r = src[i*4+2];
        // Use luminance of GDI output as alpha
        float lum = (r * 0.299f + g * 0.587f + b * 0.114f) / 255.f;
        float a   = lum * color.a;
        uint8_t R = (uint8_t)(color.r * a * 255.f + 0.5f);
        uint8_t G = (uint8_t)(color.g * a * 255.f + 0.5f);
        uint8_t B = (uint8_t)(color.b * a * 255.f + 0.5f);
        uint8_t A = (uint8_t)(a * 255.f + 0.5f);
        rgba[i] = (A << 24) | (B << 16) | (G << 8) | R;
    }

    DeleteObject(hbm); DeleteObject(hfont); DeleteDC(hdcMem);

    // Upload to D3D11 texture
    D3D11_TEXTURE2D_DESC td{};
    td.Width  = static_cast<UINT>(tw);
    td.Height = static_cast<UINT>(th);
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init{ rgba.data(), (UINT)(tw * 4), 0 };
    ID3D11Texture2D* tex = nullptr;
    if (!HR(g_device->CreateTexture2D(&td, &init, &tex))) return;

    ID3D11ShaderResourceView* srv = nullptr;
    g_device->CreateShaderResourceView(tex, nullptr, &srv);
    tex->Release();
    if (!srv) return;

    // Flush colour geometry first, then draw the text quad, then flush again
    Flush(g_whiteSRV);
    PushQuad(x, y, x + (float)tw, y + (float)th, 0.f, 0.f, 1.f, 1.f,
             Color{ 1.f, 1.f, 1.f, 1.f }); // tint already baked in
    Flush(srv);
    srv->Release();
}

// Orthographic projection helpers
int   ScreenWidth()  { return g_width; }
int   ScreenHeight() { return g_height; }

// Project a world-space point to screen via a precomputed view-projection matrix (column-major).
// Returns false if behind the near plane.
bool WorldToScreen(const float* viewProj4x4,
                   float wx, float wy, float wz,
                   float& sx, float& sy)
{
    const float* m = viewProj4x4;
    float cx = m[0]*wx + m[4]*wy + m[8]*wz  + m[12];
    float cy = m[1]*wx + m[5]*wy + m[9]*wz  + m[13];
    float cw = m[3]*wx + m[7]*wy + m[11]*wz + m[15];
    if (cw < 0.001f) return false;
    float ndcX = cx / cw;
    float ndcY = cy / cw;
    sx = ( ndcX + 1.f) * 0.5f * (float)g_width;
    sy = (-ndcY + 1.f) * 0.5f * (float)g_height;
    return true;
}

} // namespace Overlay

// ---------------------------------------------------------------------------
// Smoke-test entry point
// ---------------------------------------------------------------------------
#ifdef OVERLAY_MAIN
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int)
{
    if (!Overlay::Init(hInst, 60.f)) {
        MessageBoxW(nullptr, L"Overlay::Init failed", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    MSG msg{};
    bool running = true;
    while (running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running = false;
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }

        Overlay::BeginFrame();

        // --- sample draws ---
        Overlay::DrawRect(100, 100, 200, 100,
                          Color(0.2f, 0.8f, 0.2f, 0.8f), false);       // green outline box
        Overlay::DrawRect(400, 150, 120,  60,
                          Color(0.8f, 0.1f, 0.1f, 0.6f), true);        // filled red rect
        Overlay::DrawLine(50, 50, 400, 300,
                          Color(1.f, 1.f, 0.f, 0.9f), 2.f);             // yellow line
        Overlay::DrawText(100, 220, L"D3D11 Overlay",
                          Color(1.f, 1.f, 1.f, 1.f), L"Arial", 20);    // white text

        Overlay::EndFrame();
    }

    Overlay::Shutdown();
    return 0;
}
#endif // OVERLAY_MAIN
