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
        mapped_file f(argv[1]);

        std::printf("file : %s\n", argv[1]);
        std::printf("size : %zu bytes\n", f.size());

        if (f.empty()) {
            std::printf("(empty file)\n");
            return EXIT_SUCCESS;
        }

        // Print up to the first 32 bytes as hex + printable ASCII
        const auto bytes = f.bytes();
        const std::size_t preview = std::min(bytes.size(), std::size_t{32});

        std::printf("first %zu byte(s):\n  hex :", preview);
        for (std::size_t i = 0; i < preview; ++i)
            std::printf(" %02x", static_cast<unsigned char>(bytes[i]));

        std::printf("\n  ascii: ");
        for (std::size_t i = 0; i < preview; ++i) {
            const auto c = static_cast<unsigned char>(bytes[i]);
            std::putchar(c >= 0x20 && c < 0x7f ? c : '.');
        }
        std::putchar('\n');

    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
