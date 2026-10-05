#include "runtime_diagnostics.h"
#include <Arduino.h>
#include <esp_heap_caps.h>

namespace {
uint32_t s_loop_count = 0;
uint32_t s_last_loop_start_us = 0;
uint32_t s_loop_gap_max_us = 0;
uint32_t s_loop_work_last_us = 0;
uint32_t s_loop_work_max_us = 0;
bool s_have_loop_start = false;
bool s_startup_tasks_ready = false;
}

uint32_t runtime_diagnostics_loop_begin() {
    const uint32_t now = micros();
    if (s_have_loop_start) {
        const uint32_t gap = now - s_last_loop_start_us;
        if (gap > s_loop_gap_max_us) s_loop_gap_max_us = gap;
    }
    s_have_loop_start = true;
    s_last_loop_start_us = now;
    ++s_loop_count;
    return now;
}

void runtime_diagnostics_loop_end(uint32_t started_us) {
    s_loop_work_last_us = micros() - started_us;
    if (s_loop_work_last_us > s_loop_work_max_us)
        s_loop_work_max_us = s_loop_work_last_us;
}

void runtime_diagnostics_set_startup_health(bool ready) { s_startup_tasks_ready = ready; }

void runtime_diagnostics_json(JsonObject out) {
    out["cpu_load_available"] = false;
    out["cpu_load_reason"] = "framework_run_time_stats_disabled";
    out["startup_tasks_ready"] = s_startup_tasks_ready;
    out["loop_count"] = s_loop_count;
    out["loop_work_last_us"] = s_loop_work_last_us;
    out["loop_work_max_us"] = s_loop_work_max_us;
    out["loop_gap_max_us"] = s_loop_gap_max_us;
    // Internal RAM is the constrained resource for task stacks, radio state
    // and web JSON. PSRAM totals alone cannot reveal heap fragmentation.
    multi_heap_info_t internal = {};
    heap_caps_get_info(&internal, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    out["internal_heap_free"] = internal.total_free_bytes;
    out["internal_heap_min_free"] = internal.minimum_free_bytes;
    out["internal_heap_largest_block"] = internal.largest_free_block;
}
