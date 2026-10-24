#pragma once

#include <exception>
#include <string>
#include <cstdint>

namespace klyrdb {

enum class CrashPoint : uint8_t {
    None = 0,
    BeforePageModify,
    AfterLogBeforePageFlush,
    AfterMemoryModify,
    DuringWalWrite,
    DuringPageFlush,
    AfterOnePageFlushed,
    AfterCommitDurableBeforeSuccess,
    DuringRollback,
    DuringRecovery,
    BeforeCheckpoint,
    AfterCheckpointAppendedBeforeDurable,
    AfterPagesFlushedBeforeCheckpointDurable,
    AfterMetadataDurableBeforeTruncation,
    DuringWalTruncation,
    DuringRecoveryAnalysis,
    DuringRecoveryRedo,
    DuringRecoveryUndo
};

[[nodiscard]] inline const char* to_string(CrashPoint cp) noexcept {
    switch (cp) {
        case CrashPoint::None: return "None";
        case CrashPoint::BeforePageModify: return "BeforePageModify";
        case CrashPoint::AfterLogBeforePageFlush: return "AfterLogBeforePageFlush";
        case CrashPoint::AfterMemoryModify: return "AfterMemoryModify";
        case CrashPoint::DuringWalWrite: return "DuringWalWrite";
        case CrashPoint::DuringPageFlush: return "DuringPageFlush";
        case CrashPoint::AfterOnePageFlushed: return "AfterOnePageFlushed";
        case CrashPoint::AfterCommitDurableBeforeSuccess: return "AfterCommitDurableBeforeSuccess";
        case CrashPoint::DuringRollback: return "DuringRollback";
        case CrashPoint::DuringRecovery: return "DuringRecovery";
        case CrashPoint::BeforeCheckpoint: return "BeforeCheckpoint";
        case CrashPoint::AfterCheckpointAppendedBeforeDurable: return "AfterCheckpointAppendedBeforeDurable";
        case CrashPoint::AfterPagesFlushedBeforeCheckpointDurable: return "AfterPagesFlushedBeforeCheckpointDurable";
        case CrashPoint::AfterMetadataDurableBeforeTruncation: return "AfterMetadataDurableBeforeTruncation";
        case CrashPoint::DuringWalTruncation: return "DuringWalTruncation";
        case CrashPoint::DuringRecoveryAnalysis: return "DuringRecoveryAnalysis";
        case CrashPoint::DuringRecoveryRedo: return "DuringRecoveryRedo";
        case CrashPoint::DuringRecoveryUndo: return "DuringRecoveryUndo";
    }
    return "Unknown";
}

class CrashSimulationException : public std::exception {
public:
    explicit CrashSimulationException(std::string message)
        : message_(std::move(message)) {}

    [[nodiscard]] const char* what() const noexcept override {
        return message_.c_str();
    }

private:
    std::string message_;
};

/// Deterministic crash-injection simulator for testing crash consistency and recovery.
class CrashSimulator {
public:
    static void set_target(CrashPoint cp, uint32_t trigger_at_count = 1) noexcept {
        target_point_ = cp;
        target_count_ = trigger_at_count;
        current_count_ = 0;
        enabled_ = (cp != CrashPoint::None);
    }

    static void arm(CrashPoint cp, uint32_t trigger_at_count = 1) noexcept {
        set_target(cp, trigger_at_count);
    }

    static void reset() noexcept {
        target_point_ = CrashPoint::None;
        target_count_ = 0;
        current_count_ = 0;
        enabled_ = false;
    }

    static void disarm() noexcept {
        reset();
    }

    static void trigger(CrashPoint cp) {
        if (!enabled_ || target_point_ != cp) {
            return;
        }
        current_count_++;
        if (current_count_ >= target_count_) {
            reset();
            throw CrashSimulationException(std::string("CrashSimulator: Injected crash at ") + to_string(cp));
        }
    }

    [[nodiscard]] static bool is_enabled() noexcept { return enabled_; }
    [[nodiscard]] static CrashPoint target_point() noexcept { return target_point_; }

private:
    inline static bool enabled_{false};
    inline static CrashPoint target_point_{CrashPoint::None};
    inline static uint32_t target_count_{0};
    inline static uint32_t current_count_{0};
};

} // namespace klyrdb
