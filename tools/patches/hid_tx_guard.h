#pragma once

// No RTOS or USB dependency. The framework adapter serializes every operation
// with a short portMUX critical section; submission and waiting remain outside.
#include <stddef.h>
#include <stdint.h>

// Arduino-ESP32 2.x uses gnu++11. The same implementation is constexpr only
// in the C++17 test harness; firmware does not need a language-standard change.
#if __cplusplus >= 201402L
#define RM_HID_STATE_CONSTEXPR constexpr
#else
#define RM_HID_STATE_CONSTEXPR
#endif

namespace remotemapper {

struct HidTxGuard {
    static constexpr size_t MAX_REPORT_BYTES = 64;
    enum class Start { Started, Unavailable, PreviousPending, Invalid };
    enum class Result { Pending, Success, Cancelled, Failed };

    uint32_t sequence = 0;
    uint32_t epoch = 0;
    uint32_t generation = 0;
    uint32_t submit_epoch = 0;
    uint32_t completed_generation = 0;
    Result completed_result = Result::Failed;
    bool mounted = false;
    bool pending = false;
    bool submitting = false;
    bool callback_retired = false;
    bool cancelled = false;
    uint8_t report_id = 0;
    size_t expected_length = 0;
    uint8_t expected[MAX_REPORT_BYTES] = {};

    RM_HID_STATE_CONSTEXPR Start begin(uint8_t id, const uint8_t* data, size_t length, uint32_t& token) {
        if (!mounted) return Start::Unavailable;
        if (pending) return Start::PreviousPending;
        const size_t prefix = id ? 1 : 0;
        if (length > MAX_REPORT_BYTES - prefix || (!data && length))
            return Start::Invalid;
        if (++sequence == 0) ++sequence;
        token = generation = sequence;
        submit_epoch = epoch;
        report_id = id;
        expected_length = length + prefix;
        if (prefix) expected[0] = id;
        for (size_t i = 0; i < length; ++i) expected[prefix + i] = data[i];
        pending = submitting = true;
        callback_retired = cancelled = false;
        completed_generation = 0;
        completed_result = Result::Pending;
        return Start::Started;
    }

    // USB lifecycle callbacks run synchronously on the TinyUSB task. A reset
    // racing native submission cannot free its slot: the submission may still
    // reach a newly configured endpoint after this callback returns.
    RM_HID_STATE_CONSTEXPR bool lifecycle(bool is_mounted) {
        ++epoch;
        mounted = is_mounted;
        if (!pending) return false;
        cancelled = true;
        completed_generation = generation;
        completed_result = Result::Cancelled;
        if (!submitting) pending = false;
        return true;
    }

    // Endpoint ready alone cannot retire the old transfer. TinyUSB clears its
    // busy flag before this callback, leaving a cross-core submission window.
    RM_HID_STATE_CONSTEXPR bool complete(uint8_t instance, const uint8_t* report, size_t length) {
        if (instance != 0 || !pending || callback_retired ||
                length != expected_length || (!report && length)) return false;
        for (size_t i = 0; i < length; ++i)
            if (expected[i] != report[i]) return false;
        callback_retired = true;
        completed_generation = generation;
        completed_result = !cancelled && mounted && epoch == submit_epoch
                ? Result::Success : Result::Cancelled;
        if (!submitting) pending = false;
        return true;
    }

    RM_HID_STATE_CONSTEXPR void submitted(uint32_t token, bool accepted) {
        if (token != generation || !submitting) return;
        submitting = false;
        if (!accepted) {
            // Native rejection can never be turned into success by a wakeup.
            pending = false;
            completed_generation = token;
            completed_result = Result::Failed;
        } else if (callback_retired) {
            pending = false;
            if (cancelled || !mounted || epoch != submit_epoch) {
                completed_generation = token;
                completed_result = Result::Cancelled;
            }
        }
        // A cancelled but accepted submission remains quarantined until a
        // matching callback or a later lifecycle boundary safely retires it.
    }

    RM_HID_STATE_CONSTEXPR Result result(uint32_t token) const {
        if (completed_generation == token) return completed_result;
        return Result::Pending;
    }
};

} // namespace remotemapper

#undef RM_HID_STATE_CONSTEXPR
