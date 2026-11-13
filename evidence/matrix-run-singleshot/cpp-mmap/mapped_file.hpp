#pragma once

// RAII memory-mapped file reader — C++17, POSIX (Linux)
// Movable, non-copyable. Handles empty files.
// Exposes:
//   bytes()     → std::span<const std::byte>
//   view<T>()   → std::span<const T>  (alignment + size checked)

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

class MappedFile {
public:
    // Opens and maps the file at path.
    // Throws std::system_error on open/fstat/mmap failure.
    explicit MappedFile(const std::string& path) {
        fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd_ == -1) {
            throw std::system_error(errno, std::generic_category(),
                                    "open: " + path);
        }

        struct stat st{};
        if (::fstat(fd_, &st) == -1) {
            ::close(fd_);
            fd_ = -1;
            throw std::system_error(errno, std::generic_category(),
                                    "fstat: " + path);
        }

        size_ = static_cast<std::size_t>(st.st_size);

        // mmap(2) with length 0 is undefined; skip the call for empty files.
        if (size_ > 0) {
            void* ptr = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
            if (ptr == MAP_FAILED) {
                ::close(fd_);
                fd_ = -1;
                size_ = 0;
                throw std::system_error(errno, std::generic_category(),
                                        "mmap: " + path);
            }
            data_ = ptr;
        }
    }

    ~MappedFile() { reset(); }

    // Move constructor — transfers ownership.
    MappedFile(MappedFile&& other) noexcept
        : data_(other.data_), size_(other.size_), fd_(other.fd_) {
        other.data_ = nullptr;
        other.size_ = 0;
        other.fd_   = -1;
    }

    // Move assignment — releases current mapping, then takes ownership.
    MappedFile& operator=(MappedFile&& other) noexcept {
        if (this != &other) {
            reset();
            data_ = other.data_;
            size_ = other.size_;
            fd_   = other.fd_;
            other.data_ = nullptr;
            other.size_ = 0;
            other.fd_   = -1;
        }
        return *this;
    }

    MappedFile(const MappedFile&)            = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    [[nodiscard]] std::size_t size()  const noexcept { return size_; }
    [[nodiscard]] bool        empty() const noexcept { return size_ == 0; }

    // Raw byte view of the entire mapping.
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return { static_cast<const std::byte*>(data_), size_ };
    }

    // Reinterpret the mapping as a sequence of T.
    // Requires:
    //   - T is trivially copyable
    //   - size() is a multiple of sizeof(T)
    //   - the mapping address satisfies alignof(T)
    // Throws std::runtime_error when either runtime condition fails.
    template <typename T>
    [[nodiscard]] std::span<const T> view() const {
        static_assert(std::is_trivially_copyable_v<T>,
                      "MappedFile::view<T>: T must be trivially copyable");

        if (size_ % sizeof(T) != 0) {
            throw std::runtime_error(
                "MappedFile::view<T>: file size is not a multiple of sizeof(T)");
        }

        if (data_ != nullptr) {
            auto addr = reinterpret_cast<std::uintptr_t>(data_);
            if (addr % alignof(T) != 0) {
                throw std::runtime_error(
                    "MappedFile::view<T>: mapping address does not satisfy alignof(T)");
            }
        }

        // Empty file → empty span, no UB.
        return { static_cast<const T*>(data_), size_ / sizeof(T) };
    }

private:
    void* data_      = nullptr;
    std::size_t size_ = 0;
    int fd_           = -1;

    void reset() noexcept {
        if (data_ != nullptr) {
            ::munmap(data_, size_);
            data_ = nullptr;
        }
        if (fd_ != -1) {
            ::close(fd_);
            fd_ = -1;
        }
        size_ = 0;
    }
};
