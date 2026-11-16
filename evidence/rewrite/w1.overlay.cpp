// overlay.cpp - Transparent click-through D3D11 overlay
// Build (standalone smoke test): cl /std:c++17 /DOVERLAY_SMOKE overlay.cpp /link d3d11.lib dxgi.lib d2d1.lib dwrite.lib user32.lib gdi32.lib

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <cwchar>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// Overlay context
// ---------------------------------------------------------------------------
struct Overlay
{
    // Win32
    HWND    hwnd            = nullptr;
    int     width           = 0;
    int     height          = 0;

    // DXGI / D3D11 (needed to create a DXGI surface for D2D)
    ComPtr<ID3D11Device>            d3dDevice;
    ComPtr<ID3D11DeviceContext>     d3dCtx;
    ComPtr<IDXGISwapChain1>         swapChain;

    // D2D
    ComPtr<ID2D1Factory1>           d2dFactory;
    ComPtr<ID2D1Device>             d2dDevice;
    ComPtr<ID2D1DeviceContext>      d2dCtx;
    ComPtr<ID2D1Bitmap1>            d2dTarget;

    // DWrite
    ComPtr<IDWriteFactory>          dwFactory;
    ComPtr<IDWriteTextFormat>       dwFormat;   // default font atlas stand-in

    // Runtime state
    bool    inFrame = false;
};

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------
static LRESULT CALLBACK OverlayWndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

static bool CreateOverlayWindow(Overlay& ov, int w, int h, const wchar_t* title)
{
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = OverlayWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(NULL_BRUSH);
    wc.lpszClassName = L"D3D11Overlay";
    RegisterClassExW(&wc);

    // WS_EX_LAYERED + WS_EX_TRANSPARENT = click-through
    // WS_EX_TOPMOST keeps it above game windows
    DWORD exStyle = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE;
    DWORD style   = WS_POPUP;

    ov.hwnd = CreateWindowExW(
        exStyle, L"D3D11Overlay", title, style,
        0, 0, w, h,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

    if (!ov.hwnd) return false;

    // Make the window fully transparent by default; we'll composite via D2D alpha
    SetLayeredWindowAttributes(ov.hwnd, 0, 0xFF, LWA_ALPHA);

    ov.width  = w;
    ov.height = h;
    ShowWindow(ov.hwnd, SW_SHOW);
    return true;
}

static bool CreateD3D11Device(Overlay& ov)
{
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width       = static_cast<UINT>(ov.width);
    sd.Height      = static_cast<UINT>(ov.height);
    sd.Format      = DXGI_FORMAT_B8G8R8A8_UNORM; // Required for D2D interop
    sd.SampleDesc  = { 1, 0 };
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode   = DXGI_ALPHA_MODE_PREMULTIPLIED; // transparency
    sd.Flags       = 0;

    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1 };
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT; // needed for D2D

    if (FAILED(D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            flags, featureLevels, 2, D3D11_SDK_VERSION,
            &ov.d3dDevice, nullptr, &ov.d3dCtx)))
        return false;

    ComPtr<IDXGIDevice1> dxgiDevice;
    if (FAILED(ov.d3dDevice.As(&dxgiDevice))) return false;

    ComPtr<IDXGIAdapter> adapter;
    dxgiDevice->GetAdapter(&adapter);

    ComPtr<IDXGIFactory2> factory;
    adapter->GetParent(IID_PPV_ARGS(&factory));

    if (FAILED(factory->CreateSwapChainForHwnd(
            ov.d3dDevice.Get(), ov.hwnd, &sd,
            nullptr, nullptr, &ov.swapChain)))
        return false;

    return true;
}

static bool CreateD2DResources(Overlay& ov)
{
    // D2D factory
    D2D1_FACTORY_OPTIONS opts = {};
#if defined(_DEBUG)
    opts.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, opts, &ov.d2dFactory)))
        return false;

    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(ov.d3dDevice.As(&dxgiDevice))) return false;

    if (FAILED(ov.d2dFactory->CreateDevice(dxgiDevice.Get(), &ov.d2dDevice))) return false;
    if (FAILED(ov.d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &ov.d2dCtx)))
        return false;

    // Bind back buffer as D2D render target
    ComPtr<IDXGISurface> surface;
    if (FAILED(ov.swapChain->GetBuffer(0, IID_PPV_ARGS(&surface)))) return false;

    D2D1_BITMAP_PROPERTIES1 bp = {};
    bp.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
    bp.dpiX = bp.dpiY = 96.0f;
    bp.bitmapOptions  = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;

    if (FAILED(ov.d2dCtx->CreateBitmapFromDxgiSurface(surface.Get(), &bp, &ov.d2dTarget)))
        return false;

    ov.d2dCtx->SetTarget(ov.d2dTarget.Get());
    return true;
}

static bool CreateDWriteResources(Overlay& ov)
{
    if (FAILED(DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(ov.dwFactory.GetAddressOf()))))
        return false;

    if (FAILED(ov.dwFactory->CreateTextFormat(
            L"Consolas", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            16.0f, L"en-us",
            &ov.dwFormat)))
        return false;

    return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/// Create the overlay window and all D3D11/D2D/DWrite resources.
/// @param w, h   Window dimensions (match your target resolution).
/// @param title  Window class / title string (cosmetic, hidden).
bool Overlay_Init(Overlay& ov, int w, int h, const wchar_t* title = L"Overlay")
{
    if (!CreateOverlayWindow(ov, w, h, title)) return false;
    if (!CreateD3D11Device(ov))               return false;
    if (!CreateD2DResources(ov))              return false;
    if (!CreateDWriteResources(ov))           return false;
    return true;
}

/// Pump window messages; returns false when WM_QUIT is posted.
bool Overlay_PumpMessages()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        if (msg.message == WM_QUIT) return false;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return true;
}

/// Begin a render frame. Call before any draw functions.
void Overlay_BeginFrame(Overlay& ov)
{
    ov.d2dCtx->BeginDraw();
    ov.d2dCtx->Clear(D2D1::ColorF(0.f, 0.f, 0.f, 0.f)); // fully transparent clear
    ov.inFrame = true;
}

/// End a render frame and present. Call after all draw functions.
void Overlay_EndFrame(Overlay& ov)
{
    if (!ov.inFrame) return;
    ov.inFrame = false;
    ov.d2dCtx->EndDraw();

    DXGI_PRESENT_PARAMETERS pp = {};
    ov.swapChain->Present1(1, 0, &pp);
}

/// Draw a filled or outlined rectangle in screen space.
/// @param x, y       Top-left corner in pixels.
/// @param w, h       Width and height in pixels.
/// @param r,g,b,a    Color components [0, 1].
/// @param filled     true = filled rect; false = outline.
/// @param thickness  Outline thickness (pixels). Ignored when filled.
void Overlay_DrawRect(Overlay& ov,
                      float x, float y, float w, float h,
                      float r, float g, float b, float a,
                      bool filled, float thickness = 1.5f)
{
    ComPtr<ID2D1SolidColorBrush> brush;
    ov.d2dCtx->CreateSolidColorBrush(D2D1::ColorF(r, g, b, a), &brush);
    if (!brush) return;

    D2D1_RECT_F rc = D2D1::RectF(x, y, x + w, y + h);
    if (filled)
        ov.d2dCtx->FillRectangle(rc, brush.Get());
    else
        ov.d2dCtx->DrawRectangle(rc, brush.Get(), thickness);
}

/// Draw a line between two screen-space points.
void Overlay_DrawLine(Overlay& ov,
                      float x0, float y0, float x1, float y1,
                      float r, float g, float b, float a,
                      float thickness = 1.5f)
{
    ComPtr<ID2D1SolidColorBrush> brush;
    ov.d2dCtx->CreateSolidColorBrush(D2D1::ColorF(r, g, b, a), &brush);
    if (!brush) return;

    ov.d2dCtx->DrawLine(
        D2D1::Point2F(x0, y0),
        D2D1::Point2F(x1, y1),
        brush.Get(), thickness);
}

/// Draw a null-terminated UTF-16 string at a screen-space position.
/// Uses the DWrite font atlas (Consolas 16px by default).
/// @param x, y     Top-left anchor of the text layout box.
/// @param maxW     Maximum layout width before wrapping.
/// @param maxH     Maximum layout height.
void Overlay_DrawText(Overlay& ov,
                      float x, float y,
                      float r, float g, float b, float a,
                      const wchar_t* text,
                      float maxW = 800.f, float maxH = 600.f)
{
    if (!text || !*text) return;

    ComPtr<ID2D1SolidColorBrush> brush;
    ov.d2dCtx->CreateSolidColorBrush(D2D1::ColorF(r, g, b, a), &brush);
    if (!brush) return;

    ov.d2dCtx->DrawTextW(
        text,
        static_cast<UINT32>(wcslen(text)),
        ov.dwFormat.Get(),
        D2D1::RectF(x, y, x + maxW, y + maxH),
        brush.Get(),
        D2D1_DRAW_TEXT_OPTIONS_NONE,
        DWRITE_MEASURING_MODE_NATURAL);
}

/// Draw a narrow-string wrapper (converts to wchar_t on the stack).
void Overlay_DrawTextA(Overlay& ov,
                       float x, float y,
                       float r, float g, float b, float a,
                       const char* text,
                       float maxW = 800.f, float maxH = 600.f)
{
    if (!text || !*text) return;
    wchar_t buf[1024];
    MultiByteToWideChar(CP_UTF8, 0, text, -1, buf, 1024);
    Overlay_DrawText(ov, x, y, r, g, b, a, buf, maxW, maxH);
}

/// Release all resources and destroy the window.
void Overlay_Shutdown(Overlay& ov)
{
    // Release COM objects in reverse creation order
    ov.dwFormat.Reset();
    ov.dwFactory.Reset();
    ov.d2dTarget.Reset();
    ov.d2dCtx.Reset();
    ov.d2dDevice.Reset();
    ov.d2dFactory.Reset();
    ov.swapChain.Reset();
    ov.d3dCtx.Reset();
    ov.d3dDevice.Reset();

    if (ov.hwnd)
    {
        DestroyWindow(ov.hwnd);
        ov.hwnd = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Smoke-test entry point (compile with /DOVERLAY_SMOKE)
// ---------------------------------------------------------------------------
#ifdef OVERLAY_SMOKE
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    Overlay ov;
    if (!Overlay_Init(ov, 1920, 1080, L"Overlay Smoke"))
        return 1;

    float angle = 0.f;
    while (Overlay_PumpMessages())
    {
        angle += 0.01f;
        float cx = 960.f + cosf(angle) * 200.f;
        float cy = 540.f + sinf(angle) * 200.f;

        Overlay_BeginFrame(ov);

        // Filled rect (semi-transparent)
        Overlay_DrawRect(ov, 100, 100, 300, 200, 0.1f, 0.1f, 0.1f, 0.5f, true);

        // Outlined rect
        Overlay_DrawRect(ov, 100, 100, 300, 200, 0.0f, 1.0f, 0.4f, 1.0f, false, 2.0f);

        // Spinning line
        Overlay_DrawLine(ov, 960.f, 540.f, cx, cy, 1.0f, 0.4f, 0.0f, 1.0f, 2.0f);

        // Text
        Overlay_DrawTextA(ov, 110.f, 110.f, 1.0f, 1.0f, 1.0f, 1.0f, "D3D11 overlay active");

        Overlay_EndFrame(ov);

        Sleep(1);
    }

    Overlay_Shutdown(ov);
    return 0;
}
#endif // OVERLAY_SMOKE
