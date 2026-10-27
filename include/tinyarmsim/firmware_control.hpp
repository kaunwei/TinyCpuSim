#pragma once

#include <cerrno>
#include <fcntl.h>
#include <functional>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
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
                                                       paused_(other.paused_),
                                                       eof_reached_(other.eof_reached_),
                                                       register_provider_(std::move(other.register_provider_)),
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
            paused_ = other.paused_;
            eof_reached_ = other.eof_reached_;
            register_provider_ = std::move(other.register_provider_);
            buffer_ = std::move(other.buffer_);
            other.fd_ = -1;
        }
        return *this;
    }

    void set_register_provider(std::function<std::string()> provider) {
        register_provider_ = std::move(provider);
    }

    bool stop_requested() {
        if (stop_requested_) {
            return true;
        }

        read_available();
        process_lines();
        check_eof_status();

        while (paused_ && !stop_requested_) {
            if (fd_ < 0 || closed_without_stop_) {
                stop_requested_ = true;
                break;
            }

            struct pollfd pfd;
            pfd.fd = fd_;
            pfd.events = POLLIN;
            pfd.revents = 0;

            int ret = ::poll(&pfd, 1, -1);
            if (ret < 0) {
                if (errno == EINTR) {
                    continue;
                }
                closed_without_stop_ = true;
                stop_requested_ = true;
                break;
            }

            read_available();
            process_lines();
            check_eof_status();
        }

        if (closed_without_stop_) {
            stop_requested_ = true;
            return true;
        }

        return stop_requested_;
    }

    bool closed_without_stop() const {
        return closed_without_stop_;
    }

private:
    void check_eof_status() {
        if (eof_reached_ && !stop_requested_) {
            closed_without_stop_ = true;
        }
    }

    void process_lines() {
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
                break;
            } else if (line == "pause") {
                paused_ = true;
                send_response("{\"event\":\"paused\"}\n");
            } else if (line == "continue") {
                paused_ = false;
                send_response("{\"event\":\"continued\"}\n");
            } else if (line == "regs") {
                if (register_provider_) {
                    std::string payload = register_provider_();
                    payload.push_back('\n');
                    send_response(payload);
                } else {
                    send_response("{\"error\":\"missing register provider\"}\n");
                }
            } else {
                send_response("{\"error\":\"unknown command\"}\n");
            }
        }
    }

    void read_available() {
        if (fd_ < 0 || closed_without_stop_ || eof_reached_) {
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
                eof_reached_ = true;
                break;
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                if (errno == EINTR) {
                    continue;
                }
                // Other read error treated as EOF/closed
                eof_reached_ = true;
                break;
            }
        }
    }

    void send_response(std::string_view msg) {
        if (fd_ < 0) return;
        const char* ptr = msg.data();
        size_t remaining = msg.size();
        while (remaining > 0) {
            ssize_t n = ::send(fd_, ptr, remaining, MSG_NOSIGNAL);
            if (n > 0) {
                ptr += n;
                remaining -= static_cast<size_t>(n);
            } else if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                // If write fails (e.g. EPIPE or ECONNRESET), peer closed or reset connection
                break;
            }
        }
    }

    int fd_ = -1;
    bool stop_requested_ = false;
    bool closed_without_stop_ = false;
    bool paused_ = false;
    bool eof_reached_ = false;
    std::function<std::string()> register_provider_;
    std::string buffer_;
};

} // namespace tinyarmsim
