#include "usb/hid_guard_registry.h"

#include <stdlib.h>

using namespace remotemapper::hid_guard;

static void require(bool condition) {
    if (!condition) abort();
}

int main() {
    Registry registry = {};
    registry.generation = 1;

    // Reproduce the interleaving: guard_tick snapshots an expired old session;
    // that session is cleared and a new press registers before the timeout
    // action can run. The old decision must be rejected, leaving the new hold.
    require(add(registry, 0, 0xE6, 0, true, 1000));
    const Snapshot old_timeout_decision = take_snapshot(registry);
    require(elapsed_at_least(61000, old_timeout_decision.entries[0].since_ms, 60000));
    require(clear(registry, -1));
    require(add(registry, 0, 0xE6, 0, true, 62000));
    require(!snapshot_is_current(old_timeout_decision, registry.generation));
    require(registry.entries[0].pressed && registry.entries[0].voice);
    require(registry.entries[0].since_ms == 62000);

    // Unsigned elapsed-time checks remain correct across millis() wrap.
    const uint32_t wrapped_start = 0xfffffff0u;
    require(elapsed_at_least(0x00000020u, wrapped_start, 48u));
    require(!elapsed_at_least(0x00000020u, wrapped_start, 49u));

    // Generation zero is reserved as an uninitialized marker on wrap.
    require(next_generation(0xffffffffu) == 1u);
    return 0;
}
