#pragma once

// RAII memory-mapped read-only file — C++17, POSIX (Linux)
// Movable, non-copyable. Safe for empty files.

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

class MappedFile {
public:
    // Open and map the file at `path`. Throws std::runtime_error on failure.
    // Empty files are accepted; they produce a zero-length mapping.
    explicit MappedFile(std::string_view path) {
        std::string p(path);                       // need NUL-terminated copy

        fd_ = ::open(p.c_str(), O_RDONLY);
        if (fd_ == -1)
            throw std::runtime_error("MappedFile: open failed: " + p);

        struct stat st{};
        if (::fstat(fd_, &st) == -1) {
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error("MappedFile: fstat failed: " + p);
        }

        size_ = static_cast<std::size_t>(st.st_size);

        // mmap(2) with length 0 is EINVAL on Linux — skip for empty files.
        if (size_ == 0)
            return;

        addr_ = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
        if (addr_ == MAP_FAILED) {
            addr_ = nullptr;
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error("MappedFile: mmap failed: " + p);
        }
    }

    ~MappedFile() { release(); }

    // Non-copyable
    MappedFile(const MappedFile&)            = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    // Movable — leaves source in a valid, empty state
    MappedFile(MappedFile&& o) noexcept
        : addr_(o.addr_), size_(o.size_), fd_(o.fd_)
    {
        o.addr_ = nullptr;
        o.size_ = 0;
        o.fd_   = -1;
    }

    MappedFile& operator=(MappedFile&& o) noexcept {
        if (this != &o) {
            release();
            addr_ = o.addr_; size_ = o.size_; fd_ = o.fd_;
            o.addr_ = nullptr; o.size_ = 0; o.fd_ = -1;
        }
        return *this;
    }

    // Raw byte view of the entire mapping.
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return { static_cast<const std::byte*>(addr_), size_ };
    }

    [[nodiscard]] std::size_t size()  const noexcept { return size_; }
    [[nodiscard]] bool        empty() const noexcept { return size_ == 0; }

    // Reinterpret the mapping as a sequence of T.
    //
    // Requires:
    //   - T is trivially copyable
    //   - file size is an exact multiple of sizeof(T)
    //   - mapping address is suitably aligned for T
    //     (mmap address is page-aligned, so this only fails for over-aligned T)
    //
    // Returns an empty span for empty files without throwing.
    template <typename T>
    [[nodiscard]] std::span<const T> view() const {
        static_assert(std::is_trivially_copyable_v<T>,
                      "MappedFile::view<T>: T must be trivially copyable");

        if (size_ % sizeof(T) != 0)
            throw std::runtime_error(
                "MappedFile::view<T>: file size is not a multiple of sizeof(T)");

        if (addr_ != nullptr &&
            reinterpret_cast<std::uintptr_t>(addr_) % alignof(T) != 0)
            throw std::runtime_error(
                "MappedFile::view<T>: mapping address misaligned for T");

        return { static_cast<const T*>(addr_), size_ / sizeof(T) };
    }

private:
    void* addr_     = nullptr;
    std::size_t size_ = 0;
    int   fd_       = -1;

    void release() noexcept {
        if (addr_ != nullptr) { ::munmap(addr_, size_); addr_ = nullptr; }
        if (fd_   != -1)      { ::close(fd_);           fd_   = -1;     }
        size_ = 0;
    }
};
