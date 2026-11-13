// mapper.cpp
// Finds a named module's base address inside a running process, parses its PE
// export table (32- and 64-bit), and resolves a caller-supplied list of symbol
// names to absolute virtual addresses via ReadProcessMemory.
//
// Build (MSVC):
//   cl /std:c++17 /W4 /EHsc mapper.cpp /link
//
// Usage:
//   mapper.exe <pid> <module.dll> <symbol1> [symbol2 ...]
//
// Exit codes: 0 = all symbols resolved, 1 = fatal error, 2 = >=1 not found.

#include <windows.h>
#include <tlhelp32.h>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// RAII handle wrapper
// ---------------------------------------------------------------------------

struct UniqueHandle {
    HANDLE h;

    explicit UniqueHandle(HANDLE h_) noexcept : h(h_) {}
    ~UniqueHandle() noexcept { if (valid()) CloseHandle(h); }

    UniqueHandle(const UniqueHandle&)            = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    bool   valid()   const noexcept { return h && h != INVALID_HANDLE_VALUE; }
    operator HANDLE() const noexcept { return h; }
};

// ---------------------------------------------------------------------------
// Remote-memory helpers
// ---------------------------------------------------------------------------

// Read sizeof(T) bytes from addr in proc into *out.
// Returns false on any failure (partial reads are treated as failures).
template<typename T>
static bool rpm(HANDLE proc, uintptr_t addr, T* out) noexcept {
    SIZE_T read = 0;
    return ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(addr),
                             out, sizeof(T), &read)
           && read == sizeof(T);
}

// Read a null-terminated ASCII string from addr in proc (up to max_len bytes).
static std::optional<std::string>
rpm_str(HANDLE proc, uintptr_t addr, size_t max_len = 256) {
    std::string buf(max_len, '\0');
    SIZE_T read = 0;
    ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(addr),
                      buf.data(), max_len, &read);
    if (read == 0) return std::nullopt;
    buf.resize(strnlen(buf.data(), read));
    return buf;
}

// ---------------------------------------------------------------------------
// Module base lookup
// ---------------------------------------------------------------------------

// Returns the load address of module_name (case-insensitive) in process pid,
// or 0 if the module is not present.
// TH32CS_SNAPMODULE32 is added so that a 64-bit tool can see 32-bit modules.
static uintptr_t find_module_base(DWORD pid, const std::string& module_name) {
    UniqueHandle snap(
        CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
    if (!snap.valid()) return 0;

    // Lowercase target once
    std::string target = module_name;
    for (char& c : target)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);

    for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
        // Convert the wide module name to UTF-8 for comparison
        char narrow[MAX_PATH]{};
        WideCharToMultiByte(CP_UTF8, 0, me.szModule, -1,
                            narrow, MAX_PATH, nullptr, nullptr);

        std::string name(narrow);
        for (char& c : name)
            c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

        if (name == target)
            return reinterpret_cast<uintptr_t>(me.modBaseAddr);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// PE export table parser
// ---------------------------------------------------------------------------

// Returns a map of export-name -> absolute virtual address for every named,
// non-forwarded export in the module at base inside proc.
// Returns an empty map on any parse failure.
static std::unordered_map<std::string, uintptr_t>
parse_exports(HANDLE proc, uintptr_t base) {
    std::unordered_map<std::string, uintptr_t> result;

    // --- DOS header ---------------------------------------------------------
    IMAGE_DOS_HEADER dos{};
    if (!rpm(proc, base, &dos) || dos.e_magic != IMAGE_DOS_SIGNATURE)
        return result;

    // --- NT headers ---------------------------------------------------------
    // Read the 64-bit variant first to check the Optional Header magic.
    // We re-read as the 32-bit variant below if necessary.
    IMAGE_NT_HEADERS64 nt64{};
    if (!rpm(proc, base + static_cast<uintptr_t>(dos.e_lfanew), &nt64) ||
        nt64.Signature != IMAGE_NT_SIGNATURE)
        return result;

    IMAGE_DATA_DIRECTORY export_dir_entry{};
    const bool is64 =
        (nt64.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC);

    if (is64) {
        export_dir_entry =
            nt64.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    } else {
        IMAGE_NT_HEADERS32 nt32{};
        if (!rpm(proc, base + static_cast<uintptr_t>(dos.e_lfanew), &nt32))
            return result;
        export_dir_entry =
            nt32.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    }

    if (export_dir_entry.VirtualAddress == 0 || export_dir_entry.Size == 0)
        return result;  // module has no exports

    // Absolute VA of the export directory (used to detect forwarder RVAs)
    const uintptr_t exp_va  = base + export_dir_entry.VirtualAddress;
    const uintptr_t exp_end = exp_va + export_dir_entry.Size;

    // --- IMAGE_EXPORT_DIRECTORY ---------------------------------------------
    IMAGE_EXPORT_DIRECTORY exp{};
    if (!rpm(proc, exp_va, &exp) || exp.NumberOfNames == 0)
        return result;

    const DWORD num_names  = exp.NumberOfNames;
    const DWORD num_funcs  = exp.NumberOfFunctions;

    // --- Three parallel export arrays ---------------------------------------
    std::vector<DWORD> names(num_names);
    std::vector<WORD>  ordinals(num_names);
    std::vector<DWORD> functions(num_funcs);

    SIZE_T read = 0;

    if (!ReadProcessMemory(proc,
            reinterpret_cast<LPCVOID>(base + exp.AddressOfNames),
            names.data(), num_names * sizeof(DWORD), &read) ||
        read != num_names * sizeof(DWORD))
        return result;

    if (!ReadProcessMemory(proc,
            reinterpret_cast<LPCVOID>(base + exp.AddressOfNameOrdinals),
            ordinals.data(), num_names * sizeof(WORD), &read) ||
        read != num_names * sizeof(WORD))
        return result;

    if (!ReadProcessMemory(proc,
            reinterpret_cast<LPCVOID>(base + exp.AddressOfFunctions),
            functions.data(), num_funcs * sizeof(DWORD), &read) ||
        read != num_funcs * sizeof(DWORD))
        return result;

    // --- Walk the name table ------------------------------------------------
    for (DWORD i = 0; i < num_names; ++i) {
        auto name_opt = rpm_str(proc, base + names[i]);
        if (!name_opt) continue;

        const WORD ord = ordinals[i];
        if (ord >= num_funcs) continue;

        const DWORD   func_rva = functions[ord];
        if (func_rva == 0) continue;

        const uintptr_t func_va = base + func_rva;

        // A forwarder entry's RVA lands inside the export directory itself;
        // resolving it requires following the forwarded name, which is left
        // to the caller or a higher-level layer.
        if (func_va >= exp_va && func_va < exp_end) continue;

        result[*name_opt] = func_va;
    }

    return result;
}

// ---------------------------------------------------------------------------
// Symbol resolution
// ---------------------------------------------------------------------------

struct ResolvedSymbol {
    std::string name;
    uintptr_t   address = 0;  // 0 means not found / forwarded / ordinal-only
};

// Resolves each name in symbols against the export table of the module at base
// inside proc. Missing or forwarded symbols get address == 0.
static std::vector<ResolvedSymbol>
resolve_symbols(HANDLE proc, uintptr_t base,
                const std::vector<std::string>& symbols) {
    const auto exports = parse_exports(proc, base);

    std::vector<ResolvedSymbol> out;
    out.reserve(symbols.size());

    for (const auto& sym : symbols) {
        const auto it = exports.find(sym);
        out.push_back({ sym, it != exports.end() ? it->second : 0u });
    }
    return out;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    if (argc < 4) {
        fprintf(stderr,
            "Usage: %s <pid> <module.dll> <symbol1> [symbol2 ...]\n",
            argv[0]);
        return 1;
    }

    const DWORD       pid         = static_cast<DWORD>(strtoul(argv[1], nullptr, 10));
    const std::string module_name = argv[2];

    std::vector<std::string> symbols;
    symbols.reserve(static_cast<size_t>(argc - 3));
    for (int i = 3; i < argc; ++i)
        symbols.push_back(argv[i]);

    // Open target with the minimum access rights needed:
    //   PROCESS_VM_READ           – ReadProcessMemory
    //   PROCESS_QUERY_INFORMATION – needed by some snapshot paths on older Windows
    UniqueHandle proc(OpenProcess(
        PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid));
    if (!proc.valid()) {
        fprintf(stderr, "OpenProcess(%lu) failed: error %lu\n",
                pid, GetLastError());
        return 1;
    }

    const uintptr_t base = find_module_base(pid, module_name);
    if (!base) {
        fprintf(stderr, "Module '%s' not found in process %lu.\n",
                module_name.c_str(), pid);
        return 1;
    }

    printf("Module : %s\n", module_name.c_str());
    printf("Base   : 0x%016llX\n\n",
           static_cast<unsigned long long>(base));

    const auto resolved = resolve_symbols(proc, base, symbols);

    printf("%-48s %s\n", "Symbol", "Address");
    printf("%-48s %s\n",
           "------------------------------------------------",
           "--------------------");

    int missing = 0;
    for (const auto& r : resolved) {
        if (r.address)
            printf("%-48s 0x%016llX\n",
                   r.name.c_str(),
                   static_cast<unsigned long long>(r.address));
        else {
            printf("%-48s (not found)\n", r.name.c_str());
            ++missing;
        }
    }

    return missing ? 2 : 0;
}
