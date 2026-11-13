#pragma once

// mapped_file.hpp — RAII memory-mapped file reader
// C++17, POSIX (Linux). Uses open/fstat/mmap.
// Movable, non-copyable. Handles empty files (no mapping is created).
// Exposes a span of raw bytes and a typed view<T> with alignment + size checks.

#include <cstddef>      // std::byte, std::size_t, std::uintptr_t
#include <cstdint>       // std::uintptr_t (also via cstddef, kept for clarity)
#include <stdexcept>     // std::runtime_error
#include <string>        // std::string (error messages)

// POSIX
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// span — minimal C++17 stand-in for std::span (C++20)
// ---------------------------------------------------------------------------
template <typename T>
class span {
public:
    span() noexcept = default;
    span(T* data, std::size_t size) noexcept : data_(data), size_(size) {}

    T*  data()  const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }

    T* begin() const noexcept { return data_; }
    T* end()   const noexcept { return data_ + size_; }

    T& operator[](std::size_t i) const noexcept { return data_[i]; }

private:
    T*          data_ = nullptr;
    std::size_t size_ = 0;
};

// ---------------------------------------------------------------------------
// mapped_file — RAII wrapper around open/fstat/mmap
// ---------------------------------------------------------------------------
class mapped_file {
public:
    mapped_file() = default;

    explicit mapped_file(const char* path) {
        open(path);
    }

    explicit mapped_file(const std::string& path)
        : mapped_file(path.c_str()) {}

    // non-copyable
    mapped_file(const mapped_file&)            = delete;
    mapped_file& operator=(const mapped_file&) = delete;

    // movable
    mapped_file(mapped_file&& other) noexcept
        : data_(other.data_), size_(other.size_), fd_(other.fd_) {
        other.data_ = nullptr;
        other.size_ = 0;
        other.fd_   = -1;
    }

    mapped_file& operator=(mapped_file&& other) noexcept {
        if (this != &other) {
            close();
            data_ = other.data_;
            size_ = other.size_;
            fd_   = other.fd_;
            other.data_ = nullptr;
            other.size_ = 0;
            other.fd_   = -1;
        }
        return *this;
    }

    ~mapped_file() {
        close();
    }

    // --- accessors ----------------------------------------------------------

    /// Raw byte view of the entire mapping. Empty span for empty files.
    span<const std::byte> bytes() const noexcept {
        return {static_cast<const std::byte*>(data_), size_};
    }

    std::size_t   size()   const noexcept { return size_; }
    const void*   data()   const noexcept { return data_; }
    bool          is_open() const noexcept { return fd_ != -1; }

    /// Typed view. Throws if the file size is not a whole multiple of sizeof(T)
    /// or if the mapped address is not suitably aligned for T.
    template <typename T>
    span<const T> view() const {
        if (size_ % sizeof(T) != 0)
            throw std::runtime_error(
                "mapped_file::view: file size is not a multiple of " +
                std::to_string(sizeof(T)));
        if (reinterpret_cast<std::uintptr_t>(data_) % alignof(T) != 0)
            throw std::runtime_error(
                "mapped_file::view: mapped address is not aligned for the requested type");
        return {static_cast<const T*>(data_), size_ / sizeof(T)};
    }

private:
    // --- internals ---------------------------------------------------------

    void open(const char* path) {
        fd_ = ::open(path, O_RDONLY);
        if (fd_ == -1)
            throw std::runtime_error("mapped_file: open(\"" +
                                      std::string(path) + "\") failed");

        struct stat st;
        if (fstat(fd_, &st) == -1) {
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error("mapped_file: fstat failed");
        }

        size_ = static_cast<std::size_t>(st.st_size);

        // mmap with length 0 is invalid (returns MAP_FAILED, errno == EINVAL).
        // An empty file is a valid state: keep fd open, data_ stays null.
        if (size_ == 0) {
            data_ = nullptr;
            return;
        }

        void* ptr = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
        if (ptr == MAP_FAILED) {
            ::close(fd_);
            fd_   = -1;
            size_ = 0;
            throw std::runtime_error("mapped_file: mmap failed");
        }

        data_ = ptr;
    }

    void close() noexcept {
        if (data_) {
            munmap(data_, size_);
            data_ = nullptr;
        }
        if (fd_ != -1) {
            ::close(fd_);
            fd_ = -1;
        }
        size_ = 0;
    }

    void*        data_ = nullptr;
    std::size_t  size_ = 0;
    int          fd_   = -1;
};
