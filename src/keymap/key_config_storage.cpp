#include "key_config_storage.h"
#include "log/app_log.h"
#include "wol_manager.h"
#include "usb/usb_composite.h"
#include <Preferences.h>
#include <ArduinoJson.h>

void key_config_storage_init(key_mapper_engine_t *engine) {
    if (!key_config_storage_load(engine)) {
        key_engine_load_defaults(engine);
        app_log("KEYMAP", "Loaded safe factory defaults (%u bindings)", (unsigned int)engine->layers[0].binding_count);
    } else {
        app_log("KEYMAP", "Loaded custom multi-layer keymap from NVS (active layer: %u)", (unsigned int)engine->active_layer);
    }
}

static String key_config_layout_to_json(const key_layer_t layers[MAX_LAYERS], uint8_t active_layer) {
    JsonDocument doc;
    doc["active_layer"] = active_layer;
    JsonArray layer_arr = doc["layers"].to<JsonArray>();

    for (size_t l = 0; l < MAX_LAYERS; l++) {
        const key_layer_t *layer = &layers[l];
        JsonObject layer_obj = layer_arr.add<JsonObject>();
        layer_obj["id"] = (int)l;
        layer_obj["name"] = layer->name;
        layer_obj["type"] = (int)layer->type;
        layer_obj["timeout"] = layer->timeout_sec;
        
        char color_buf[16];
        snprintf(color_buf, sizeof(color_buf), "0x%06X", (unsigned int)(layer->led_color & 0xFFFFFF));
        layer_obj["color"] = color_buf;

        JsonArray arr = layer_obj["bindings"].to<JsonArray>();
        for (size_t i = 0; i < layer->binding_count; i++) {
            const key_binding_t *b = &layer->bindings[i];
            JsonObject obj = arr.add<JsonObject>();

            obj["source_vk"] = b->source_vk;
            
            // Click action
            if (b->has_click) {
                obj["has_click"] = true;
                obj["click_type"] = (int)b->click_action.type;
                if (b->click_action.modifier != 0) obj["click_mod"] = b->click_action.modifier;
                if (b->click_action.key_code != 0) obj["click_key"] = b->click_action.key_code;
                if (b->click_action.consumer_code != 0) obj["click_cons"] = b->click_action.consumer_code;
                if (b->click_action.type == ACTION_SWITCH_LAYER) obj["click_layer"] = b->click_action.target_layer;
                if (b->click_action.type == ACTION_WOL) {
                    char mac_buf[20];
                    wol_manager_mac_to_str(b->click_action.wol_mac, mac_buf, sizeof(mac_buf));
                    obj["click_mac"] = mac_buf;
                }
            }

            // Long action
            if (b->has_long) {
                obj["has_long"] = true;
                obj["long_ms"] = b->long_ms;
                obj["long_type"] = (int)b->long_action.type;
                if (b->long_action.modifier != 0) obj["long_mod"] = b->long_action.modifier;
                if (b->long_action.key_code != 0) obj["long_key"] = b->long_action.key_code;
                if (b->long_action.consumer_code != 0) obj["long_cons"] = b->long_action.consumer_code;
                if (b->long_action.type == ACTION_SWITCH_LAYER) obj["long_layer"] = b->long_action.target_layer;
                if (b->long_action.type == ACTION_WOL) {
                    char mac_buf[20];
                    wol_manager_mac_to_str(b->long_action.wol_mac, mac_buf, sizeof(mac_buf));
                    obj["long_mac"] = mac_buf;
                }
            }

            // Double action
            if (b->has_double) {
                obj["has_double"] = true;
                obj["double_ms"] = b->double_ms;
                obj["double_type"] = (int)b->double_action.type;
                if (b->double_action.modifier != 0) obj["double_mod"] = b->double_action.modifier;
                if (b->double_action.key_code != 0) obj["double_key"] = b->double_action.key_code;
                if (b->double_action.consumer_code != 0) obj["double_cons"] = b->double_action.consumer_code;
                if (b->double_action.type == ACTION_SWITCH_LAYER) obj["double_layer"] = b->double_action.target_layer;
                if (b->double_action.type == ACTION_WOL) {
                    char mac_buf[20];
                    wol_manager_mac_to_str(b->double_action.wol_mac, mac_buf, sizeof(mac_buf));
                    obj["double_mac"] = mac_buf;
                }
            }
            if (b->has_repeat) {
                obj["has_repeat"] = true;
                obj["repeat_delay_ms"] = b->repeat_delay_ms;
                obj["repeat_interval_ms"] = b->repeat_interval_ms;
                obj["repeat_type"] = (int)b->repeat_action.type;
                if (b->repeat_action.modifier != 0) obj["repeat_mod"] = b->repeat_action.modifier;
                if (b->repeat_action.key_code != 0) obj["repeat_key"] = b->repeat_action.key_code;
                if (b->repeat_action.consumer_code != 0) obj["repeat_cons"] = b->repeat_action.consumer_code;
                if (b->repeat_action.type == ACTION_SWITCH_LAYER) obj["repeat_layer"] = b->repeat_action.target_layer;
                if (b->repeat_action.type == ACTION_WOL) {
                    char mac_buf[20];
                    wol_manager_mac_to_str(b->repeat_action.wol_mac, mac_buf, sizeof(mac_buf));
                    obj["repeat_mac"] = mac_buf;
                }
            }
        }
    }

    String out;
    if (doc.overflowed()) return "";
    size_t serialized = serializeJson(doc, out);
    if (serialized == 0 || serialized != out.length()) return "";
    return out;
}

String key_config_to_json(const key_mapper_engine_t *engine) {
    if (!engine || !key_engine_is_ready()) return "";
    key_layer_t layers[MAX_LAYERS];
    uint8_t active_layer;
    key_engine_lock_state((key_mapper_engine_t *)engine);
    memcpy(layers, engine->layers, sizeof(layers));
    active_layer = engine->active_layer;
    key_engine_unlock_state((key_mapper_engine_t *)engine);
    return key_config_layout_to_json(layers, active_layer);
}

static uint32_t parse_u32_or_hex(JsonVariant v, uint32_t default_val = 0) {
    if (v.isNull()) return default_val;
    if (v.is<int>() || v.is<unsigned int>() || v.is<long>() || v.is<unsigned long>()) {
        return v.as<uint32_t>();
    }
    if (v.is<const char*>() || v.is<String>()) {
        String s = v.as<String>();
        s.trim();
        if (s.startsWith("0x") || s.startsWith("0X")) {
            return (uint32_t)strtoul(s.c_str(), NULL, 16);
        }
        return (uint32_t)strtoul(s.c_str(), NULL, 10);
    }
    return default_val;
}

static bool valid_source_vk(uint32_t vk) {
    switch (vk) {
        case MI_KEY_POWER: case MI_KEY_POWER_ALT: case MI_KEY_VOICE: case MI_KEY_VOICE_ALT:
        case MI_KEY_UP: case MI_KEY_DOWN: case MI_KEY_LEFT: case MI_KEY_RIGHT:
        case MI_KEY_OK: case MI_KEY_BACK: case MI_KEY_HOME: case MI_KEY_HOME_ALT:
        case MI_KEY_MENU: case MI_KEY_MENU_ALT: case MI_KEY_VOL_UP: case MI_KEY_VOL_DOWN:
        case MI_KEY_TV: case MI_KEY_TV_ALT: return true;
        default: return false;
    }
}

static bool parse_bindings_array(JsonArray arr, key_layer_t *layer) {
    if (!layer || arr.isNull() || arr.size() > MAX_KEY_BINDINGS) return false;
    layer->binding_count = 0;
    for (JsonObject obj : arr) {
        if (obj.isNull() || !obj["source_vk"].is<uint32_t>()) return false;

        key_binding_t b;
        memset(&b, 0, sizeof(b));

        if ((!obj["has_click"].isNull() && !obj["has_click"].is<bool>()) ||
            (!obj["has_long"].isNull() && !obj["has_long"].is<bool>()) ||
            (!obj["has_double"].isNull() && !obj["has_double"].is<bool>()) ||
            (!obj["has_repeat"].isNull() && !obj["has_repeat"].is<bool>())) return false;

        uint32_t source = parse_u32_or_hex(obj["source_vk"], UINT32_MAX);
        if (source > UINT8_MAX || !valid_source_vk(source)) return false;
        b.source_vk = (uint8_t)source;

        const uint32_t click_type = parse_u32_or_hex(obj["click_type"], 0);
        const uint32_t click_mod = parse_u32_or_hex(obj["click_mod"], 0);
        const uint32_t click_key = parse_u32_or_hex(obj["click_key"], 0);
        const uint32_t click_cons = parse_u32_or_hex(obj["click_cons"], 0);
        const uint32_t click_layer = parse_u32_or_hex(obj["click_layer"], 0);
        const uint32_t long_type = parse_u32_or_hex(obj["long_type"], 1);
        const uint32_t long_mod = parse_u32_or_hex(obj["long_mod"], 0);
        const uint32_t long_key = parse_u32_or_hex(obj["long_key"], 0);
        const uint32_t long_cons = parse_u32_or_hex(obj["long_cons"], 0);
        const uint32_t long_layer = parse_u32_or_hex(obj["long_layer"], 0);
        const uint32_t double_type = parse_u32_or_hex(obj["double_type"], 1);
        const uint32_t double_mod = parse_u32_or_hex(obj["double_mod"], 0);
        const uint32_t double_key = parse_u32_or_hex(obj["double_key"], 0);
        const uint32_t double_cons = parse_u32_or_hex(obj["double_cons"], 0);
        const uint32_t double_layer = parse_u32_or_hex(obj["double_layer"], 0);
        const uint32_t long_ms = parse_u32_or_hex(obj["long_ms"], 600);
        const uint32_t double_ms = parse_u32_or_hex(obj["double_ms"], 250);
        const uint32_t repeat_type = parse_u32_or_hex(obj["repeat_type"], 0);
        const uint32_t repeat_mod = parse_u32_or_hex(obj["repeat_mod"], 0);
        const uint32_t repeat_key = parse_u32_or_hex(obj["repeat_key"], 0);
        const uint32_t repeat_cons = parse_u32_or_hex(obj["repeat_cons"], 0);
        const uint32_t repeat_layer = parse_u32_or_hex(obj["repeat_layer"], 0);
        const uint32_t repeat_delay = parse_u32_or_hex(obj["repeat_delay_ms"], 0);
        const uint32_t repeat_interval = parse_u32_or_hex(obj["repeat_interval_ms"], 0);
        if (click_type > ACTION_ADV_MQTT || long_type > ACTION_ADV_MQTT || double_type > ACTION_ADV_MQTT ||
            repeat_type > ACTION_ADV_MQTT ||
            click_mod > UINT8_MAX || click_key > UINT8_MAX || click_cons > UINT16_MAX || click_layer > UINT8_MAX ||
            long_mod > UINT8_MAX || long_key > UINT8_MAX || long_cons > UINT16_MAX || long_layer > UINT8_MAX ||
            double_mod > UINT8_MAX || double_key > UINT8_MAX || double_cons > UINT16_MAX || double_layer > UINT8_MAX ||
            repeat_mod > UINT8_MAX || repeat_key > UINT8_MAX || repeat_cons > UINT16_MAX || repeat_layer > UINT8_MAX ||
            repeat_delay > UINT16_MAX || repeat_interval > UINT16_MAX ||
            long_ms > UINT16_MAX || double_ms > UINT16_MAX) return false;

        b.has_click = obj["has_click"] | false;
        b.click_action.type = (key_action_type_t)click_type;
        b.click_action.modifier = (uint8_t)click_mod;
        b.click_action.key_code = (uint8_t)click_key;
        b.click_action.consumer_code = (uint16_t)click_cons;
        b.click_action.target_layer = (uint8_t)click_layer;
        if (!obj["click_mac"].isNull()) {
            if (!wol_manager_parse_mac(obj["click_mac"].as<String>().c_str(), b.click_action.wol_mac)) return false;
        }

        // Normalize MI_KEY_VOICE_ALT (0x3E) to MI_KEY_VOICE (0x04)
        if (b.source_vk == MI_KEY_VOICE_ALT) {
            b.source_vk = MI_KEY_VOICE;
        }

        // FORCE ACTION_VOICE_HOLD for Voice key on Layer 0 only
        if (b.source_vk == MI_KEY_VOICE && b.click_action.type != ACTION_SWITCH_LAYER && b.click_action.type != ACTION_TRANSPARENT) {
            b.click_action.type = ACTION_VOICE_HOLD;
        }

        b.has_long = obj["has_long"] | false;
        b.long_ms = (uint16_t)long_ms;
        b.long_action.type = (key_action_type_t)long_type;
        b.long_action.modifier = (uint8_t)long_mod;
        b.long_action.key_code = (uint8_t)long_key;
        b.long_action.consumer_code = (uint16_t)long_cons;
        b.long_action.target_layer = (uint8_t)long_layer;
        if (!obj["long_mac"].isNull()) {
            if (!wol_manager_parse_mac(obj["long_mac"].as<String>().c_str(), b.long_action.wol_mac)) return false;
        }

        b.has_double = obj["has_double"] | false;
        b.double_ms = (uint16_t)double_ms;
        b.double_action.type = (key_action_type_t)double_type;
        b.double_action.modifier = (uint8_t)double_mod;
        b.double_action.key_code = (uint8_t)double_key;
        b.double_action.consumer_code = (uint16_t)double_cons;
        b.double_action.target_layer = (uint8_t)double_layer;
        if (!obj["double_mac"].isNull()) {
            if (!wol_manager_parse_mac(obj["double_mac"].as<String>().c_str(), b.double_action.wol_mac)) return false;
        }

        b.has_repeat = obj["has_repeat"] | false;
        b.repeat_delay_ms = (uint16_t)repeat_delay;
        b.repeat_interval_ms = (uint16_t)repeat_interval;
        b.repeat_action.type = (key_action_type_t)repeat_type;
        b.repeat_action.modifier = (uint8_t)repeat_mod;
        b.repeat_action.key_code = (uint8_t)repeat_key;
        b.repeat_action.consumer_code = (uint16_t)repeat_cons;
        b.repeat_action.target_layer = (uint8_t)repeat_layer;
        if (!obj["repeat_mac"].isNull()) {
            if (!wol_manager_parse_mac(obj["repeat_mac"].as<String>().c_str(), b.repeat_action.wol_mac)) return false;
        }

        if ((uint32_t)b.click_action.type > ACTION_ADV_MQTT ||
            (uint32_t)b.long_action.type > ACTION_ADV_MQTT ||
            (uint32_t)b.double_action.type > ACTION_ADV_MQTT ||
            (uint32_t)b.repeat_action.type > ACTION_ADV_MQTT ||
            (b.has_long && (b.long_ms == 0 || b.long_ms > 60000)) ||
            (b.has_double && (b.double_ms == 0 || b.double_ms > 60000)) ||
            (b.has_repeat && (b.repeat_delay_ms == 0 || b.repeat_interval_ms == 0))) return false;
        if ((b.has_click && b.click_action.type == ACTION_SWITCH_LAYER && b.click_action.target_layer >= MAX_LAYERS) ||
            (b.has_long && b.long_action.type == ACTION_SWITCH_LAYER && b.long_action.target_layer >= MAX_LAYERS) ||
            (b.has_double && b.double_action.type == ACTION_SWITCH_LAYER && b.double_action.target_layer >= MAX_LAYERS) ||
            (b.has_repeat && b.repeat_action.type == ACTION_SWITCH_LAYER && b.repeat_action.target_layer >= MAX_LAYERS)) return false;
        for (size_t i = 0; i < layer->binding_count; ++i) {
            if (layer->bindings[i].source_vk == b.source_vk) return false;
        }

        layer->bindings[layer->binding_count++] = b;
    }
    return true;
}

bool key_config_parse_json(const String &json_str, key_mapper_engine_t *candidate) {
    if (!candidate || json_str.length() == 0) return false;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json_str);
    if (err) {
        app_log("KEYMAP", "JSON deserialize failed: %s", err.c_str());
        return false;
    }
    if (doc.overflowed()) {
        app_log("KEYMAP", "JSON document exceeded parser capacity");
        return false;
    }

    key_mapper_engine_t parsed = {};
    key_engine_init_candidate_defaults(&parsed);

    // Check if new multi-layer schema. A full layout must contain every layer exactly once.
    if (doc["layers"].is<JsonArray>()) {
        JsonArray layers_arr = doc["layers"].as<JsonArray>();
        if (layers_arr.size() != MAX_LAYERS) return false;
        bool seen[MAX_LAYERS] = {};
        for (JsonObject l_obj : layers_arr) {
            if (l_obj.isNull() || !l_obj["id"].is<uint32_t>()) return false;
            uint32_t raw_id = parse_u32_or_hex(l_obj["id"], UINT32_MAX);
            if (raw_id >= MAX_LAYERS || seen[raw_id]) return false;
            uint8_t id = (uint8_t)raw_id;
            seen[id] = true;

            key_layer_t *layer = &parsed.layers[id];
            if (!l_obj["name"].isNull()) {
                String nm = l_obj["name"].as<String>();
                if (nm.length() >= sizeof(layer->name)) return false;
                memset(layer->name, 0, sizeof(layer->name));
                strncpy(layer->name, nm.c_str(), sizeof(layer->name) - 1);
            }
            uint32_t type = parse_u32_or_hex(l_obj["type"], UINT32_MAX);
            uint32_t timeout = parse_u32_or_hex(l_obj["timeout"], UINT32_MAX);
            if (type > LAYER_TYPE_TIMEOUT || timeout > UINT16_MAX ||
                !l_obj["bindings"].is<JsonArray>()) return false;
            layer->type = (layer_type_t)type;
            layer->timeout_sec = (uint16_t)timeout;
            if (!l_obj["color"].isNull()) {
                layer->led_color = parse_u32_or_hex(l_obj["color"], layer->led_color);
            }
            if (layer->led_color > 0xFFFFFF) return false;

            JsonArray b_arr = l_obj["bindings"].as<JsonArray>();
            if (!parse_bindings_array(b_arr, layer)) return false;
        }
        uint32_t active = parse_u32_or_hex(doc["active_layer"], 0);
        if (active >= MAX_LAYERS) return false;
        parsed.active_layer = (uint8_t)active;
        *candidate = parsed;
        app_log("KEYMAP", "Loaded multi-layer keymap from JSON (5 layers)");
        return true;
    }

    // Backward compatibility: old single layer JSON with root "bindings" array
    if (doc["bindings"].is<JsonArray>()) {
        if (!parse_bindings_array(doc["bindings"].as<JsonArray>(), &parsed.layers[0])) return false;
        *candidate = parsed;
        app_log("KEYMAP", "Migrated legacy single-layer keymap into Layer 0 (%u bindings)", 
                (unsigned int)parsed.layers[0].binding_count);
        return true;
    }

    return false;
}

bool key_config_apply_candidate(key_mapper_engine_t *engine, const key_mapper_engine_t *candidate) {
    if (!engine || !candidate || !key_engine_is_ready() || candidate->active_layer >= MAX_LAYERS) return false;
    key_engine_lock_state(engine);
    // Clear HID/audio guard state before replacing the bindings that explain it.
    usb_composite_force_release_all("keymap-reload");
    bool ok = key_engine_replace_layout(engine, candidate->layers, candidate->active_layer);
    key_engine_unlock_state(engine);
    return ok;
}

bool key_config_from_json(key_mapper_engine_t *engine, const String &json_str) {
    key_mapper_engine_t candidate = {};
    return engine && key_config_parse_json(json_str, &candidate) &&
           key_config_apply_candidate(engine, &candidate);
}

bool key_config_storage_save(key_mapper_engine_t *engine) {
    if (!engine || !key_engine_is_ready()) return false;
    key_mapper_engine_t snapshot = {};
    key_engine_lock_state(engine);
    memcpy(snapshot.layers, engine->layers, sizeof(snapshot.layers));
    snapshot.layer_count = engine->layer_count;
    snapshot.active_layer = engine->active_layer;
    key_engine_unlock_state(engine);
    return key_config_storage_save_candidate(&snapshot);
}

bool key_config_storage_save_candidate(const key_mapper_engine_t *candidate) {
    if (!candidate || candidate->active_layer >= MAX_LAYERS) return false;
    String json = key_config_layout_to_json(candidate->layers, candidate->active_layer);
    if (json.length() == 0) return false;

    Preferences prefs;
    if (!prefs.begin("keymap_conf", false)) {
        app_log("KEYMAP", "Failed to open keymap_conf NVS namespace for writing!");
        return false;
    }
    size_t written = prefs.putString("cfg_json", json);
    String readback = prefs.getString("cfg_json", "");
    prefs.end();

    if (written != json.length() || readback != json) {
        app_log("KEYMAP", "ERROR: keymap NVS write failed verification (written=%u expected=%u readback=%s)",
                (unsigned int)written, (unsigned int)json.length(), readback == json ? "match" : "mismatch");
        return false;
    }
    app_log("KEYMAP", "Saved keymap to NVS successfully (%u bytes written)", (unsigned int)written);
    return true;
}

bool key_config_storage_load(key_mapper_engine_t *engine) {
    if (!engine) return false;

    Preferences prefs;
    if (!prefs.begin("keymap_conf", true)) {
        key_engine_load_defaults(engine);
        return false;
    }
    String json = prefs.getString("cfg_json", "");
    prefs.end();

    if (json.length() == 0) {
        key_engine_load_defaults(engine);
        return false;
    }
    key_mapper_engine_t candidate = {};
    bool ok = key_config_parse_json(json, &candidate);
    if (!ok || candidate.layers[0].binding_count == 0) {
        app_log("KEYMAP", "NVS keymap is invalid or corrupted -> auto-fallback to safe defaults!");
        key_engine_load_defaults(engine);
        return false;
    }
    return key_config_apply_candidate(engine, &candidate);
}

bool key_config_storage_import(key_mapper_engine_t *engine, const String &json_str) {
    if (!engine) return false;
    key_mapper_engine_t candidate = {};
    if (!key_config_parse_json(json_str, &candidate)) return false;
    if (!key_config_storage_save_candidate(&candidate)) return false;
    return key_config_apply_candidate(engine, &candidate);
}

bool key_config_storage_reset_defaults(key_mapper_engine_t *engine) {
    if (!engine || !key_engine_is_ready()) return false;
    key_mapper_engine_t candidate = {};
    key_engine_init_candidate_defaults(&candidate);
    if (!key_config_storage_save_candidate(&candidate)) return false;
    if (!key_config_apply_candidate(engine, &candidate)) return false;
    app_log("KEYMAP", "Reset keymap to factory defaults (%u bindings in Layer 0)", (unsigned int)engine->layers[0].binding_count);
    return true;
}

String key_telemetry_to_json(const key_mapper_engine_t *engine) {
    JsonDocument doc;
    if (engine) {
        key_event_telemetry_t telemetry;
        uint8_t active_layer;
        key_engine_lock_state((key_mapper_engine_t *)engine);
        telemetry = engine->last_telemetry;
        active_layer = engine->active_layer;
        key_engine_unlock_state((key_mapper_engine_t *)engine);
        doc["source_vk"] = telemetry.source_vk;
        doc["is_pressed"] = telemetry.is_pressed;
        doc["duration_ms"] = telemetry.duration_ms;
        doc["action_type"] = telemetry.action_type;
        doc["modifier"] = telemetry.modifier;
        doc["key_code"] = telemetry.key_code;
        doc["consumer_code"] = telemetry.consumer_code;
        doc["active_layer"] = active_layer;
    }
    String out;
    serializeJson(doc, out);
    return out;
}

