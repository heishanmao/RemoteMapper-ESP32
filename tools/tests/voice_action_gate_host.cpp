#include "ble/voice_action_gate.h"
#include "keymap/key_state_machine.h"
#include <Arduino.h>

#include "../../src/audio/audio_pipeline.c"
#include "../../src/audio/audio_ring_buffer.c"
#include "../../src/audio/adpcm_decoder.c"
#include "../../src/audio/audio_filter.c"
#include "../../src/audio/audio_agc.c"

#include <stdlib.h>

static remotemapper::ble::VoiceActionGate gate;
static unsigned emitted_holds;
static unsigned emitted_releases;
static unsigned emitted_other;
static audio_pipeline_t pipeline;
static uint32_t test_now_ms;

void app_log(const char*, const char*, ...) {}
void led_indicator_set_layer_color(uint32_t) {}
uint32_t millis(void) { return test_now_ms; }
extern "C" bool wol_manager_parse_mac(const char*, uint8_t out[6]) {
    for (int i = 0; i < 6; ++i) out[i] = 0;
    return true;
}
extern "C" void wol_manager_mac_to_str(const uint8_t*, char* out, size_t n) {
    if (n) out[0] = 0;
}
extern "C" bool wol_manager_send(const uint8_t*, uint16_t) { return true; }
extern "C" bool wol_manager_send_str(const char*, uint16_t) { return true; }

static void production_gate_adapter(const key_action_t* action) {
    if (action->type == ACTION_VOICE_HOLD) {
        if (gate.press()) {
            ++emitted_holds;
            audio_pipeline_start_session(&pipeline, 0);
        }
    } else if (action->type == ACTION_VOICE_RELEASE) {
        if (gate.release()) {
            ++emitted_releases;
            audio_pipeline_stop_session(&pipeline);
        }
    } else {
        ++emitted_other;
    }
}

static void require(bool condition) {
    if (!condition) abort();
}

int main() {
    audio_pipeline_init(&pipeline);
    key_mapper_engine_t engine = {};
    key_engine_init(&engine, production_gate_adapter);

    using remotemapper::ble::voice_link_ready;
    require(!voice_link_ready(true, true, false, 0)); // CAPS unknown
    require(!voice_link_ready(true, true, true, 0x01)); // 8 kHz only
    require(!voice_link_ready(false, true, true, 0x02)); // CAPS before handshake
    require(!voice_link_ready(true, false, true, 0x02)); // no command handle

    // Early press is rejected. Becoming ready while it remains physically down
    // does not replay the Alt/audio action; its UP also emits no release.
    gate.reset_connection();
    key_engine_feed_key(&engine, MI_KEY_VOICE, true, 10);
    require(emitted_holds == 0 && !gate.active());
    require(voice_link_ready(true, true, true, 0x02));
    gate.set_ready(true);
    require(emitted_holds == 0 && !gate.active());
    key_engine_feed_key(&engine, MI_KEY_VOICE, false, 20);
    require(emitted_releases == 0 && !gate.active());

    // A fresh ready press starts immediately and the normal UP releases it.
    key_engine_feed_key(&engine, MI_KEY_VOICE, true, 30);
    require(emitted_holds == 1 && gate.active());
    key_engine_feed_key(&engine, MI_KEY_VOICE, false, 40);
    require(emitted_releases == 1 && !gate.active());

    // A MIC_OPEN write failure and disconnect both require an explicit local
    // release; invalidation clears active state but keeps a healthy link ready
    // so a later physical press can retry.
    key_engine_feed_key(&engine, MI_KEY_VOICE, true, 50);
    require(gate.active() && emitted_holds == 2);
    require(gate.invalidate());
    require(!gate.active() && gate.ready());
    audio_pipeline_stop_session(&pipeline);
    ++emitted_releases; // production invalidation performs the matching release
    key_engine_feed_key(&engine, MI_KEY_VOICE, false, 60);
    require(emitted_releases == 2); // later UP is suppressed, never double-releases

    key_engine_feed_key(&engine, MI_KEY_VOICE, true, 70);
    require(gate.active() && emitted_holds == 3);
    gate.set_ready(false);
    require(gate.invalidate());
    require(!gate.active() && !gate.ready());
    audio_pipeline_stop_session(&pipeline);
    ++emitted_releases; // disconnect cleanup
    key_engine_feed_key(&engine, MI_KEY_VOICE, false, 80);
    require(emitted_releases == 3);
    require(!voice_link_ready(false, true, true, 0x02)); // new connection cannot inherit CAPS

    // The gate is action-specific: an ordinary mapping on the same physical
    // voice button still reaches the output callback while voice is not ready.
    key_binding_t binding = {};
    require(key_engine_get_binding(&engine, MI_KEY_VOICE, &binding));
    binding.click_action.type = ACTION_KEYBOARD_TAP;
    require(key_engine_set_binding(&engine, &binding));
    key_engine_feed_key(&engine, MI_KEY_VOICE, true, 90);
    key_engine_feed_key(&engine, MI_KEY_VOICE, false, 100);
    require(emitted_other == 1);

    // Exercise both production event orders. HOGP first emits one hold and
    // starts audio; ATVV START then prepares decoder state and must restart the
    // local session without re-emitting the hold. In the reverse order, ATVV
    // starts both once and the duplicate HOGP DOWN adds nothing.
    key_binding_t voice_binding = {};
    require(key_engine_get_binding(&engine, MI_KEY_VOICE, &voice_binding));
    voice_binding.click_action.type = ACTION_VOICE_HOLD;
    require(key_engine_set_binding(&engine, &voice_binding));
    gate.set_ready(true);

    test_now_ms = 110;
    key_engine_feed_key(&engine, MI_KEY_VOICE, true, test_now_ms); // HOGP DOWN
    require(emitted_holds == 4 && audio_pipeline_is_active(&pipeline));
    audio_pipeline_prepare_session(&pipeline, 17); // ATVV START resets active
    key_engine_feed_key(&engine, MI_KEY_VOICE, true, 111); // shared slot: no second hold
    if (remotemapper::ble::voice_action_needs_pipeline_start(
                gate.active(), audio_pipeline_is_active(&pipeline))) {
        audio_pipeline_start_session(&pipeline, 17);
    }
    require(emitted_holds == 4 && audio_pipeline_is_active(&pipeline));
    key_engine_feed_key(&engine, MI_KEY_VOICE, false, 120);
    require(emitted_releases == 4 && !audio_pipeline_is_active(&pipeline));

    audio_pipeline_prepare_session(&pipeline, 18); // ATVV START preparation
    key_engine_feed_key(&engine, MI_KEY_VOICE, true, 130);
    require(emitted_holds == 5 && audio_pipeline_is_active(&pipeline));
    key_engine_feed_key(&engine, MI_KEY_VOICE, true, 131); // HOGP DOWN duplicate
    require(emitted_holds == 5 && audio_pipeline_is_active(&pipeline));
    key_engine_feed_key(&engine, MI_KEY_VOICE, false, 140);
    require(emitted_releases == 5 && !audio_pipeline_is_active(&pipeline));
    return 0;
}
