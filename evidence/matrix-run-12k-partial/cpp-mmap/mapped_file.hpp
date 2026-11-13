#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

class MappedFile {
public:
    // Opens and memory-maps the file at `path` for reading.
    // Throws std::runtime_error on any OS error.
    // Empty files are valid: data() returns nullptr, size() returns 0.
    explicit MappedFile(const std::string& path)
        : addr_(nullptr), size_(0)
    {
        int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd == -1)
            throw std::runtime_error("MappedFile: open failed: " + path);

        struct stat st{};
        if (::fstat(fd, &st) == -1) {
            ::close(fd);
            throw std::runtime_error("MappedFile: fstat failed: " + path);
        }

        size_ = static_cast<std::size_t>(st.st_size);

        if (size_ > 0) {
            void* p = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
            ::close(fd);
            if (p == MAP_FAILED)
                throw std::runtime_error("MappedFile: mmap failed: " + path);
            addr_ = static_cast<const std::byte*>(p);
        } else {
            ::close(fd);
        }
    }

    ~MappedFile() {
        if (addr_)
            ::munmap(const_cast<void*>(static_cast<const void*>(addr_)), size_);
    }

    // Non-copyable
    MappedFile(const MappedFile&)            = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    // Movable
    MappedFile(MappedFile&& other) noexcept
        : addr_(other.addr_), size_(other.size_)
    {
        other.addr_ = nullptr;
        other.size_ = 0;
    }

    MappedFile& operator=(MappedFile&& other) noexcept {
        if (this != &other) {
            if (addr_)
                ::munmap(const_cast<void*>(static_cast<const void*>(addr_)), size_);
            addr_       = other.addr_;
            size_       = other.size_;
            other.addr_ = nullptr;
            other.size_ = 0;
        }
        return *this;
    }

    // Raw byte span over the mapped region.
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return { addr_, size_ };
    }

    [[nodiscard]] const std::byte* data() const noexcept { return addr_; }
    [[nodiscard]] std::size_t      size() const noexcept { return size_; }
    [[nodiscard]] bool             empty() const noexcept { return size_ == 0; }

    // Reinterpret the mapped region as a span of T.
    //
    // Requirements:
    //   - T must be trivially copyable
    //   - the mapping address must satisfy alignof(T)
    //   - the byte size must be a multiple of sizeof(T)
    //
    // Throws std::runtime_error when either alignment or size constraint fails.
    template <typename T>
    [[nodiscard]] std::span<const T> view() const {
        static_assert(std::is_trivially_copyable_v<T>,
                      "MappedFile::view<T>: T must be trivially copyable");

        if (size_ % sizeof(T) != 0)
            throw std::runtime_error(
                "MappedFile::view<T>: byte size is not a multiple of sizeof(T)");

        if (addr_ && (reinterpret_cast<std::uintptr_t>(addr_) % alignof(T)) != 0)
            throw std::runtime_error(
                "MappedFile::view<T>: mapping is not sufficiently aligned for T");

        return { reinterpret_cast<const T*>(addr_), size_ / sizeof(T) };
    }

private:
    const std::byte* addr_;
    std::size_t      size_;
};
