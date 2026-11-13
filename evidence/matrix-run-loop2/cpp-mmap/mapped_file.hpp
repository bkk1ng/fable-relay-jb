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

class mapped_file {
public:
    // Opens and maps the file at `path` read-only.
    // Throws std::runtime_error on any OS error.
    // Empty files are valid: data() returns nullptr, size() returns 0.
    explicit mapped_file(const std::string& path)
        : fd_(-1), addr_(nullptr), size_(0)
    {
        fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd_ == -1)
            throw std::runtime_error("mapped_file: open failed: " + path);

        struct stat st{};
        if (::fstat(fd_, &st) == -1) {
            ::close(fd_);
            throw std::runtime_error("mapped_file: fstat failed: " + path);
        }

        size_ = static_cast<std::size_t>(st.st_size);

        if (size_ > 0) {
            void* p = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
            if (p == MAP_FAILED) {
                ::close(fd_);
                throw std::runtime_error("mapped_file: mmap failed: " + path);
            }
            addr_ = p;
        }
        // size_ == 0: leave addr_ as nullptr, nothing to map.
    }

    ~mapped_file() { reset(); }

    // Non-copyable
    mapped_file(const mapped_file&)            = delete;
    mapped_file& operator=(const mapped_file&) = delete;

    // Movable
    mapped_file(mapped_file&& other) noexcept
        : fd_(other.fd_), addr_(other.addr_), size_(other.size_)
    {
        other.fd_   = -1;
        other.addr_ = nullptr;
        other.size_ = 0;
    }

    mapped_file& operator=(mapped_file&& other) noexcept {
        if (this != &other) {
            reset();
            fd_         = other.fd_;
            addr_       = other.addr_;
            size_       = other.size_;
            other.fd_   = -1;
            other.addr_ = nullptr;
            other.size_ = 0;
        }
        return *this;
    }

    // Raw byte span over the mapped region.
    // Returns an empty span for empty files.
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return { static_cast<const std::byte*>(addr_), size_ };
    }

    // Reinterpret the mapped region as a span of T.
    // Requires:
    //   - T is trivially copyable
    //   - the mapping address is suitably aligned for T
    //   - the mapping size is an exact multiple of sizeof(T)
    // Throws std::runtime_error when any constraint is violated.
    template<typename T>
    [[nodiscard]] std::span<const T> view() const {
        static_assert(std::is_trivially_copyable_v<T>,
                      "mapped_file::view<T>: T must be trivially copyable");

        if (size_ == 0)
            return {};

        // Alignment check
        const auto addr_uint = reinterpret_cast<std::uintptr_t>(addr_);
        if (addr_uint % alignof(T) != 0)
            throw std::runtime_error(
                "mapped_file::view: mapping address is not aligned for T");

        // Size divisibility check
        if (size_ % sizeof(T) != 0)
            throw std::runtime_error(
                "mapped_file::view: mapping size is not a multiple of sizeof(T)");

        return { static_cast<const T*>(addr_), size_ / sizeof(T) };
    }

    [[nodiscard]] const void* data() const noexcept { return addr_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool        empty() const noexcept { return size_ == 0; }

private:
    void reset() noexcept {
        if (addr_)  { ::munmap(addr_, size_); addr_ = nullptr; }
        if (fd_!=-1){ ::close(fd_);           fd_   = -1;      }
        size_ = 0;
    }

    int         fd_;
    void*       addr_;
    std::size_t size_;
};
