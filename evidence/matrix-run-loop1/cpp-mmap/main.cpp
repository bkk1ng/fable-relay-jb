#include "mapped_file.hpp"

#include <cstdio>
#include <exception>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <file>\n", argv[0]);
        return 1;
    }

    try {
        mapped_file mf(argv[1]);

        auto data = mf.bytes();
        std::printf("size: %zu bytes\n", data.size());

        // Dump up to 32 leading bytes as hex.
        constexpr std::size_t limit = 32;
        const std::size_t n = data.size() < limit ? data.size() : limit;

        std::printf("first %zu bytes:", n);
        for (std::size_t i = 0; i < n; ++i)
            std::printf(" %02x",
                        static_cast<unsigned>(
                            static_cast<unsigned char>(data[i])));
        std::printf("\n");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
