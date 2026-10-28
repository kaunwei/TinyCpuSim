#pragma once

#include <chrono>
#include <cerrno>
#include <fcntl.h>
#include <functional>
#include <optional>
#include <poll.h>
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

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
                                                       transport_failed_(other.transport_failed_),
                                                       paused_(other.paused_),
                                                       step_pending_(other.step_pending_),
                                                       breakpoint_hit_reported_(other.breakpoint_hit_reported_),
                                                       stepped_notify_pending_(other.stepped_notify_pending_),
                                                       eof_reached_(other.eof_reached_),
                                                       instruction_counter_(std::move(other.instruction_counter_)),
                                                       instruction_baseline_(other.instruction_baseline_),
                                                       register_provider_(std::move(other.register_provider_)),
                                                       memory_provider_(std::move(other.memory_provider_)),
                                                       pc_provider_(std::move(other.pc_provider_)),
                                                       breakpoint_(other.breakpoint_),
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
            transport_failed_ = other.transport_failed_;
            paused_ = other.paused_;
            step_pending_ = other.step_pending_;
            breakpoint_hit_reported_ = other.breakpoint_hit_reported_;
            stepped_notify_pending_ = other.stepped_notify_pending_;
            eof_reached_ = other.eof_reached_;
            instruction_counter_ = std::move(other.instruction_counter_);
            instruction_baseline_ = other.instruction_baseline_;
            register_provider_ = std::move(other.register_provider_);
            memory_provider_ = std::move(other.memory_provider_);
            pc_provider_ = std::move(other.pc_provider_);
            breakpoint_ = other.breakpoint_;
            buffer_ = std::move(other.buffer_);
            other.fd_ = -1;
        }
        return *this;
    }

    void set_instruction_counter(std::function<uint64_t()> provider) {
        instruction_counter_ = std::move(provider);
    }

    void set_register_provider(std::function<std::string()> provider) {
        register_provider_ = std::move(provider);
    }

    void set_memory_provider(std::function<std::optional<std::vector<uint8_t>>(uint32_t, size_t)> provider) {
        memory_provider_ = std::move(provider);
    }

    void set_pc_provider(std::function<uint32_t()> provider) {
        pc_provider_ = std::move(provider);
    }

    bool stop_requested() {
        if (stop_requested_) {
            return true;
        }

        if (stepped_notify_pending_) {
            read_available();
            check_eof_status();
            if (closed_without_stop_ || transport_failed_) {
                stop_requested_ = true;
                return true;
            }
            if (instruction_counter_ && instruction_counter_() == instruction_baseline_) {
                return false;
            }
            stepped_notify_pending_ = false;
            send_response("{\"event\":\"stepped\"}\n");
        }

        read_available();
        process_lines();
        check_eof_status();

        if (step_pending_ && !transport_failed_ && !closed_without_stop_) {
            step_pending_ = false;
            if (instruction_counter_) instruction_baseline_ = instruction_counter_();
            stepped_notify_pending_ = true;
            return false;
        }

        if (!paused_ && !breakpoint_hit_reported_ && !stop_requested_ && !closed_without_stop_ && !transport_failed_ && breakpoint_ && pc_provider_) {
            const uint32_t pc = pc_provider_();
            if (pc == *breakpoint_) {
                paused_ = true;
                breakpoint_hit_reported_ = true;
                send_response("{\"event\":\"breakpoint\",\"pc\":" + std::to_string(pc) + "}\n");
            }
        }

        while (paused_ && !stop_requested_) {
            if (fd_ < 0 || closed_without_stop_ || transport_failed_) {
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

            if (step_pending_ && !transport_failed_ && !closed_without_stop_) {
                step_pending_ = false;
                if (instruction_counter_) instruction_baseline_ = instruction_counter_();
                stepped_notify_pending_ = true;
                return false;
            }
        }

        if (closed_without_stop_ || transport_failed_) {
            stop_requested_ = true;
            return true;
        }

        return stop_requested_;
    }

    bool closed_without_stop() const {
        return closed_without_stop_;
    }

    bool transport_failed() const {
        return transport_failed_;
    }

private:
    void check_eof_status() {
        if (eof_reached_ && !stop_requested_) {
            // Scan remaining buffer_ for complete newline-delimited stop command
            bool has_stop = false;
            size_t start = 0;
            while (start < buffer_.size()) {
                auto pos = buffer_.find('\n', start);
                if (pos == std::string::npos) {
                    break;
                }
                std::string_view line(&buffer_[start], pos - start);
                if (!line.empty() && line.back() == '\r') {
                    line.remove_suffix(1);
                }
                if (line == "stop") {
                    has_stop = true;
                    break;
                }
                start = pos + 1;
            }
            if (!has_stop) {
                closed_without_stop_ = true;
            }
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
                const bool was_paused = paused_;
                paused_ = false;
                send_response("{\"event\":\"continued\"}\n");
                if (was_paused && breakpoint_hit_reported_) {
                    breakpoint_hit_reported_ = false;
                } else if (was_paused && breakpoint_ && pc_provider_) {
                    const uint32_t pc = pc_provider_();
                    if (pc == *breakpoint_) {
                        paused_ = true;
                        breakpoint_hit_reported_ = true;
                        send_response("{\"event\":\"breakpoint\",\"pc\":" + std::to_string(pc) + "}\n");
                    }
                }
            } else if (line == "step") {
                if (paused_) {
                    step_pending_ = true;
                    breakpoint_hit_reported_ = false;
                    break;
                } else {
                    send_response("{\"error\":\"step requires pause\"}\n");
                }
            } else if (line == "break clear") {
                breakpoint_.reset();
                breakpoint_hit_reported_ = false;
                send_response("{\"event\":\"breakpoint_cleared\"}\n");
            } else if (line == "break" || line.rfind("break ", 0) == 0 || line.rfind("break\t", 0) == 0) {
                uint32_t address = 0;
                if (!parse_breakpoint_address(std::string_view(line).substr(5), address)) {
                    send_response("{\"error\":\"invalid breakpoint address\"}\n");
                } else if (!pc_provider_) {
                    send_response("{\"error\":\"missing pc provider\"}\n");
                } else {
                    breakpoint_ = address;
                    breakpoint_hit_reported_ = false;
                    send_response("{\"event\":\"breakpoint_set\",\"pc\":" + std::to_string(address) + "}\n");
                }
            } else if (line == "regs") {
                if (register_provider_) {
                    std::string payload = register_provider_();
                    payload.push_back('\n');
                    send_response(payload);
                } else {
                    send_response("{\"error\":\"missing register provider\"}\n");
                }
            } else if (line.rfind("mem", 0) == 0) {
                uint32_t address = 0;
                size_t length = 0;
                if ((line.size() > 3 && line[3] != ' ' && line[3] != '\t') ||
                    !parse_memory_arguments(std::string_view(line).substr(3), address, length)) {
                    send_response("{\"error\":\"invalid memory arguments\"}\n");
                } else if (!memory_provider_) {
                    send_response("{\"error\":\"missing memory provider\"}\n");
                } else {
                    auto bytes = memory_provider_(address, length);
                    if (!bytes) {
                        send_response("{\"error\":\"memory access out of bounds or mmio rejected\"}\n");
                    } else {
                        std::string payload = "{\"event\":\"memory\",\"address\":" + std::to_string(address) + ",\"bytes\":[";
                        for (size_t i = 0; i < bytes->size(); ++i) {
                            if (i != 0) payload += ',';
                            payload += std::to_string((*bytes)[i]);
                        }
                        payload += "]}\n";
                        send_response(payload);
                    }
                }
            } else {
                send_response("{\"error\":\"unknown command\"}\n");
            }
        }
    }

    static bool parse_memory_arguments(std::string_view args, uint32_t& address, size_t& length) {
        size_t pos = args.find_first_not_of(" \t");
        if (pos == std::string_view::npos) return false;
        args.remove_prefix(pos);
        size_t address_end = args.find_first_of(" \t");
        if (address_end == std::string_view::npos) return false;
        auto address_text = args.substr(0, address_end);
        int base = 10;
        if (address_text.size() > 2 && address_text[0] == '0' &&
            (address_text[1] == 'x' || address_text[1] == 'X')) {
            base = 16;
            address_text.remove_prefix(2);
            if (address_text.empty()) return false;
        }
        uint64_t parsed_address = 0;
        auto address_result = std::from_chars(address_text.data(), address_text.data() + address_text.size(), parsed_address, base);
        if (address_result.ec != std::errc{} || address_result.ptr != address_text.data() + address_text.size() ||
            parsed_address > UINT32_MAX) return false;
        args.remove_prefix(address_end);
        pos = args.find_first_not_of(" \t");
        if (pos == std::string_view::npos) return false;
        args.remove_prefix(pos);
        size_t length_end = args.find_first_of(" \t");
        auto length_text = args.substr(0, length_end);
        uint64_t parsed_length = 0;
        auto length_result = std::from_chars(length_text.data(), length_text.data() + length_text.size(), parsed_length, 10);
        if (length_result.ec != std::errc{} || length_result.ptr != length_text.data() + length_text.size() ||
            parsed_length < 1 || parsed_length > 64) return false;
        if (length_end != std::string_view::npos && args.find_first_not_of(" \t", length_end) != std::string_view::npos) return false;
        address = static_cast<uint32_t>(parsed_address);
        length = static_cast<size_t>(parsed_length);
        return true;
    }

    static bool parse_breakpoint_address(std::string_view args, uint32_t& address) {
        size_t pos = args.find_first_not_of(" \t");
        if (pos == std::string_view::npos) return false;
        args.remove_prefix(pos);
        size_t end = args.find_first_of(" \t");
        auto text = args.substr(0, end);
        int base = 10;
        if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
            text.remove_prefix(2);
            base = 16;
            if (text.empty()) return false;
        }
        uint64_t parsed = 0;
        auto result = std::from_chars(text.data(), text.data() + text.size(), parsed, base);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || parsed > UINT32_MAX) return false;
        if (end != std::string_view::npos && args.find_first_not_of(" \t", end) != std::string_view::npos) return false;
        address = static_cast<uint32_t>(parsed);
        return true;
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
        if (fd_ < 0 || transport_failed_) return;
        const char* ptr = msg.data();
        size_t remaining = msg.size();
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);

        while (remaining > 0) {
            ssize_t n = ::send(fd_, ptr, remaining, MSG_NOSIGNAL);
            if (n > 0) {
                ptr += n;
                remaining -= static_cast<size_t>(n);
                continue;
            }
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    auto now = std::chrono::steady_clock::now();
                    if (now >= deadline) {
                        transport_failed_ = true;
                        break;
                    }
                    auto ms_left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
                    int timeout_ms = static_cast<int>(ms_left > 0 ? ms_left : 1);

                    struct pollfd pfd;
                    pfd.fd = fd_;
                    pfd.events = POLLOUT;
                    pfd.revents = 0;

                    int pret = ::poll(&pfd, 1, timeout_ms);
                    if (pret > 0) {
                        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                            // Check if POLLOUT is also set, but if error/hup with no output capacity,
                            // next send will fail with EPIPE or handle it.
                        }
                        continue;
                    } else if (pret == 0) {
                        // Timed out waiting for buffer space
                        transport_failed_ = true;
                        break;
                    } else {
                        if (errno == EINTR) {
                            continue;
                        }
                        transport_failed_ = true;
                        break;
                    }
                }
                // Any other send failure (e.g. EPIPE, ECONNRESET, etc.)
                // Peer closed or reset connection. Not terminal delivery transport_failed timeout.
                break;
            }
        }
    }

    int fd_ = -1;
    bool stop_requested_ = false;
    bool closed_without_stop_ = false;
    bool transport_failed_ = false;
    bool paused_ = false;
    bool step_pending_ = false;
    bool breakpoint_hit_reported_ = false;
    bool stepped_notify_pending_ = false;
    bool eof_reached_ = false;
    std::function<uint64_t()> instruction_counter_;
    uint64_t instruction_baseline_ = 0;
    std::function<std::string()> register_provider_;
    std::function<std::optional<std::vector<uint8_t>>(uint32_t, size_t)> memory_provider_;
    std::function<uint32_t()> pc_provider_;
    std::optional<uint32_t> breakpoint_;
    std::string buffer_;
};

} // namespace tinyarmsim
