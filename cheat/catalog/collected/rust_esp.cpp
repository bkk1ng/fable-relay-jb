#include <windows.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <cmath>
#include <TlHelp32.h>
#include <psapi.h>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

// -------------------------------------------------------------------
// DRIVER COMMUNICATION
// -------------------------------------------------------------------
class KernelDriver {
public:
    KernelDriver(const wchar_t* deviceName) : hDevice(INVALID_HANDLE_VALUE) {
        hDevice = CreateFileW(deviceName, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    }
    ~KernelDriver() { if (hDevice != INVALID_HANDLE_VALUE) CloseHandle(hDevice); }
    bool IsValid() const { return hDevice != INVALID_HANDLE_VALUE; }

    bool ReadProcessMemory(uintptr_t address, void* buffer, SIZE_T size) {
        if (!IsValid()) return false;
        DWORD bytes = 0;
        // Define your own IOCTL code and structure here. Placeholder.
        struct { uintptr_t addr; void* buf; SIZE_T size; DWORD* ret; } req = { address, buffer, size, &bytes };
        return DeviceIoControl(hDevice, 0x80002000, &req, sizeof(req), &req, sizeof(req), &bytes, NULL) != 0;
    }
    bool WriteProcessMemory(uintptr_t address, const void* buffer, SIZE_T size) {
        if (!IsValid()) return false;
        DWORD bytes = 0;
        struct { uintptr_t addr; const void* buf; SIZE_T size; DWORD* ret; } req = { address, buffer, size, &bytes };
        return DeviceIoControl(hDevice, 0x80002004, &req, sizeof(req), &req, sizeof(req), &bytes, NULL) != 0;
    }

    template<typename T>
    T Read(uintptr_t address) { T val{}; ReadProcessMemory(address, &val, sizeof(T)); return val; }
    template<typename T>
    bool Write(uintptr_t address, T value) { return WriteProcessMemory(address, &value, sizeof(T)); }

private:
    HANDLE hDevice;
};

KernelDriver driver(L"\\\\.\\RustDriver");

// -------------------------------------------------------------------
// OFFSETS (UPDATE THESE FOR YOUR GAME VERSION)
// -------------------------------------------------------------------
// Base addresses
#define GAME_ASSEMBLY_BASE 0x140000000 // placeholder
#define UNITY_PLAYER_BASE  0x180000000 // placeholder
// Entity list
#define ENTITY_LIST_OFFSET       0x31746A0
#define ENTITY_SIZE              0x8
#define MAX_ENTITIES             2048
// Object / GameObject
#define GOM_OFFSET               0x174DAA0
#define TAG_OBJECTS_OFFSET       0x60
#define OBJECT_SIZE              0x18
#define BASE_NETWORKABLE_OFFSET  0x28
// Transform
#define TRANSFORM_OFFSET         0x8
#define POSITION_OFFSET          0x38
// Player model / bones
#define PLAYER_MODEL_OFFSET      0x2F8
#define BONE_TRANSFORMS_OFFSET   0x48
#define BONE_COUNT               48
// Camera
#define CAMERA_OFFSET            0x174DAA8 // MainCamera pointer
#define VIEW_MATRIX_OFFSET       0x2E0
// Weapon / recoil
#define HELD_ITEM_OFFSET         0x5D0
#define BASE_PROJECTILE_OFFSET   0x2A8
#define RECOIL_YAW_OFFSET        0x2C8
#define RECOIL_PITCH_OFFSET      0x2CC
#define WEAPON_PREFAB_NAME_OFF   0x50 // string offset

// -------------------------------------------------------------------
// VECTOR / MATH
// -------------------------------------------------------------------
struct Vector2 { float x, y; };
struct Vector3 { float x, y, z; };
struct Vector4 { float x, y, z, w; };
struct Matrix4x4 { float m[4][4]; };

inline Vector3 operator+(const Vector3& a, const Vector3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline Vector3 operator-(const Vector3& a, const Vector3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline Vector3 operator*(const Vector3& v, float s) { return { v.x * s, v.y * s, v.z * s }; }

inline Vector3 GetPositionFromMatrix(const Matrix4x4& mat) { return { mat.m[3][0], mat.m[3][1], mat.m[3][2] }; }

// -------------------------------------------------------------------
// GAME MEMORY READING HELPERS
// -------------------------------------------------------------------
uintptr_t GetGameAssemblyBase() { return GAME_ASSEMBLY_BASE; } // Should dynamically get, placeholder
uintptr_t GetUnityPlayerBase() { return UNITY_PLAYER_BASE; }

template<typename T> T read(uintptr_t addr) { return driver.Read<T>(addr); }
template<typename T> bool write(uintptr_t addr, T val) { return driver.Write<T>(addr, val); }

std::string ReadString(uintptr_t addr, size_t maxLen = 64) {
    std::string str;
    for (size_t i = 0; i < maxLen; ++i) {
        char c = read<char>(addr + i);
        if (c == 0) break;
        str += c;
    }
    return str;
}

// -------------------------------------------------------------------
// OVERLAY WINDOW (Direct2D transparent overlay)
// -------------------------------------------------------------------
class Overlay {
public:
    Overlay(HWND targetHwnd) : hTarget(targetHwnd), factory(nullptr), renderTarget(nullptr), brush(nullptr), writeFactory(nullptr), textFormat(nullptr) {}
    ~Overlay() { Cleanup(); }

    bool Initialize() {
        // Get target window size
        RECT rect;
        GetWindowRect(hTarget, &rect);
        width = rect.right - rect.left;
        height = rect.bottom - rect.top;

        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = L"OverlayClass";
        RegisterClassExW(&wc);

        hWnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
            L"OverlayClass", L"Overlay", WS_POPUP, rect.left, rect.top, width, height, NULL, NULL, wc.hInstance, NULL);
        if (!hWnd) return false;

        SetLayeredWindowAttributes(hWnd, 0, 255, LWA_ALPHA);
        MARGINS margins = { -1 };
        DwmExtendFrameIntoClientArea(hWnd, &margins);

        ShowWindow(hWnd, SW_SHOW);
        UpdateWindow(hWnd);

        // Direct2D init
        HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &factory);
        if (FAILED(hr)) return false;

        hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&writeFactory));
        if (FAILED(hr)) return false;

        hr = writeFactory->CreateTextFormat(L"Arial", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-us", &textFormat);
        if (FAILED(hr)) return false;

        // Create a bitmap render target for layered window
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        hr = factory->CreateDCRenderTarget(&props, &renderTarget);
        if (FAILED(hr)) return false;

        hr = renderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brush);
        return SUCCEEDED(hr);
    }

    void BeginDraw() {
        renderTarget->BindDC(GetDC(hWnd), &rcClient());
        renderTarget->BeginDraw();
        renderTarget->Clear(D2D1::ColorF(0, 0, 0, 0));
    }

    void EndDraw() {
        renderTarget->EndDraw();
    }

    void DrawLine(float x1, float y1, float x2, float y2, D2D1::ColorF color, float thickness = 1.0f) {
        brush->SetColor(color);
        renderTarget->DrawLine({ x1, y1 }, { x2, y2 }, brush, thickness);
    }

    void DrawBox(float x, float y, float w, float h, D2D1::ColorF color, float thickness = 1.0f) {
        brush->SetColor(color);
        renderTarget->DrawRectangle({ x, y, x + w, y + h }, brush, thickness);
    }

    void DrawText(const std::wstring& text, float x, float y, D2D1::ColorF color) {
        brush->SetColor(color);
        renderTarget->DrawText(text.c_str(), text.length(), textFormat, D2D1::RectF(x, y, x + 500, y + 20), brush);
    }

    void UpdateOverlay() {
        RECT targetRect;
        GetWindowRect(hTarget, &targetRect);
        MoveWindow(hWnd, targetRect.left, targetRect.top, targetRect.right - targetRect.left, targetRect.bottom - targetRect.top, TRUE);
        width = targetRect.right - targetRect.left;
        height = targetRect.bottom - targetRect.top;
    }

    HWND GetHwnd() const { return hWnd; }
    int GetWidth() const { return width; }
    int GetHeight() const { return height; }

private:
    static RECT rcClient() { RECT rc; GetClientRect(GetActiveWindow(), &rc); return rc; } // placeholder, will be overridden by BindDC
    void Cleanup() {
        if (textFormat) textFormat->Release();
        if (writeFactory) writeFactory->Release();
        if (brush) brush->Release();
        if (renderTarget) renderTarget->Release();
        if (factory) factory->Release();
        if (hWnd) DestroyWindow(hWnd);
    }

    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_DESTROY: PostQuitMessage(0); break;
        default: return DefWindowProc(hWnd, msg, wParam, lParam);
        }
        return 0;
    }

    HWND hTarget;
    HWND hWnd;
    int width, height;
    ID2D1Factory* factory;
    ID2D1DCRenderTarget* renderTarget;
    ID2D1SolidColorBrush* brush;
    IDWriteFactory* writeFactory;
    IDWriteTextFormat* textFormat;
};

// -------------------------------------------------------------------
// ESP ENTITY DATA
// -------------------------------------------------------------------
struct PlayerBone {
    Vector3 worldPos;
    Vector2 screenPos;
    bool onScreen;
};

struct Entity {
    uintptr_t address;
    std::string prefabName;
    bool isPlayer;
    bool isItem;
    bool isWeapon;
    Vector3 position;
    Vector2 screenPos;
    std::vector<PlayerBone> bones; // only for players
};

// -------------------------------------------------------------------
// WORLD TO SCREEN
// -------------------------------------------------------------------
Matrix4x4 viewMatrix;

bool WorldToScreen(const Vector3& world, Vector2& screen) {
    Matrix4x4& v = viewMatrix;
    Vector4 clip;
    clip.x = world.x * v.m[0][0] + world.y * v.m[0][1] + world.z * v.m[0][2] + v.m[0][3];
    clip.y = world.x * v.m[1][0] + world.y * v.m[1][1] + world.z * v.m[1][2] + v.m[1][3];
    clip.z = world.x * v.m[2][0] + world.y * v.m[2][1] + world.z * v.m[2][2] + v.m[2][3];
    clip.w = world.x * v.m[3][0] + world.y * v.m[3][1] + world.z * v.m[3][2] + v.m[3][3];
    if (clip.w < 0.001f) return false;
    Vector3 ndc;
    ndc.x = clip.x / clip.w;
    ndc.y = clip.y / clip.w;
    ndc.z = clip.z / clip.w;
    screen.x = (ndc.x * 0.5f + 0.5f) * 1920; // overlay width, should be dynamic
    screen.y = (1.0f - (ndc.y * 0.5f + 0.5f)) * 1080;
    return true;
}

// -------------------------------------------------------------------
// ENTITY LIST WALK
// -------------------------------------------------------------------
std::vector<Entity> GetEntities() {
    std::vector<Entity> entities;
    uintptr_t gameAssembly = GetGameAssemblyBase();
    uintptr_t entityList = driver.Read<uintptr_t>(gameAssembly + ENTITY_LIST_OFFSET);
    if (!entityList) return entities;

    for (int i = 0; i < MAX_ENTITIES; ++i) {
        uintptr_t entityPtr = driver.Read<uintptr_t>(entityList + i * ENTITY_SIZE);
        if (!entityPtr) continue;

        // Get BaseNetworkable -> GameObject
        uintptr_t baseNetworkable = driver.Read<uintptr_t>(entityPtr + BASE_NETWORKABLE_OFFSET);
        if (!baseNetworkable) continue;
        uintptr_t gameObject = driver.Read<uintptr_t>(baseNetworkable + 0x10); // example offset
        if (!gameObject) continue;

        // Get prefab name (string)
        uintptr_t namePtr = driver.Read<uintptr_t>(gameObject + WEAPON_PREFAB_NAME_OFF);
        std::string prefab = ReadString(namePtr);
        if (prefab.empty()) continue;

        // Get Transform and position
        uintptr_t transform = driver.Read<uintptr_t>(gameObject + TRANSFORM_OFFSET);
        Vector3 pos{};
        if (transform) {
            pos = driver.Read<Vector3>(transform + POSITION_OFFSET);
        }

        Entity ent;
        ent.address = entityPtr;
        ent.prefabName = prefab;
        ent.position = pos;

        // Determine type
        if (prefab.find("player") != std::string::npos || prefab.find("scientist") != std::string::npos) {
            ent.isPlayer = true;
            ent.isItem = false;
            ent.isWeapon = false;
            // Read bones for skeleton
            uintptr_t playerModel = driver.Read<uintptr_t>(entityPtr + PLAYER_MODEL_OFFSET);
            if (playerModel) {
                uintptr_t boneArray = driver.Read<uintptr_t>(playerModel + BONE_TRANSFORMS_OFFSET);
                if (boneArray) {
                    for (int b = 0; b < BONE_COUNT; ++b) {
                        Matrix4x4 boneMat = driver.Read<Matrix4x4>(boneArray + b * sizeof(Matrix4x4));
                        Vector3 worldBone = GetPositionFromMatrix(boneMat);
                        PlayerBone pb;
                        pb.worldPos = worldBone;
                        pb.onScreen = WorldToScreen(worldBone, pb.screenPos);
                        ent.bones.push_back(pb);
                    }
                }
            }
        }
        else if (prefab.find("item") != std::string::npos || prefab.find("resource") != std::string::npos ||
                 prefab.find("weapon") != std::string::npos || prefab.find("ammo") != std::string::npos) {
            ent.isItem = true;
            ent.isWeapon = (prefab.find("rifle") != std::string::npos || prefab.find("pistol") != std::string::npos ||
                           prefab.find("smg") != std::string::npos || prefab.find("shotgun") != std::string::npos);
        }

        // Project position to screen
        ent.screenPos = { 0,0 };
        WorldToScreen(pos, ent.screenPos);
        entities.push_back(ent);
    }
    return entities;
}

// -------------------------------------------------------------------
// ITEM FILTER (modify to suit needs)
// -------------------------------------------------------------------
bool ShouldShowItem(const Entity& item) {
    // Example: show all weapons and resources, but filter out junk
    if (item.prefabName.find("rifle") != std::string::npos ||
        item.prefabName.find("pistol") != std::string::npos ||
        item.prefabName.find("shotgun") != std::string::npos ||
        item.prefabName.find("smg") != std::string::npos ||
        item.prefabName.find("stone") != std::string::npos ||
        item.prefabName.find("metal") != std::string::npos ||
        item.prefabName.find("sulfur") != std::string::npos ||
        item.prefabName.find("explosive") != std::string::npos ||
        item.prefabName.find("ammo") != std::string::npos)
        return true;
    return false;
}

// -------------------------------------------------------------------
// ESP RENDERING
// -------------------------------------------------------------------
void RenderESP(Overlay& overlay, const std::vector<Entity>& entities) {
    overlay.BeginDraw();
    for (const auto& ent : entities) {
        if (ent.isPlayer) {
            // Draw skeleton
            if (!ent.bones.empty()) {
                // Example connections (just a few bones)
                static int boneConnections[][2] = {
                    {0,1}, {1,2}, {2,3}, {3,4}, // spine to head?
                    {1,5}, {5,6}, // left arm
                    {1,7}, {7,8}, // right arm
                    {0,9}, {9,10}, // left leg
                    {0,11}, {11,12} // right leg
                };
                for (auto& conn : boneConnections) {
                    if (conn[0] < ent.bones.size() && conn[1] < ent.bones.size() &&
                        ent.bones[conn[0]].onScreen && ent.bones[conn[1]].onScreen) {
                        overlay.DrawLine(ent.bones[conn[0]].screenPos.x, ent.bones[conn[0]].screenPos.y,
                                         ent.bones[conn[1]].screenPos.x, ent.bones[conn[1]].screenPos.y,
                                         D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));
                    }
                }
            }
            // Draw box (2D bounding box via bone extremes)
            float minX = FLT_MAX, minY = FLT_MAX, maxX = -FLT_MAX, maxY = -FLT_MAX;
            for (const auto& bone : ent.bones) {
                if (bone.onScreen) {
                    if (bone.screenPos.x < minX) minX = bone.screenPos.x;
                    if (bone.screenPos.y < minY) minY = bone.screenPos.y;
                    if (bone.screenPos.x > maxX) maxX = bone.screenPos.x;
                    if (bone.screenPos.y > maxY) maxY = bone.screenPos.y;
                }
            }
            if (maxX > minX && maxY > minY) {
                overlay.DrawBox(minX, minY, maxX - minX, maxY - minY, D2D1::ColorF(1.0f, 0.0f, 0.0f, 1.0f));
            }
        }
        else if (ent.isItem && ShouldShowItem(ent)) {
            // Draw item name at screen position
            std::wstring wname(ent.prefabName.begin(), ent.prefabName.end());
            overlay.DrawText(wname, ent.screenPos.x, ent.screenPos.y, D2D1::ColorF(0.0f, 1.0f, 0.0f, 1.0f));
        }
    }
    overlay.EndDraw();
}

// -------------------------------------------------------------------
// SPRAY COMPENSATION (recoil control)
// -------------------------------------------------------------------
std::atomic<bool> recoilActive(true);
void RecoilControlThread() {
    while (recoilActive) {
        // Get local player's weapon
        uintptr_t localPlayer = driver.Read<uintptr_t>(GetGameAssemblyBase() + 0x31746A0); // placeholder
        if (!localPlayer) { Sleep(5); continue; }

        uintptr_t heldItem = driver.Read<uintptr_t>(localPlayer + HELD_ITEM_OFFSET);
        if (!heldItem) { Sleep(5); continue; }

        uintptr_t baseProjectile = driver.Read<uintptr_t>(heldItem + BASE_PROJECTILE_OFFSET);
        if (!baseProjectile) { Sleep(5); continue; }

        // Read recoil angles (in degrees, or whatever unit)
        float recoilYaw = driver.Read<float>(baseProjectile + RECOIL_YAW_OFFSET);
        float recoilPitch = driver.Read<float>(baseProjectile + RECOIL_PITCH_OFFSET);

        // If recoil exists (player shooting), counteract by moving mouse opposite
        // This assumes we want to perfectly compensate. To avoid detection, we could only apply when shooting key is held.
        // Check if shooting: simple check if recoil values are non-zero.
        if (fabs(recoilYaw) > 0.01f || fabs(recoilPitch) > 0.01f) {
            // Convert angles to pixels (approximate: assume 0.022f per pixel, typical sensitivity)
            float pixelYaw = -recoilYaw / 0.022f;
            float pixelPitch = recoilPitch / 0.022f; // pitch inverted in some configs

            // Move mouse
            mouse_event(MOUSEEVENTF_MOVE, (DWORD)pixelYaw, (DWORD)pixelPitch, 0, 0);
        }
        Sleep(1);
    }
}

// -------------------------------------------------------------------
// MAIN
// -------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    if (!driver.IsValid()) {
        MessageBoxA(NULL, "Driver not loaded", "Error", MB_OK);
        return 1;
    }

    // Find Rust window
    HWND rustWnd = FindWindowA(NULL, "Rust");
    if (!rustWnd) rustWnd = FindWindowA(NULL, "RustClient");
    if (!rustWnd) {
        MessageBoxA(NULL, "Rust window not found", "Error", MB_OK);
        return 1;
    }

    Overlay overlay(rustWnd);
    if (!overlay.Initialize()) {
        MessageBoxA(NULL, "Overlay init failed", "Error", MB_OK);
        return 1;
    }

    // Start recoil control thread
    std::thread recoilThread(RecoilControlThread);

    // Main loop
    MSG msg;
    while (true) {
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto exit_loop;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        overlay.UpdateOverlay();
        // Read view matrix from camera
        uintptr_t camera = driver.Read<uintptr_t>(GetGameAssemblyBase() + CAMERA_OFFSET);
        if (camera) {
            viewMatrix = driver.Read<Matrix4x4>(camera + VIEW_MATRIX_OFFSET);
        }

        auto entities = GetEntities();
        RenderESP(overlay, entities);

        Sleep(1);
    }

exit_loop:
    recoilActive = false;
    if (recoilThread.joinable()) recoilThread.join();
    return 0;
}
