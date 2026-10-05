#include "hid_diagnostics.h"
#include "uac_microphone.h"
#include <Arduino.h>
#include "tusb.h"
#include "soc/usb_struct.h"
#include <Preferences.h>

namespace {
constexpr unsigned EVENT_COUNT = 24;
constexpr unsigned STAGES = 12;
// Hook ABI: attempt, uninitialized, mutex_timeout, not_ready, submitted,
// submit_rejected, completion_timeout, wait_success, callback,
// previous_pending, callback_rejected, lifecycle_cancelled.
const char* const names[STAGES] = {"attempt", "uninitialized", "mutex_timeout",
    "not_ready", "submitted", "submit_rejected", "completion_timeout",
    "wait_success", "callback", "previous_pending", "callback_rejected",
    "lifecycle_cancelled"};
struct Registers {
    uint32_t empty_mask, ctl, size, interrupt, fifo_free, all_interrupt,
        all_mask, global_interrupt, global_mask, status;
    uint8_t endpoint;
    bool ready;
};
struct Event { uint32_t ms; uint8_t stage, report, core; };
struct Trace {
    uint32_t counts[STAGES];
    Event events[EVENT_COUNT];
    uint32_t total;
    Registers timeout_regs;
    uint32_t timeout_ms;
};
struct Fault { Trace trace; Registers regs, audio_regs; uac_tx_stats_t audio; uint32_t ms, reason, count; };
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
Trace live = {};
Fault fault = {};
Fault first_audio_fault = {};

uint8_t hid_endpoint() {
    const uint8_t* config = tud_descriptor_configuration_cb(0);
    if (!config || config[0] < 9) return 0;
    const unsigned length = config[2] | (unsigned(config[3]) << 8);
    bool hid = false;
    for (unsigned off = 0; off + 2 <= length;) {
        const uint8_t* d = config + off;
        if (d[0] < 2 || off + d[0] > length) break;
        if (d[1] == TUSB_DESC_INTERFACE && d[0] >= 9)
            hid = d[5] == TUSB_CLASS_HID;
        if (hid && d[1] == TUSB_DESC_ENDPOINT && d[0] >= 7 &&
            (d[2] & 0x80) && (d[3] & 3) == TUSB_XFER_INTERRUPT)
            return d[2];
        off += d[0];
    }
    return 0;
}

Registers registers(uint8_t endpoint = 0) {
    Registers r = {};
    r.endpoint = endpoint ? endpoint : hid_endpoint();
    r.ready = tud_hid_n_ready(0);
    r.empty_mask = USB0.dtknqr4_fifoemptymsk;
    r.all_interrupt = USB0.daint;
    r.all_mask = USB0.daintmsk;
    r.global_interrupt = USB0.gintsts;
    r.global_mask = USB0.gintmsk;
    r.status = USB0.dsts;
    const unsigned ep = r.endpoint & 0x7f;
    if (r.endpoint && ep < USB_IN_EP_NUM) {
        const auto& hw = USB0.in_ep_reg[ep];
        r.ctl = hw.diepctl;
        r.size = hw.dieptsiz;
        r.interrupt = hw.diepint;
        r.fifo_free = hw.dtxfsts;
    }
    // Sequential observations, not an atomic hardware snapshot. No FIFO reads,
    // status-pop reads, or register writes are performed.
    return r;
}

void emit_regs(JsonObject out, const Registers& r) {
    out["endpoint"] = r.endpoint; out["hid_ready"] = r.ready;
    out["fifo_empty_mask"] = r.empty_mask; out["diepctl"] = r.ctl;
    out["dieptsiz"] = r.size; out["diepint"] = r.interrupt;
    out["dtxfsts"] = r.fifo_free; out["daint"] = r.all_interrupt;
    out["daintmsk"] = r.all_mask; out["gintsts"] = r.global_interrupt;
    out["gintmsk"] = r.global_mask; out["dsts"] = r.status;
}
void emit_trace(JsonObject out, const Trace& t) {
    for (unsigned i = 0; i < STAGES; ++i) out[names[i]] = t.counts[i];
    out["timeout_ms"] = t.timeout_ms;
    emit_regs(out["timeout_registers"].to<JsonObject>(), t.timeout_regs);
    JsonArray events = out["events"].to<JsonArray>();
    const unsigned n = t.total < EVENT_COUNT ? t.total : EVENT_COUNT;
    for (unsigned i = 0; i < n; ++i) {
        const Event& e = t.events[(t.total - n + i) % EVENT_COUNT];
        JsonObject item = events.add<JsonObject>();
        item["ms"] = e.ms; item["stage"] = names[e.stage];
        item["report"] = e.report; item["core"] = e.core;
    }
}
}

// Called only from USB/application tasks by the reproducible framework patch.
// No allocation, logging, waiting or USB writes on the normal send path.
extern "C" void remotemapper_hid_trace(uint8_t stage, uint8_t report) {
    if (stage >= STAGES) return;
    const Event e = {millis(), stage, report, uint8_t(xPortGetCoreID())};
    Registers timeout = {};
    if (stage == 6) timeout = registers();
    portENTER_CRITICAL(&mux);
    ++live.counts[stage];
    live.events[live.total++ % EVENT_COUNT] = e;
    if (stage == 6) { live.timeout_ms = e.ms; live.timeout_regs = timeout; }
    portEXIT_CRITICAL(&mux);
}

void hid_diagnostics_capture(uint32_t reason) {
    const Registers r = registers();
    uac_tx_stats_t audio = {};
    uac_microphone_get_stats(&audio);
    const Registers ar = registers(audio.endpoint);
    const uint32_t now = millis();
    portENTER_CRITICAL(&mux);
    fault.trace = live;
    fault.regs = r; fault.ms = now; fault.reason = reason; ++fault.count;
    fault.audio_regs = ar; fault.audio = audio;
    if (reason == 1 && first_audio_fault.count == 0) first_audio_fault = fault;
    portEXIT_CRITICAL(&mux);
}

void hid_diagnostics_persist_first_audio_fault() {
    Fault saved;
    portENTER_CRITICAL(&mux);
    saved = first_audio_fault;
    portEXIT_CRITICAL(&mux);
    if (!saved.count) return;
    Preferences p;
    if (p.begin("usb_fault", false)) {
        p.putBytes("first_audio", &saved, sizeof(saved));
        p.end();
    }
}

void hid_diagnostics_json(JsonObject out) {
    Trace current;
    Fault saved;
    portENTER_CRITICAL(&mux);
    current = live; saved = fault;
    portEXIT_CRITICAL(&mux);
    out["schema"] = 2;
    emit_trace(out["live"].to<JsonObject>(), current);
    JsonObject last = out["last_fault"].to<JsonObject>();
    last["count"] = saved.count; last["ms"] = saved.ms;
    last["reason"] = saved.reason;
    if (saved.count) {
        emit_regs(last["registers"].to<JsonObject>(), saved.regs);
        emit_regs(last["audio_registers"].to<JsonObject>(), saved.audio_regs);
        last["audio_completed"] = saved.audio.completed;
        last["audio_failed"] = saved.audio.failed;
        last["audio_last_complete_ms"] = saved.audio.last_complete_ms;
        emit_trace(last["trace"].to<JsonObject>(), saved.trace);
    }
    static Fault persisted = {};
    static bool persisted_loaded = false;
    if (!persisted_loaded) {
        Preferences p;
        if (p.begin("usb_fault", true)) {
            if (p.getBytesLength("first_audio") == sizeof(persisted))
                p.getBytes("first_audio", &persisted, sizeof(persisted));
            p.end();
        }
        persisted_loaded = true;
    }
    if (persisted.count && persisted.reason == 1) {
        JsonObject prior = out["persisted_first_audio_fault"].to<JsonObject>();
        prior["ms"] = persisted.ms;
        prior["audio_completed"] = persisted.audio.completed;
        prior["audio_failed"] = persisted.audio.failed;
        prior["audio_last_complete_ms"] = persisted.audio.last_complete_ms;
        emit_regs(prior["audio_registers"].to<JsonObject>(), persisted.audio_regs);
        emit_trace(prior["trace"].to<JsonObject>(), persisted.trace);
    }
}
