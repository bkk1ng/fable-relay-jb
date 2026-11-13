// test_dma.cpp — independent validation of the artifact's address translation and scatter path.
//
// The artifact ships its own --selftest (7 cases, all pass). That test builds its own fake page tables,
// so passing it proves self-consistency, not correctness. This harness builds a *different* layout with
// values the artifact has never seen, computes the expected physical addresses by hand, and calls the
// artifact's own `virt_to_phys`, `device_read`, `device_write` and `scatter_read`.
#define _GNU_SOURCE
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>

#include "../cheat/valorant/dma.cpp"

namespace {

struct VecMem : MemProc {
    std::vector<uint8_t> mem;
    explicit VecMem(size_t n) : mem(n, 0) {}
    bool open() override { return true; }
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
    void put64(uint64_t pa, uint64_t v) { std::memcpy(mem.data() + pa, &v, 8); }
    uint64_t pte(uint64_t frame, bool ps = false) {
        return (frame & 0x000F'FFFF'FFFF'F000ULL) | 0x1ULL | (ps ? (1ULL << 7) : 0);
    }
};

int failures = 0;
void check(bool cond, const char* what) {
    if (!cond) { ++failures; std::printf("  FAIL %s\n", what); }
}

// layout chosen here, unlike the artifact's selftest:
//   cr3 0x9000 -> PML4[1] = PDPT 0x8000 ; PDPT[2] = PD 0x7000 ; PD[3] = PT 0x6000
//   PT[5] -> data page 0x100000 (4 KiB) ; PD[4] = 2 MiB page at 0x200000 ; PML4[7] absent
constexpr uint64_t CR3 = 0x9000, PML4 = 0x9000, PDPT = 0x8000, PD = 0x7000, PT = 0x6000;
constexpr uint64_t DATA_PAGE = 0x100000, LARGE_PAGE = 0x200000;

uint64_t va_of(unsigned i4, unsigned i3, unsigned i2, unsigned i1, uint64_t off) {
    return (uint64_t(i4) << 39) | (uint64_t(i3) << 30) | (uint64_t(i2) << 21) |
           (uint64_t(i1) << 12) | off;
}

}  // namespace

int main() {
    VecMem mp(0x800000);  // 8 MiB of physical memory

    mp.put64(PML4 + 1 * 8, mp.pte(PDPT));
    mp.put64(PDPT + 2 * 8, mp.pte(PD));
    mp.put64(PD + 3 * 8, mp.pte(PT));
    mp.put64(PT + 5 * 8, mp.pte(DATA_PAGE));
    mp.put64(PD + 4 * 8, mp.pte(LARGE_PAGE, true));

    // known bytes in the 4 KiB data page and in the 2 MiB page
    const char payload[] = "dma-walk-ok";
    std::memcpy(mp.mem.data() + DATA_PAGE + 0x123, payload, sizeof payload);
    const uint32_t sentinel = 0xC0FFEE11u;
    std::memcpy(mp.mem.data() + LARGE_PAGE + 0x2'0000, &sentinel, sizeof sentinel);

    // 1. 4 KiB page walk
    PhysAddr pa = 0;
    const uint64_t va4k = va_of(1, 2, 3, 5, 0x123);
    check(virt_to_phys(mp, CR3, va4k, pa), "4 KiB walk resolves");
    check(pa == DATA_PAGE + 0x123, "4 KiB walk yields the expected physical address");

    // 2. 2 MiB page walk
    const uint64_t va2m = va_of(1, 2, 4, 0, 0x2'0000);
    check(virt_to_phys(mp, CR3, va2m, pa), "2 MiB page resolves");
    check(pa == LARGE_PAGE + 0x2'0000, "2 MiB walk yields the expected physical address");

    // 3. absent mapping
    check(!virt_to_phys(mp, CR3, va_of(7, 0, 0, 0, 0), pa), "absent PML4 entry fails");

    // 4. device_read through the walk
    char got[sizeof payload] = {0};
    check(device_read(mp, DATA_PAGE + 0x123, got, sizeof got), "device_read succeeds");
    check(std::memcmp(got, payload, sizeof payload) == 0, "device_read returns the stored bytes");

    // 5. device_write round-trip at a raw physical address
    const uint32_t fresh = 0x1234FEDCu;
    check(device_write(mp, LARGE_PAGE + 0x2'0100, &fresh, sizeof fresh), "device_write succeeds");
    uint32_t back = 0;
    check(device_read(mp, LARGE_PAGE + 0x2'0100, &back, sizeof back) && back == fresh,
          "device_write lands at the physical address");

    // 6. scatter_read: one entry inside a page, one crossing a VA page boundary where the two pages are
    //    physically contiguous (so the batching path must merge them), one unmapped entry.
    //    PT[6] -> phys 0x203000, PT[7] -> phys 0x204000; "abcdefgh" sits at 0x203FFC so it straddles both.
    mp.put64(PT + 6 * 8, mp.pte(LARGE_PAGE + 0x3000));
    mp.put64(PT + 7 * 8, mp.pte(LARGE_PAGE + 0x4000));
    std::memcpy(mp.mem.data() + LARGE_PAGE + 0x3FFC, "abcdefgh", 8);

    std::vector<uint8_t> buf_a(4), buf_b(8), buf_c(4);
    std::vector<ScatterEntry> entries = {
        {va4k, 4, buf_a.data()},
        {va_of(1, 2, 3, 6, 0xFFC), 8, buf_b.data()},
        {va_of(7, 0, 0, 0, 0), 4, buf_c.data()},
    };
    ScatterResult sr = scatter_read(mp, CR3, entries);
    check(sr.entries_ok == 2, "scatter_read reports 2 satisfied entries");
    check(sr.entries_fail == 1, "scatter_read reports 1 failed entry");
    check(std::memcmp(buf_a.data(), "dma-", 4) == 0, "scatter_read filled the first buffer");
    check(std::memcmp(buf_b.data(), "abcdefgh", 8) == 0, "scatter_read filled the cross-page buffer");

    if (failures) { std::printf("FAIL dma (%d)\n", failures); return 1; }
    std::printf("PASS dma — independent page-table layout: 4 KiB + 2 MiB walks, absent entry, "
                "typed read/write, scatter with one bad entry\n");
    return 0;
}
