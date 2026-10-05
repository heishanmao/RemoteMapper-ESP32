#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "../../src/usb/hid_keyboard_report_ring.h"

using remotemapper::hid_diag::KeyboardReportRing;

static void test_hold_release_and_payload_copy() {
    KeyboardReportRing ring;
    uint8_t hold[8] = {0x40, 0, 0x04, 0, 0, 0, 0, 0};
    uint8_t release[8] = {};
    ring.record(10, KeyboardReportRing::Source::Queue, 7, 3, hold, true);
    memset(hold, 0xA5, sizeof(hold)); // Ring owns a snapshot, not this buffer.
    ring.record(11, KeyboardReportRing::Source::Queue, 8, 3, release, true);
    assert(ring.size() == 2);
    const auto& first = ring.at(0);
    assert(first.report[0] == 0x40 && first.report[2] == 0x04);
    assert(first.command_id == 7 && first.epoch == 3 && first.completed);
    const auto& second = ring.at(1);
    assert(second.report[0] == 0 && second.report[7] == 0);
}

static void test_failed_release_then_success_is_not_merged() {
    KeyboardReportRing ring;
    uint8_t release[8] = {};
    ring.record(20, KeyboardReportRing::Source::Queue, 9, 4, release, false);
    ring.record(70, KeyboardReportRing::Source::Queue, 9, 4, release, true);
    assert(ring.size() == 2);
    assert(!ring.at(0).completed && ring.at(0).count == 1);
    assert(ring.at(1).completed && ring.at(1).count == 1);
}

static void test_repeat_compression_preserves_transitions() {
    KeyboardReportRing ring;
    uint8_t zero[8] = {};
    uint8_t alt[8] = {0x40, 0, 0, 0, 0, 0, 0, 0};
    ring.record(1, KeyboardReportRing::Source::Stress, 0, 0, zero, true);
    ring.record(2, KeyboardReportRing::Source::Stress, 0, 0, zero, true);
    ring.record(3, KeyboardReportRing::Source::Stress, 0, 0, alt, true);
    ring.record(4, KeyboardReportRing::Source::Stress, 0, 0, zero, true);
    ring.record(5, KeyboardReportRing::Source::Stress, 0, 0, zero, false);
    assert(ring.size() == 4);
    assert(ring.at(0).count == 2 && ring.at(0).first_ms == 1 && ring.at(0).last_ms == 2);
    assert(ring.at(1).report[0] == 0x40);
    assert(ring.at(2).completed && ring.at(2).count == 1);
    assert(!ring.at(3).completed && ring.at(3).count == 1);
}

static void test_ring_wrap_keeps_latest_chronological_entries() {
    KeyboardReportRing ring;
    uint8_t report[8] = {};
    for (uint32_t i = 0; i < KeyboardReportRing::CAPACITY + 3; ++i) {
        report[0] = static_cast<uint8_t>(i);
        ring.record(i, KeyboardReportRing::Source::Queue, i + 1, 1, report, true);
    }
    assert(ring.size() == KeyboardReportRing::CAPACITY);
    assert(ring.at(0).first_ms == 3 && ring.at(0).report[0] == 3);
    assert(ring.at(KeyboardReportRing::CAPACITY - 1).first_ms == KeyboardReportRing::CAPACITY + 2);
}

int main() {
    test_hold_release_and_payload_copy();
    test_failed_release_then_success_is_not_merged();
    test_repeat_compression_preserves_transitions();
    test_ring_wrap_keeps_latest_chronological_entries();
}
