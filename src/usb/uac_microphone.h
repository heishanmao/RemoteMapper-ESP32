#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t completed, failed, claim_skips, recoveries, last_complete_ms;
    uint8_t endpoint;
    bool recovery_pending;
} uac_tx_stats_t;
void uac_microphone_get_stats(uac_tx_stats_t* stats);
void uac_microphone_get_fifo_rearm_stats(uint32_t* attempts, uint32_t* completions);
void uac_microphone_get_pcm_stats(uint32_t* samples, uint32_t* nonzero,
                                  uint64_t* absolute_sum, uint16_t* peak);
bool uac_microphone_init(void);
void uac_microphone_task(void);
bool uac_microphone_is_streaming(void);
void uac_microphone_get_control(uint8_t* mute, int16_t* volume);
uint8_t uac_microphone_get_alt(void);
uint32_t uac_microphone_get_stream_start_ms(void);

#ifdef __cplusplus
}
#endif
