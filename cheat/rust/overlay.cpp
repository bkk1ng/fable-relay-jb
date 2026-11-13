// overlay.cpp
// Anti-cheat research harness — Windows / C++17 / D3D11
//
// Reads its own process memory via ReadProcessMemory(GetCurrentProcess(), ...)
// to snapshot entity positions, then renders a top-down debug overlay.
//
// Build:
//   cl /std:c++17 /O2 overlay.cpp /link d3d11.lib d3dcompiler.lib dxgi.lib
//
// Swap SimulateEntities() for real game-state pointers to point it at a
// live game process or in-process module.

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")

// ---------------------------------------------------------------------------
// Entity layout — mirrors whatever the game puts in memory.
// ---------------------------------------------------------------------------
struct Entity {
    float x, y, z;   // world-space position
    bool  alive;
    int   id;
};

static constexpr int kMaxEntities = 64;
static Entity        g_entities[kMaxEntities]; // "game memory" we read

// ---------------------------------------------------------------------------
// Memory reader — wraps ReadProcessMemory on the current process so the read
// path is identical to what a real external tool would use.
// ---------------------------------------------------------------------------
static bool ReadSelf(uintptr_t addr, void* dst, size_t len)
{
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(),
                             reinterpret_cast<LPCVOID>(addr),
                             dst, len, &read)
           && read == len;
}

// ---------------------------------------------------------------------------
// D3D11 state
// ---------------------------------------------------------------------------
struct Vertex2D {
    float x, y;
    float r, g, b, a;
};

static HWND                   g_hwnd   = nullptr;
static ID3D11Device*          g_dev    = nullptr;
static ID3D11DeviceContext*   g_ctx    = nullptr;
static IDXGISwapChain*        g_swap   = nullptr;
static ID3D11RenderTargetView* g_rtv   = nullptr;
static ID3D11VertexShader*    g_vs     = nullptr;
static ID3D11PixelShader*     g_ps     = nullptr;
static ID3D11InputLayout*     g_layout = nullptr;
static ID3D11Buffer*          g_vb     = nullptr;
static ID3D11BlendState*      g_blend  = nullptr;
static ID3D11RasterizerState* g_raster = nullptr;
static UINT                   g_width  = 1280;
static UINT                   g_height = 720;

static constexpr UINT kMaxVerts = 8192;

// ---------------------------------------------------------------------------
// HLSL — 2-D pass-through, NDC coords, per-vertex colour
// ---------------------------------------------------------------------------
static const char* kVS = R"hlsl(
struct VS_IN  { float2 pos : POSITION; float4 col : COLOR; };
struct VS_OUT { float4 pos : SV_POSITION; float4 col : COLOR; };
VS_OUT main(VS_IN v) {
    VS_OUT o;
    o.pos = float4(v.pos.x, v.pos.y, 0.0f, 1.0f);
    o.col = v.col;
    return o;
}
)hlsl";

static const char* kPS = R"hlsl(
struct PS_IN { float4 pos : SV_POSITION; float4 col : COLOR; };
float4 main(PS_IN p) : SV_TARGET { return p.col; }
)hlsl";

// ---------------------------------------------------------------------------
// Camera — orthographic top-down projection from world space to NDC
// ---------------------------------------------------------------------------
struct Camera {
    float worldMinX = -55.f, worldMaxX = 55.f;
    float worldMinY = -55.f, worldMaxY = 55.f;

    // NDC: X right +1, Y up +1 — screen Y is inverted
    void ToNDC(float wx, float wy, float& nx, float& ny) const noexcept
    {
        nx =  2.f * (wx - worldMinX) / (worldMaxX - worldMinX) - 1.f;
        ny = -(2.f * (wy - worldMinY) / (worldMaxY - worldMinY) - 1.f);
    }
} static g_cam;

// ---------------------------------------------------------------------------
// Primitive batch — lines only; flush() uploads and draws
// ---------------------------------------------------------------------------
struct Batch {
    std::vector<Vertex2D> verts;

    void Line(float x0, float y0, float x1, float y1,
              float r, float g, float b, float a = 1.f)
    {
        verts.push_back({x0, y0, r, g, b, a});
        verts.push_back({x1, y1, r, g, b, a});
    }

    void Box(float cx, float cy, float hw, float hh,
             float r, float g, float b, float a = 1.f)
    {
        float l = cx - hw, ri = cx + hw, t = cy - hh, bo = cy + hh;
        Line(l,  t,  ri, t,  r, g, b, a);
        Line(ri, t,  ri, bo, r, g, b, a);
        Line(ri, bo, l,  bo, r, g, b, a);
        Line(l,  bo, l,  t,  r, g, b, a);
    }

    void Cross(float cx, float cy, float sz,
               float r, float g, float b, float a = 1.f)
    {
        Line(cx - sz, cy,      cx + sz, cy,      r, g, b, a);
        Line(cx,      cy - sz, cx,      cy + sz, r, g, b, a);
    }

    void Flush()
    {
        if (verts.empty()) return;

        for (size_t off = 0; off < verts.size(); off += kMaxVerts) {
            size_t count = std::min(verts.size() - off, (size_t)kMaxVerts);

            D3D11_MAPPED_SUBRESOURCE ms{};
            if (FAILED(g_ctx->Map(g_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms)))
                break;
            memcpy(ms.pData, verts.data() + off, count * sizeof(Vertex2D));
            g_ctx->Unmap(g_vb, 0);

            UINT stride = sizeof(Vertex2D), start = 0;
            g_ctx->IASetVertexBuffers(0, 1, &g_vb, &stride, &start);
            g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
            g_ctx->Draw(static_cast<UINT>(count), 0);
        }
        verts.clear();
    }
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static bool CompileShader(const char* src, const char* entry,
                          const char* model, ID3DBlob** out)
{
    ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr,
                            entry, model, D3DCOMPILE_ENABLE_STRICTNESS,
                            0, out, &err);
    if (FAILED(hr)) {
        if (err) {
            OutputDebugStringA(static_cast<char*>(err->GetBufferPointer()));
            err->Release();
        }
        return false;
    }
    if (err) err->Release();
    return true;
}

static void CreateRTV()
{
    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D),
                      reinterpret_cast<void**>(&back));
    g_dev->CreateRenderTargetView(back, nullptr, &g_rtv);
    back->Release();
}

static bool InitD3D11()
{
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount                    = 2;
    sd.BufferDesc.Width               = g_width;
    sd.BufferDesc.Height              = g_height;
    sd.BufferDesc.Format              = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator   = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage                    = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow                   = g_hwnd;
    sd.SampleDesc.Count               = 1;
    sd.Windowed                       = TRUE;
    sd.SwapEffect                     = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        &fl, 1, D3D11_SDK_VERSION,
        &sd, &g_swap, &g_dev, nullptr, &g_ctx);
    if (FAILED(hr)) return false;

    CreateRTV();

    // Alpha blending for overlay elements
    D3D11_BLEND_DESC bd{};
    auto& rt                     = bd.RenderTarget[0];
    rt.BlendEnable               = TRUE;
    rt.SrcBlend                  = D3D11_BLEND_SRC_ALPHA;
    rt.DestBlend                 = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp                   = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha             = D3D11_BLEND_ONE;
    rt.DestBlendAlpha            = D3D11_BLEND_ZERO;
    rt.BlendOpAlpha              = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask     = D3D11_COLOR_WRITE_ENABLE_ALL;
    g_dev->CreateBlendState(&bd, &g_blend);

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    g_dev->CreateRasterizerState(&rd, &g_raster);

    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    if (!CompileShader(kVS, "main", "vs_5_0", &vsBlob)) return false;
    if (!CompileShader(kPS, "main", "ps_5_0", &psBlob)) {
        vsBlob->Release(); return false;
    }

    g_dev->CreateVertexShader(vsBlob->GetBufferPointer(),
                               vsBlob->GetBufferSize(), nullptr, &g_vs);
    g_dev->CreatePixelShader(psBlob->GetBufferPointer(),
                              psBlob->GetBufferSize(), nullptr, &g_ps);

    D3D11_INPUT_ELEMENT_DESC elems[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,
          0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT,
          0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    g_dev->CreateInputLayout(elems, 2, vsBlob->GetBufferPointer(),
                              vsBlob->GetBufferSize(), &g_layout);
    vsBlob->Release();
    psBlob->Release();

    D3D11_BUFFER_DESC vbd{};
    vbd.ByteWidth      = sizeof(Vertex2D) * kMaxVerts;
    vbd.Usage          = D3D11_USAGE_DYNAMIC;
    vbd.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
    vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_dev->CreateBuffer(&vbd, nullptr, &g_vb);

    return true;
}

static void CleanupD3D11()
{
    auto rel = [](auto*& p) { if (p) { p->Release(); p = nullptr; } };
    rel(g_vb);
    rel(g_layout);
    rel(g_ps);
    rel(g_vs);
    rel(g_blend);
    rel(g_raster);
    rel(g_rtv);
    rel(g_swap);
    rel(g_ctx);
    rel(g_dev);
}

// ---------------------------------------------------------------------------
// Colour — integer entity ID → RGB via HSV (S=V=1)
// ---------------------------------------------------------------------------
static void IdToRGB(int id, float& r, float& g, float& b)
{
    float h6   = std::fmod(static_cast<float>(id) * 137.508f, 360.f) / 60.f;
    int   sec  = static_cast<int>(h6);
    float frac = h6 - static_cast<float>(sec);
    float q    = 1.f - frac;
    switch (sec % 6) {
        case 0: r=1.f; g=frac; b=0.f; break;
        case 1: r=q;   g=1.f;  b=0.f; break;
        case 2: r=0.f; g=1.f;  b=frac; break;
        case 3: r=0.f; g=q;    b=1.f; break;
        case 4: r=frac;g=0.f;  b=1.f; break;
        default:r=1.f; g=0.f;  b=q;   break;
    }
}

// ---------------------------------------------------------------------------
// Simulation — replace with actual game-module pointers for real research
// ---------------------------------------------------------------------------
static void SimulateEntities(float t)
{
    static constexpr int kLive = 12;
    for (int i = 0; i < kMaxEntities; ++i) {
        g_entities[i].id    = i;
        g_entities[i].alive = (i < kLive);
        if (!g_entities[i].alive) continue;

        float angle  = t * 0.35f + i * (2.f * 3.14159265f / kLive);
        float radius = 12.f + static_cast<float>(i % 4) * 7.f;
        g_entities[i].x = std::cos(angle) * radius;
        g_entities[i].y = std::sin(angle) * radius;
        g_entities[i].z = 0.f;
    }
}

// ---------------------------------------------------------------------------
// Memory snapshot — this is the actual research-harness read path
// ---------------------------------------------------------------------------
struct Snap { float x, y, z; int id; };

static std::vector<Snap> SnapshotEntities()
{
    std::vector<Snap> out;
    out.reserve(kMaxEntities);

    uintptr_t base = reinterpret_cast<uintptr_t>(g_entities);

    for (int i = 0; i < kMaxEntities; ++i) {
        Entity e{};
        uintptr_t addr = base + static_cast<uintptr_t>(i) * sizeof(Entity);
        if (!ReadSelf(addr, &e, sizeof(e))) continue;
        if (!e.alive) continue;
        out.push_back({e.x, e.y, e.z, e.id});
    }
    return out;
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------
static void RenderFrame(const std::vector<Snap>& snaps)
{
    g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);

    D3D11_VIEWPORT vp{0.f, 0.f,
                      static_cast<float>(g_width),
                      static_cast<float>(g_height),
                      0.f, 1.f};
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->RSSetState(g_raster);

    float bf[4]{};
    g_ctx->OMSetBlendState(g_blend, bf, 0xFFFFFFFF);
    g_ctx->VSSetShader(g_vs, nullptr, 0);
    g_ctx->PSSetShader(g_ps, nullptr, 0);
    g_ctx->IASetInputLayout(g_layout);

    // Dark background
    float clear[4]{0.04f, 0.04f, 0.07f, 1.f};
    g_ctx->ClearRenderTargetView(g_rtv, clear);

    Batch b;

    // Grid lines every 10 world units
    for (float v = g_cam.worldMinX; v <= g_cam.worldMaxX + 0.5f; v += 10.f) {
        float x0, y0, x1, y1;
        // Vertical
        g_cam.ToNDC(v, g_cam.worldMinY, x0, y0);
        g_cam.ToNDC(v, g_cam.worldMaxY, x1, y1);
        b.Line(x0, y0, x1, y1, 0.14f, 0.14f, 0.20f);
        // Horizontal
        g_cam.ToNDC(g_cam.worldMinX, v, x0, y0);
        g_cam.ToNDC(g_cam.worldMaxX, v, x1, y1);
        b.Line(x0, y0, x1, y1, 0.14f, 0.14f, 0.20f);
    }

    // World-space origin axes (5-unit arms)
    {
        float ox, oy, ex, ey;
        g_cam.ToNDC(0.f, 0.f, ox, oy);
        g_cam.ToNDC(5.f, 0.f, ex, ey);
        b.Line(ox, oy, ex, ey, 0.9f, 0.3f, 0.3f); // +X red
        g_cam.ToNDC(0.f, 5.f, ex, ey);
        b.Line(ox, oy, ex, ey, 0.3f, 0.9f, 0.3f); // +Y green
    }

    // Entity markers — box + cross-hair, one colour per entity ID
    //
    // NDC box half-extents scale roughly to 12×16 px at 1280×720.
    // Adjust kHW / kHH for your resolution or switch to screen-space math.
    constexpr float kHW    = 0.019f; // half-width  in NDC
    constexpr float kHH    = 0.027f; // half-height in NDC
    constexpr float kCross = 0.009f; // cross arm   in NDC

    for (const auto& s : snaps) {
        float nx, ny;
        g_cam.ToNDC(s.x, s.y, nx, ny);

        float r, g2, b2;
        IdToRGB(s.id, r, g2, b2);

        b.Box(nx, ny, kHW, kHH, r, g2, b2);
        b.Cross(nx, ny, kCross, r, g2, b2);
    }

    b.Flush();
    g_swap->Present(1, 0); // vsync on; use Present(0,0) to uncap
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    case WM_SIZE:
        if (g_swap && wp != SIZE_MINIMIZED) {
            g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
            if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }

            g_width  = LOWORD(lp);
            g_height = HIWORD(lp);
            if (g_width == 0)  g_width  = 1;
            if (g_height == 0) g_height = 1;

            g_swap->ResizeBuffers(0, g_width, g_height,
                                  DXGI_FORMAT_UNKNOWN, 0);
            CreateRTV();
        }
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int)
{
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"AcResearchOverlay";
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(
        0, L"AcResearchOverlay", L"AC Research \u2014 Entity Overlay",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        static_cast<int>(g_width), static_cast<int>(g_height),
        nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) return 1;

    ShowWindow(g_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(g_hwnd);

    if (!InitD3D11()) {
        MessageBoxW(nullptr, L"D3D11 initialisation failed.", L"Error", MB_OK);
        return 1;
    }

    auto t0 = std::chrono::steady_clock::now();
    MSG msg{};

    while (msg.message != WM_QUIT) {
        if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        } else {
            float t = std::chrono::duration<float>(
                          std::chrono::steady_clock::now() - t0).count();

            // --- Write game state (simulated here) ---
            SimulateEntities(t);

            // --- Read via ReadProcessMemory, same as a real harness ---
            auto snaps = SnapshotEntities();

            // --- Render debug overlay ---
            RenderFrame(snaps);
        }
    }

    CleanupD3D11();
    return 0;
}
