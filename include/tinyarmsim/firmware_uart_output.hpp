#pragma once

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <csignal>
#include <fcntl.h>
#include <pthread.h>
#include <stdexcept>
#include <system_error>
#include <unistd.h>

namespace tinyarmsim {

class FirmwareUartOutput {
public:
    static constexpr std::size_t kCapacity = 65536;

    explicit FirmwareUartOutput(int output_fd = STDOUT_FILENO) {
        owned_fd_ = ::dup(output_fd);
        if (owned_fd_ == -1) {
            throw std::runtime_error("failed to duplicate firmware UART output descriptor");
        }

        original_flags_ = ::fcntl(owned_fd_, F_GETFL);
        if (original_flags_ == -1) {
            close_owned_fd();
            throw std::runtime_error("failed to read firmware UART output descriptor flags");
        }

        if (::fcntl(owned_fd_, F_SETFL, original_flags_ | O_NONBLOCK) == -1) {
            close_owned_fd();
            throw std::runtime_error("failed to set firmware UART output nonblocking");
        }
        flags_changed_ = true;
    }

    FirmwareUartOutput(const FirmwareUartOutput&) = delete;
    FirmwareUartOutput& operator=(const FirmwareUartOutput&) = delete;
    FirmwareUartOutput(FirmwareUartOutput&&) = delete;
    FirmwareUartOutput& operator=(FirmwareUartOutput&&) = delete;

    ~FirmwareUartOutput() noexcept {
        if (owned_fd_ != -1) {
            if (flags_changed_) {
                (void)::fcntl(owned_fd_, F_SETFL, original_flags_);
            }
            close_owned_fd();
        }
    }

    [[nodiscard]] bool enqueue(uint8_t byte) noexcept {
        if (size_ == ring_.size()) {
            failed_ = true;
            return false;
        }
        ring_[(head_ + size_) % ring_.size()] = byte;
        ++size_;
        return true;
    }

    [[nodiscard]] std::size_t pending_bytes() const noexcept { return size_; }
    [[nodiscard]] bool failed() const noexcept { return failed_; }

    // Make one bounded, nonblocking write attempt. Bytes remain queued on error.
    std::size_t write_pending() {
        if (size_ == 0) return 0;

        sigset_t pipe_set;
        sigemptyset(&pipe_set);
        sigaddset(&pipe_set, SIGPIPE);

        sigset_t original_mask;
        const int block_error = ::pthread_sigmask(SIG_BLOCK, &pipe_set, &original_mask);
        if (block_error != 0) {
            throw std::system_error(block_error, std::generic_category(), "block SIGPIPE");
        }

        ssize_t written = -1;
        int write_error = 0;
        try {
            sigset_t pending_before;
            if (::sigpending(&pending_before) != 0) {
                throw std::system_error(errno, std::generic_category(), "inspect pending SIGPIPE");
            }
            const bool was_pending = ::sigismember(&pending_before, SIGPIPE) == 1;

            const std::size_t contiguous = ring_.size() - head_;
            const std::size_t count = size_ < contiguous ? size_ : contiguous;
            written = ::write(owned_fd_, ring_.data() + head_, count);
            write_error = written == -1 ? errno : 0;

            if (written == -1 && write_error == EPIPE && !was_pending) {
                sigset_t pending_after;
                if (::sigpending(&pending_after) != 0) {
                    throw std::system_error(errno, std::generic_category(), "inspect generated SIGPIPE");
                }
                if (::sigismember(&pending_after, SIGPIPE) == 1) {
                    timespec no_wait{};
                    bool consumed = false;
                    for (int attempt = 0; attempt < 3; ++attempt) {
                        const int wait_result = ::sigtimedwait(&pipe_set, nullptr, &no_wait);
                        if (wait_result == SIGPIPE) {
                            consumed = true;
                            break;
                        }
                        if (wait_result == -1 && errno == EINTR) continue;
                        const int error = wait_result == -1 ? errno : EIO;
                        throw std::system_error(error, std::generic_category(), "consume generated SIGPIPE");
                    }
                    if (!consumed) {
                        throw std::system_error(EINTR, std::generic_category(), "consume generated SIGPIPE");
                    }
                }
            }
        } catch (...) {
            (void)::pthread_sigmask(SIG_SETMASK, &original_mask, nullptr);
            throw;
        }

        const int restore_error = ::pthread_sigmask(SIG_SETMASK, &original_mask, nullptr);
        if (restore_error != 0) {
            throw std::system_error(restore_error, std::generic_category(), "restore signal mask");
        }
        if (written == -1) {
            throw std::system_error(write_error, std::generic_category(), "write firmware UART output");
        }
        head_ = (head_ + static_cast<std::size_t>(written)) % ring_.size();
        size_ -= static_cast<std::size_t>(written);
        return static_cast<std::size_t>(written);
    }

private:
    void close_owned_fd() noexcept {
        if (owned_fd_ != -1) {
            (void)::close(owned_fd_);
            owned_fd_ = -1;
        }
    }

    int owned_fd_{-1};
    int original_flags_{-1};
    bool flags_changed_{false};
    std::array<uint8_t, kCapacity> ring_{};
    std::size_t head_{0};
    std::size_t size_{0};
    bool failed_{false};
};

} // namespace tinyarmsim
