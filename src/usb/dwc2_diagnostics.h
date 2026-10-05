#pragma once
#include <stdint.h>
#include <stdbool.h>

#if defined(REMOTEMAPPER_DWC2_DRIVER)
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    uint32_t bus_resets;
    uint32_t fifo_reset_failures;
    uint32_t iso_incomplete_events;
    uint32_t iso_retries;
    uint32_t iso_aborts;
    uint32_t first_iso_ms;
    uint32_t first_iso_ep;
    uint32_t first_iso_epctl;
    uint32_t first_iso_tsiz;
    uint32_t first_iso_dsts;
} remotemapper_dwc2_stats_t;

bool remotemapper_dwc2_close_failed(void);
bool remotemapper_dwc2_controller_faulted(void);
void remotemapper_dwc2_get_stats(remotemapper_dwc2_stats_t* stats);
#ifdef __cplusplus
}
#endif
#endif
