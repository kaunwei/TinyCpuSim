#pragma once

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <stdexcept>
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
