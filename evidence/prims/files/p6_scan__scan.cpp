// scan.cpp
// Pattern scan with wildcard support and RIP-relative displacement resolution.
// C++17, no external dependencies.
//
// Exported functions:
//   std::vector<std::size_t> scan_pattern(const uint8_t* buf, std::size_t buf_len,
//                                          std::string_view pattern);
//     Scan `buf` for `pattern`.  Returns all match offsets (0-based) into `buf`.
//
//   uintptr_t resolve_rip_rel(uintptr_t insn_addr, std::size_t disp_offset,
//                              int disp_size = 4);
//     Given the runtime address of the first byte of an instruction, the
//     offset of the displacement field within that instruction, and the size
//     of the displacement field (default 4 bytes, i.e. a 32-bit rel32),
//     return the absolute address the instruction references.
//     The next-instruction address is computed as insn_addr + disp_offset + disp_size.

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <stdexcept>
#include <sstream>
#include <algorithm>
#include <cctype>

// ---------------------------------------------------------------------------
// Internal pattern representation
// ---------------------------------------------------------------------------

struct PatternByte {
    uint8_t  value;   // expected byte value (ignored when wildcard == true)
    bool     wildcard;
};

// ---------------------------------------------------------------------------
// parse_pattern
//
// Accepts a whitespace-separated hex byte pattern where "??" (or a single "?")
// denotes a wildcard.  Examples:
//   "48 8B ?? ?? 89"
//   "E8 ? ? ? ? 48"
//   "0F 1F 44 00 00"
// ---------------------------------------------------------------------------

static std::vector<PatternByte> parse_pattern(std::string_view pattern)
{
    std::vector<PatternByte> result;

    std::size_t i = 0;
    while (i < pattern.size()) {
        // skip whitespace
        while (i < pattern.size() && std::isspace(static_cast<unsigned char>(pattern[i])))
            ++i;
        if (i >= pattern.size())
            break;

        // wildcard token?
        if (pattern[i] == '?') {
            result.push_back({0x00, true});
            ++i;
            // consume optional second '?'
            if (i < pattern.size() && pattern[i] == '?')
                ++i;
            continue;
        }

        // must be two hex digits
        if (i + 1 >= pattern.size())
            throw std::invalid_argument("parse_pattern: incomplete hex byte at end of pattern");

        char hi = pattern[i];
        char lo = pattern[i + 1];

        auto hex_digit = [](char c) -> uint8_t {
            if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
            if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
            if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
            throw std::invalid_argument(std::string("parse_pattern: invalid hex character '") + c + "'");
            return 0; // unreachable
        };

        uint8_t byte_val = static_cast<uint8_t>((hex_digit(hi) << 4) | hex_digit(lo));
        result.push_back({byte_val, false});
        i += 2;
    }

    return result;
}

// ---------------------------------------------------------------------------
// scan_pattern
//
// Scans [buf, buf + buf_len) for every occurrence of `pattern`.
// Returns a vector of offsets (0-based) at which the pattern starts.
//
// Complexity: O(buf_len * pattern_len) — adequate for typical use-cases
// such as scanning process memory regions at game-start.  A Boyer-Moore-
// style skip can be layered on top if needed.
// ---------------------------------------------------------------------------

std::vector<std::size_t> scan_pattern(const uint8_t* buf,
                                      std::size_t    buf_len,
                                      std::string_view pattern)
{
    if (!buf || buf_len == 0 || pattern.empty())
        return {};

    const std::vector<PatternByte> pat = parse_pattern(pattern);
    if (pat.empty())
        return {};

    const std::size_t pat_len = pat.size();
    std::vector<std::size_t> matches;

    // We need at least pat_len bytes remaining for a match.
    if (buf_len < pat_len)
        return {};

    const std::size_t limit = buf_len - pat_len;

    for (std::size_t i = 0; i <= limit; ++i) {
        bool found = true;
        for (std::size_t j = 0; j < pat_len; ++j) {
            if (!pat[j].wildcard && buf[i + j] != pat[j].value) {
                found = false;
                break;
            }
        }
        if (found)
            matches.push_back(i);
    }

    return matches;
}

// ---------------------------------------------------------------------------
// resolve_rip_rel
//
// Resolves a RIP-relative displacement to an absolute address.
//
// x86-64 RIP-relative addressing:
//   effective_address = RIP + sign_extended_disp
// where RIP is the address of the *next* instruction, i.e.:
//   rip_next = insn_addr + disp_offset + disp_size
//
// Parameters:
//   insn_addr   – runtime address of the first byte of the instruction
//   disp_offset – byte offset of the displacement field within the instruction
//                 (e.g. 2 for a 2-byte opcode + ModRM before the disp32)
//   disp_size   – size of the displacement field in bytes (1, 2, or 4; default 4)
//
// The function reads `disp_size` bytes starting at (insn_addr + disp_offset),
// sign-extends them to a 64-bit signed integer, and adds to the next-instruction
// address.
// ---------------------------------------------------------------------------

uintptr_t resolve_rip_rel(uintptr_t insn_addr,
                           std::size_t disp_offset,
                           int         disp_size /*= 4*/)
{
    if (disp_size != 1 && disp_size != 2 && disp_size != 4)
        throw std::invalid_argument("resolve_rip_rel: disp_size must be 1, 2, or 4");

    // Address of the next instruction (RIP value during execution)
    const uintptr_t next_insn = insn_addr + disp_offset + static_cast<uintptr_t>(disp_size);

    // Read displacement bytes from the instruction stream.
    // We use memcpy to avoid strict-aliasing UB.
    const uint8_t* disp_ptr = reinterpret_cast<const uint8_t*>(insn_addr + disp_offset);

    int64_t disp = 0;
    switch (disp_size) {
        case 1: {
            int8_t d;
            std::memcpy(&d, disp_ptr, 1);
            disp = static_cast<int64_t>(d);
            break;
        }
        case 2: {
            int16_t d;
            std::memcpy(&d, disp_ptr, 2);
            disp = static_cast<int64_t>(d);
            break;
        }
        case 4: {
            int32_t d;
            std::memcpy(&d, disp_ptr, 4);
            disp = static_cast<int64_t>(d);
            break;
        }
    }

    return static_cast<uintptr_t>(static_cast<int64_t>(next_insn) + disp);
}

// ---------------------------------------------------------------------------
// Standalone smoke-test (compile with -DRUN_SMOKE_TEST)
// ---------------------------------------------------------------------------

#ifdef RUN_SMOKE_TEST
#include <cstdio>
#include <cassert>

int main()
{
    // --- scan_pattern tests ---

    // Buffer: 00 48 8B 05 xx yy zz 00 48 8B 05 aa bb cc dd 00
    const uint8_t buf[] = {
        0x00,
        0x48, 0x8B, 0x05, 0x11, 0x22, 0x33, 0x44,  // offset 1
        0x00,
        0x48, 0x8B, 0x05, 0xAA, 0xBB, 0xCC, 0xDD,  // offset 9
        0x00
    };
    const std::size_t buf_len = sizeof(buf);

    // Pattern with wildcards: "48 8B 05 ?? ?? ?? ??"
    auto hits = scan_pattern(buf, buf_len, "48 8B 05 ?? ?? ?? ??");
    assert(hits.size() == 2);
    assert(hits[0] == 1);
    assert(hits[1] == 9);
    std::printf("scan_pattern: found %zu matches at offsets %zu and %zu — OK\n",
                hits.size(), hits[0], hits[1]);

    // No-wildcard exact match
    auto hits2 = scan_pattern(buf, buf_len, "48 8B 05 AA BB CC DD");
    assert(hits2.size() == 1);
    assert(hits2[0] == 9);
    std::printf("scan_pattern exact: found %zu match at offset %zu — OK\n",
                hits2.size(), hits2[0]);

    // No match
    auto hits3 = scan_pattern(buf, buf_len, "FF FF FF");
    assert(hits3.empty());
    std::printf("scan_pattern no-match: OK\n");

    // --- resolve_rip_rel test ---
    // Build a fake instruction in a local buffer:
    //   LEA rax, [rip + disp32]
    //   opcode: 48 8D 05 <disp32>   (3 bytes before the disp)
    // We want it to point to `target`.
    uint32_t target_offset = 0xDEAD;  // fake target relative to next insn
    uint8_t insn[7] = {0x48, 0x8D, 0x05, 0, 0, 0, 0};
    // displacement = target_offset (little-endian)
    std::memcpy(&insn[3], &target_offset, 4);

    uintptr_t insn_addr = reinterpret_cast<uintptr_t>(insn);
    uintptr_t resolved  = resolve_rip_rel(insn_addr, 3, 4);
    uintptr_t expected  = insn_addr + 7 /* next insn */ + target_offset;
    assert(resolved == expected);
    std::printf("resolve_rip_rel: resolved 0x%zX == expected 0x%zX — OK\n",
                (std::size_t)resolved, (std::size_t)expected);

    std::printf("All smoke tests passed.\n");
    return 0;
}
#endif // RUN_SMOKE_TEST
