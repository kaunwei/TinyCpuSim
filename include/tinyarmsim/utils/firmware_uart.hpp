#pragma once

#include <span>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <array>
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <pty.h>
#include <stdexcept>
#include <string>
#include <system_error>
#include <termios.h>
#include <unistd.h>
#include <utility>
#include "tinyarmsim/dev/uart.hpp"

namespace tinyarmsim {

class IUartTransport {
public:
    virtual ~IUartTransport() = default;
    virtual bool send(std::span<const uint8_t> data) = 0;
    virtual std::size_t receive(std::span<uint8_t> dest) = 0;
    [[nodiscard]] virtual bool connected() const noexcept = 0;
    virtual void close() noexcept = 0;
};

class FdUartTransport : public IUartTransport {
public:
    explicit FdUartTransport(int fd) {
        if (fd < 0) {
            throw std::invalid_argument("invalid file descriptor for FdUartTransport");
        }
        owned_fd_ = ::dup(fd);
        if (owned_fd_ == -1) {
            throw std::runtime_error("failed to duplicate descriptor for FdUartTransport");
        }
        original_flags_ = ::fcntl(owned_fd_, F_GETFL);
        if (original_flags_ == -1) {
            close();
            throw std::runtime_error("failed to get flags for FdUartTransport");
        }
        if (::fcntl(owned_fd_, F_SETFL, original_flags_ | O_NONBLOCK) == -1) {
            close();
            throw std::runtime_error("failed to set nonblocking for FdUartTransport");
        }
        flags_changed_ = true;
    }

    ~FdUartTransport() override {
        close();
    }

    bool send(std::span<const uint8_t> data) override {
        if (owned_fd_ == -1 || data.empty()) return true;
        std::size_t written = 0;
        while (written < data.size()) {
            ssize_t res = ::write(owned_fd_, data.data() + written, data.size() - written);
            if (res > 0) {
                written += static_cast<std::size_t>(res);
            } else if (res < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                if (errno == EIO || errno == EPIPE) break;
                connected_ = false;
                return false;
            } else {
                break;
            }
        }
        return true;
    }

    std::size_t receive(std::span<uint8_t> dest) override {
        if (owned_fd_ == -1 || dest.empty() || !connected_) return 0;
        ssize_t res = ::read(owned_fd_, dest.data(), dest.size());
        if (res > 0) {
            return static_cast<std::size_t>(res);
        }
        if (res == 0) {
            eof_ = true;
            return 0;
        }
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK || errno == EIO) {
            return 0;
        }
        connected_ = false;
        return 0;
    }

    [[nodiscard]] bool connected() const noexcept override {
        return connected_ && owned_fd_ != -1;
    }

    [[nodiscard]] bool eof() const noexcept {
        return eof_;
    }

    void close() noexcept override {
        if (owned_fd_ != -1) {
            if (flags_changed_) {
                (void)::fcntl(owned_fd_, F_SETFL, original_flags_);
                flags_changed_ = false;
            }
            (void)::close(owned_fd_);
            owned_fd_ = -1;
        }
        connected_ = false;
    }

    [[nodiscard]] int fd() const noexcept { return owned_fd_; }

private:
    int owned_fd_{-1};
    int original_flags_{0};
    bool flags_changed_{false};
    bool connected_{true};
    bool eof_{false};
};

class FirmwareUartInput {
public:
    static constexpr std::size_t kCapacity = 4096;
    static constexpr std::size_t kMaxReadSyscalls = 8;
    static constexpr std::size_t kReadChunkSize = 256;

    explicit FirmwareUartInput(int input_fd) {
        if (input_fd < 0) {
            throw std::invalid_argument("invalid firmware UART input descriptor");
        }
        owned_fd_ = ::dup(input_fd);
        if (owned_fd_ == -1) {
            throw std::runtime_error("failed to duplicate firmware UART input descriptor");
        }

        original_flags_ = ::fcntl(owned_fd_, F_GETFL);
        if (original_flags_ == -1) {
            close_owned_fd();
            throw std::runtime_error("failed to read firmware UART input descriptor flags");
        }

        if (::fcntl(owned_fd_, F_SETFL, original_flags_ | O_NONBLOCK) == -1) {
            close_owned_fd();
            throw std::runtime_error("failed to set firmware UART input nonblocking");
        }
        flags_changed_ = true;
    }

    FirmwareUartInput(const FirmwareUartInput&) = delete;
    FirmwareUartInput& operator=(const FirmwareUartInput&) = delete;
    FirmwareUartInput(FirmwareUartInput&&) = delete;
    FirmwareUartInput& operator=(FirmwareUartInput&&) = delete;

    ~FirmwareUartInput() noexcept {
        if (owned_fd_ != -1) {
            if (flags_changed_) {
                (void)::fcntl(owned_fd_, F_SETFL, original_flags_);
            }
            close_owned_fd();
        }
    }

    [[nodiscard]] bool eof_reached() const noexcept { return eof_; }
    [[nodiscard]] std::size_t queued_bytes() const noexcept { return size_; }

    std::size_t poll_ingress() {
        if (eof_ || owned_fd_ == -1) return 0;

        std::size_t total_read = 0;
        for (std::size_t attempt = 0; attempt < kMaxReadSyscalls && size_ < ring_.size(); ++attempt) {
            std::array<uint8_t, kReadChunkSize> buf{};
            const std::size_t max_to_read = std::min(buf.size(), ring_.size() - size_);
            if (max_to_read == 0) break;

            const ssize_t bytes = ::read(owned_fd_, buf.data(), max_to_read);
            if (bytes == 0) {
                eof_ = true;
                break;
            }
            if (bytes < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EIO) {
                    break;
                }
                throw std::system_error(errno, std::generic_category(), "read firmware UART input");
            }

            for (ssize_t i = 0; i < bytes; ++i) {
                ring_[(head_ + size_) % ring_.size()] = buf[static_cast<std::size_t>(i)];
                ++size_;
            }
            total_read += static_cast<std::size_t>(bytes);
        }
        return total_read;
    }

    std::size_t drain_to_guest(mmio::UartDevice& uart) noexcept {
        std::size_t delivered = 0;
        while (size_ > 0) {
            const uint8_t byte = ring_[head_];
            if (!uart.try_inject_rx(byte)) {
                break;
            }
            head_ = (head_ + 1) % ring_.size();
            --size_;
            ++delivered;
        }
        return delivered;
    }

private:
    void close_owned_fd() noexcept {
        if (owned_fd_ != -1) {
            (void)::close(owned_fd_);
            owned_fd_ = -1;
        }
    }

    int owned_fd_{-1};
    int original_flags_{0};
    bool flags_changed_{false};
    bool eof_{false};
    std::size_t head_{0};
    std::size_t size_{0};
    std::array<uint8_t, kCapacity> ring_{};
};

class PtyDevice {
public:
    explicit PtyDevice(std::string pty_file = "") : pty_file_(std::move(pty_file)) {
        char slave_name[256] = {0};
        struct termios tio{};
        cfmakeraw(&tio);
        int slave_fd = -1;
        if (::openpty(&master_fd_, &slave_fd, slave_name, &tio, nullptr) != 0) {
            throw std::system_error(errno, std::generic_category(), "openpty failed");
        }
        slave_path_ = slave_name;

        // Immediately close the initial slave descriptor; external clients connect via slave_path_.
        if (slave_fd != -1) {
            (void)::close(slave_fd);
        }

        int flags = ::fcntl(master_fd_, F_GETFL);
        if (flags == -1) {
            close();
            throw std::system_error(errno, std::generic_category(), "fcntl F_GETFL failed on PTY master");
        }
        if (::fcntl(master_fd_, F_SETFL, flags | O_NONBLOCK) == -1) {
            close();
            throw std::system_error(errno, std::generic_category(), "fcntl F_SETFL failed on PTY master");
        }

        if (!pty_file_.empty()) {
            std::ofstream ofs(pty_file_);
            if (ofs.is_open()) {
                ofs << slave_path_ << "\n";
            }
        }
    }

    ~PtyDevice() noexcept {
        close();
    }

    PtyDevice(const PtyDevice&) = delete;
    PtyDevice& operator=(const PtyDevice&) = delete;

    PtyDevice(PtyDevice&& other) noexcept
        : master_fd_(other.master_fd_),
          slave_path_(std::move(other.slave_path_)),
          pty_file_(std::move(other.pty_file_)) {
        other.master_fd_ = -1;
    }

    PtyDevice& operator=(PtyDevice&& other) noexcept {
        if (this != &other) {
            close();
            master_fd_ = other.master_fd_;
            slave_path_ = std::move(other.slave_path_);
            pty_file_ = std::move(other.pty_file_);
            other.master_fd_ = -1;
        }
        return *this;
    }

    [[nodiscard]] int master_fd() const noexcept { return master_fd_; }
    [[nodiscard]] const std::string& slave_path() const noexcept { return slave_path_; }

    void close() noexcept {
        if (master_fd_ != -1) {
            (void)::close(master_fd_);
            master_fd_ = -1;
        }
        if (!pty_file_.empty()) {
            (void)::unlink(pty_file_.c_str());
            pty_file_.clear();
        }
    }

private:
    int master_fd_{-1};
    std::string slave_path_;
    std::string pty_file_;
};

} // namespace tinyarmsim
