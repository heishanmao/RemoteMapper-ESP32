#pragma once
#include <ArduinoJson.h>
#include <stdint.h>

// Task inventory and core placement for the dual-core ESP32-S3.
//
// What this reports, and why it is deliberately narrow:
//
//   * Each registered task's recorded name, live priority, stack headroom and current
//     core affinity, queried through the handle it was created with.
//   * Per-core counts of pinned tasks.
//
// What it cannot report: CPU load percentages, and an enumeration of tasks the
// project does not create. Both need FreeRTOS run-time stats and the task-list
// walk, which this framework build does not provide: sdkconfig.h contains
// neither CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS nor
// CONFIG_FREERTOS_USE_TRACE_FACILITY, so configGENERATE_RUN_TIME_STATS is
// undefined and the prebuilt libfreertos.a exports no uxTaskGetSystemState.
// The panic-oriented uxTaskGetSnapshotAll() is present but documented as unsafe
// while the scheduler runs.
//
// Registering only our own tasks is a deliberate limit, not an oversight: it
// makes the output a direct check of the pinning contract in app_config.h
// rather than a partial view of kernel-internal tasks nobody here controls.
// Per-core load balance remains unmeasured; that needs a framework rebuild with
// run-time stats, or a hardware PMU counter.
//
// Cost model: nothing runs on a timer. A fixed-size registry copy is protected
// by a short critical section when the endpoint is read. Task queries and JSON
// construction run outside that section; stack headroom scans can take time
// proportional to the task's untouched stack. The live task fields are read
// individually and are not an atomic scheduler snapshot.

#define CORE_DIAGNOSTICS_MAX_TASKS 16

// Register a permanent task created elsewhere in this project. Call once,
// right after creation. The caller must keep the task and name storage alive
// for this module's lifetime: querying a deleted or recycled handle is unsafe,
// and pcTaskGetName() is not a handle-validity check. There is deliberately no
// unregister API; supporting temporary tasks would require coordination with
// in-flight readers before deleting the task. Null handles are ignored, and
// registration beyond CORE_DIAGNOSTICS_MAX_TASKS is ignored.
void core_diagnostics_register(const char *name, void *handle, uint8_t expected_core);

// Emit one snapshot into `out`. Safe to call from any core.
void core_diagnostics_json(JsonObject out);
