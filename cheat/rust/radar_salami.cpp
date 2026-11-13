// radar.cpp — wires mem.cpp reads to proj.cpp's radar transform
//
// Reads an entity array of 64 entries at a fixed stride of 0x10 bytes from a
// target process, then projects each live entity onto a 2D radar using the
// world_to_radar transform from proj.cpp.
//
// C++17 | Windows | x64
//
// Build (MSVC):
//   cl /std:c++17 /O2 mem.cpp proj.cpp radar.cpp /link psapi.lib
// Build (MinGW):
//   g++ -std=c++17 -O2 mem.cpp proj.cpp radar.cpp -lpsapi -o radar.exe
//
// ─── layout assumptions ──────────────────────────────────────────────────────
// Entity array base  : module_base + ENTITY_ARRAY_OFFSET
// Entry stride       : 0x10  (each entry is a pointer-to-entity-struct)
// Entity struct
//   +0x00  float x   (world position X)
//   +0x04  float y   (world position Y)
//   +0x08  float z   (world position Z)
//   +0x0C  uint32_t flags  (bit 0 = alive; 0 → skip)
//
// Local player base  : module_base + LOCAL_PLAYER_OFFSET
// Local player struct
//   +0x00  float x
//   +0x04  float y
//   +0x08  float z
//   +0x0C  float yaw  (radians, clockwise from +Z)
// ─────────────────────────────────────────────────────────────────────────────

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>

#pragma comment(lib, "psapi.lib")

// ─── config — edit for your target ───────────────────────────────────────────
namespace cfg
{
    constexpr wchar_t   kProcessName[]       = L"target.exe";
    constexpr wchar_t   kModuleName[]        = L"target.exe";

    // Offset from module base to the entity pointer array.
    constexpr uintptr_t kEntityArrayOffset   = 0x00C80000;
    // Number of slots in the array.
    constexpr int       kEntityCount         = 64;
    // Bytes between consecutive pointer slots.
    constexpr uintptr_t kEntityStride        = 0x10;

    // Offset from module base to the local player struct.
    constexpr uintptr_t kLocalPlayerOffset   = 0x00C90000;

    // Radar display settings.
    constexpr float kRadarRadius             = 64.0f;  // px
    constexpr float kWorldScale              = 2.0f;   // world units per radar px
}

// ─── forward declarations for implementations in the other TUs ───────────────
//
// These match the signatures in mem.cpp and proj.cpp exactly so this TU links
// against those object files without needing to reorganise them into headers.

// mem.cpp
DWORD     get_process_id(const wchar_t* name);
HANDLE    open_process(DWORD pid);
uintptr_t get_module_base(HANDLE proc, const wchar_t* mod_name);

template <typename T>
T rpm(HANDLE proc, uintptr_t addr);

// proj.cpp
struct Vec2 { float x, y; };
struct Vec3 { float x, y, z; };

Vec2 world_to_radar(Vec3 local_pos, Vec3 target_pos, float local_yaw,
                    float radar_radius, float scale, bool clamp_to_edge);

// ─── entity structs ───────────────────────────────────────────────────────────

struct EntityData
{
    float    x, y, z;
    uint32_t flags;
};

struct LocalPlayer
{
    float x, y, z;
    float yaw;  // radians
};

// ─── memory helpers (specialised for the struct layouts above) ────────────────

static std::optional<EntityData> read_entity(HANDLE proc, uintptr_t entity_ptr)
{
    if (entity_ptr == 0)
        return std::nullopt;

    EntityData e{};
    // Read x, y, z, flags as a contiguous block of 16 bytes.
    SIZE_T bytes_read = 0;
    if (!ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(entity_ptr),
                           &e, sizeof(e), &bytes_read) ||
        bytes_read != sizeof(e))
        return std::nullopt;

    return e;
}

static std::optional<LocalPlayer> read_local_player(HANDLE    proc,
                                                     uintptr_t base)
{
    uintptr_t ptr = rpm<uintptr_t>(proc, base + cfg::kLocalPlayerOffset);
    if (ptr == 0)
        return std::nullopt;

    LocalPlayer lp{};
    SIZE_T bytes_read = 0;
    if (!ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(ptr),
                           &lp, sizeof(lp), &bytes_read) ||
        bytes_read != sizeof(lp))
        return std::nullopt;

    return lp;
}

// ─── radar snapshot ───────────────────────────────────────────────────────────

struct RadarBlip
{
    int   index;   // slot in the entity array
    Vec2  pos;     // radar-space position relative to centre
};

// Reads the full entity array and converts every live entity to a radar blip.
// Returns the blip list; empty if the local player could not be read.
static std::vector<RadarBlip> build_radar(HANDLE proc, uintptr_t mod_base)
{
    // ── local player ──
    const auto lp_opt = read_local_player(proc, mod_base);
    if (!lp_opt)
    {
        fprintf(stderr, "[radar] local player not found\n");
        return {};
    }
    const LocalPlayer& lp = *lp_opt;
    const Vec3 lp_pos = { lp.x, lp.y, lp.z };

    // ── entity array ──
    const uintptr_t array_base = mod_base + cfg::kEntityArrayOffset;

    std::vector<RadarBlip> blips;
    blips.reserve(cfg::kEntityCount);

    for (int i = 0; i < cfg::kEntityCount; ++i)
    {
        // Each slot holds a pointer to the entity struct (stride = 0x10).
        const uintptr_t slot_addr = array_base +
                                    static_cast<uintptr_t>(i) * cfg::kEntityStride;

        uintptr_t entity_ptr = 0;
        SIZE_T    bytes_read = 0;
        if (!ReadProcessMemory(proc,
                               reinterpret_cast<LPCVOID>(slot_addr),
                               &entity_ptr, sizeof(entity_ptr),
                               &bytes_read) ||
            bytes_read != sizeof(entity_ptr))
            continue;

        const auto ent_opt = read_entity(proc, entity_ptr);
        if (!ent_opt)
            continue;

        const EntityData& e = *ent_opt;

        // Skip dead / inactive entities (bit 0 of flags must be set).
        if (!(e.flags & 0x1u))
            continue;

        const Vec3 ent_pos = { e.x, e.y, e.z };

        const Vec2 blip = world_to_radar(
            lp_pos, ent_pos, lp.yaw,
            cfg::kRadarRadius, cfg::kWorldScale,
            /*clamp_to_edge=*/true);

        blips.push_back({ i, blip });
    }

    return blips;
}

// ─── ASCII radar renderer ─────────────────────────────────────────────────────
//
// Renders a minimal text-mode radar to stdout so the demo is self-contained
// without a GUI.  Replace with your actual overlay draw calls in production.

static void render_radar_ascii(const std::vector<RadarBlip>& blips)
{
    // Map the ±radar_radius range to a 33×33 character grid.
    constexpr int   kGrid   = 33;      // must be odd
    constexpr float kHalf   = static_cast<float>((kGrid - 1) / 2);
    constexpr float kScale  = kHalf / cfg::kRadarRadius;

    char grid[kGrid][kGrid];
    for (auto& row : grid) for (auto& c : row) c = '.';

    // Local player at centre.
    grid[kGrid / 2][kGrid / 2] = '@';

    for (const auto& b : blips)
    {
        // Radar space: +x right, +y up (screen-space y is flipped for text).
        const int col = static_cast<int>(std::round( b.pos.x * kScale + kHalf));
        const int row = static_cast<int>(std::round(-b.pos.y * kScale + kHalf));

        if (col >= 0 && col < kGrid && row >= 0 && row < kGrid)
            grid[row][col] = 'E';
    }

    printf("\n  RADAR  (%d entities)\n", static_cast<int>(blips.size()));
    for (const auto& row : grid)
    {
        printf("  ");
        for (char c : row) printf("%c", c);
        printf("\n");
    }
    printf("  @ = you   E = enemy\n\n");
}

// ─── entry point ─────────────────────────────────────────────────────────────

int main()
{
    try
    {
        const DWORD     pid  = get_process_id(cfg::kProcessName);
        const HANDLE    proc = open_process(pid);
        const uintptr_t mod  = get_module_base(proc, cfg::kModuleName);

        printf("[radar] attached to PID %lu  module base 0x%llX\n",
               static_cast<unsigned long>(pid),
               static_cast<unsigned long long>(mod));

        // Single snapshot — wrap in a loop with a Sleep() for a live overlay.
        const std::vector<RadarBlip> blips = build_radar(proc, mod);

        for (const auto& b : blips)
            printf("  slot %02d  radar (% .1f, % .1f)\n",
                   b.index, static_cast<double>(b.pos.x),
                   static_cast<double>(b.pos.y));

        render_radar_ascii(blips);

        CloseHandle(proc);
    }
    catch (const std::exception& e)
    {
        fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }

    return 0;
}
