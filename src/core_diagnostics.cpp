#include "core_diagnostics.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "app_config.h"

namespace {

struct Entry {
    const char *name;
    TaskHandle_t handle;
    int8_t expected_core;
    bool in_use;
};

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
Entry s_entries[CORE_DIAGNOSTICS_MAX_TASKS] = {};

// Entries refer only to permanent tasks. FreeRTOS task-query functions do not
// validate deleted handles; this registry lock protects entries, not task
// lifetime. Callers must keep both the task and its name alive for the module's
// lifetime. Temporary tasks need separate lifecycle coordination before they
// can be registered here.

}  // namespace

void core_diagnostics_register(const char *name, void *handle, int8_t expected_core) {
    const TaskHandle_t task = (TaskHandle_t)handle;
    if (!task) return;

    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < CORE_DIAGNOSTICS_MAX_TASKS; i++) {
        if (s_entries[i].in_use) continue;
        s_entries[i].name = name;
        s_entries[i].handle = task;
        s_entries[i].expected_core = expected_core;
        s_entries[i].in_use = true;
        break;
    }
    portEXIT_CRITICAL(&s_mux);
}

void core_diagnostics_discover_framework_tasks(void) {
    // Owned by loopTask. Neither USB.end nor NimBLEDevice::deinit is used by
    // the bridge, hence these handles are not deleted/recycled after discovery.
    static bool usb_found = false;
    static bool ble_found = false;
    static uint32_t last_check_ms = 0;
    if (usb_found && ble_found) return;
    const uint32_t now_ms = millis();
    if (now_ms - last_check_ms < 1000) return;
    last_check_ms = now_ms;
    if (!usb_found) {
        TaskHandle_t task = xTaskGetHandle("usbd");
        if (task) {
            core_diagnostics_register("usbd", task, -1);
            usb_found = true;
        }
    }
    if (!ble_found) {
        TaskHandle_t task = xTaskGetHandle("nimble_host");
        if (task) {
            core_diagnostics_register("nimble_host", task, TASK_CORE_BLE);
            ble_found = true;
        }
    }
}

void core_diagnostics_json(JsonObject out) {
    Entry snapshot[CORE_DIAGNOSTICS_MAX_TASKS];
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < CORE_DIAGNOSTICS_MAX_TASKS; i++) {
        snapshot[i] = s_entries[i];
    }
    portEXIT_CRITICAL(&s_mux);

    // Stack headroom scans and JSON allocation can take significant time.
    // Neither may hold the registry lock or extend its interrupt-off window.
    JsonArray tasks = out["tasks"].to<JsonArray>();
    int per_core[portNUM_PROCESSORS] = {0};
    int unregistered = 0;
    uint8_t count = 0;

    for (int i = 0; i < CORE_DIAGNOSTICS_MAX_TASKS; i++) {
        const Entry &e = snapshot[i];
        if (!e.in_use) continue;
        count++;

        const BaseType_t affinity = xTaskGetAffinity(e.handle);
        const UBaseType_t prio = uxTaskPriorityGet(e.handle);
        const UBaseType_t stack_free = uxTaskGetStackHighWaterMark(e.handle);

        int core;
        if (affinity == tskNO_AFFINITY) {
            core = -1;  // free to migrate
        } else if (affinity >= 0 && affinity < portNUM_PROCESSORS) {
            core = (int)affinity;
            per_core[affinity]++;
        } else {
            core = -2;  // unexpected value
        }

        JsonObject o = tasks.add<JsonObject>();
        o["name"] = e.name ? e.name : "unnamed";
        o["prio"] = (int)prio;
        o["stack_free"] = (int)stack_free;
        o["core"] = core;
        o["expected_core"] = (int)e.expected_core;
        // The whole point of the module: if a framework upgrade or an edit moves
        // a task off its declared core, this is where it becomes visible.
        o["as_expected"] = (core == (int)e.expected_core);
        if (core < 0) unregistered++;
    }

    JsonArray cores = out["cores"].to<JsonArray>();
    for (int c = 0; c < portNUM_PROCESSORS; c++) {
        JsonObject o = cores.add<JsonObject>();
        o["core"] = c;
        o["pinned_tasks"] = per_core[c];
    }

    out["registered"] = (int)count;
    out["unpinned"] = unregistered;
    out["slots"] = CORE_DIAGNOSTICS_MAX_TASKS;
    // Stated explicitly so a reader does not mistake this for a load report.
    out["cpu_load_available"] = false;
    out["cpu_load_reason"] = "framework_run_time_stats_disabled";
}
