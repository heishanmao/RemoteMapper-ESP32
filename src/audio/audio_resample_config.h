#pragma once

#include <stdint.h>

#ifdef __cplusplus
#define AUDIO_RS_CONSTEXPR static constexpr
#else
#define AUDIO_RS_CONSTEXPR static inline
#endif

typedef struct {
    uint16_t num;
    uint16_t den;
} audio_resample_ratio_t;

// A 32-bit packed ratio lets request/status readers take an atomic snapshot
// without a lock or a partially updated numerator/denominator pair.
AUDIO_RS_CONSTEXPR uint32_t audio_rs_pack(uint16_t num, uint16_t den) {
    return ((uint32_t)num << 16) | (uint32_t)den;
}

AUDIO_RS_CONSTEXPR uint16_t audio_rs_num(uint32_t packed) {
    return (uint16_t)(packed >> 16);
}

AUDIO_RS_CONSTEXPR uint16_t audio_rs_den(uint32_t packed) {
    return (uint16_t)(packed & 0xFFFFu);
}

AUDIO_RS_CONSTEXPR uint8_t audio_rs_valid(uint16_t num, uint16_t den,
                                           uint16_t min_pct, uint16_t max_pct) {
    return den != 0 && num != 0 &&
           (uint32_t)num * 100u <= (uint32_t)den * max_pct &&
           (uint32_t)num * 100u >= (uint32_t)den * min_pct;
}

AUDIO_RS_CONSTEXPR uint8_t audio_rs_is_pending(uint32_t requested, uint32_t applied) {
    return requested != applied;
}

static inline uint32_t audio_rs_gcd(uint32_t a, uint32_t b) {
    while (b) {
        const uint32_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

#undef AUDIO_RS_CONSTEXPR
