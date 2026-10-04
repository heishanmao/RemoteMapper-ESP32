#include "core_diagnostics.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {

struct Entry {
    const char *name;
    TaskHandle_t handle;
    uint8_t expected_core;
    bool in_use;
};

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
Entry s_entries[CORE_DIAGNOSTICS_MAX_TASKS] = {};

// A task handle can be recycled after the task is deleted, so a live handle
// alone does not prove the task still exists. Detecting that reliably would
// need a kernel lock this module does not take. Instead pcTaskGetName() returns
// null for a dead task and the recorded name is used, so a stale entry shows up
// as an affinity of tskNO_AFFINITY rather than as a plausible-looking core.
const char *live_name(TaskHandle_t handle, const char *fallback) {
    const char *name = pcTaskGetName(handle);
    return name ? name : fallback;
}

}  // namespace

void core_diagnostics_register(const char *name, void *handle, uint8_t expected_core) {
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

void core_diagnostics_json(JsonObject out) {
    portENTER_CRITICAL(&s_mux);

    JsonArray tasks = out["tasks"].to<JsonArray>();
    int per_core[portNUM_PROCESSORS] = {0};
    int unregistered = 0;
    uint8_t count = 0;

    for (int i = 0; i < CORE_DIAGNOSTICS_MAX_TASKS; i++) {
        const Entry &e = s_entries[i];
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
        o["name"] = live_name(e.handle, e.name);
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

    portEXIT_CRITICAL(&s_mux);
}