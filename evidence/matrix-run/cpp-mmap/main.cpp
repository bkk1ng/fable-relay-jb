// main.cpp — prints size and first bytes of the file given in argv[1].
// C++17, POSIX (Linux).
//
// Build:  g++ -std=c++17 -O2 -Wall -Wextra -o mapped main.cpp
// Run:    ./mapped <file>

#include "mapped_file.hpp"

#include <cstdio>
#include <exception>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <file>\n", argv[0]);
        return 1;
    }

    try {
        mapped_file f(argv[1]);

        std::printf("size: %zu bytes\n", f.size());

        auto b   = f.bytes();
        std::size_t show = b.size() < 16 ? b.size() : 16;

        std::printf("first %zu bytes:", show);
        for (std::size_t i = 0; i < show; ++i)
            std::printf(" %02x", static_cast<unsigned>(b[i]));
        std::printf("\n");

    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }

    return 0;
}
