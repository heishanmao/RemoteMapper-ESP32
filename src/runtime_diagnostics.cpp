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
RuntimeStageTimings s_stage_timings;
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

uint32_t runtime_diagnostics_stage_end(RuntimeStage stage, uint32_t started_us) {
    const uint32_t ended_us = micros();
    s_stage_timings.record(stage, started_us, ended_us, millis());
    return ended_us;
}

void runtime_diagnostics_json(JsonObject out) {
    out["cpu_load_available"] = false;
    out["cpu_load_reason"] = "framework_run_time_stats_disabled";
    out["startup_tasks_ready"] = s_startup_tasks_ready;
    out["loop_count"] = s_loop_count;
    out["loop_work_last_us"] = s_loop_work_last_us;
    out["loop_work_max_us"] = s_loop_work_max_us;
    out["loop_gap_max_us"] = s_loop_gap_max_us;
    out["stage_budget_us"] = static_cast<uint32_t>(RuntimeStageTimings::budget_us);
    JsonObject stages = out["stages"].to<JsonObject>();
    static const char *names[] = {"usb", "wifi", "web", "cli", "maintenance", "log"};
    static_assert(sizeof(names) / sizeof(names[0]) == RuntimeStageTimings::count,
                  "Every measured loop stage needs a diagnostic name");
    for (uint8_t i = 0; i < RuntimeStageTimings::count; ++i) {
        const RuntimeStageTiming &timing = s_stage_timings.get(static_cast<RuntimeStage>(i));
        JsonObject entry = stages[names[i]].to<JsonObject>();
        entry["calls"] = timing.calls;
        entry["last_us"] = timing.last_us;
        entry["max_us"] = timing.max_us;
        entry["over_budget"] = timing.over_budget;
        entry["max_at_ms"] = timing.max_at_ms;
    }
    // Internal RAM is the constrained resource for task stacks, radio state
    // and web JSON. PSRAM totals alone cannot reveal heap fragmentation.
    multi_heap_info_t internal = {};
    heap_caps_get_info(&internal, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    out["internal_heap_free"] = internal.total_free_bytes;
    out["internal_heap_min_free"] = internal.minimum_free_bytes;
    out["internal_heap_largest_block"] = internal.largest_free_block;
}
