#include "config_backup.h"
#include "config_manager.h"
#include "version.h"
#include "app_config.h"
#include "log/app_log.h"
#include "wifi/wifi_manager.h"
#include "keymap/key_state_machine.h"
#include "keymap/key_config_storage.h"
#include <ArduinoJson.h>

#define BACKUP_TYPE      "remotemapper-config-backup"
#define BACKUP_SCHEMA    1
#define REDACTED         "********"

extern key_mapper_engine_t g_key_engine;

String config_backup_export(bool full) {
    JsonDocument doc;
    doc["type"] = BACKUP_TYPE;
    doc["schema"] = BACKUP_SCHEMA;
    doc["firmware"] = FIRMWARE_VERSION;
    doc["hardware"] = HARDWARE_TARGET;
    doc["config_version"] = config_manager_get_version();

    JsonObject wifi = doc["wifi"].to<JsonObject>();
    wifi["policy"] = (int)wifi_manager_get_policy();
    wifi["timeout_min"] = wifi_manager_get_timeout_min();
    wifi["timeout_enabled"] = wifi_manager_get_timeout_enabled();

    String ssid    = wifi_manager_get_sta_ssid();
    String sta_pass = wifi_manager_get_sta_pass();
    wifi["sta_ssid"] = ssid;
    if (full) {
        wifi["sta_pass"] = sta_pass;
    } else {
        wifi["sta_has_pass"] = (sta_pass.length() > 0);
        wifi["sta_pass"]     = REDACTED;
    }

    String ap_pass = wifi_manager_get_ap_pass();
    if (full) {
        wifi["ap_pass"] = ap_pass;
    } else {
        wifi["ap_secured"] = (ap_pass.length() >= 8);
        wifi["ap_pass"]    = REDACTED;
    }

    // Keymap (5-layer binding rules)
    String km = key_config_to_json(&g_key_engine);
    JsonDocument kmDoc;
    if (deserializeJson(kmDoc, km) != DeserializationError::Ok) {
        doc["keymap"] = JsonObject();
    } else {
        doc["keymap"] = kmDoc.as<JsonObject>();
    }

    String out;
    serializeJsonPretty(doc, out);
    app_log("CONFIG", "Config backup exported (mode=%s, wifi_policy=%s)",
            full ? "full" : "safe",
            wifi_manager_policy_str(wifi_manager_get_policy()));
    return out;
}

String config_backup_import(const String& json) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        return "不是有效的 JSON 文件";
    }
    if (!doc["wifi"].is<JsonObject>() && !doc["keymap"].is<JsonObject>()) {
        return "备份文件缺少 wifi 或 keymap 内容";
    }
    if ((!doc["wifi"].isNull() && !doc["wifi"].is<JsonObject>()) ||
        (!doc["keymap"].isNull() && !doc["keymap"].is<JsonObject>())) {
        return "备份文件的 wifi 或 keymap 字段格式错误";
    }

    bool has_wifi = doc["wifi"].is<JsonObject>();
    bool has_keymap = doc["keymap"].is<JsonObject>();
    String ssid, sta_pass, ap_pass;
    wifi_policy_t policy = (wifi_policy_t)WIFI_DEFAULT_POLICY;
    uint32_t timeout = WIFI_DEFAULT_TIMEOUT_MIN;
    bool timeout_enabled = WIFI_DEFAULT_TIMEOUT_ENABLED ? true : false;
    key_mapper_engine_t candidate = {};

    // Validate every section before touching either NVS namespace or live state.
    if (has_wifi) {
        JsonObject w = doc["wifi"].as<JsonObject>();
        if (!w["sta_ssid"].is<const char*>() || !w["sta_pass"].is<const char*>() ||
            !w["ap_pass"].is<const char*>() || !w["policy"].is<int>() ||
            !w["timeout_min"].is<uint32_t>()) {
            return "Wi-Fi 备份内容不完整或字段类型错误";
        }
        ssid = w["sta_ssid"] | "";
        sta_pass = w["sta_pass"] | "";
        ap_pass = w["ap_pass"] | "";
        int raw_policy = w["policy"] | (int)WIFI_DEFAULT_POLICY;
        uint32_t raw_timeout = w["timeout_min"] | WIFI_DEFAULT_TIMEOUT_MIN;
        if (raw_policy < WIFI_POLICY_ALWAYS_ON || raw_policy > WIFI_POLICY_DISABLED) {
            return "Wi-Fi 配置恢复失败（参数非法）";
        }
        policy = (wifi_policy_t)raw_policy;
        timeout = raw_timeout;
        timeout_enabled = w["timeout_enabled"].is<bool>()
            ? (w["timeout_enabled"] | false) : (WIFI_DEFAULT_TIMEOUT_ENABLED ? true : false);
        if (sta_pass == REDACTED || ap_pass == REDACTED) {
            return "该备份来自“安全模式”导出，密码已打码无法还原，请使用完整备份或重新输入密码";
        }
        if (!wifi_manager_validate_backup(ssid, sta_pass, ap_pass, policy, timeout)) {
            return "Wi-Fi 配置恢复失败（参数非法）";
        }
    }
    if (has_keymap) {
        String km;
        serializeJson(doc["keymap"].as<JsonObject>(), km);
        if (!key_config_parse_json(km, &candidate)) {
            return "按键映射恢复失败（格式不兼容）";
        }
    }

    // Commit Wi-Fi first, then the single-key keymap record. If the latter fails,
    // restore the old Wi-Fi values before returning. NVS offers no cross-namespace
    // power-loss transaction, so this is a runtime rollback boundary only.
    String old_ssid = wifi_manager_get_sta_ssid();
    String old_sta_pass = wifi_manager_get_sta_pass();
    String old_ap_pass = wifi_manager_get_ap_pass();
    wifi_policy_t old_policy = wifi_manager_get_policy();
    uint32_t old_timeout = wifi_manager_get_timeout_min();
    bool old_timeout_enabled = wifi_manager_get_timeout_enabled();
    if (has_wifi) {
        if (!wifi_manager_restore_backup(ssid, sta_pass, ap_pass, policy, timeout, timeout_enabled, false)) {
            return "Wi-Fi 配置写入失败；旧运行配置仍保留（若 NVS 回滚失败请重启检查）";
        }
    }

    if (has_keymap && !key_config_storage_save_candidate(&candidate)) {
        if (has_wifi) {
            bool rolled_back = wifi_manager_restore_backup(old_ssid, old_sta_pass, old_ap_pass,
                                                            old_policy, old_timeout, old_timeout_enabled, false);
            return rolled_back
                ? "按键映射写入失败；Wi-Fi 配置已回滚，旧运行配置仍保留"
                : "按键映射写入失败且 Wi-Fi 回滚未完成；请重启检查配置（NVS 无跨命名空间原子提交）";
        }
        return "按键映射写入失败；旧运行映射仍保留";
    }
    if (has_keymap && !key_config_apply_candidate(&g_key_engine, &candidate)) {
        return "按键映射应用失败；请重启检查已写入的配置";
    }
    if (has_wifi) {
        wifi_manager_apply_backup_state(ssid, policy, timeout, timeout_enabled);
    }

    app_log("CONFIG", "Config backup restored; reboot required for policy change to apply");
    return "";
}
