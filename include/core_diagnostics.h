#pragma once
#include <ArduinoJson.h>
#include <stdint.h>

// Task inventory and core placement for the dual-core ESP32-S3.
//
// What this reports, and why it is deliberately narrow:
//
//   * Each registered task's name, live priority, stack headroom and current
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
// Cost model: nothing runs on a timer. The snapshot is taken only when the
// endpoint is read, and every field comes from an O(1) lookup by handle.

#define CORE_DIAGNOSTICS_MAX_TASKS 16

// Register a task created elsewhere in this project. Call once, right after the
// task is created, and only with a handle the caller owns. A null handle (task
// creation failed) is accepted and reported as such.
void core_diagnostics_register(const char *name, void *handle, uint8_t expected_core);

// Emit one snapshot into `out`. Safe to call from any core.
void core_diagnostics_json(JsonObject out);