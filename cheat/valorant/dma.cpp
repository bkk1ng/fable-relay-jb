// dma.cpp — FPGA-backed physical-memory access layer
// C++17, Win32 real backend behind #ifdef DMA_WIN32_BACKEND
// Build (selftest):  cl /std:c++17 /DDMA_SELFTEST /EHsc dma.cpp /Fe:dma_test.exe
// Build (library):   cl /std:c++17 /EHsc /c dma.cpp

#include <cstdint>
#include <cstring>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>
#include <algorithm>
#include <stdexcept>

#ifdef DMA_WIN32_BACKEND
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

using PhysAddr = uint64_t;
using VirtAddr = uint64_t;

// ---------------------------------------------------------------------------
// Abstract MemProc interface
// ---------------------------------------------------------------------------

struct MemProc {
    virtual ~MemProc() = default;

    // Open / close the target process or device.
    virtual bool open()  = 0;
    virtual void close() = 0;

    // Read/write a physically-addressed range.
    // Returns the number of bytes actually transferred.
    virtual size_t read (PhysAddr pa, void* buf,        size_t len) = 0;
    virtual size_t write(PhysAddr pa, const void* buf,  size_t len) = 0;
};

// ---------------------------------------------------------------------------
// Thin helpers that map the MemProc interface onto typed I/O
// ---------------------------------------------------------------------------

static bool mem_read_exact(MemProc& mp, PhysAddr pa, void* buf, size_t len)
{
    return mp.read(pa, buf, len) == len;
}

template<typename T>
static bool mem_read_val(MemProc& mp, PhysAddr pa, T& out)
{
    return mem_read_exact(mp, pa, &out, sizeof(T));
}

// ---------------------------------------------------------------------------
// device_read / device_write — public API over MemProc
// ---------------------------------------------------------------------------

/// Read `len` bytes from physical address `phys_addr` into `buf`.
/// Returns true on success.
bool device_read(MemProc& mp, PhysAddr phys_addr, void* buf, size_t len)
{
    return mem_read_exact(mp, phys_addr, buf, len);
}

/// Write `len` bytes from `buf` to physical address `phys_addr`.
/// Returns true on success.
bool device_write(MemProc& mp, PhysAddr phys_addr, const void* buf, size_t len)
{
    return mp.write(phys_addr, buf, len) == len;
}

// ---------------------------------------------------------------------------
// x86-64 four-level page-table walk (PML4 → PDPT → PD → PT)
// Supports 4 KiB (PT-mapped) and 2 MiB (PD large-page) leaves.
// ---------------------------------------------------------------------------

// Page-table entry flags
static constexpr uint64_t PTE_PRESENT   = 1ULL << 0;
static constexpr uint64_t PTE_PS        = 1ULL << 7;   // large page at PD level
static constexpr uint64_t PTE_ADDR_MASK = 0x000F'FFFF'FFFF'F000ULL; // bits [51:12]

// 2 MiB large page: physical base in bits [51:21]
static constexpr uint64_t PDE_LARGE_ADDR_MASK = 0x000F'FFFF'FFE0'0000ULL;

static constexpr uint64_t PAGE_SIZE_4K  = 0x1000ULL;
static constexpr uint64_t PAGE_SIZE_2M  = 0x200000ULL;

struct PageWalkResult {
    PhysAddr phys_base = 0;    // base of the page (not of the byte)
    uint64_t page_size = 0;    // 4096 or 2 MiB
    bool     valid     = false;
};

/// Walk the four-level page table rooted at `cr3` (physical address of PML4)
/// for virtual address `va`.  All reads go through `mp`.
static PageWalkResult virt_to_phys_walk(MemProc& mp, PhysAddr cr3, VirtAddr va)
{
    // Extract 9-bit indices from the virtual address
    const uint64_t pml4_idx = (va >> 39) & 0x1FF;
    const uint64_t pdpt_idx = (va >> 30) & 0x1FF;
    const uint64_t pd_idx   = (va >> 21) & 0x1FF;
    const uint64_t pt_idx   = (va >> 12) & 0x1FF;

    uint64_t entry = 0;

    // ---- PML4 ----
    PhysAddr pml4e_pa = (cr3 & PTE_ADDR_MASK) + pml4_idx * 8;
    if (!mem_read_val(mp, pml4e_pa, entry)) return {};
    if (!(entry & PTE_PRESENT)) return {};

    // ---- PDPT ----
    PhysAddr pdpte_pa = (entry & PTE_ADDR_MASK) + pdpt_idx * 8;
    if (!mem_read_val(mp, pdpte_pa, entry)) return {};
    if (!(entry & PTE_PRESENT)) return {};
    // 1 GiB pages are uncommon and not supported here
    if (entry & PTE_PS) return {};

    // ---- PD ----
    PhysAddr pde_pa = (entry & PTE_ADDR_MASK) + pd_idx * 8;
    if (!mem_read_val(mp, pde_pa, entry)) return {};
    if (!(entry & PTE_PRESENT)) return {};

    if (entry & PTE_PS) {
        // 2 MiB large page
        PageWalkResult r;
        r.phys_base = entry & PDE_LARGE_ADDR_MASK;
        r.page_size = PAGE_SIZE_2M;
        r.valid     = true;
        return r;
    }

    // ---- PT ----
    PhysAddr pte_pa = (entry & PTE_ADDR_MASK) + pt_idx * 8;
    if (!mem_read_val(mp, pte_pa, entry)) return {};
    if (!(entry & PTE_PRESENT)) return {};

    PageWalkResult r;
    r.phys_base = entry & PTE_ADDR_MASK;
    r.page_size = PAGE_SIZE_4K;
    r.valid     = true;
    return r;
}

/// Translate a virtual address to a physical address.
/// Returns true and writes the physical address to `pa_out` on success.
bool virt_to_phys(MemProc& mp, PhysAddr cr3, VirtAddr va, PhysAddr& pa_out)
{
    PageWalkResult r = virt_to_phys_walk(mp, cr3, va);
    if (!r.valid) return false;
    uint64_t page_offset_mask = r.page_size - 1;
    pa_out = r.phys_base | (va & page_offset_mask);
    return true;
}

// ---------------------------------------------------------------------------
// Scatter read — batch read of a list of (virtual-address, length) pairs
// ---------------------------------------------------------------------------

struct ScatterEntry {
    VirtAddr  va;
    size_t    len;
    void*     buf;      // caller-allocated output buffer of at least `len` bytes
};

struct ScatterResult {
    size_t entries_ok;   // number of entries fully satisfied
    size_t entries_fail; // number of entries that encountered a translation error
};

/// Read each entry in `entries`, translating virtual→physical page by page.
/// Contiguous physical runs (same page, adjacent bytes) are merged into a
/// single mp.read() call.  `cr3` is the physical address of the target PML4.
ScatterResult scatter_read(MemProc& mp, PhysAddr cr3, const std::vector<ScatterEntry>& entries)
{
    ScatterResult result{};

    for (const auto& e : entries) {
        uint8_t* dst      = static_cast<uint8_t*>(e.buf);
        size_t   remaining = e.len;
        VirtAddr va        = e.va;
        bool     ok        = true;

        // We iterate page-by-page and accumulate physically contiguous sub-runs.
        // A sub-run ends when we cross a page boundary or the physical address
        // is not contiguous with the previous page.

        while (remaining > 0 && ok) {
            // Determine how many bytes are left in the current virtual page
            uint64_t page_offset    = va & (PAGE_SIZE_4K - 1);
            size_t   bytes_in_page  = static_cast<size_t>(PAGE_SIZE_4K - page_offset);
            size_t   chunk          = std::min(bytes_in_page, remaining);

            // Start of a physically-contiguous run
            PhysAddr run_pa   = 0;
            size_t   run_len  = 0;
            uint8_t* run_dst  = dst;

            // We'll keep extending the run as long as successive pages are
            // physically adjacent (address-contiguous).
            VirtAddr  run_va  = va;
            size_t    run_rem = remaining;
            uint8_t*  run_ptr = dst;

            while (run_rem > 0) {
                uint64_t off      = run_va & (PAGE_SIZE_4K - 1);
                size_t   bip      = static_cast<size_t>(PAGE_SIZE_4K - off);
                size_t   c        = std::min(bip, run_rem);

                PhysAddr pa = 0;
                if (!virt_to_phys(mp, cr3, run_va, pa)) {
                    ok = false;
                    break;
                }

                if (run_len == 0) {
                    // First page of this run
                    run_pa  = pa;
                    run_len = c;
                } else if (pa == run_pa + run_len) {
                    // Physically contiguous — extend
                    run_len += c;
                } else {
                    // Gap: flush accumulated run before starting the new one
                    if (mp.read(run_pa, run_dst, run_len) != run_len) {
                        ok = false;
                        break;
                    }
                    run_dst += run_len;
                    run_pa   = pa;
                    run_len  = c;
                }

                run_va  += c;
                run_rem -= c;
            }

            // Flush the final accumulated run (if any)
            if (run_len > 0 && ok) {
                if (mp.read(run_pa, run_dst, run_len) != run_len) {
                    ok = false;
                }
            }

            // Advance outer cursor by everything we tried to consume this iteration.
            // Because we rebuilt the entire remaining range inside the inner loop,
            // the outer while-loop only needs one pass.
            break; // inner loop handled all remaining bytes
        }

        if (ok) ++result.entries_ok;
        else    ++result.entries_fail;
    }

    return result;
}

// ---------------------------------------------------------------------------
// Win32 real backend (FPGA device via a driver that exposes a file handle)
// ---------------------------------------------------------------------------

#ifdef DMA_WIN32_BACKEND

struct Win32MemProc : MemProc {
    std::wstring device_path;
    HANDLE       hdev = INVALID_HANDLE_VALUE;

    explicit Win32MemProc(std::wstring path) : device_path(std::move(path)) {}

    bool open() override {
        hdev = ::CreateFileW(
            device_path.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0, nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        return hdev != INVALID_HANDLE_VALUE;
    }

    void close() override {
        if (hdev != INVALID_HANDLE_VALUE) {
            ::CloseHandle(hdev);
            hdev = INVALID_HANDLE_VALUE;
        }
    }

    // The assumed driver protocol: a 64-bit physical address is passed via
    // IOCTL_INPUT, then the driver maps and returns `len` bytes.  Adjust
    // the IOCTL codes to match your actual driver interface.
    static constexpr DWORD IOCTL_READ_PHYS  = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_DATA);
    static constexpr DWORD IOCTL_WRITE_PHYS = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_WRITE_DATA);

#pragma pack(push,1)
    struct IoctlHeader {
        uint64_t phys_addr;
        uint64_t length;
    };
#pragma pack(pop)

    size_t read(PhysAddr pa, void* buf, size_t len) override {
        if (hdev == INVALID_HANDLE_VALUE || len == 0) return 0;

        IoctlHeader hdr{ pa, static_cast<uint64_t>(len) };
        DWORD bytes_returned = 0;
        BOOL ok = ::DeviceIoControl(
            hdev,
            IOCTL_READ_PHYS,
            &hdr, static_cast<DWORD>(sizeof(hdr)),
            buf,  static_cast<DWORD>(len),
            &bytes_returned,
            nullptr);
        return ok ? static_cast<size_t>(bytes_returned) : 0;
    }

    size_t write(PhysAddr pa, const void* buf, size_t len) override {
        if (hdev == INVALID_HANDLE_VALUE || len == 0) return 0;

        // Build a contiguous input buffer: header followed by data.
        std::vector<uint8_t> in_buf(sizeof(IoctlHeader) + len);
        IoctlHeader hdr{ pa, static_cast<uint64_t>(len) };
        std::memcpy(in_buf.data(), &hdr, sizeof(hdr));
        std::memcpy(in_buf.data() + sizeof(hdr), buf, len);

        DWORD bytes_returned = 0;
        BOOL ok = ::DeviceIoControl(
            hdev,
            IOCTL_WRITE_PHYS,
            in_buf.data(), static_cast<DWORD>(in_buf.size()),
            nullptr, 0,
            &bytes_returned,
            nullptr);
        return ok ? len : 0;
    }
};

#endif // DMA_WIN32_BACKEND

// ---------------------------------------------------------------------------
// In-memory fake MemProc for testing
// ---------------------------------------------------------------------------

struct FakeMemProc : MemProc {
    std::vector<uint8_t> mem;

    explicit FakeMemProc(size_t size) : mem(size, 0) {}

    bool open()  override { return true; }
    void close() override {}

    size_t read(PhysAddr pa, void* buf, size_t len) override {
        if (pa + len > mem.size()) return 0;
        std::memcpy(buf, mem.data() + pa, len);
        return len;
    }

    size_t write(PhysAddr pa, const void* buf, size_t len) override {
        if (pa + len > mem.size()) return 0;
        std::memcpy(mem.data() + pa, buf, len);
        return len;
    }
};

// ---------------------------------------------------------------------------
// Self-test
// ---------------------------------------------------------------------------

#ifdef DMA_SELFTEST

// Build a minimal identity-mapped four-level page table inside FakeMemProc.
//
// Layout (all addresses are within the fake memory arena):
//   0x0000 — PML4  (one entry pointing to PDPT)
//   0x1000 — PDPT  (one entry pointing to PD)
//   0x2000 — PD    (entry 0 → PT for 4 KiB; entry 1 → 2 MiB large page at phys 0x200000)
//   0x3000 — PT    (one entry pointing to page at 0x4000)
//   0x4000 — data page (4 KiB)
//
// Virtual address layout used in tests:
//   PML4[0] → PDPT[0] → PD[0] → PT[1] → phys 0x4000  (4 KiB, offset 0x010 → phys 0x4010)
//   PML4[0] → PDPT[0] → PD[1] → 2 MiB large page at phys 0x200000

static constexpr PhysAddr PA_PML4    = 0x0000;
static constexpr PhysAddr PA_PDPT    = 0x1000;
static constexpr PhysAddr PA_PD      = 0x2000;
static constexpr PhysAddr PA_PT      = 0x3000;
static constexpr PhysAddr PA_DATA    = 0x4000;
static constexpr PhysAddr PA_LARGE   = 0x200000; // base for 2 MiB page

static constexpr size_t FAKE_MEM_SIZE = 0x400000; // 4 MiB

// Virtual address helper: build a canonical x86-64 VA from table indices.
// sign-extend bit 47
static VirtAddr make_va(unsigned pml4, unsigned pdpt, unsigned pd, unsigned pt, uint64_t offset)
{
    uint64_t va =
        (static_cast<uint64_t>(pml4) << 39) |
        (static_cast<uint64_t>(pdpt) << 30) |
        (static_cast<uint64_t>(pd)   << 21) |
        (static_cast<uint64_t>(pt)   << 12) |
        (offset & 0xFFF);
    // sign-extend bit 47
    if (va & (1ULL << 47)) va |= 0xFFFF000000000000ULL;
    return va;
}

static void setup_fake_page_tables(FakeMemProc& mp)
{
    auto write64 = [&](PhysAddr addr, uint64_t val) {
        mp.write(addr, &val, 8);
    };

    // PML4 entry 0 → PDPT
    write64(PA_PML4 + 0 * 8, PA_PDPT | PTE_PRESENT);

    // PDPT entry 0 → PD
    write64(PA_PDPT + 0 * 8, PA_PD | PTE_PRESENT);

    // PD entry 0 → PT (4 KiB mapping)
    write64(PA_PD + 0 * 8, PA_PT | PTE_PRESENT);

    // PD entry 1 → 2 MiB large page at PA_LARGE
    write64(PA_PD + 1 * 8, PA_LARGE | PTE_PRESENT | PTE_PS);

    // PT entry 1 → data page at PA_DATA
    write64(PA_PT + 1 * 8, PA_DATA | PTE_PRESENT);
}

static int selftest()
{
    printf("=== dma.cpp selftest ===\n");
    int failures = 0;

    FakeMemProc mp(FAKE_MEM_SIZE);
    assert(mp.open());

    setup_fake_page_tables(mp);

    // --- Test 1: virtual→physical walk, 4 KiB page ---
    {
        // VA for PML4[0]→PDPT[0]→PD[0]→PT[1], offset 0x010
        VirtAddr va = make_va(0, 0, 0, 1, 0x010);
        PhysAddr pa = 0;
        bool ok = virt_to_phys(mp, PA_PML4, va, pa);
        if (!ok || pa != (PA_DATA + 0x010)) {
            printf("FAIL test1: virt_to_phys 4KiB — ok=%d pa=0x%llx (expected 0x%llx)\n",
                   (int)ok, (unsigned long long)pa, (unsigned long long)(PA_DATA + 0x010));
            ++failures;
        } else {
            printf("PASS test1: virt_to_phys 4KiB => 0x%llx\n", (unsigned long long)pa);
        }
    }

    // --- Test 2: virtual→physical walk, 2 MiB large page ---
    {
        // VA for PML4[0]→PDPT[0]→PD[1], offset within 2 MiB = 0xABCD
        VirtAddr va = make_va(0, 0, 1, 0, 0xABCD) & ~0xFFFULL;
        va |= 0xABCD; // set full 21-bit offset inside the 2 MiB page
        // Properly: build from scratch with a 21-bit page offset
        uint64_t large_offset = 0x1ABCD; // 21-bit offset
        VirtAddr va2 =
            (static_cast<uint64_t>(0) << 39) |
            (static_cast<uint64_t>(0) << 30) |
            (static_cast<uint64_t>(1) << 21) |
            large_offset;
        PhysAddr pa = 0;
        bool ok = virt_to_phys(mp, PA_PML4, va2, pa);
        PhysAddr expected = PA_LARGE + large_offset;
        if (!ok || pa != expected) {
            printf("FAIL test2: virt_to_phys 2MiB — ok=%d pa=0x%llx (expected 0x%llx)\n",
                   (int)ok, (unsigned long long)pa, (unsigned long long)expected);
            ++failures;
        } else {
            printf("PASS test2: virt_to_phys 2MiB => 0x%llx\n", (unsigned long long)pa);
        }
    }

    // --- Test 3: not-present entry returns false ---
    {
        // PML4 index 1 is not set → should fail
        VirtAddr va = make_va(1, 0, 0, 0, 0);
        PhysAddr pa = 0;
        bool ok = virt_to_phys(mp, PA_PML4, va, pa);
        if (ok) {
            printf("FAIL test3: expected walk failure for not-present PML4 entry\n");
            ++failures;
        } else {
            printf("PASS test3: not-present PML4 entry correctly returns false\n");
        }
    }

    // --- Test 4: device_read / device_write round-trip ---
    {
        const char msg[] = "hello_dma";
        bool wok = device_write(mp, PA_DATA + 0x100, msg, sizeof(msg));
        char rbuf[sizeof(msg)] = {};
        bool rok = device_read(mp, PA_DATA + 0x100, rbuf, sizeof(rbuf));
        if (!wok || !rok || std::memcmp(msg, rbuf, sizeof(msg)) != 0) {
            printf("FAIL test4: device_read/device_write round-trip\n");
            ++failures;
        } else {
            printf("PASS test4: device_read/device_write round-trip\n");
        }
    }

    // --- Test 5: scatter_read, single contiguous entry ---
    {
        // Write a known pattern to the data page at offset 0x020
        uint8_t pattern[16];
        for (int i = 0; i < 16; i++) pattern[i] = static_cast<uint8_t>(0xA0 + i);
        mp.write(PA_DATA + 0x020, pattern, sizeof(pattern));

        VirtAddr va = make_va(0, 0, 0, 1, 0x020);
        uint8_t  rbuf[16] = {};
        std::vector<ScatterEntry> entries{ ScatterEntry{ va, sizeof(rbuf), rbuf } };
        ScatterResult sr = scatter_read(mp, PA_PML4, entries);
        if (sr.entries_ok != 1 || sr.entries_fail != 0 || std::memcmp(rbuf, pattern, 16) != 0) {
            printf("FAIL test5: scatter_read single entry — ok=%zu fail=%zu\n",
                   sr.entries_ok, sr.entries_fail);
            ++failures;
        } else {
            printf("PASS test5: scatter_read single entry\n");
        }
    }

    // --- Test 6: scatter_read, multiple entries, one invalid VA ---
    {
        uint8_t buf_valid[8] = {};
        uint8_t buf_invalid[8] = {};
        VirtAddr va_good    = make_va(0, 0, 0, 1, 0x040);
        VirtAddr va_bad     = make_va(2, 0, 0, 0, 0); // PML4[2] not present
        std::vector<ScatterEntry> entries{
            ScatterEntry{ va_good,  sizeof(buf_valid),   buf_valid   },
            ScatterEntry{ va_bad,   sizeof(buf_invalid),  buf_invalid  },
        };
        ScatterResult sr = scatter_read(mp, PA_PML4, entries);
        if (sr.entries_ok != 1 || sr.entries_fail != 1) {
            printf("FAIL test6: scatter_read mixed — ok=%zu fail=%zu (expected 1/1)\n",
                   sr.entries_ok, sr.entries_fail);
            ++failures;
        } else {
            printf("PASS test6: scatter_read mixed valid/invalid entries\n");
        }
    }

    // --- Test 7: scatter_read spanning a page boundary (PT[1] + PT[2] contiguous) ---
    {
        // Map PT[2] → data page+1 (PA_DATA + PAGE_SIZE_4K), making it
        // physically contiguous with PT[1]'s page so a cross-boundary
        // scatter merges into one read.
        auto write64 = [&](PhysAddr addr, uint64_t val) {
            mp.write(addr, &val, 8);
        };
        PhysAddr pa_data2 = PA_DATA + PAGE_SIZE_4K;
        // Extend FakeMemProc isn't possible after construction, but we have 4 MiB;
        // PA_DATA = 0x4000, PA_DATA+0x1000 = 0x5000, well within 0x400000.
        write64(PA_PT + 2 * 8, pa_data2 | PTE_PRESENT);

        // Write pattern across the boundary: last 8 bytes of PT[1]'s page,
        // first 8 bytes of PT[2]'s page.
        uint8_t cross[16];
        for (int i = 0; i < 16; i++) cross[i] = static_cast<uint8_t>(0xC0 + i);
        mp.write(PA_DATA  + (PAGE_SIZE_4K - 8), cross,     8);
        mp.write(pa_data2 + 0,                   cross + 8, 8);

        VirtAddr va_cross = make_va(0, 0, 0, 1, PAGE_SIZE_4K - 8);
        uint8_t rbuf[16] = {};
        std::vector<ScatterEntry> entries{ ScatterEntry{ va_cross, sizeof(rbuf), rbuf } };
        ScatterResult sr = scatter_read(mp, PA_PML4, entries);
        if (sr.entries_ok != 1 || std::memcmp(rbuf, cross, 16) != 0) {
            printf("FAIL test7: scatter_read cross-page-boundary — ok=%zu fail=%zu\n",
                   sr.entries_ok, sr.entries_fail);
            ++failures;
        } else {
            printf("PASS test7: scatter_read cross-page-boundary\n");
        }
    }

    printf("\n%s — %d failure(s)\n", failures == 0 ? "ALL PASSED" : "SOME FAILED", failures);
    mp.close();
    return failures == 0 ? 0 : 1;
}

int main(int argc, char* argv[])
{
    (void)argc; (void)argv;
    return selftest();
}

#endif // DMA_SELFTEST
