#pragma once

#include <ArduinoJson.h>
#include <stdint.h>

// Loop timings are wall-clock latency, not CPU utilization. The loop is the
// sole writer; HTTP readers run on that same task.
uint32_t runtime_diagnostics_loop_begin();
void runtime_diagnostics_loop_end(uint32_t started_us);
void runtime_diagnostics_set_startup_health(bool ready);
void runtime_diagnostics_json(JsonObject out);
