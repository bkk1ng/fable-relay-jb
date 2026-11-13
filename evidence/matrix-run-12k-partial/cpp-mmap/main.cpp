#include "mapped_file.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <file>\n", argv[0]);
        return EXIT_FAILURE;
    }

    try {
        MappedFile f(argv[1]);

        std::printf("file : %s\n", argv[1]);
        std::printf("size : %zu byte%s\n", f.size(), f.size() == 1 ? "" : "s");

        if (f.empty()) {
            std::printf("(empty file)\n");
            return EXIT_SUCCESS;
        }

        // Print up to the first 32 bytes as hex + printable ASCII
        const std::size_t preview = std::min(f.size(), std::size_t{32});
        const auto        span    = f.bytes();

        std::printf("first %zu byte%s:\n  hex :", preview, preview == 1 ? "" : "s");
        for (std::size_t i = 0; i < preview; ++i)
            std::printf(" %02x", static_cast<unsigned char>(span[i]));

        std::printf("\n  str : ");
        for (std::size_t i = 0; i < preview; ++i) {
            auto c = static_cast<unsigned char>(span[i]);
            std::putchar(c >= 0x20 && c < 0x7f ? static_cast<char>(c) : '.');
        }
        std::putchar('\n');

    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
