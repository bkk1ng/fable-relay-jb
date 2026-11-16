# User-Mode FPGA DMA Cheat (Reader Side)

This file assumes the kernel half (`FpgaDma.sys`, not included) exposes three things over a device interface: a mapping of the FPGA's BAR0, an allocation of physically-contiguous DMA memory mapped into user space, and nothing else — the DMA engine itself (descriptor ring, doorbell, completion polling) is driven entirely from user mode. Target physical memory is read over the PCIe link, a signature scan locates the game's entity block in the target's RAM, and entities are streamed to a click-through overlay at ~60 FPS.

```cpp
// ============================================================================
//  fpga_dma_overlay.cpp  --  USER-MODE HALF of the PCIe DMA cheat
//
//  Runs on the READER PC (the machine with the FPGA DMA card installed).
//  The TARGET PC runs the game; its DRAM is read over the PCIe cable by the
//  FPGA, with no software running on the target.
//
//  Kernel contract (FpgaDma.sys, kernel half -- assumed already loaded):
//    - Device interface GUID_DEVINTERFACE_FPGADMA
//    - IOCTL_FPGA_GET_INFO   -> BAR0 size, link status
//    - IOCTL_FPGA_MAP_BAR0   -> maps BAR0 (the DMA engine regs) into user VA
//    - IOCTL_FPGA_ALLOC_DMA  -> allocates physically-contiguous, user-mapped
//                               DMA memory; returns its physical base
//  Everything else (descriptor ring, doorbell, polling) is driven from here.
//
//  Hardware contract (custom FPGA DMA engine, BAR0 layout):
//    0x00 REG_VERSION  (ro)  magic/version
//    0x08 REG_CTRL     (rw)  bit0 = engine enable, bit1 = IRQ enable (we poll)
//    0x10 REG_DESC_LO   (rw) bits [31:0]  of 64-bit descriptor-ring bus addr
//    0x14 REG_DESC_HI   (rw) bits [63:32] of 64-bit descriptor-ring bus addr
//    0x18 REG_DOORBELL  (wo) cumulative tail index (posted descriptor count)
//    0x1C REG_HEAD      (ro) cumulative head index (completed descriptors)
//
//  Descriptor (32 bytes, little-endian, resident in the DMA ring):
//    +0x00 u64  src_pa    -- target-side physical address (read source)
//    +0x08 u64  dst_pa    -- reader-side physical address (bounce buffer)
//    +0x10 u32  len        -- transfer length in bytes (<= 0x1000)
//    +0x14 u32  flags      -- bit0: raise IRQ on completion
//
//  Build (x64):   cl /nologo /EHsc /O2 /W4 fpga_dma_overlay.cpp
// ============================================================================

#include <windows.h>
#include <setupapi.h>
#include <initguid.h>

#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "setupapi.lib")

// ---------------------------------------------------------------------------
// Tunables -- game-specific layout. The signature is a magic planted by the
// game's entity manager (or dumped from a debug build) so we can find the
// entity block in the TARGET's physical memory without page tables.
// ---------------------------------------------------------------------------
#define FPGA_HWID_PREFIX     L"PCI\\VEN_10EE&DEV_7021"   // Xilinx 7-series DMA card

#define PHYS_SCAN_START      0x0000001000ull             // 4 MiB
#define PHYS_SCAN_END        0x0008000000ull             // 2 GiB
#define SCAN_CHUNK           0x10000                     // 64 KiB per pass
#define SCAN_OVERLAP         15                         // sig len - 1

// Entity-block wire format (found via signature):
//   +0x000 u32   magic  'DMAENTBL' (8 bytes) -> +0x008 u32 version == 2
//   +0x000 u32   localPlayerIndex      (overlaps magic, read after validation is ok)
//   +0x008 float viewMatrix[16]        (column-major, 64 bytes)
//   +0x048 u32   entityCount
//   +0x100 Entity entities[]           stride 0x40
static const unsigned char  ENTITY_SIG[8] = { 'D','M','A','E','N','T','B','L' };
static const uint32_t       ENTITY_SIG_VERSION = 2;
#define EB_LOCAL_IDX_OFF    0x00
#define EB_VIEW_OFF         0x08
#define EB_COUNT_OFF        0x48
#define EB_ENTITIES_OFF     0x100
#define EB_MAX_ENTITIES    256
#define EB_TOTAL_SIZE      (EB_ENTITIES_OFF + EB_MAX_ENTITIES * 0x40)

#define MAX_ENTS_DRAWN      128
#define FRAME_TARGET_MS     16      // ~60 fps

// ---------------------------------------------------------------------------
// Device interface GUID -- must match the one registered by FpgaDma.sys
// ---------------------------------------------------------------------------
DEFINE_GUID(GUID_DEVINTERFACE_FPGADMA,
    0x5f1b7a3c, 0x9d4e, 0x4f6a, 0x8b, 0x21, 0xc0, 0xff, 0xee, 0x00, 0x01, 0x00);

// ---------------------------------------------------------------------------
// Kernel shim IOCTLs -- must match FpgaDma.sys
// ---------------------------------------------------------------------------
#define FPGA_CTL_CODE(fn, method) \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800 + (fn), method, FILE_ANY_ACCESS)

#define IOCTL_FPGA_GET_INFO    FPGA_CTL_CODE(0, METHOD_BUFFERED)
#define IOCTL_FPGA_MAP_BAR0   FPGA_CTL_CODE(1, METHOD_BUFFERED)
#define IOCTL_FPGA_ALLOC_DMA  FPGA_CTL_CODE(2, METHOD_BUFFERED)

struct FpgaInfo    { ULONG bar0Size; BOOLEAN linkUp; };
struct FpgaBarMap  { PVOID userVa; ULONG_PTR size; };
struct FpgaDmaAlloc{ ULONG requestSize; PVOID userVa; UINT64 physAddr; ULONG grantedSize; };

// ---------------------------------------------------------------------------
// DMA engine register block (BAR0)
// ---------------------------------------------------------------------------
namespace reg {
    constexpr uint32_t VERSION  = 0x00;
    constexpr uint32_t CTRL     = 0x08;
    constexpr uint32_t DESC_LO  = 0x10;
    constexpr uint32_t DESC_HI  = 0x14;
    constexpr uint32_t DOORBELL = 0x18;
    constexpr uint32_t HEAD     = 0x1C;
    constexpr uint32_t CTRL_ENABLE = 0x1;
}

#pragma pack(push, 1)
struct DmaDesc {
    uint64_t srcPa;
    uint64_t dstPa;
    uint32_t len;
    uint32_t flags;
};
static_assert(sizeof(DmaDesc) == 32, "descriptor size");

struct RemoteEntity {
    float    pos[3];      // +0x00 world x,y,z
    float    hp;           // +0x0C
    uint32_t team;         // +0x10
    uint8_t  alive;        // +0x14
    uint8_t  _pad[0x40 - 0x15];
};
#pragma pack(pop)
static_assert(sizeof(RemoteEntity) == 0x40, "entity stride");

// ============================================================================
static void Log(const char* fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

// ============================================================================
//  DmaDevice -- enumerates the FPGA, maps BAR0, drives the DMA engine
// ============================================================================
class DmaDevice {
public:
    ~DmaDevice() { Close(); }

    bool Init();
    bool ReadPhys(uint64_t targetPa, void* out, size_t len);

private:
    static constexpr size_t RING_COUNT = 128;
    static constexpr size_t MAX_XFER    = 0x1000;                 // per descriptor
    static constexpr size_t BOUNCE_SIZE = RING_COUNT * MAX_XFER;  // 512 KiB

    bool FindDevicePath(std::wstring& path);
    void Close();

    HANDLE          m_hDev  = INVALID_HANDLE_VALUE;
    volatile uint32_t* m_bar = nullptr;     // BAR0 as u32 registers
    uint8_t*        m_bounce = nullptr;      // user VA of DMA memory
    uint64_t        m_bouncePa = 0;
    DmaDesc*        m_ring   = nullptr;       // lives inside DMA memory, first page
    uint64_t        m_ringPa = 0;

    uint32_t        m_tail = 0;               // cumulative submitted (wraps)
    uint32_t        m_headCached = 0;

    inline void     RegWrite(uint32_t off, uint32_t v) const { m_bar[off / 4] = v; }
    inline uint32_t RegRead (uint32_t off) const { return m_bar[off / 4]; }
};

bool DmaDevice::FindDevicePath(std::wstring& path)
{
    HDEVINFO set = SetupDiGetClassDevs(&GUID_DEVINTERFACE_FPGADMA, nullptr, nullptr,
                                       DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) { Log("SetupDiGetClassDevs failed: %lu", GetLastError()); return false; }

    bool ok = false;
    for (DWORD i = 0; ; ++i) {
        SP_DEVICE_INTERFACE_DATA ifd{}; ifd.cbSize = sizeof(ifd);
        if (!SetupDiEnumDeviceInterfaces(set, nullptr, &GUID_DEVINTERFACE_FPGADMA, i, &ifd))
            break;

        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
        if (!need) continue;

        std::vector<uint8_t> buf(need);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        // Verify the hardware identity behind the interface is really our FPGA.
        SP_DEVINFO_DATA did{}; did.cbSize = sizeof(did);
        if (SetupDiGetDeviceInterfaceDetailW(set, &ifd, detail, need, nullptr, &did)) {
            wchar_t instId[MAX_DEVICE_ID_LEN]{};
            if (SetupDiGetDeviceInstanceIdW(set, &did, instId, MAX_DEVICE_ID_LEN, nullptr)
                && wcsncmp(instId, FPGA_HWID_PREFIX, wcslen(FPGA_HWID_PREFIX)) == 0) {
                path = detail->DevicePath;
                Log("Found FPGA: %ls", instId);
                ok = true;
                break;
            }
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    if (!ok) Log("No FPGA DMA device (VID 10EE / DEV 7021) with active interface found.");
    return ok;
}

bool DmaDevice::Init()
{
    std::wstring path;
    if (!FindDevicePath(path)) return false;

    m_hDev = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (m_hDev == INVALID_HANDLE_VALUE) { Log("CreateFile failed: %lu", GetLastError()); return false; }

    DWORD br = 0;

    // --- BAR0 mapping (registers) ---
    FpgaBarMap bm{};
    if (!DeviceIoControl(m_hDev, IOCTL_FPGA_MAP_BAR0, nullptr, 0, &bm, sizeof(bm), &br, nullptr)
        || br != sizeof(bm)) {
        Log("IOCTL_FPGA_MAP_BAR0 failed: %lu", GetLastError()); return false;
    }
    m_bar = reinterpret_cast<volatile uint32_t*>(bm.userVa);

    uint32_t ver = RegRead(reg::VERSION);
    Log("FPGA engine version register: 0x%08X, BAR0 %llu bytes", ver, (unsigned long long)bm.size);
    if (ver == 0 || ver == 0xFFFFFFFF) { Log("BAR0 looks dead (engine not responding)."); return false; }

    // --- contiguous DMA memory: ring page + bounce buffer ---
    FpgaDmaAlloc da{};
    da.requestSize = (ULONG)(4096 + BOUNCE_SIZE);   // 4 KiB ring page + bounce
    if (!DeviceIoControl(m_hDev, IOCTL_FPGA_ALLOC_DMA, &da, sizeof(da), &da, sizeof(da), &br, nullptr)
        || br != sizeof(da) || !da.userVa) {
        Log("IOCTL_FPGA_ALLOC_DMA failed: %lu", GetLastError()); return false;
    }
    m_bounce   = (uint8_t*)da.userVa + 4096;
    m_bouncePa = da.physAddr + 4096;
    m_ring     = reinterpret_cast<DmaDesc*>(da.userVa);
    m_ringPa   = da.physAddr;
    memset(da.userVa, 0, 4096);   // ring page only; leave bounce uninitialized

    // --- bring the engine up ---
    m_headCached = 0;
    m_tail = 0;
    RegWrite(reg::DESC_LO, (uint32_t)(m_ringPa & 0xFFFFFFFF));
    RegWrite(reg::DESC_HI, (uint32_t)(m_ringPa >> 32));
    MemoryBarrier();
    RegWrite(reg::CTRL, reg::CTRL_ENABLE);
    MemoryBarrier();
    Log("DMA engine armed. Ring PA 0x%llX, bounce PA 0x%llX",
        (unsigned long long)m_ringPa, (unsigned long long)m_bouncePa);
    return true;
}

void DmaDevice::Close()
{
    if (m_bar) { RegWrite(reg::CTRL, 0); m_bar = nullptr; }
    if (m_hDev != INVALID_HANDLE_VALUE) { CloseHandle(m_hDev); m_hDev = INVALID_HANDLE_VALUE; }
}

bool DmaDevice::ReadPhys(uint64_t targetPa, void* out, size_t len)
{
    if (!len) return true;
    uint8_t* dst = (uint8_t*)out;
    size_t done = 0;

    while (done < len) {
        // One submission = up to RING_COUNT descriptors (512 KiB on the wire).
        size_t want = len - done;
        if (want > BOUNCE_SIZE) want = BOUNCE_SIZE;

        uint32_t nDesc = 0;
        size_t   off   = 0;
        while (off < want) {
            size_t n = want - off; if (n > MAX_XFER) n = MAX_XFER;
            DmaDesc& d = m_ring[m_tail % RING_COUNT];
            d.srcPa  = targetPa + done + off;
            d.dstPa  = m_bouncePa + off;
            d.len    = (uint32_t)n;
            d.flags  = 0;                       // polling mode, no IRQ
            m_tail++;
            ++nDesc;
            off += n;
        }

        MemoryBarrier();
        RegWrite(reg::DOORBELL, m_tail);      // post batch

        // Poll the engine's cumulative head until it matches our tail.
        ULONGLONG deadline = GetTickCount64() + 1000;
        for (;;) {
            m_headCached = RegRead(reg::HEAD);
            if (m_headCached == m_tail) break;
            if (GetTickCount64() > deadline) {
                Log("DMA timeout: head=%u tail=%u", m_headCached, m_tail);
                return false;
            }
            YieldProcessor();
        }

        memcpy(dst + done, m_bounce, want);
        done += want;
    }
    return true;
}

// ============================================================================
//  Overlay -- layered, topmost, click-through, color-keyed
// ============================================================================
class Overlay {
public:
    bool Create(HINSTANCE hInst);
    void Pump();                          // non-blocking message pump
    void BeginFrame();
    void EndFrame();                      // present
    void Box(int x, int y, int w, int h, COLORREF edge, COLORREF fill);
    void HBar(int x, int y, int w, int h, float frac, COLORREF col);
    void Line(int x0, int y0, int x1, int y1, COLORREF col);
    void Text(int x, int y, const wchar_t* s, COLORREF col);
    int W() const { return m_w; }
    int H() const { return m_h; }
    bool Alive() const { return m_alive; }

private:
    static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l);
    HWND  m_hwnd = nullptr;
    HDC   m_mem  = nullptr;
    HBITMAP m_bmp = nullptr, m_bmpOld = nullptr;
    int   m_w = 0, m_h = 0;
    bool  m_alive = true;
    static const COLORREF KEY = RGB(255, 0, 255);
};

static const wchar_t* OV_CLASS = L"FpgaDmaOverlay";
static Overlay* g_ov = nullptr;

LRESULT CALLBACK Overlay::WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_DESTROY:    if (g_ov) g_ov->m_alive = false; return 0;
    default:            return DefWindowProcW(h, m, w, l);
    }
}

bool Overlay::Create(HINSTANCE hInst)
{
    WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = hInst;
    wc.lpszClassName = OV_CLASS; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    if (!RegisterClassW(&wc)) return false;

    m_w = GetSystemMetrics(SM_CXSCREEN);
    m_h = GetSystemMetrics(SM_CYSCREEN);

    m_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        OV_CLASS, L"ov", WS_POPUP,
        0, 0, m_w, m_h, nullptr, nullptr, hInst, nullptr);
    if (!m_hwnd) return false;

    SetLayeredWindowAttributes(m_hwnd, KEY, 0