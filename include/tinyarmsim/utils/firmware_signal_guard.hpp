#pragma once

#include <atomic>
#include <cerrno>
#include <csignal>
#include <pthread.h>
#include <stdexcept>
#include <system_error>

namespace tinyarmsim {

class FirmwareSignalGuard {
public:
    FirmwareSignalGuard() {
        bool expected = false;
        if (!active_.compare_exchange_strong(expected, true)) {
            throw std::logic_error("firmware signal guard already active");
        }

        sigset_t managed_signals;
        ::sigemptyset(&managed_signals);
        ::sigaddset(&managed_signals, SIGINT);
        ::sigaddset(&managed_signals, SIGTERM);

        const int block_error = ::pthread_sigmask(SIG_BLOCK, &managed_signals, &original_mask_);
        if (block_error != 0) {
            active_.store(false, std::memory_order_relaxed);
            throw std::system_error(block_error, std::generic_category(), "block firmware signals");
        }
        mask_saved_ = true;

        received_signal_ = 0;
        try {
            if (::sigaction(SIGINT, nullptr, &old_int_action_) != 0) {
                throw std::system_error(errno, std::generic_category(), "read previous SIGINT action");
            }
            if (::sigaction(SIGTERM, nullptr, &old_term_action_) != 0) {
                throw std::system_error(errno, std::generic_category(), "read previous SIGTERM action");
            }

            struct sigaction action {};
            action.sa_handler = &handle_signal;
            ::sigemptyset(&action.sa_mask);
            action.sa_flags = 0;
            if (::sigaction(SIGINT, &action, nullptr) != 0) {
                throw std::system_error(errno, std::generic_category(), "install SIGINT handler");
            }
            int_action_installed_ = true;
            if (::sigaction(SIGTERM, &action, nullptr) != 0) {
                throw std::system_error(errno, std::generic_category(), "install SIGTERM handler");
            }
            term_action_installed_ = true;

            // Unblock managed signals so handler can trigger on SIGINT/SIGTERM
            const int unblock_error = ::pthread_sigmask(SIG_UNBLOCK, &managed_signals, nullptr);
            if (unblock_error != 0) {
                throw std::system_error(unblock_error, std::generic_category(),
                                        "unblock signals after installing firmware handlers");
            }
        } catch (...) {
            restore();
            throw;
        }
    }

    FirmwareSignalGuard(const FirmwareSignalGuard&) = delete;
    FirmwareSignalGuard& operator=(const FirmwareSignalGuard&) = delete;
    FirmwareSignalGuard(FirmwareSignalGuard&&) = delete;
    FirmwareSignalGuard& operator=(FirmwareSignalGuard&&) = delete;

    ~FirmwareSignalGuard() noexcept {
        restore();
    }

    [[nodiscard]] int received_signal() const noexcept {
        return received_signal_;
    }

private:
    static void handle_signal(int signal) noexcept {
        if (received_signal_ == 0) {
            received_signal_ = signal;
        }
    }

    void restore() noexcept {
        if (!active_.load(std::memory_order_relaxed)) {
            return;
        }
        sigset_t managed_signals;
        ::sigemptyset(&managed_signals);
        ::sigaddset(&managed_signals, SIGINT);
        ::sigaddset(&managed_signals, SIGTERM);
        (void)::pthread_sigmask(SIG_BLOCK, &managed_signals, nullptr);

        if (term_action_installed_) {
            (void)::sigaction(SIGTERM, &old_term_action_, nullptr);
            term_action_installed_ = false;
        }
        if (int_action_installed_) {
            (void)::sigaction(SIGINT, &old_int_action_, nullptr);
            int_action_installed_ = false;
        }
        if (mask_saved_) {
            (void)::pthread_sigmask(SIG_SETMASK, &original_mask_, nullptr);
            mask_saved_ = false;
        }
        active_.store(false, std::memory_order_relaxed);
    }

    inline static std::atomic<bool> active_{false};
    inline static volatile std::sig_atomic_t received_signal_{0};

    struct sigaction old_int_action_ {};
    struct sigaction old_term_action_ {};
    sigset_t original_mask_{};
    bool mask_saved_{false};
    bool int_action_installed_{false};
    bool term_action_installed_{false};
};

} // namespace tinyarmsim
