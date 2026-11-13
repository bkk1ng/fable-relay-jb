#ifndef MAPPED_FILE_HPP
#define MAPPED_FILE_HPP

#include <span>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// RAII wrapper around open/fstat/mmap for read-only memory-mapped files.
// Movable but not copyable. Empty files are handled: size() reports 0 and
// bytes()/view<T>() yield empty spans, with no mmap call issued.
class mapped_file {
public:
    mapped_file() = default;

    explicit mapped_file(const char* path) {
        open_file(path);
    }

    explicit mapped_file(std::string_view path)
        : mapped_file(std::string(path).c_str()) {}

    ~mapped_file() {
        release();
    }

    mapped_file(const mapped_file&) = delete;
    mapped_file& operator=(const mapped_file&) = delete;

    mapped_file(mapped_file&& other) noexcept
        : fd_(other.fd_), data_(other.data_), size_(other.size_)
    {
        other.fd_ = -1;
        other.data_ = nullptr;
        other.size_ = 0;
    }

    mapped_file& operator=(mapped_file&& other) noexcept {
        if (this != &other) {
            release();
            fd_ = other.fd_;
            data_ = other.data_;
            size_ = other.size_;
            other.fd_ = -1;
            other.data_ = nullptr;
            other.size_ = 0;
        }
        return *this;
    }

    // Raw byte view of the mapping.
    std::span<const std::byte> bytes() const noexcept {
        return {static_cast<const std::byte*>(data_), size_};
    }

    std::size_t size() const noexcept {
        return size_;
    }

    explicit operator bool() const noexcept {
        return fd_ >= 0;
    }

    // Typed view with alignment and size-divisibility checks.
    // T must be trivially copyable. Throws if the mapping's address is not
    // aligned to alignof(T) or if the byte count is not a multiple of sizeof(T).
    template<typename T>
    std::span<const T> view() const {
        static_assert(std::is_trivially_copyable_v<T>,
                      "mapped_file::view: T must be trivially copyable");

        if (size_ == 0)
            return {};

        auto addr = reinterpret_cast<std::uintptr_t>(data_);
        if (addr % alignof(T) != 0)
            throw std::runtime_error(
                "mapped_file::view: mapping is not aligned for the requested type");

        if (size_ % sizeof(T) != 0)
            throw std::runtime_error(
                "mapped_file::view: size is not a multiple of sizeof(T)");

        return {static_cast<const T*>(data_), size_ / sizeof(T)};
    }

private:
    void open_file(const char* path) {
        fd_ = ::open(path, O_RDONLY);
        if (fd_ < 0)
            throw std::system_error(errno, std::system_category(),
                                    "mapped_file: open failed");

        struct stat st;
        if (::fstat(fd_, &st) < 0) {
            ::close(fd_);
            fd_ = -1;
            throw std::system_error(errno, std::system_category(),
                                    "mapped_file: fstat failed");
        }

        size_ = static_cast<std::size_t>(st.st_size);

        // mmap with length 0 is invalid on Linux; skip it for empty files.
        if (size_ > 0) {
            void* p = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
            if (p == MAP_FAILED) {
                ::close(fd_);
                fd_ = -1;
                size_ = 0;
                throw std::system_error(errno, std::system_category(),
                                        "mapped_file: mmap failed");
            }
            data_ = p;
        }
    }

    void release() noexcept {
        if (data_) {
            ::munmap(data_, size_);
            data_ = nullptr;
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        size_ = 0;
    }

    int fd_ = -1;
    void* data_ = nullptr;
    std::size_t size_ = 0;
};

#endif // MAPPED_FILE_HPP
