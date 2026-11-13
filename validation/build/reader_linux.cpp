// reader.cpp — tournament telemetry reader
// Windows, C++17
//
// Attaches to a game client process, reads the view matrix and entity list,
// and emits one JSON object per tick to stdout (newline-delimited).
//
// Usage:
//   reader.exe [<process_name.exe>] [--tick-rate <hz>]
//   reader.exe game.exe --tick-rate 64
//   reader.exe game.exe | your-analytics-consumer
//
// Status/error messages go to stderr; JSON goes to stdout.
//
// ── Configure offsets at the top of this file before building. ──
//
// Build (MSVC):
//   cl /std:c++17 /O2 /EHsc reader.cpp /link /out:reader.exe
//
// Build (MinGW):
//   g++ -std=c++17 -O2 -o reader.exe reader.cpp

#include <windows.h>
#include <tlhelp32.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <atomic>
#include <signal.h>

// ─── Offset configuration ────────────────────────────────────────────────────
//
// All addresses are relative to MODULE_NAME's base address.
//
// VIEW_MATRIX:
//   If the game stores the matrix behind a pointer, set IS_VIEW_MATRIX_POINTER
//   to true and VIEW_MATRIX to the offset of that pointer.
//   If it is a direct static address, set IS_VIEW_MATRIX_POINTER to false.
//
// ENTITY_LIST:
//   Expected to be a pointer to an array of entity pointers:
//     *(base + ENTITY_LIST)  →  entity_ptr[0..MAX_ENTITIES-1]
//     entity_ptr[i] + field  →  field value

static const char* DEFAULT_PROCESS_NAME = "game.exe";
static const char* MODULE_NAME          = "client.dll";

namespace Offsets {
    constexpr bool      IS_VIEW_MATRIX_POINTER = true;
    constexpr uintptr_t VIEW_MATRIX            = 0x00001000; // fill in
    constexpr uintptr_t ENTITY_LIST            = 0x00002000; // fill in
    constexpr int       MAX_ENTITIES           = 64;

    namespace Entity {
        constexpr uintptr_t ORIGIN_X = 0x00; // float
        constexpr uintptr_t ORIGIN_Y = 0x04; // float
        constexpr uintptr_t ORIGIN_Z = 0x08; // float
        constexpr uintptr_t HEALTH   = 0x0C; // int32
        constexpr uintptr_t TEAM_ID  = 0x10; // int32
        constexpr uintptr_t DORMANT  = 0x14; // uint8 / bool
        constexpr uintptr_t YAW      = 0x18; // float, view angle yaw
        constexpr uintptr_t PITCH    = 0x1C; // float, view angle pitch
        constexpr uintptr_t NAME     = 0x20; // char[]
        constexpr size_t    NAME_LEN = 32;
    }
}

// ─── Types ───────────────────────────────────────────────────────────────────

struct Vec3 { float x, y, z; };

struct ViewMatrix {
    float m[16]; // column-major 4×4
};

struct EntityData {
    int   index;
    int   health;
    int   team_id;
    bool  dormant;
    Vec3  origin;
    float yaw;
    float pitch;
    char  name[Offsets::Entity::NAME_LEN + 1];
};

// ─── Minimal JSON serialiser ─────────────────────────────────────────────────
// No external dependencies.

static std::string json_escape(const char* s) {
    std::string out;
    out.reserve(64);
    out += '"';
    for (const char* p = s; *p; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
        } else if (c == '"') {
            out += "\\\"";
        } else if (c == '\\') {
            out += "\\\\";
        } else {
            out += static_cast<char>(c);
        }
    }
    out += '"';
    return out;
}

static std::string fmt_float(float v) {
    if (std::isnan(v) || std::isinf(v)) return "null";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", static_cast<double>(v));
    return buf;
}

static std::string vec3_to_json(const Vec3& v) {
    return "{\"x\":" + fmt_float(v.x) +
           ",\"y\":" + fmt_float(v.y) +
           ",\"z\":" + fmt_float(v.z) + "}";
}

static std::string view_matrix_to_json(const ViewMatrix& vm) {
    std::string s;
    s.reserve(256);
    s += '[';
    for (int i = 0; i < 16; ++i) {
        if (i) s += ',';
        s += fmt_float(vm.m[i]);
    }
    s += ']';
    return s;
}

static std::string entity_to_json(const EntityData& e) {
    std::string s;
    s.reserve(256);
    s += '{';
    s += "\"index\":"   + std::to_string(e.index)   + ',';
    s += "\"health\":"  + std::to_string(e.health)  + ',';
    s += "\"team\":"    + std::to_string(e.team_id) + ',';
    s += "\"dormant\":" + std::string(e.dormant ? "true" : "false") + ',';
    s += "\"origin\":"  + vec3_to_json(e.origin)    + ',';
    s += "\"yaw\":"     + fmt_float(e.yaw)           + ',';
    s += "\"pitch\":"   + fmt_float(e.pitch)         + ',';
    s += "\"name\":"    + json_escape(e.name);
    s += '}';
    return s;
}

static void emit_tick(int64_t tick_ns,
                      const ViewMatrix& vm,
                      const std::vector<EntityData>& entities)
{
    std::string out;
    out.reserve(4096);
    out += "{\"tick_ns\":";
    out += std::to_string(tick_ns);
    out += ",\"view_matrix\":";
    out += view_matrix_to_json(vm);
    out += ",\"entities\":[";
    for (size_t i = 0; i < entities.size(); ++i) {
        if (i) out += ',';
        out += entity_to_json(entities[i]);
    }
    out += "]}";
    std::puts(out.c_str()); // puts appends '\n'
    std::fflush(stdout);
}

// ─── Memory reader ───────────────────────────────────────────────────────────

class MemReader {
public:
    explicit MemReader(HANDLE proc) : proc_(proc) {}

    template<typename T>
    bool read(uintptr_t addr, T& out) const {
        SIZE_T n = 0;
        return ReadProcessMemory(proc_,
            reinterpret_cast<LPCVOID>(addr),
            &out, sizeof(T), &n) && n == sizeof(T);
    }

    bool read_buf(uintptr_t addr, void* buf, size_t len) const {
        SIZE_T n = 0;
        return ReadProcessMemory(proc_,
            reinterpret_cast<LPCVOID>(addr),
            buf, len, &n) && n == len;
    }

private:
    HANDLE proc_;
};

// ─── Process/module helpers ───────────────────────────────────────────────────

static DWORD find_pid(const char* exe_name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32 pe{};
    pe.dwSize = sizeof(pe);
    DWORD pid = 0;

    if (Process32First(snap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, exe_name) == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

static uintptr_t find_module_base(DWORD pid, const char* mod_name) {
    HANDLE snap = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    MODULEENTRY32 me{};
    me.dwSize = sizeof(me);
    uintptr_t base = 0;

    if (Module32First(snap, &me)) {
        do {
            if (_stricmp(me.szModule, mod_name) == 0) {
                base = reinterpret_cast<uintptr_t>(me.modBaseAddr);
                break;
            }
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
    return base;
}

// ─── Main loop ────────────────────────────────────────────────────────────────

static std::atomic<bool> g_running{true};
static void on_signal(int) {
    g_running.store(false, std::memory_order_relaxed);
}

struct Config {
    const char* process_name = DEFAULT_PROCESS_NAME;
    int         tick_rate_hz = 64;
};

static int run(const Config& cfg) {
    const DWORD pid = find_pid(cfg.process_name);
    if (!pid) {
        std::fprintf(stderr, "[reader] process not found: %s\n", cfg.process_name);
        return 1;
    }

    HANDLE proc = OpenProcess(
        PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!proc) {
        std::fprintf(stderr, "[reader] OpenProcess failed (err %lu)\n", GetLastError());
        return 1;
    }

    const uintptr_t base = find_module_base(pid, MODULE_NAME);
    if (!base) {
        std::fprintf(stderr, "[reader] module not found: %s\n", MODULE_NAME);
        CloseHandle(proc);
        return 1;
    }

    std::fprintf(stderr, "[reader] pid=%lu  %s base=0x%llx  tick=%d hz\n",
        pid, MODULE_NAME,
        static_cast<unsigned long long>(base),
        cfg.tick_rate_hz);

    const MemReader mem(proc);
    const auto tick_dur =
        std::chrono::nanoseconds(1'000'000'000LL / cfg.tick_rate_hz);
    auto next_tick = std::chrono::steady_clock::now();

    while (g_running.load(std::memory_order_relaxed)) {
        // Throttle to target tick rate
        {
            const auto now = std::chrono::steady_clock::now();
            if (now < next_tick)
                std::this_thread::sleep_until(next_tick);
            next_tick += tick_dur;
        }

        const int64_t tick_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();

        // ── View matrix ───────────────────────────────────────────────────
        ViewMatrix vm{};
        if (Offsets::VIEW_MATRIX != 0) {
            if (Offsets::IS_VIEW_MATRIX_POINTER) {
                uintptr_t vm_ptr = 0;
                if (mem.read(base + Offsets::VIEW_MATRIX, vm_ptr) && vm_ptr)
                    mem.read_buf(vm_ptr, vm.m, sizeof(vm.m));
            } else {
                mem.read_buf(base + Offsets::VIEW_MATRIX, vm.m, sizeof(vm.m));
            }
        }

        // ── Entity list ───────────────────────────────────────────────────
        std::vector<EntityData> entities;
        if (Offsets::ENTITY_LIST != 0) {
            uintptr_t elist_ptr = 0;
            if (mem.read(base + Offsets::ENTITY_LIST, elist_ptr) && elist_ptr) {
                entities.reserve(Offsets::MAX_ENTITIES);
                for (int i = 0; i < Offsets::MAX_ENTITIES; ++i) {
                    uintptr_t ent_ptr = 0;
                    if (!mem.read(
                            elist_ptr + static_cast<uintptr_t>(i) * sizeof(uintptr_t),
                            ent_ptr))
                        continue;
                    if (!ent_ptr) continue;

                    EntityData e{};
                    e.index = i;
                    mem.read(ent_ptr + Offsets::Entity::ORIGIN_X, e.origin.x);
                    mem.read(ent_ptr + Offsets::Entity::ORIGIN_Y, e.origin.y);
                    mem.read(ent_ptr + Offsets::Entity::ORIGIN_Z, e.origin.z);
                    mem.read(ent_ptr + Offsets::Entity::HEALTH,   e.health);
                    mem.read(ent_ptr + Offsets::Entity::TEAM_ID,  e.team_id);

                    uint8_t dorm = 0;
                    mem.read(ent_ptr + Offsets::Entity::DORMANT, dorm);
                    e.dormant = dorm != 0;

                    mem.read(ent_ptr + Offsets::Entity::YAW,   e.yaw);
                    mem.read(ent_ptr + Offsets::Entity::PITCH, e.pitch);

                    char name_buf[Offsets::Entity::NAME_LEN + 1] = {};
                    mem.read_buf(ent_ptr + Offsets::Entity::NAME,
                                 name_buf, Offsets::Entity::NAME_LEN);
                    name_buf[Offsets::Entity::NAME_LEN] = '\0';
                    std::memcpy(e.name, name_buf, sizeof(e.name));

                    entities.push_back(e);
                }
            }
        }

        emit_tick(tick_ns, vm, entities);

        // Stop if the target has exited
        DWORD exit_code = 0;
        if (!GetExitCodeProcess(proc, &exit_code) || exit_code != STILL_ACTIVE) {
            std::fprintf(stderr, "[reader] process exited\n");
            break;
        }
    }

    CloseHandle(proc);
    std::fprintf(stderr, "[reader] stopped\n");
    return 0;
}

int main(int argc, char** argv) {
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    Config cfg;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--tick-rate") == 0 && i + 1 < argc) {
            cfg.tick_rate_hz = std::atoi(argv[++i]);
            if (cfg.tick_rate_hz <= 0 || cfg.tick_rate_hz > 1000) {
                std::fprintf(stderr, "[reader] bad tick rate: %s\n", argv[i]);
                return 1;
            }
        } else {
            cfg.process_name = argv[i];
        }
    }

    return run(cfg);
}
