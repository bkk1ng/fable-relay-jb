// overlay.cpp
// Transparent click-through D3D11 overlay window
// Features: BeginFrame/EndFrame, DrawRect (filled/outlined), DrawLine, DrawText (font atlas)
//
// Build (standalone smoke test):
//   cl /std:c++17 /DOVERLAY_STANDALONE /W3 /O2 overlay.cpp /link d3d11.lib dxgi.lib d2d1.lib dwrite.lib user32.lib gdi32.lib
//
// Exported API (define OVERLAY_STANDALONE to build a demo entry point):
//   Overlay* Overlay_Create(const wchar_t* windowTitle, int width, int height);
//   void     Overlay_Destroy(Overlay* ov);
//   bool     Overlay_BeginFrame(Overlay* ov);   // returns false when window closed
//   void     Overlay_EndFrame(Overlay* ov);
//   void     Overlay_DrawRect(Overlay* ov, float x, float y, float w, float h,
//                             float r, float g, float b, float a, bool filled, float thickness);
//   void     Overlay_DrawLine(Overlay* ov, float x0, float y0, float x1, float y1,
//                             float r, float g, float b, float a, float thickness);
//   void     Overlay_DrawText(Overlay* ov, const wchar_t* text,
//                             float x, float y, float r, float g, float b, float a,
//                             float fontSize);

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d2d1.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <cstdint>
#include <cmath>
#include <cassert>
#include <vector>
#include <unordered_map>
#include <string>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// Internal structure
// ---------------------------------------------------------------------------

struct FontKey {
    float size;
    bool operator==(const FontKey& o) const { return size == o.size; }
};
struct FontKeyHash {
    size_t operator()(const FontKey& k) const {
        return std::hash<float>{}(k.size);
    }
};

struct Overlay {
    // Win32
    HWND    hwnd            = nullptr;
    int     width           = 0;
    int     height          = 0;
    bool    alive           = true;

    // DXGI / D3D11 (needed to create a shared surface for D2D)
    ComPtr<IDXGISwapChain>          swapChain;
    ComPtr<ID3D11Device>            d3dDevice;
    ComPtr<ID3D11DeviceContext>     d3dCtx;
    ComPtr<ID3D11RenderTargetView>  rtv;

    // D2D / DWrite
    ComPtr<ID2D1Factory1>           d2dFactory;
    ComPtr<ID2D1RenderTarget>       d2dRT;
    ComPtr<IDWriteFactory>          dwFactory;

    // Cached brushes / text formats
    ComPtr<ID2D1SolidColorBrush>    brush;
    std::unordered_map<FontKey, ComPtr<IDWriteTextFormat>, FontKeyHash> textFormats;
};

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------

static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_DESTROY) {
        Overlay* ov = reinterpret_cast<Overlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (ov) ov->alive = false;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// D2D render target from DXGI surface
// ---------------------------------------------------------------------------

static HRESULT CreateD2DRenderTarget(Overlay* ov)
{
    ComPtr<IDXGISurface> dxgiSurface;
    ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT hr = ov->swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr)) return hr;
    hr = backBuffer.As(&dxgiSurface);
    if (FAILED(hr)) return hr;

    D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        0.0f, 0.0f
    );

    ComPtr<ID2D1RenderTarget> rt;
    hr = ov->d2dFactory->CreateDxgiSurfaceRenderTarget(dxgiSurface.Get(), &rtProps, &rt);
    if (FAILED(hr)) return hr;

    ov->d2dRT = rt;

    // Create the shared solid brush
    hr = ov->d2dRT->CreateSolidColorBrush(D2D1::ColorF(1,1,1,1), &ov->brush);
    return hr;
}

// ---------------------------------------------------------------------------
// Public API implementation
// ---------------------------------------------------------------------------

Overlay* Overlay_Create(const wchar_t* windowTitle, int width, int height)
{
    Overlay* ov = new Overlay();
    ov->width  = width;
    ov->height = height;

    // Register window class
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = OverlayWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(NULL_BRUSH);
    wc.lpszClassName = L"D3D11OverlayClass";
    RegisterClassExW(&wc); // may fail if already registered; that is fine

    // Extended styles for transparent, layered, click-through, topmost window
    DWORD exStyle = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE;
    DWORD style   = WS_POPUP;

    ov->hwnd = CreateWindowExW(
        exStyle,
        L"D3D11OverlayClass",
        windowTitle,
        style,
        0, 0, width, height,
        nullptr, nullptr,
        GetModuleHandleW(nullptr),
        nullptr
    );
    if (!ov->hwnd) { delete ov; return nullptr; }

    SetWindowLongPtrW(ov->hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ov));

    // Make window fully transparent (we paint via D2D alpha)
    SetLayeredWindowAttributes(ov->hwnd, 0, 255, LWA_ALPHA);

    // D3D11 device + swap chain
    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferDesc.Width  = static_cast<UINT>(width);
    scd.BufferDesc.Height = static_cast<UINT>(height);
    scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count  = 1;
    scd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount       = 2;
    scd.OutputWindow      = ov->hwnd;
    scd.Windowed          = TRUE;
    scd.SwapEffect        = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.Flags             = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    UINT createFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT; // required for D2D interop
#ifdef _DEBUG
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        createFlags,
        &featureLevel, 1,
        D3D11_SDK_VERSION,
        &scd,
        &ov->swapChain,
        &ov->d3dDevice,
        nullptr,
        &ov->d3dCtx
    );
    if (FAILED(hr)) { DestroyWindow(ov->hwnd); delete ov; return nullptr; }

    // Back-buffer RTV (used to clear each frame)
    ComPtr<ID3D11Texture2D> backBuffer;
    ov->swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    ov->d3dDevice->CreateRenderTargetView(backBuffer.Get(), nullptr, &ov->rtv);

    // D2D factory
    D2D1_FACTORY_OPTIONS opts = {};
#ifdef _DEBUG
    opts.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, opts, ov->d2dFactory.GetAddressOf());
    if (FAILED(hr)) { DestroyWindow(ov->hwnd); delete ov; return nullptr; }

    // DWrite factory
    hr = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(ov->dwFactory.GetAddressOf())
    );
    if (FAILED(hr)) { DestroyWindow(ov->hwnd); delete ov; return nullptr; }

    // D2D render target over DXGI surface
    hr = CreateD2DRenderTarget(ov);
    if (FAILED(hr)) { DestroyWindow(ov->hwnd); delete ov; return nullptr; }

    ShowWindow(ov->hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(ov->hwnd);

    return ov;
}

void Overlay_Destroy(Overlay* ov)
{
    if (!ov) return;
    ov->textFormats.clear();
    ov->brush.Reset();
    ov->d2dRT.Reset();
    ov->d2dFactory.Reset();
    ov->dwFactory.Reset();
    ov->rtv.Reset();
    ov->swapChain.Reset();
    ov->d3dCtx.Reset();
    ov->d3dDevice.Reset();
    if (ov->hwnd) DestroyWindow(ov->hwnd);
    delete ov;
}

bool Overlay_BeginFrame(Overlay* ov)
{
    // Pump messages
    MSG msg = {};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (!ov->alive) return false;

    // Clear back buffer to fully transparent black
    const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    ov->d3dCtx->ClearRenderTargetView(ov->rtv.Get(), clearColor);

    // Begin D2D drawing
    ov->d2dRT->BeginDraw();
    ov->d2dRT->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    return true;
}

void Overlay_EndFrame(Overlay* ov)
{
    HRESULT hr = ov->d2dRT->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        // Recreate render target on device loss
        ov->brush.Reset();
        ov->d2dRT.Reset();
        ov->rtv.Reset();
        ov->textFormats.clear();

        // Resize buffers to force recreation
        ov->swapChain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);

        ComPtr<ID3D11Texture2D> backBuffer;
        ov->swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
        ov->d3dDevice->CreateRenderTargetView(backBuffer.Get(), nullptr, &ov->rtv);
        CreateD2DRenderTarget(ov);
    }

    ov->swapChain->Present(1, 0);
}

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------

static inline void SetBrushColor(Overlay* ov, float r, float g, float b, float a)
{
    ov->brush->SetColor(D2D1::ColorF(r, g, b, a));
}

void Overlay_DrawRect(Overlay* ov,
                      float x, float y, float w, float h,
                      float r, float g, float b, float a,
                      bool filled, float thickness)
{
    SetBrushColor(ov, r, g, b, a);
    D2D1_RECT_F rect = D2D1::RectF(x, y, x + w, y + h);
    if (filled) {
        ov->d2dRT->FillRectangle(rect, ov->brush.Get());
    } else {
        ov->d2dRT->DrawRectangle(rect, ov->brush.Get(), thickness);
    }
}

void Overlay_DrawLine(Overlay* ov,
                      float x0, float y0, float x1, float y1,
                      float r, float g, float b, float a,
                      float thickness)
{
    SetBrushColor(ov, r, g, b, a);
    ov->d2dRT->DrawLine(
        D2D1::Point2F(x0, y0),
        D2D1::Point2F(x1, y1),
        ov->brush.Get(),
        thickness
    );
}

void Overlay_DrawText(Overlay* ov,
                      const wchar_t* text,
                      float x, float y,
                      float r, float g, float b, float a,
                      float fontSize)
{
    if (!text || !text[0]) return;

    // Retrieve or create a text format for this font size
    FontKey key{ fontSize };
    auto it = ov->textFormats.find(key);
    if (it == ov->textFormats.end()) {
        ComPtr<IDWriteTextFormat> fmt;
        HRESULT hr = ov->dwFactory->CreateTextFormat(
            L"Consolas",          // font family — monospace works well for HUDs
            nullptr,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            fontSize,
            L"en-us",
            &fmt
        );
        if (FAILED(hr)) return;
        fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        ov->textFormats[key] = fmt;
        it = ov->textFormats.find(key);
    }

    SetBrushColor(ov, r, g, b, a);

    // Lay out into a large rect; text is not wrapped
    D2D1_RECT_F layoutRect = D2D1::RectF(
        x, y,
        static_cast<float>(ov->width),
        static_cast<float>(ov->height)
    );

    ov->d2dRT->DrawTextW(
        text,
        static_cast<UINT32>(wcslen(text)),
        it->second.Get(),
        layoutRect,
        ov->brush.Get(),
        D2D1_DRAW_TEXT_OPTIONS_NONE
    );
}

// ---------------------------------------------------------------------------
// Standalone smoke-test entry point
// ---------------------------------------------------------------------------

#ifdef OVERLAY_STANDALONE

#include <cmath>
#include <cstdio>

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    // Create a 1920x1080 fullscreen-sized overlay for the smoke test.
    // Adjust to your primary monitor resolution as needed.
    const int W = 1920, H = 1080;

    Overlay* ov = Overlay_Create(L"D3D11 Overlay", W, H);
    if (!ov) {
        MessageBoxW(nullptr, L"Overlay_Create failed", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    float angle = 0.0f;
    while (Overlay_BeginFrame(ov)) {
        float cx = W * 0.5f, cy = H * 0.5f;
        float radius = 200.0f;

        // Rotating line
        float lx = cx + radius * cosf(angle);
        float ly = cy + radius * sinf(angle);
        Overlay_DrawLine(ov, cx, cy, lx, ly, 1.0f, 0.4f, 0.1f, 1.0f, 2.0f);
        angle += 0.01f;

        // Filled rect (semi-transparent)
        Overlay_DrawRect(ov, 50.0f, 50.0f, 300.0f, 80.0f,
                         0.08f, 0.08f, 0.08f, 0.70f, true, 0.0f);

        // Outline rect
        Overlay_DrawRect(ov, 50.0f, 50.0f, 300.0f, 80.0f,
                         0.9f, 0.9f, 0.9f, 1.0f, false, 1.5f);

        // Text
        Overlay_DrawText(ov, L"D3D11 Overlay — running",
                         60.0f, 65.0f,
                         1.0f, 1.0f, 1.0f, 1.0f,
                         22.0f);

        Overlay_EndFrame(ov);
    }

    Overlay_Destroy(ov);
    return 0;
}

#endif // OVERLAY_STANDALONE
