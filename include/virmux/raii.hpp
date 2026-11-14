#pragma once

#include <unistd.h>
#include <utility>

namespace virmux {

/**
 * @brief RAII wrapper for POSIX file descriptors.
 *
 * Automatically closes the managed file descriptor upon destruction.
 * Implements strict move-only semantics (Rule of Five).
 */
class ScopedFd {
public:
    /**
     * @brief Construct an empty ScopedFd holding no file descriptor (-1).
     */
    constexpr ScopedFd() noexcept : fd_(-1) {}

    /**
     * @brief Construct a ScopedFd managing the specified file descriptor.
     * @param fd File descriptor to manage.
     */
    explicit ScopedFd(int fd) noexcept : fd_(fd) {}

    /**
     * @brief Destroy the ScopedFd, closing the managed file descriptor if valid.
     */
    ~ScopedFd() noexcept {
        reset();
    }

    // Non-copyable (Rule of Five)
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;

    // Movable (Rule of Five)
    ScopedFd(ScopedFd&& other) noexcept : fd_(other.release()) {}

    ScopedFd& operator=(ScopedFd&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    /**
     * @brief Check if this ScopedFd holds a valid non-negative file descriptor.
     * @return true if valid (>= 0), false otherwise.
     */
    [[nodiscard]] bool valid() const noexcept {
        return fd_ >= 0;
    }

    /**
     * @brief Boolean conversion operator equivalent to valid().
     */
    [[nodiscard]] explicit operator bool() const noexcept {
        return valid();
    }

    /**
     * @brief Get the underlying raw file descriptor without transferring ownership.
     * @return Underlying descriptor or -1 if empty.
     */
    [[nodiscard]] int get() const noexcept {
        return fd_;
    }

    /**
     * @brief Release ownership of the managed descriptor.
     * @return Previous descriptor value (caller takes ownership of closing it).
     */
    int release() noexcept {
        int old = fd_;
        fd_ = -1;
        return old;
    }

    /**
     * @brief Close current descriptor (if valid) and take ownership of new_fd.
     * @param new_fd Replacement descriptor, defaults to -1.
     */
    void reset(int new_fd = -1) noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = new_fd;
    }

private:
    int fd_{-1};
};

} // namespace virmux
