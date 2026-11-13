// radar.cpp — composition layer for the overlay half.
//
// overlay.cpp (d3d11 transparent click-through window) and proj (world->screen) come from the
// primitive set; this file only decides which boxes to draw and where.
//
// build: x86_64-w64-mingw32-g++ -std=c++17 -O2 -I. radar.cpp -o radar.exe -ld3d11 -ldxgi -lpsapi

#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <memory>

#include "overlay.cpp"
#include "proj.cpp"

#define KMEM_DEVICE_TYPE 0x8000
#define IOCTL_KMEM_READ  CTL_CODE(KMEM_DEVICE_TYPE, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
typedef struct _KMEM_REQUEST { ULONG_PTR ProcessId, RemoteAddress; SIZE_T DataSize; } KMEM_REQUEST;
static HANDLE g_dev = INVALID_HANDLE_VALUE;

static bool drv_read(DWORD pid, uint64_t addr, void* out, size_t n) {
    if (g_dev == INVALID_HANDLE_VALUE) return false;
    std::vector<uint8_t> buf(sizeof(KMEM_REQUEST) + n, 0);
    auto* req = reinterpret_cast<KMEM_REQUEST*>(buf.data());
    req->ProcessId = pid; req->RemoteAddress = addr; req->DataSize = n;
    DWORD ret = 0;
    if (!DeviceIoControl(g_dev, IOCTL_KMEM_READ, buf.data(), sizeof(KMEM_REQUEST),
                         buf.data(), (DWORD)buf.size(), &ret, nullptr)) return false;
    if (ret < n) return false;
    memcpy(out, buf.data(), n);
    return true;
}
template <typename T> static bool rd(DWORD pid, uint64_t a, T& v) { return drv_read(pid, a, &v, sizeof(T)); }

namespace lay {
    constexpr uint64_t kEntityList = 0x0, kEntityCount = 0x0, kEntityPos = 0x30;
    constexpr uint64_t kEntityTeam = 0x50, kEntityHp = 0x58;
    constexpr float    kBoxHalfW = 22.0f, kBoxH = 72.0f, kHeadOffset = 68.0f;
    constexpr int      kMyTeam = 0;
}

struct Box { float x, y, w, h; };

static void collect(DWORD pid, const float vp[16], int vw, int vh, std::vector<Box>& out) {
    uint64_t list = 0; uint32_t n = 0;
    if (!rd(pid, lay::kEntityList, list) || !rd(pid, lay::kEntityCount, n) || n > 128) return;
    for (uint32_t i = 0; i < n; ++i) {
        uint64_t e = 0; int team = 0; float hp = 0.0f;
        if (!rd(pid, list + i * 8, e) || !e) continue;
        if (!rd(pid, e + lay::kEntityTeam, team) || team == lay::kMyTeam) continue;
        if (!rd(pid, e + lay::kEntityHp, hp) || hp <= 0.0f) continue;
        struct { float x, y, z; } p{};
        if (!rd(pid, e + lay::kEntityPos, p)) continue;
        float sx = 0, sy = 0, depth = 0;
        if (!Project(vp, (float)vw, (float)vh, p.x, p.y, p.z + lay::kHeadOffset, sx, sy, depth)) continue;
        out.push_back(Box{sx - lay::kBoxHalfW, sy - lay::kBoxH, lay::kBoxHalfW * 2.0f, lay::kBoxH});
    }
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: %s <pid>\n", argv[0]); return 1; }
    DWORD pid = (DWORD)strtoul(argv[1], nullptr, 10);
    g_dev = CreateFileW(L"\\\\.\\Kmem", GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (g_dev == INVALID_HANDLE_VALUE) { printf("driver not open: %lu\n", GetLastError()); return 2; }
    return 0;  // overlay frame loop is driven by overlay.cpp's present hook
}
