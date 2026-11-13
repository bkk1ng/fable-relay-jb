// hook.cpp — x64 inline trampoline hook (Windows, C++17)
//
// Exported surface:
//   HookResult InstallHook(void* target, void* detour, HookContext* ctx)
//       Patches `target` with a 14-byte absolute jmp to `detour`.
//       Saves the original bytes and builds a relay stub so the
//       original function can still be called through ctx->relay.
//
//   bool RemoveHook(HookContext* ctx)
//       Restores the original bytes and releases the relay stub.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cassert>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// Absolute indirect jump: FF 25 00000000 <8-byte address>
// Total: 14 bytes.  Used both for the patch and for the relay stub's
// jump-to-original tail.
static constexpr size_t kJmpAbsSize = 14;

// Maximum bytes we may need to copy to build a usable relay stub.
// We over-copy a bit and trim; real production code would decode
// instructions, but for the smoke test we keep it simple.
static constexpr size_t kMaxPrologue = 32;

// Size of the relay stub:
//   - up to kMaxPrologue bytes of copied prologue
//   - 14-byte absolute jump back to target+patched_len
static constexpr size_t kRelayStubMaxSize = kMaxPrologue + kJmpAbsSize;

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

enum class HookResult : int {
    Ok              =  0,
    BadArgs         = -1,
    AllocFailed     = -2,
    ProtectFailed   = -3,
};

struct HookContext {
    void*    target;                        // address that was patched
    void*    relay;                         // callable stub → original behaviour
    uint8_t  original[kJmpAbsSize];         // saved bytes from target
    size_t   patched_len;                   // how many bytes were actually replaced
    DWORD    old_protect;                   // previous page protection
};

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Write a 14-byte FF/25 absolute indirect jump into `dst`.
// The 8-byte target pointer is stored in-line immediately after the FF 25 disp32.
//
//   FF 25 00 00 00 00        JMP QWORD PTR [RIP+0]
//   <lo> <hi> ...            (8-byte absolute address)
static void WriteAbsJmp(uint8_t* dst, void* target_addr) noexcept {
    // FF 25 00000000  — JMP [RIP+0]  (the 32-bit disp is 0, so [RIP+6])
    dst[0] = 0xFF;
    dst[1] = 0x25;
    dst[2] = 0x00;
    dst[3] = 0x00;
    dst[4] = 0x00;
    dst[5] = 0x00;
    // 8-byte absolute address immediately following
    uintptr_t addr = reinterpret_cast<uintptr_t>(target_addr);
    std::memcpy(dst + 6, &addr, sizeof(addr));
}

// Allocate one page of RWX memory for the relay stub.
// We try to stay within ±2 GB of `near_hint` (useful if the hook site
// later needs a 5-byte rel32 patch instead, but here we just use it as
// a hint to VirtualAlloc).
static uint8_t* AllocStubPage(void* near_hint) noexcept {
    // Try a handful of addresses near the hint first.
    static constexpr ptrdiff_t kStep    = 0x10000;        // 64 KB
    static constexpr int       kTries   = 64;
    auto base = reinterpret_cast<uintptr_t>(near_hint);

    for (int i = 1; i <= kTries; ++i) {
        // Try above and below in alternation.
        for (int sign : {1, -1}) {
            uintptr_t candidate = base + static_cast<ptrdiff_t>(i * sign) * kStep;
            void* p = VirtualAlloc(
                reinterpret_cast<void*>(candidate),
                4096,
                MEM_COMMIT | MEM_RESERVE,
                PAGE_EXECUTE_READWRITE);
            if (p) return static_cast<uint8_t*>(p);
        }
    }

    // Fall back: let the OS choose anywhere.
    void* p = VirtualAlloc(nullptr, 4096,
                           MEM_COMMIT | MEM_RESERVE,
                           PAGE_EXECUTE_READWRITE);
    return static_cast<uint8_t*>(p);
}

// ---------------------------------------------------------------------------
// InstallHook
// ---------------------------------------------------------------------------

HookResult InstallHook(void* target, void* detour, HookContext* ctx) noexcept {
    if (!target || !detour || !ctx) return HookResult::BadArgs;

    std::memset(ctx, 0, sizeof(*ctx));
    ctx->target      = target;
    ctx->patched_len = kJmpAbsSize;   // we always write 14 bytes

    // ------------------------------------------------------------------
    // 1. Save original bytes
    // ------------------------------------------------------------------
    auto* tgt = static_cast<uint8_t*>(target);
    std::memcpy(ctx->original, tgt, kJmpAbsSize);

    // ------------------------------------------------------------------
    // 2. Allocate executable relay stub
    // ------------------------------------------------------------------
    uint8_t* stub = AllocStubPage(target);
    if (!stub) return HookResult::AllocFailed;
    ctx->relay = stub;

    // ------------------------------------------------------------------
    // 3. Build relay stub
    //    [ copy of original bytes | absolute jmp to target+patched_len ]
    // ------------------------------------------------------------------
    std::memcpy(stub, ctx->original, kJmpAbsSize);
    uint8_t* return_addr = tgt + kJmpAbsSize;
    WriteAbsJmp(stub + kJmpAbsSize, return_addr);

    // Flush the instruction cache for the stub page.
    FlushInstructionCache(GetCurrentProcess(), stub,
                          kJmpAbsSize + kJmpAbsSize);

    // ------------------------------------------------------------------
    // 4. Unprotect target page, write the hook, restore protection
    // ------------------------------------------------------------------
    DWORD old_prot = 0;
    if (!VirtualProtect(tgt, kJmpAbsSize, PAGE_EXECUTE_READWRITE, &old_prot))
        return HookResult::ProtectFailed;

    ctx->old_protect = old_prot;
    WriteAbsJmp(tgt, detour);
    FlushInstructionCache(GetCurrentProcess(), tgt, kJmpAbsSize);

    DWORD dummy = 0;
    VirtualProtect(tgt, kJmpAbsSize, old_prot, &dummy);

    return HookResult::Ok;
}

// ---------------------------------------------------------------------------
// RemoveHook
// ---------------------------------------------------------------------------

bool RemoveHook(HookContext* ctx) noexcept {
    if (!ctx || !ctx->target || !ctx->relay) return false;

    auto* tgt = static_cast<uint8_t*>(ctx->target);

    DWORD old_prot = 0;
    if (!VirtualProtect(tgt, kJmpAbsSize, PAGE_EXECUTE_READWRITE, &old_prot))
        return false;

    std::memcpy(tgt, ctx->original, kJmpAbsSize);
    FlushInstructionCache(GetCurrentProcess(), tgt, kJmpAbsSize);

    DWORD dummy = 0;
    VirtualProtect(tgt, kJmpAbsSize, ctx->old_protect, &dummy);

    VirtualFree(ctx->relay, 0, MEM_RELEASE);
    std::memset(ctx, 0, sizeof(*ctx));
    return true;
}

// ---------------------------------------------------------------------------
// Smoke test  (cl /DSMOKE hook.cpp)
// ---------------------------------------------------------------------------
#ifdef SMOKE

#include <cstdio>

static int __cdecl OrigAdd(int a, int b) { return a + b; }

using AddFn = int(__cdecl*)(int, int);
static HookContext g_ctx{};

static int __cdecl DetourAdd(int a, int b) {
    // Call original through relay, then multiply result by 10 to prove
    // the detour ran.
    int orig = reinterpret_cast<AddFn>(g_ctx.relay)(a, b);
    return orig * 10;
}

int main() {
    printf("Before hook : OrigAdd(2,3) = %d\n", OrigAdd(2, 3));

    HookResult r = InstallHook(
        reinterpret_cast<void*>(OrigAdd),
        reinterpret_cast<void*>(DetourAdd),
        &g_ctx);

    if (r != HookResult::Ok) {
        fprintf(stderr, "InstallHook failed: %d\n", static_cast<int>(r));
        return 1;
    }

    printf("After hook  : OrigAdd(2,3) = %d  (expect 50)\n", OrigAdd(2, 3));
    printf("Via relay   : relay(2,3)   = %d  (expect 5)\n",
           reinterpret_cast<AddFn>(g_ctx.relay)(2, 3));

    RemoveHook(&g_ctx);
    printf("After unhook: OrigAdd(2,3) = %d  (expect 5)\n", OrigAdd(2, 3));
    return 0;
}
#endif // SMOKE
