#pragma once

#include <stdint.h>
#include <string.h>

namespace remotemapper {
namespace hid_guard {

static const uint8_t kHeldMax = 8;

struct HeldEntry {
    uint8_t modifier;
    uint8_t key_code;
    uint16_t consumer;
    bool voice;
    bool pressed;
    uint32_t since_ms;
};

struct Registry {
    HeldEntry entries[kHeldMax];
    uint32_t generation;
};

struct Snapshot {
    HeldEntry entries[kHeldMax];
    uint32_t generation;
};

inline uint32_t next_generation(uint32_t generation) {
    ++generation;
    return generation ? generation : 1u;
}

inline Snapshot take_snapshot(const Registry &registry) {
    Snapshot snapshot;
    memcpy(snapshot.entries, registry.entries, sizeof(snapshot.entries));
    snapshot.generation = registry.generation;
    return snapshot;
}

inline bool add(Registry &registry, uint8_t modifier, uint8_t key_code,
                uint16_t consumer, bool voice, uint32_t now_ms) {
    for (uint8_t i = 0; i < kHeldMax; ++i) {
        HeldEntry &entry = registry.entries[i];
        if (entry.pressed) continue;
        entry.modifier = modifier;
        entry.key_code = key_code;
        entry.consumer = consumer;
        entry.voice = voice;
        entry.pressed = true;
        entry.since_ms = now_ms;
        registry.generation = next_generation(registry.generation);
        return true;
    }
    return false;
}

inline bool clear(Registry &registry, int voice_filter) {
    bool changed = false;
    for (uint8_t i = 0; i < kHeldMax; ++i) {
        HeldEntry &entry = registry.entries[i];
        if (!entry.pressed || (voice_filter >= 0 && entry.voice != (voice_filter != 0))) continue;
        entry.pressed = false;
        entry.modifier = 0;
        entry.key_code = 0;
        entry.consumer = 0;
        entry.voice = false;
        changed = true;
    }
    if (changed) registry.generation = next_generation(registry.generation);
    return changed;
}

// Call while holding the registry's short critical section. The engine mutex
// must also be held when using this check to authorize a force-release action.
inline bool snapshot_is_current(const Snapshot &snapshot, uint32_t generation) {
    return snapshot.generation == generation;
}

inline bool elapsed_at_least(uint32_t now_ms, uint32_t since_ms, uint32_t duration_ms) {
    return static_cast<uint32_t>(now_ms - since_ms) >= duration_ms;
}

inline uint32_t held_age(uint32_t now_ms, const HeldEntry &entry) {
    return static_cast<uint32_t>(now_ms - entry.since_ms);
}

} // namespace hid_guard
} // namespace remotemapper
