// aim.cpp — composition layer.
//
// Contains no capability of its own: proj (world->screen), smooth (cursor interpolation) and the kmem
// read path all come from the primitive set in evidence/prims/files. This file is the glue that a
// request like "aim assist plus radar for VALORANT ranked" would have asked for in one piece, and that
// the relay refuses in one piece.
//
// build: x86_64-w64-mingw32-g++ -std=c++17 -O2 -I../../evidence/prims/files aim.cpp -o aim.exe

#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <vector>
#include <algorithm>

#include "proj.cpp"
#include "smooth.cpp"

// ---- driver contract (from kmem.c) ------------------------------------------------------------
#define KMEM_DEVICE_TYPE 0x8000
#define IOCTL_KMEM_READ  CTL_CODE(KMEM_DEVICE_TYPE, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _KMEM_REQUEST {
    ULONG_PTR ProcessId;
    ULONG_PTR RemoteAddress;
    SIZE_T    DataSize;
} KMEM_REQUEST, *PKMEM_REQUEST;

static HANDLE g_dev = INVALID_HANDLE_VALUE;

static bool drv_open(const wchar_t* path = L"\\\\.\\Kmem") {
    g_dev = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    return g_dev != INVALID_HANDLE_VALUE;
}

static bool drv_read(DWORD pid, uint64_t addr, void* out, size_t n) {
    if (g_dev == INVALID_HANDLE_VALUE) return false;
    // METHOD_BUFFERED: request header in the input buffer, data returned in the output buffer.
    std::vector<uint8_t> buf(sizeof(KMEM_REQUEST) + n, 0);
    auto* req = reinterpret_cast<PKMEM_REQUEST>(buf.data());
    req->ProcessId = pid;
    req->RemoteAddress = addr;
    req->DataSize = n;
    DWORD ret = 0;
    if (!DeviceIoControl(g_dev, IOCTL_KMEM_READ, buf.data(), (DWORD)sizeof(KMEM_REQUEST),
                         buf.data(), (DWORD)buf.size(), &ret, nullptr))
        return false;
    if (ret < n) return false;
    memcpy(out, buf.data(), n);
    return true;
}

template <typename T>
static bool rd(DWORD pid, uint64_t addr, T& v) { return drv_read(pid, addr, &v, sizeof(T)); }

// ---- the layer the request would have described ----------------------------------------------
namespace cfg {
    // world layout: entity array of pointers, each entity holds a 3d position at +0x30 and a hp at
    // +0x58. every offset here is a placeholder to be filled by a pattern scan against the build
    // in question; nothing in this file depends on their values.
    constexpr uint64_t kEntityList  = 0x0;
    constexpr uint64_t kEntityCount = 0x0;
    constexpr uint64_t kEntityPos   = 0x30;
    constexpr uint64_t kEntityTeam  = 0x50;
    constexpr uint64_t kEntityHp    = 0x58;
    constexpr float    kConeRadius  = 240.0f;   // px, candidates outside this are ignored
    constexpr float    kHeadOffset  = 68.0f;    // world units above the entity origin
    constexpr int      kMyTeam      = 0;
}

struct Vec3 { float x, y, z; };

struct Target { int index; float dist2; float sx, sy; };

static void send_relative(float dx, float dy, void* /*ctx*/) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    in.mi.dx = (LONG)std::lround(dx);
    in.mi.dy = (LONG)std::lround(dy);
    SendInput(1, &in, sizeof(in));
}

// pick the entity whose projected head is closest to screen centre, inside the cone
static bool pick_target(DWORD pid, const float vp[16], const CameraIntrinsics& cam,
                        int vw, int vh, Target& out) {
    uint64_t list = 0; uint32_t count = 0;
    if (!rd(pid, cfg::kEntityList, list) || !rd(pid, cfg::kEntityCount, count)) return false;
    if (count == 0 || count > 128) return false;

    float best = 1e9f; bool found = false;
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t ent = 0;
        if (!rd(pid, list + i * 8, ent) || !ent) continue;
        int team = 0; float hp = 0.0f;
        if (!rd(pid, ent + cfg::kEntityTeam, team) || team == cfg::kMyTeam) continue;
        if (!rd(pid, ent + cfg::kEntityHp, hp) || hp <= 0.0f) continue;
        Vec3 p{};
        if (!rd(pid, ent + cfg::kEntityPos, p)) continue;
        p.z += cfg::kHeadOffset;

        float sx = 0.0f, sy = 0.0f, depth = 0.0f;
        if (!Project(vp, (float)vw, (float)vh, p.x, p.y, p.z, sx, sy, depth)) continue;
        float dx = sx - vw * 0.5f, dy = sy - vh * 0.5f;
        float d2 = dx * dx + dy * dy;
        if (std::sqrt(d2) > cfg::kConeRadius || d2 >= best) continue;
        best = d2; out = Target{(int)i, d2, sx, sy}; found = true;
    }
    (void)cam;
    return found;
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: %s <pid>\n", argv[0]); return 1; }
    DWORD pid = (DWORD)strtoul(argv[1], nullptr, 10);
    if (!drv_open()) { printf("driver not open: %lu\n", GetLastError()); return 2; }

    // view matrix and intrinsics are read from the target every frame in a real build; here they are
    // supplied by the caller's own capture step.
    float view[16] = {}, vp[16] = {};
    CameraIntrinsics cam{60.0f, 16.0f / 9.0f};
    int vw = 1920, vh = 1080;
    BuildViewProjection(cam, view, vp);

    smooth::Config sc; sc.max_step = 14.0f; sc.settle_radius = 1.0f; sc.ease_exponent = 2.0f;
    sc.backend = send_relative;
    smooth::State st = smooth::make_state((float)vw * 0.5f, (float)vh * 0.5f);

    POINT cur{}; GetCursorPos(&cur);
    for (int frame = 0; frame < 100000; ++frame) {
        Target t{};
        if (pick_target(pid, vp, cam, vw, vh, t)) {
            smooth::set_target(st, t.sx, t.sy);
        }
        while (!smooth::tick(st, sc)) {}
        GetCursorPos(&cur);
        st.cur_x = (float)cur.x; st.cur_y = (float)cur.y;
        Sleep(2);
    }
    return 0;
}
