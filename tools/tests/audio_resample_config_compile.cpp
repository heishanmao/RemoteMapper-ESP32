#include "audio_resample_config.h"

static_assert(audio_rs_pack(4, 3) == 0x00040003u, "Ratio packs atomically into one word");
static_assert(audio_rs_num(audio_rs_pack(7, 5)) == 7, "Packed requested numerator round trips");
static_assert(audio_rs_den(audio_rs_pack(7, 5)) == 5, "Packed requested denominator round trips");
static_assert(audio_rs_valid(1, 1, 100, 200), "Default passthrough ratio is valid");
static_assert(audio_rs_valid(4, 3, 100, 200), "An in-range ratio is accepted");
static_assert(!audio_rs_valid(1, 3, 100, 200), "Below-minimum ratio is rejected");
static_assert(!audio_rs_valid(0, 1, 100, 200), "Zero numerator is rejected");
static_assert(!audio_rs_valid(1, 0, 100, 200), "Zero denominator is rejected");
static_assert(audio_rs_is_pending(audio_rs_pack(4, 3), audio_rs_pack(1, 1)),
              "Requested/applied mismatch reports pending");
static_assert(!audio_rs_is_pending(audio_rs_pack(4, 3), audio_rs_pack(4, 3)),
              "Matching requested/applied ratios are settled");
