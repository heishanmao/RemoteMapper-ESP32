#include "../../src/usb/hid_command_queue.h"
#include <cassert>

using Queue = remotemapper::HidCommandQueue;

static void rapid_press_release_is_fifo() {
    Queue queue;
    assert(queue.enqueue_keyboard(2, 4, false));
    assert(queue.enqueue_keyboard_release());
    Queue::Command down = {};
    assert(queue.peek(down) && down.kind == Queue::Kind::KeyboardState);
    assert(queue.begin_send(down));
    queue.end_send();
    assert(queue.complete(down));
    Queue::Command up = {};
    assert(queue.peek(up) && up.kind == Queue::Kind::KeyboardRelease);
    assert(queue.begin_send(up));
    queue.end_send();
    assert(queue.complete(up));
    assert(!queue.has_pending());
}

static void tap_is_one_ordered_command() {
    Queue queue;
    assert(queue.enqueue_keyboard(0, 7, true));
    assert(queue.enqueue_consumer_release());
    Queue::Command tap = {};
    assert(queue.peek(tap) && tap.kind == Queue::Kind::KeyboardTap);
    assert(queue.complete(tap));
    Queue::Command release = {};
    assert(queue.peek(release) && release.kind == Queue::Kind::ConsumerRelease);
}

static void overflow_cancels_old_work_but_keeps_zero_debt() {
    Queue queue;
    for (uint8_t i = 0; i < Queue::NORMAL_LIMIT; ++i)
        assert(queue.enqueue_keyboard(0, static_cast<uint8_t>(i + 1), false));
    assert(queue.enqueue_keyboard_release());
    assert(queue.enqueue_consumer_release());
    assert(queue.size() == Queue::CAPACITY);
    assert(!queue.enqueue_keyboard(0, 99, false));
    assert(queue.size() == 0 && queue.has_pending());
    Queue::Command command = {};
    assert(queue.peek(command) && command.kind == Queue::Kind::EmergencyKeyboardRelease);
    assert(queue.complete(command));
    assert(queue.peek(command) && command.kind == Queue::Kind::EmergencyConsumerRelease);
}

static void force_release_invalidates_inflight_epoch() {
    Queue queue;
    assert(queue.enqueue_keyboard(0, 5, false));
    Queue::Command old = {};
    assert(queue.peek(old));
    assert(queue.begin_send(old));
    queue.force_release_all();
    assert(!queue.is_current(old));
    queue.end_send();
    Queue::Command zero = {};
    assert(queue.peek(zero) && zero.kind == Queue::Kind::EmergencyKeyboardRelease);
}

static void recovery_blocks_press_and_preserves_emergency_release() {
    Queue queue;
    queue.begin_recovery();
    assert(queue.recovering());
    assert(!queue.enqueue_keyboard(0, 5, false));
    Queue::Command zero = {};
    assert(queue.peek(zero) && zero.kind == Queue::Kind::EmergencyKeyboardRelease);
    assert(queue.begin_send(zero)); // releases remain eligible during recovery
    queue.end_send();
    assert(queue.complete(zero));
    queue.end_recovery();
    assert(queue.enqueue_keyboard(0, 5, false));
}

static void failed_completion_does_not_consume_release_debt() {
    Queue queue;
    assert(queue.enqueue_keyboard_release());
    Queue::Command first = {};
    assert(queue.peek(first) && queue.begin_send(first));
    queue.end_send(); // model SendReport failure / retryable completion timeout
    Queue::Command retry = {};
    assert(queue.peek(retry) && retry.id == first.id && queue.has_pending(1));
}

int main() {
    rapid_press_release_is_fifo();
    tap_is_one_ordered_command();
    overflow_cancels_old_work_but_keeps_zero_debt();
    force_release_invalidates_inflight_epoch();
    recovery_blocks_press_and_preserves_emergency_release();
    failed_completion_does_not_consume_release_debt();
    return 0;
}
