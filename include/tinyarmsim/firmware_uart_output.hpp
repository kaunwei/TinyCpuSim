#pragma once

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <csignal>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <pthread.h>
#include <stdexcept>
#include <system_error>
#include <unistd.h>

namespace tinyarmsim {

class FirmwareUartOutput {
public:
    static constexpr std::size_t kCapacity = 65536;
    static constexpr std::size_t kMaxServiceSyscalls = 8;
    static constexpr std::size_t kMaxServiceBytes = 4096;

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
            if (!fatal_failure_) {
                failure_code_ = std::make_error_code(std::errc::no_buffer_space);
            }
            return false;
        }
        ring_[(head_ + size_) % ring_.size()] = byte;
        ++size_;
        return true;
    }

    [[nodiscard]] std::size_t pending_bytes() const noexcept { return size_; }
    [[nodiscard]] bool failed() const noexcept { return failed_; }

    // Write a bounded amount of queued output without waiting for descriptor space.
    std::size_t service() {
        if (failed_) {
            throw std::system_error(std::make_error_code(std::errc::no_buffer_space),
                                    "firmware UART output overflow");
        }

        std::size_t total_written = 0;
        for (std::size_t attempt = 0;
             attempt < kMaxServiceSyscalls && total_written < kMaxServiceBytes && size_ != 0;
             ++attempt) {
            try {
                total_written += write_pending(kMaxServiceBytes - total_written);
            } catch (const std::system_error& error) {
                if (error.code().value() == EINTR) continue;
                if (error.code().value() == EAGAIN || error.code().value() == EWOULDBLOCK) {
                    return total_written;
                }
                throw;
            }
        }
        return total_written;
    }

    // Drain all pending bytes before one absolute deadline. A timeout leaves
    // unwritten bytes queued and is reported as std::errc::timed_out.
    void flush(std::chrono::milliseconds timeout) {
        using Clock = std::chrono::steady_clock;
        const auto started = Clock::now();
        const auto available = Clock::time_point::max() - started;
        const auto max_timeout = std::chrono::duration_cast<std::chrono::milliseconds>(available);
        const auto deadline = timeout <= std::chrono::milliseconds::zero()
                                  ? started
                                  : timeout >= max_timeout
                                        ? Clock::time_point::max()
                                        : started + std::chrono::duration_cast<Clock::duration>(timeout);

        if (fatal_failure_) {
            throw std::system_error(failure_code_, "flush firmware UART output");
        }

        while (size_ > 0) {
            if (Clock::now() >= deadline) {
                throw std::system_error(std::make_error_code(std::errc::timed_out),
                                        "flush firmware UART output");
            }

            try {
                const std::size_t written = write_pending();
                if (written == 0) {
                    latch_fatal_error(std::make_error_code(std::errc::io_error));
                }
            } catch (const std::system_error& error) {
                const int code = error.code().value();
                if (code == EINTR) {
                    continue;
                }
                if (code != EAGAIN && code != EWOULDBLOCK) {
                    latch_fatal_error(error.code());
                }

                const auto now = Clock::now();
                if (now >= deadline) {
                    throw std::system_error(std::make_error_code(std::errc::timed_out),
                                            "flush firmware UART output");
                }
                const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
                const auto capped = remaining.count() > std::numeric_limits<int>::max()
                                        ? std::numeric_limits<int>::max()
                                        : static_cast<int>(remaining.count());
                pollfd descriptor{owned_fd_, POLLOUT, 0};
                const int ready = ::poll(&descriptor, 1, capped);
                if (ready == -1) {
                    if (errno == EINTR) continue;
                    latch_fatal_error(std::error_code(errno, std::generic_category()));
                }
                if (ready > 0 && (descriptor.revents & POLLNVAL) != 0) {
                    latch_fatal_error(std::make_error_code(std::errc::bad_file_descriptor));
                }
            }
        }

        if (failed_) {
            throw std::system_error(failure_code_, "flush firmware UART output");
        }
    }

    // Make one bounded, nonblocking write attempt. Bytes remain queued on error.
    std::size_t write_pending(std::size_t byte_limit = kCapacity) {
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
            const std::size_t available = size_ < contiguous ? size_ : contiguous;
            const std::size_t count = available < byte_limit ? available : byte_limit;
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
    [[noreturn]] void latch_fatal_error(const std::error_code& error) {
        failed_ = true;
        fatal_failure_ = true;
        failure_code_ = error;
        throw std::system_error(error, "flush firmware UART output");
    }

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
    bool fatal_failure_{false};
    std::error_code failure_code_{};
};

} // namespace tinyarmsim
