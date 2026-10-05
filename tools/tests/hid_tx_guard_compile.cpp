// These compile-time checks execute the exact C++ guard embedded in USBHID.cpp.
// They verify state transitions, not real USB hardware or RTOS scheduling.
#include "../patches/hid_tx_guard.h"

using Guard = remotemapper::HidTxGuard;
constexpr uint8_t payload[] = {0x40, 0};
constexpr uint8_t report[] = {1, 0x40, 0};
constexpr uint8_t release[] = {0, 0};
constexpr uint8_t release_report[] = {1, 0, 0};

constexpr bool normal_completion() {
    Guard guard;
    guard.lifecycle(true);
    uint32_t a = 0;
    if (guard.begin(1, payload, 2, a) != Guard::Start::Started) return false;
    guard.submitted(a, true);
    if (guard.result(a) != Guard::Result::Pending) return false;
    if (!guard.complete(0, report, 3)) return false;
    return !guard.pending && guard.result(a) == Guard::Result::Success;
}

constexpr bool callback_before_submit_returns() {
    Guard guard;
    guard.lifecycle(true);
    uint32_t a = 0;
    guard.begin(1, payload, 2, a);
    if (!guard.complete(0, report, 3) || !guard.pending) return false;
    uint32_t b = 0;
    if (guard.begin(1, release, 2, b) != Guard::Start::PreviousPending) return false;
    guard.submitted(a, true);
    return !guard.pending && guard.result(a) == Guard::Result::Success;
}

constexpr bool timeout_keeps_slot() {
    Guard guard;
    guard.lifecycle(true);
    uint32_t a = 0, b = 0;
    guard.begin(1, payload, 2, a);
    guard.submitted(a, true);
    // Sender timeout does not retire the slot, even if the endpoint is ready.
    if (guard.begin(1, release, 2, b) != Guard::Start::PreviousPending) return false;
    if (!guard.complete(0, report, 3)) return false;
    if (guard.begin(1, release, 2, b) != Guard::Start::Started || b == a) return false;
    guard.submitted(b, true);
    // The old callback can give the semaphore here, after B was submitted.
    // No wakeup changes B's result: it requires B's own completion.
    if (guard.result(b) != Guard::Result::Pending) return false;
    if (guard.complete(0, report, 3)) return false;
    if (!guard.complete(0, release_report, 3)) return false;
    return guard.result(b) == Guard::Result::Success;
}

constexpr bool mismatched_callbacks_are_rejected() {
    Guard guard;
    guard.lifecycle(true);
    uint32_t a = 0;
    guard.begin(1, payload, 2, a);
    guard.submitted(a, true);
    constexpr uint8_t wrong_id[] = {2, 0x40, 0};
    if (guard.complete(1, report, 3) || guard.complete(0, wrong_id, 3) ||
            guard.complete(0, report, 2) || guard.complete(0, release_report, 3) ||
            guard.complete(0, nullptr, 3)) return false;
    return guard.pending && guard.result(a) == Guard::Result::Pending;
}

constexpr bool lifecycle_cancels_waiting() {
    Guard guard;
    guard.lifecycle(true);
    uint32_t a = 0, b = 0;
    guard.begin(1, payload, 2, a);
    guard.submitted(a, true);
    if (!guard.lifecycle(false) || guard.pending) return false;
    if (guard.result(a) != Guard::Result::Cancelled) return false;
    if (guard.complete(0, report, 3)) return false;
    if (guard.begin(1, payload, 2, b) != Guard::Start::Unavailable) return false;
    guard.lifecycle(true);
    return guard.begin(1, release, 2, b) == Guard::Start::Started && b != a;
}

constexpr bool reset_during_submit_is_quarantined() {
    Guard guard;
    guard.lifecycle(true);
    uint32_t a = 0, b = 0;
    guard.begin(1, payload, 2, a);
    guard.lifecycle(false);
    guard.lifecycle(true);
    if (!guard.pending || !guard.submitting) return false;
    guard.submitted(a, true);
    if (guard.result(a) != Guard::Result::Cancelled || !guard.pending) return false;
    if (guard.begin(1, release, 2, b) != Guard::Start::PreviousPending) return false;
    if (!guard.complete(0, report, 3) || guard.pending) return false;
    return guard.result(a) == Guard::Result::Cancelled;
}

constexpr bool later_mount_can_retire_quarantine() {
    Guard guard;
    guard.lifecycle(true);
    uint32_t a = 0, b = 0;
    guard.begin(1, payload, 2, a);
    guard.lifecycle(true);
    guard.submitted(a, true);
    if (!guard.pending) return false;
    guard.lifecycle(true);
    return !guard.pending && guard.result(a) == Guard::Result::Cancelled &&
            guard.begin(1, release, 2, b) == Guard::Start::Started;
}

constexpr bool rejection_never_becomes_success() {
    Guard guard;
    guard.lifecycle(true);
    uint32_t a = 0;
    guard.begin(1, payload, 2, a);
    // Even an unexpected matching callback before native rejection does not
    // turn tud_hid_n_report(false) into a confirmed send.
    guard.complete(0, report, 3);
    guard.submitted(a, false);
    return !guard.pending && guard.result(a) == Guard::Result::Failed;
}

constexpr bool reset_after_early_callback_cancels() {
    Guard guard;
    guard.lifecycle(true);
    uint32_t a = 0;
    guard.begin(1, payload, 2, a);
    guard.complete(0, report, 3);
    guard.lifecycle(true);
    guard.submitted(a, true);
    return !guard.pending && guard.result(a) == Guard::Result::Cancelled;
}

constexpr bool invalid_inputs_and_generation_wrap() {
    Guard guard;
    guard.lifecycle(true);
    uint32_t a = 0;
    if (guard.begin(1, nullptr, 2, a) != Guard::Start::Invalid ||
            guard.begin(1, payload, 64, a) != Guard::Start::Invalid) return false;
    guard.sequence = UINT32_MAX;
    if (guard.begin(1, payload, 2, a) != Guard::Start::Started || a != 1) return false;
    guard.submitted(a, true);
    return guard.result(a) == Guard::Result::Pending;
}

static_assert(normal_completion(), "Normal completion must confirm its report");
static_assert(callback_before_submit_returns(), "An early callback must not free a submitting slot");
static_assert(timeout_keeps_slot(), "Old callback/wakeup must not acknowledge a new release");
static_assert(mismatched_callbacks_are_rejected(), "Callback identity and full packet must match");
static_assert(lifecycle_cancels_waiting(), "Synchronous lifecycle must cancel a waiting generation");
static_assert(reset_during_submit_is_quarantined(), "Reset racing submission must retain its slot");
static_assert(later_mount_can_retire_quarantine(), "A later mount must allow transport recovery");
static_assert(rejection_never_becomes_success(), "Native submission failure must remain failure");
static_assert(reset_after_early_callback_cancels(), "Reset after callback but before return must cancel");
static_assert(invalid_inputs_and_generation_wrap(), "Validate payload and skip the zero generation");
