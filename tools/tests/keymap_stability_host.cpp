#include "key_config_storage.h"
#include "config_backup.h"
#include "Preferences.h"
#include "wifi/wifi_manager.h"
#include "wol_manager.h"
#include <cassert>
#include <thread>

static int keyboard_releases = 0;
static int keyboard_taps = 0;
static int voice_holds = 0;
static int voice_releases = 0;
static uint8_t last_tap_modifier = 0;
static uint8_t last_tap_key_code = 0;
static int wol_sends = 0;
key_mapper_engine_t g_key_engine{};
static String wifi_nvs_ssid = "old-ssid";
static String wifi_runtime_ssid = "old-ssid";
static wifi_policy_t wifi_runtime_policy = WIFI_POLICY_ALWAYS_ON;
static int wifi_restore_calls = 0;

void app_log(const char*, const char*, ...) {}
void led_indicator_set_layer_color(uint32_t) {}
extern "C" bool wol_manager_parse_mac(const char*, uint8_t out[6]) {
    for (int i = 0; i < 6; ++i) out[i] = 0;
    return true;
}
extern "C" void wol_manager_mac_to_str(const uint8_t*, char* out, size_t n) {
    if (n) out[0] = 0;
}
extern "C" bool wol_manager_send(const uint8_t*, uint16_t) { ++wol_sends; return true; }
extern "C" bool wol_manager_send_str(const char*, uint16_t) { return true; }
extern "C" void usb_composite_force_release_all(const char*) {
    key_engine_release_all(&g_key_engine, 100);
}
extern "C" uint32_t config_manager_get_version(void) { return 1; }
extern "C" wifi_policy_t wifi_manager_get_policy(void) { return wifi_runtime_policy; }
extern "C" uint32_t wifi_manager_get_timeout_min(void) { return 5; }
extern "C" bool wifi_manager_get_timeout_enabled(void) { return false; }
String wifi_manager_get_sta_ssid(void) { return wifi_runtime_ssid; }
String wifi_manager_get_sta_pass(void) { return "old-pass"; }
String wifi_manager_get_ap_pass(void) { return ""; }
extern "C" bool wifi_manager_validate_backup(const String& ssid, const String& pass,
                                               const String& ap, wifi_policy_t policy, uint32_t timeout) {
    return ssid.length() <= 32 && pass.length() <= 64 && (ap.length() == 0 || ap.length() >= 8) &&
           policy <= WIFI_POLICY_DISABLED && timeout == 5;
}
extern "C" bool wifi_manager_restore_backup(const String& ssid, const String& pass, const String& ap,
                                              wifi_policy_t policy, uint32_t timeout, bool enabled,
                                              bool publish_runtime) {
    (void)pass; (void)ap; (void)timeout; (void)enabled;
    ++wifi_restore_calls;
    wifi_nvs_ssid = ssid;
    if (publish_runtime) {
        wifi_runtime_ssid = ssid;
        wifi_runtime_policy = policy;
    }
    return true;
}
extern "C" void wifi_manager_apply_backup_state(const String& ssid, wifi_policy_t policy,
                                                   uint32_t, bool) {
    wifi_runtime_ssid = ssid;
    wifi_runtime_policy = policy;
}
extern "C" const char* wifi_manager_policy_str(wifi_policy_t) { return "test"; }

static void output(const key_action_t* action) {
    if (action->type == ACTION_KEYBOARD_RELEASE) ++keyboard_releases;
    if (action->type == ACTION_KEYBOARD_TAP) {
        ++keyboard_taps;
        last_tap_modifier = action->modifier;
        last_tap_key_code = action->key_code;
    }
    if (action->type == ACTION_VOICE_HOLD) ++voice_holds;
    if (action->type == ACTION_VOICE_RELEASE) ++voice_releases;
}

int main() {
    key_mapper_engine_t& engine = g_key_engine;
    key_engine_init(&engine, output);

    // Default voice is a real-time hold when no long/double gesture delays it.
    key_engine_feed_key(&engine, MI_KEY_VOICE, true, 1);
    key_engine_feed_key(&engine, MI_KEY_VOICE, false, 2);
    assert(voice_holds == 1 && voice_releases == 1);

    // A long voice hold must receive exactly one release, including duplicate UPs.
    key_binding_t voice_long{};
    voice_long.source_vk = MI_KEY_POWER;
    voice_long.has_click = true;
    voice_long.click_action = { ACTION_KEYBOARD_TAP, USB_MOD_LALT, USB_KEY_TAB, 0, 0 };
    voice_long.has_long = true;
    voice_long.long_ms = 100;
    voice_long.long_action = { ACTION_VOICE_HOLD, USB_MOD_LALT, USB_KEY_H, 0, 0 };
    assert(key_engine_set_binding(&engine, &voice_long));
    key_engine_feed_key(&engine, MI_KEY_POWER, true, 10);
    key_engine_tick(&engine, 110);
    key_engine_feed_key(&engine, MI_KEY_POWER, false, 111);
    key_engine_feed_key(&engine, MI_KEY_POWER, false, 112);
    assert(voice_holds == 2 && voice_releases == 2);

    // Delayed clicks cannot start an unpaired voice hold after physical UP.
    key_binding_t delayed_voice{};
    delayed_voice.source_vk = MI_KEY_POWER;
    delayed_voice.has_click = true;
    delayed_voice.click_action = { ACTION_VOICE_HOLD, USB_MOD_LALT, USB_KEY_H, 0, 0 };
    delayed_voice.has_long = true;
    delayed_voice.long_ms = 500;
    delayed_voice.long_action.type = ACTION_NONE;
    assert(key_engine_set_binding(&engine, &delayed_voice));
    int taps_before_voice_click = keyboard_taps;
    key_engine_feed_key(&engine, MI_KEY_POWER, true, 200);
    key_engine_feed_key(&engine, MI_KEY_POWER, false, 210);
    assert(keyboard_taps == taps_before_voice_click + 1);
    assert(last_tap_modifier == USB_MOD_LALT && last_tap_key_code == USB_KEY_H);
    assert(voice_holds == 2 && voice_releases == 2);

    delayed_voice.has_double = true;
    delayed_voice.double_ms = 250;
    delayed_voice.double_action.type = ACTION_NONE;
    assert(key_engine_set_binding(&engine, &delayed_voice));
    taps_before_voice_click = keyboard_taps;
    key_engine_feed_key(&engine, MI_KEY_POWER, true, 300);
    key_engine_feed_key(&engine, MI_KEY_POWER, false, 310);
    key_engine_tick(&engine, 560);
    assert(keyboard_taps == taps_before_voice_click + 1);
    assert(last_tap_modifier == USB_MOD_LALT && last_tap_key_code == USB_KEY_H);
    assert(voice_holds == 2 && voice_releases == 2);

    delayed_voice.double_action = { ACTION_VOICE_HOLD, USB_MOD_LALT, USB_KEY_H, 0, 0 };
    assert(key_engine_set_binding(&engine, &delayed_voice));
    taps_before_voice_click = keyboard_taps;
    key_engine_feed_key(&engine, MI_KEY_POWER, true, 600);
    key_engine_feed_key(&engine, MI_KEY_POWER, false, 610);
    key_engine_feed_key(&engine, MI_KEY_POWER, true, 700);
    key_engine_feed_key(&engine, MI_KEY_POWER, false, 710);
    assert(keyboard_taps == taps_before_voice_click + 1);
    assert(last_tap_modifier == USB_MOD_LALT && last_tap_key_code == USB_KEY_H);
    assert(voice_holds == 2 && voice_releases == 2);

    // Keep the original stability assertions scoped to their own scenarios.
    keyboard_taps = 0;

    key_binding_t before{};
    assert(key_engine_get_binding(&engine, MI_KEY_OK, &before));

    key_mapper_engine_t candidate{};
    key_engine_init(&candidate, nullptr);
    key_binding_t changed{};
    assert(key_engine_get_binding(&candidate, MI_KEY_OK, &changed));
    changed.click_action.key_code = USB_KEY_ESCAPE;
    assert(key_engine_set_binding(&candidate, &changed));
    String valid = key_config_to_json(&candidate);

    assert(!key_config_parse_json("{\"layers\":[]}", &candidate));
    key_binding_t after_bad{};
    assert(key_engine_get_binding(&engine, MI_KEY_OK, &after_bad));
    assert(after_bad.click_action.key_code == before.click_action.key_code);

    Preferences::fail_writes = true;
    assert(!key_config_storage_import(&engine, valid));
    Preferences::fail_writes = false;
    assert(engine.layers[0].bindings[6].click_action.key_code == before.click_action.key_code);

    key_engine_feed_key(&engine, MI_KEY_UP, true, 1);
    assert(key_config_storage_import(&engine, valid));
    assert(engine.layers[0].bindings[6].click_action.key_code == USB_KEY_ESCAPE);
    assert(keyboard_releases == 1);

    key_binding_t delayed{};
    delayed.source_vk = MI_KEY_POWER;
    delayed.has_click = true;
    delayed.click_action.type = ACTION_WOL;
    delayed.has_long = true;
    delayed.long_ms = 600;
    delayed.long_action.type = ACTION_NONE;
    assert(key_engine_set_binding(&engine, &delayed));
    key_engine_feed_key(&engine, MI_KEY_POWER, true, 10);
    key_engine_release_all(&engine, 11);
    assert(wol_sends == 0);
    assert(keyboard_taps == 0);

    delayed.click_action.type = ACTION_KEYBOARD_TAP;
    assert(key_engine_set_binding(&engine, &delayed));
    key_engine_feed_key(&engine, MI_KEY_POWER, true, 12);
    key_engine_release_all(&engine, 13);
    assert(keyboard_taps == 0);

    key_layer_t layers[MAX_LAYERS];
    key_engine_lock_state(&engine);
    for (size_t i = 0; i < MAX_LAYERS; ++i) layers[i] = engine.layers[i];
    key_engine_unlock_state(&engine);
    key_binding_t oneshot_hold{};
    oneshot_hold.source_vk = MI_KEY_UP;
    oneshot_hold.has_click = true;
    oneshot_hold.click_action.type = ACTION_KEYBOARD_HOLD;
    oneshot_hold.click_action.key_code = USB_KEY_UP;
    layers[3].type = LAYER_TYPE_ONESHOT;
    layers[3].binding_count = 1;
    layers[3].bindings[0] = oneshot_hold;
    assert(key_engine_replace_layout(&engine, layers, 3));
    int releases_before_oneshot = keyboard_releases;
    key_engine_feed_key(&engine, MI_KEY_UP, true, 20);
    key_engine_release_all(&engine, 21);
    assert(keyboard_releases == releases_before_oneshot + 1);
    assert(key_engine_get_active_layer(&engine) == 0);

    String invalid_backup = "{\"wifi\":{\"sta_ssid\":\"new-ssid\",\"sta_pass\":\"pass\",\"ap_pass\":\"\",\"policy\":2,\"timeout_min\":5},\"keymap\":{\"layers\":[]}}";
    int restore_calls_before = wifi_restore_calls;
    String import_error = config_backup_import(invalid_backup);
    assert(import_error.length() > 0);
    assert(wifi_restore_calls == restore_calls_before);
    assert(wifi_nvs_ssid == "old-ssid" && wifi_runtime_ssid == "old-ssid");

    String valid_backup = "{\"wifi\":{\"sta_ssid\":\"new-ssid\",\"sta_pass\":\"pass\",\"ap_pass\":\"\",\"policy\":2,\"timeout_min\":5},\"keymap\":";
    valid_backup += valid;
    valid_backup += "}";
    Preferences::fail_writes = true;
    import_error = config_backup_import(valid_backup);
    Preferences::fail_writes = false;
    assert(import_error.length() > 0);
    assert(wifi_restore_calls == restore_calls_before + 2);
    assert(wifi_nvs_ssid == "old-ssid" && wifi_runtime_ssid == "old-ssid");
    assert(wifi_runtime_policy == WIFI_POLICY_ALWAYS_ON);

    std::thread feed([]() {
        for (uint32_t i = 1; i < 10000; ++i) {
            key_engine_feed_key(&engine, MI_KEY_UP, (i & 1) != 0, i);
            key_engine_tick(&engine, i);
        }
    });
    for (int i = 0; i < 200; ++i) {
        key_layer_t snapshot[MAX_LAYERS];
        key_engine_lock_state(&engine);
        for (size_t layer = 0; layer < MAX_LAYERS; ++layer) snapshot[layer] = engine.layers[layer];
        key_engine_unlock_state(&engine);
        assert(key_engine_replace_layout(&engine, snapshot, 0));
    }
    feed.join();
    assert(engine.active_layer < MAX_LAYERS);
    return 0;
}
