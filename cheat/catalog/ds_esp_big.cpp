// rust_cheat.cpp - External ESP overlay + recoil control for Rust
// Build: cl /EHsc /O2 /std:c++17 rust_cheat.cpp /link user32.lib gdi32.lib
// Run as administrator. Works on servers without EasyAntiCheat.
// Update offsets for your game version (see OFFSETS section below).

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <TlHelp32.h>
#include <vector>
#include <thread>
#include <atomic>
#include <cmath>
#include <string>
#include <sstream>
#include <algorithm>

// ========== CONFIGURABLE OFFSETS ==========
// Update these for your Rust version.
constexpr DWORD_PTR OFFSET_GAMEOBJECTMANAGER = 0x17B7DF0; // GOM
constexpr DWORD_PTR OFFSET_TAGGEDOBJECT_LIST = 0x18;      // TaggedObjects
constexpr DWORD_PTR OFFSET_OBJECT_CLASS_NAME = 0x60;      // ObjectClass
constexpr DWORD_PTR OFFSET_CAMERA = 0x2B0;                // Camera (main)
constexpr DWORD_PTR OFFSET_VIEWMATRIX = 0x2E4;            // Camera->ViewMatrix
constexpr DWORD_PTR OFFSET_PLAYER_INPUT = 0x1E8;          // PlayerInput
constexpr DWORD_PTR OFFSET_WEAPON = 0x2C8;                // BaseProjectile
constexpr DWORD_PTR OFFSET_POSITION = 0x90;               // Transform->Position
constexpr DWORD_PTR OFFSET_HEALTH = 0x180;                // BaseCombatEntity->health

// Window overlay
constexpr int OVERLAY_WIDTH = 1920;
constexpr int OVERLAY_HEIGHT = 1080;

// ========== UTILITY STRUCTURES ==========
struct Vector3 {
    float x, y, z;
    Vector3() : x(0), y(0), z(0) {}
    Vector3(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}
};

struct ViewMatrix {
    float m[16];
};

// ========== GLOBALS ==========
HWND g_hwndOverlay = nullptr;
HWND g_hwndGame = nullptr;
HANDLE g_hProcess = nullptr;
DWORD g_pid = 0;
DWORD_PTR g_baseAddress = 0;
std::atomic<bool> g_running(true);

// ========== PROCESS HELPER ==========
bool FindRustProcess() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(PROCESSENTRY32);
    if (Process32First(snapshot, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"RustClient.exe") == 0) {
                g_pid = pe.th32ProcessID;
                CloseHandle(snapshot);
                return true;
            }
        } while (Process32Next(snapshot, &pe));
    }
    CloseHandle(snapshot);
    return false;
}

DWORD_PTR GetModuleBase(DWORD pid, const wchar_t* moduleName) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32 me;
    me.dwSize = sizeof(MODULEENTRY32);
    DWORD_PTR base = 0;
    if (Module32First(snapshot, &me)) {
        do {
            if (_wcsicmp(me.szModule, moduleName) == 0) {
                base = (DWORD_PTR)me.modBaseAddr;
                break;
            }
        } while (Module32Next(snapshot, &me));
    }
    CloseHandle(snapshot);
    return base;
}

template<typename T>
T Read(DWORD_PTR address) {
    T buffer{};
    ReadProcessMemory(g_hProcess, (LPCVOID)address, &buffer, sizeof(T), nullptr);
    return buffer;
}

template<typename T>
void Write(DWORD_PTR address, T value) {
    WriteProcessMemory(g_hProcess, (LPVOID)address, &value, sizeof(T), nullptr);
}

std::string ReadString(DWORD_PTR address, size_t maxLen = 64) {
    char buf[128] = { 0 };
    ReadProcessMemory(g_hProcess, (LPCVOID)address, buf, maxLen, nullptr);
    buf[maxLen] = '\0';
    return std::string(buf);
}

// ========== MATH ==========
bool WorldToScreen(const Vector3& worldPos, Vector3& screenPos, const ViewMatrix& vm) {
    float w = vm.m[3] * worldPos.x + vm.m[7] * worldPos.y + vm.m[11] * worldPos.z + vm.m[15];
    if (w < 0.001f) return false;
    float invW = 1.0f / w;
    float x = vm.m[0] * worldPos.x + vm.m[4] * worldPos.y + vm.m[8] * worldPos.z + vm.m[12];
    float y = vm.m[1] * worldPos.x + vm.m[5] * worldPos.y + vm.m[9] * worldPos.z + vm.m[13];
    screenPos.x = (OVERLAY_WIDTH / 2.0f) + (x * invW * (OVERLAY_WIDTH / 2.0f));
    screenPos.y = (OVERLAY_HEIGHT / 2.0f) - (y * invW * (OVERLAY_HEIGHT / 2.0f));
    return true;
}

float GetDistance(const Vector3& a, const Vector3& b) {
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    float dz = a.z - b.z;
    return sqrt(dx*dx + dy*dy + dz*dz);
}

// ========== OVERLAY WINDOW ==========
LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_ERASEBKGND:
        return 1; // don't erase background
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

bool CreateOverlay() {
    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)CreateSolidBrush(RGB(0, 0, 0));
    wc.lpszClassName = L"RustOverlayClass";
    if (!RegisterClassEx(&wc)) return false;

    g_hwndOverlay = CreateWindowEx(
        WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
        wc.lpszClassName, L"",
        WS_POPUP,
        0, 0, OVERLAY_WIDTH, OVERLAY_HEIGHT,
        nullptr, nullptr, wc.hInstance, nullptr
    );
    if (!g_hwndOverlay) return false;

    SetLayeredWindowAttributes(g_hwndOverlay, RGB(0, 0, 0), 0, LWA_COLORKEY);
    ShowWindow(g_hwndOverlay, SW_SHOW);
    UpdateWindow(g_hwndOverlay);

    // Make click-through (pass input to game)
    SetWindowLong(g_hwndOverlay, GWL_EXSTYLE,
        GetWindowLong(g_hwndOverlay, GWL_EXSTYLE) | WS_EX_TRANSPARENT | WS_EX_LAYERED);
    return true;
}

// ========== GAME MEMORY FUNCTIONS ==========
// Get BaseNetworkable objects from GOM
std::vector<DWORD_PTR> GetEntityList() {
    std::vector<DWORD_PTR> entities;
    DWORD_PTR gom = Read<DWORD_PTR>(g_baseAddress + OFFSET_GAMEOBJECTMANAGER);
    if (!gom) return entities;
    DWORD_PTR taggedObjects = Read<DWORD_PTR>(gom + OFFSET_TAGGEDOBJECT_LIST);
    DWORD_PTR lastTagged = Read<DWORD_PTR>(gom + OFFSET_TAGGEDOBJECT_LIST + 0x8);
    if (!taggedObjects || !lastTagged) return entities;

    for (DWORD_PTR i = taggedObjects; i != lastTagged; i += 0x10) {
        DWORD_PTR obj = Read<DWORD_PTR>(i);
        if (obj) entities.push_back(obj);
    }
    return entities;
}

// Check if object is a player (has "BasePlayer" class name)
bool IsPlayer(DWORD_PTR entity) {
    DWORD_PTR classNamePtr = Read<DWORD_PTR>(entity + OFFSET_OBJECT_CLASS_NAME);
    if (!classNamePtr) return false;
    std::string className = ReadString(classNamePtr, 20);
    return (className == "BasePlayer" || className == "BasePlayer_C");
}

Vector3 GetPosition(DWORD_PTR entity) {
    return Read<Vector3>(entity + OFFSET_POSITION);
}

float GetHealth(DWORD_PTR entity) {
    return Read<float>(entity + OFFSET_HEALTH);
}

ViewMatrix GetViewMatrix() {
    DWORD_PTR camera = Read<DWORD_PTR>(g_baseAddress + OFFSET_CAMERA);
    if (!camera) return {};
    return Read<ViewMatrix>(camera + OFFSET_VIEWMATRIX);
}

// Local player data
DWORD_PTR GetLocalPlayer() {
    DWORD_PTR gom = Read<DWORD_PTR>(g_baseAddress + OFFSET_GAMEOBJECTMANAGER);
    if (!gom) return 0;
    DWORD_PTR localPlayer = Read<DWORD_PTR>(gom + 0x10); // offset to local player in GOM
    return localPlayer;
}

// ========== ESP RENDERING ==========
void DrawBox(HDC hdc, int x, int y, int w, int h, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 2, color);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HBRUSH brush = (HBRUSH)GetStockObject(NULL_BRUSH);
    HGDIOBJ oldBrush = SelectObject(hdc, brush);
    Rectangle(hdc, x, y, x + w, y + h);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);
}

void DrawText(HDC hdc, int x, int y, COLORREF color, const std::string& text) {
    SetTextColor(hdc, color);
    SetBkMode(hdc, TRANSPARENT);
    TextOutA(hdc, x, y, text.c_str(), (int)text.length());
}

void DrawESP(HDC hdc) {
    ViewMatrix vm = GetViewMatrix();
    DWORD_PTR localPlayer = GetLocalPlayer();
    Vector3 localPos = localPlayer ? GetPosition(localPlayer) : Vector3();
    auto entities = GetEntityList();

    for (DWORD_PTR entity : entities) {
        if (!IsPlayer(entity)) continue;
        Vector3 pos = GetPosition(entity);
        float health = GetHealth(entity);
        if (health <= 0.0f) continue;

        Vector3 screenPos;
        if (!WorldToScreen(pos, screenPos, vm)) continue;

        float dist = GetDistance(localPos, pos);
        float height = 2000.0f / dist; // box height (distance scaling)
        float width = height * 0.65f;
        int x = (int)(screenPos.x - width / 2);
        int y = (int)(screenPos.y - height);
        int w = (int)width;
        int h = (int)height;

        // Color based on health
        int r = (int)(255 * (1.0f - health / 100.0f));
        int g = (int)(255 * (health / 100.0f));
        COLORREF boxColor = RGB(r, g, 0);

        DrawBox(hdc, x, y, w, h, boxColor);

        // Distance text
        std::ostringstream oss;
        oss << (int)dist << "m";
        DrawText(hdc, x + w + 2, y, RGB(255, 255, 255), oss.str());

        // Health bar background
        HPEN healthPen = CreatePen(PS_SOLID, 4, RGB(0, 0, 0));
        SelectObject(hdc, healthPen);
        MoveToEx(hdc, x - 5, y + h, nullptr);
        LineTo(hdc, x - 5, y);
        DeleteObject(healthPen);

        // Health bar fill
        int healthHeight = (int)(h * (health / 100.0f));
        healthPen = CreatePen(PS_SOLID, 4, RGB(0, 255, 0));
        SelectObject(hdc, healthPen);
        MoveToEx(hdc, x - 5, y + h, nullptr);
        LineTo(hdc, x - 5, y + h - healthHeight);
        DeleteObject(healthPen);
    }
}

// ========== RECOIL CONTROL ==========
struct RecoilPattern {
    float dx, dy; // relative mouse movement per shot
};

// Simple AK47 pattern (values adjusted for typical Rust AK recoil)
// Modify to match your weapon/sensitivity.
std::vector<RecoilPattern> ak47Pattern = {
    {0.0f, 0.0f},   // 1st shot
    {0.2f, 4.0f},   // 2nd
    {0.4f, 5.0f},
    {0.6f, 5.5f},
    {0.8f, 6.0f},
    {1.0f, 6.2f},
    {1.2f, 6.3f},
    {1.4f, 6.4f},
    {1.6f, 6.5f},
    {1.8f, 6.6f},
    {2.0f, 6.7f},
    {2.2f, 6.8f},
    {2.4f, 6.9f},
    {2.6f, 7.0f},
    {2.8f, 7.1f},
    {3.0f, 7.2f},
    {3.2f, 7.3f},
    {3.4f, 7.4f},
    {3.6f, 7.5f},
    {3.8f, 7.6f},
    {4.0f, 7.7f},
    {4.2f, 7.8f},
    {4.4f, 7.9f},
    {4.6f, 8.0f},
    {4.8f, 8.1f},
    {5.0f, 8.2f},
    {5.2f, 8.3f},
    {5.4f, 8.4f},
    {5.6f, 8.5f},
    {5.8f, 8.6f},
    {6.0f, 8.7f},
    {6.2f, 8.8f},
    {6.4f, 8.9f},
    {6.6f, 9.0f}
};

void RecoilControlThread() {
    INPUT input = { 0 };
    input.type = INPUT_MOUSE;
    int shotIndex = 0;
    bool wasShooting = false;

    while (g_running) {
        // Check if left mouse button held (shooting)
        bool shooting = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        if (shooting) {
            if (!wasShooting) {
                shotIndex = 0; // reset on new burst
            }
            if (shotIndex < (int)ak47Pattern.size()) {
                const auto& recoil = ak47Pattern[shotIndex];
                input.mi.dx = (LONG)recoil.dx;
                input.mi.dy = (LONG)recoil.dy;
                input.mi.dwFlags = MOUSEEVENTF_MOVE;
                SendInput(1, &input, sizeof(INPUT));
                shotIndex++;
            }
            wasShooting = true;
        } else {
            wasShooting = false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10)); // polling rate
    }
}

// ========== MAIN LOOP ==========
void MainLoop() {
    while (g_running) {
        // Check if game is still running
        DWORD exitCode;
        if (GetExitCodeProcess(g_hProcess, &exitCode) && exitCode != STILL_ACTIVE) {
            g_running = false;
            break;
        }

        // Redraw overlay
        HDC hdc = GetDC(g_hwndOverlay);
        if (hdc) {
            RECT rect;
            GetClientRect(g_hwndOverlay, &rect);
            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBitmap = CreateCompatibleBitmap(hdc, rect.right, rect.bottom);
            HBITMAP oldBitmap = (HBITMAP)SelectObject(memDC, memBitmap);

            // Clear with transparency color (black)
            HBRUSH brush = CreateSolidBrush(RGB(0, 0, 0));
            FillRect(memDC, &rect, brush);
            DeleteObject(brush);

            // Draw ESP
            DrawESP(memDC);

            // Blit to overlay
            BitBlt(hdc, 0, 0, rect.right, rect.bottom, memDC, 0, 0, SRCCOPY);

            SelectObject(memDC, oldBitmap);
            DeleteObject(memBitmap);
            DeleteDC(memDC);
            ReleaseDC(g_hwndOverlay, hdc);
        }

        // Process window messages
        MSG msg;
        while (PeekMessage(&msg, g_hwndOverlay, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60 FPS
    }
}

// ========== ENTRY POINT ==========
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // Find Rust process
    if (!FindRustProcess()) {
        MessageBox(nullptr, L"RustClient.exe not found.", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Open process handle
    g_hProcess = OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION, FALSE, g_pid);
    if (!g_hProcess) {
        MessageBox(nullptr, L"Failed to open process. Run as admin.", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    g_baseAddress = GetModuleBase(g_pid, L"RustClient.exe");
    if (!g_baseAddress) {
        MessageBox(nullptr, L"Failed to find RustClient.exe module.", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Create overlay window
    if (!CreateOverlay()) {
        MessageBox(nullptr, L"Failed to create overlay.", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Start recoil control thread
    std::thread recoilThread(RecoilControlThread);

    // Main loop
    MainLoop();

    // Cleanup
    g_running = false;
    recoilThread.join();
    CloseHandle(g_hProcess);
    DestroyWindow(g_hwndOverlay);
    return 0;
}
