#pragma once

#include <cerrno>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <unistd.h>

namespace tinyarmsim {

class FirmwareControl {
public:
    explicit FirmwareControl(int fd) : fd_(fd) {
        if (fd_ >= 0) {
            int flags = fcntl(fd_, F_GETFL, 0);
            if (flags >= 0) {
                fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
            }
        }
    }

    ~FirmwareControl() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    FirmwareControl(const FirmwareControl&) = delete;
    FirmwareControl& operator=(const FirmwareControl&) = delete;

    FirmwareControl(FirmwareControl&& other) noexcept : fd_(other.fd_),
                                                       stop_requested_(other.stop_requested_),
                                                       closed_without_stop_(other.closed_without_stop_),
                                                       buffer_(std::move(other.buffer_)) {
        other.fd_ = -1;
    }

    FirmwareControl& operator=(FirmwareControl&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) {
                ::close(fd_);
            }
            fd_ = other.fd_;
            stop_requested_ = other.stop_requested_;
            closed_without_stop_ = other.closed_without_stop_;
            buffer_ = std::move(other.buffer_);
            other.fd_ = -1;
        }
        return *this;
    }

    bool stop_requested() {
        if (stop_requested_) {
            return true;
        }

        read_available();

        while (true) {
            auto pos = buffer_.find('\n');
            if (pos == std::string::npos) {
                break;
            }

            std::string line = buffer_.substr(0, pos);
            buffer_.erase(0, pos + 1);

            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            if (line == "stop") {
                stop_requested_ = true;
                send_response("{\"event\":\"stopped\",\"reason\":\"requested\"}\n");
                return true;
            } else {
                send_response("{\"error\":\"unknown command\"}\n");
            }
        }

        if (closed_without_stop_) {
            stop_requested_ = true;
            return true;
        }

        return false;
    }

    bool closed_without_stop() const {
        return closed_without_stop_;
    }

private:
    void read_available() {
        if (fd_ < 0 || closed_without_stop_) {
            return;
        }

        char buf[256];
        while (true) {
            ssize_t n = ::read(fd_, buf, sizeof(buf));
            if (n > 0) {
                buffer_.append(buf, static_cast<size_t>(n));
                if (buffer_.size() > 65536) {
                    // Prevent unbounded memory growth if no newlines sent
                    buffer_.clear();
                }
            } else if (n == 0) {
                // EOF reached
                if (!stop_requested_) {
                    closed_without_stop_ = true;
                }
                break;
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                if (errno == EINTR) {
                    continue;
                }
                // Other read error treated as EOF/closed
                if (!stop_requested_) {
                    closed_without_stop_ = true;
                }
                break;
            }
        }
    }

    void send_response(std::string_view msg) {
        if (fd_ < 0) return;
        const char* ptr = msg.data();
        size_t remaining = msg.size();
        while (remaining > 0) {
            ssize_t n = ::write(fd_, ptr, remaining);
            if (n > 0) {
                ptr += n;
                remaining -= static_cast<size_t>(n);
            } else if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                // If write fails (e.g. EPIPE), break
                break;
            }
        }
    }

    int fd_ = -1;
    bool stop_requested_ = false;
    bool closed_without_stop_ = false;
    std::string buffer_;
};

} // namespace tinyarmsim
