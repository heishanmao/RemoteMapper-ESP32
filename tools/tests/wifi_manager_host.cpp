#include "wifi_manager.h"
#include "Preferences.h"
#include "WiFi.h"
#include "wifi_request_validation.h"
#include "log/app_log.h"
#include <ArduinoJson.h>

#include <cstdlib>
#include <cstdio>
#include <string>

using wifi_test_prefs::state;

#define require(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "require failed at line %d\n", __LINE__); \
        std::exit(1); \
    } \
} while (0)

static std::string value(const char* key) {
    const auto it = state.values.find(key);
    return it == state.values.end() ? "<missing>" : it->second;
}

static bool parse_power(const char* json, wifi_power_request_t* request) {
    JsonDocument doc;
    if (deserializeJson(doc, json)) return false;
    return wifi_parse_power_request(doc, request);
}

int main(int argc, char**) {
    wifi_test_prefs::reset();
    if (argc > 1) {
        state.begin_ok = false;
        wifi_manager_init();
        require(!wifi_manager_set_policy(WIFI_POLICY_DISABLED));
        require(!wifi_manager_save_sta_config(String("network"), String("secret")));
        require(!wifi_manager_save_ap_config(String("")));
        require(state.put_uint_call == 0 && state.raw_string_sets == 0);
        return 0;
    }
    state.values["policy"] = "1";
    state.values["timeout_min"] = "5";
    state.values["timeout_en"] = "0";
    state.values["ssid"] = "old-network";
    state.values["pass"] = "old-secret";
    state.values["ap_pass"] = "old-ap-secret";
    wifi_manager_init();
    require(wifi_manager_get_policy() == WIFI_POLICY_ON_DEMAND);

    // A failed policy write must not publish a different runtime policy.
    state.fail_uint_call = state.put_uint_call + 1;
    require(!wifi_manager_set_policy(WIFI_POLICY_DISABLED));
    require(wifi_manager_get_policy() == WIFI_POLICY_ON_DEMAND);
    require(wifi_manager_get_enabled());
    require(value("policy") == "1");
    state.fail_uint_call = -1;

    // Failed old-value reads reject the update before any NVS write or RAM change.
    const int writes_before_read_failure = state.put_uint_call;
    state.fail_raw_uint_read_call = state.raw_uint_reads + 1;
    require(!wifi_manager_set_policy(WIFI_POLICY_DISABLED));
    require(state.put_uint_call == writes_before_read_failure);
    require(wifi_manager_get_policy() == WIFI_POLICY_ON_DEMAND);
    state.fail_raw_uint_read_call = -1;

    wifi_power_request_t invalid_request = {};
    require(!parse_power("[]", &invalid_request));
    require(!parse_power("null", &invalid_request));
    require(!parse_power("{\"policy\":null}", &invalid_request));
    require(!parse_power("{\"policy\":2,\"timeout_min\":99}", &invalid_request));
    require(wifi_manager_get_policy() == WIFI_POLICY_ON_DEMAND);
    require(wifi_manager_get_timeout_min() == 5);
    JsonDocument sta_doc;
    String parsed_ssid;
    String parsed_pass;
    require(!deserializeJson(sta_doc, "{\"ssid\":\"network\",\"pass\":12}"));
    require(!wifi_parse_sta_request(sta_doc, &parsed_ssid, &parsed_pass));
    require(!deserializeJson(sta_doc, "{\"ssid\":\"network\",\"pass\":\"\"}"));
    require(wifi_parse_sta_request(sta_doc, &parsed_ssid, &parsed_pass));
    require(parsed_ssid == "network" && parsed_pass == "");
    JsonDocument ap_doc;
    String parsed_ap;
    require(!deserializeJson(ap_doc, "{\"ap_pass\":\"\"}"));
    require(wifi_parse_ap_request(ap_doc, &parsed_ap) && parsed_ap == "");

    const int writes_before_backup_read_failure = state.raw_string_sets + state.put_uint_call + state.put_bool_call;
    state.fail_raw_string_read_call = state.raw_string_reads + 1;
    require(!wifi_manager_restore_backup(String("backup-network"), String("backup-pass"),
            String("backup-ap-pass"), WIFI_POLICY_ALWAYS_ON, 10, true, false));
    require(state.raw_string_sets + state.put_uint_call + state.put_bool_call ==
            writes_before_backup_read_failure);
    require(wifi_manager_get_policy() == WIFI_POLICY_ON_DEMAND);
    require(value("ssid") == "old-network");
    state.fail_raw_string_read_call = -1;

    // Same-value RAM state cannot conceal a missing or unpersistable key.
    state.values.erase("timeout_min");
    state.fail_uint_call = state.put_uint_call + 1;
    require(!wifi_manager_set_timeout_min(5));
    require(wifi_manager_get_timeout_min() == 5);
    require(value("timeout_min") == "<missing>");
    state.fail_uint_call = -1;
    require(wifi_manager_set_timeout_min(5));
    require(value("timeout_min") == "5");

    // Multi-field power updates restore every selected key after the second write fails.
    const int uint_call = state.put_uint_call;
    state.fail_uint_call = uint_call + 2;
    const int bool_call = state.put_bool_call;
    require(!wifi_manager_update_power_config(true, WIFI_POLICY_ALWAYS_ON,
            true, 10, true, true));
    require(wifi_manager_get_policy() == WIFI_POLICY_ON_DEMAND);
    require(wifi_manager_get_timeout_min() == 5);
    require(!wifi_manager_get_timeout_enabled());
    require(value("policy") == "1" && value("timeout_min") == "5" && value("timeout_en") == "0");
    require(state.put_bool_call == bool_call); // failed second write stops later writes
    state.fail_uint_call = -1;
    require(wifi_manager_update_power_config(true, WIFI_POLICY_ALWAYS_ON,
            true, 10, true, true));
    require(wifi_manager_get_policy() == WIFI_POLICY_ALWAYS_ON);
    require(wifi_manager_get_timeout_min() == 10);
    require(wifi_manager_get_timeout_enabled());

    // If restoring a formerly absent key fails, the rollback is reported incomplete;
    // runtime state remains old and the failure is visible to diagnostics.
    state.values.erase("timeout_min");
    state.fail_bool_call = state.put_bool_call + 1;
    state.fail_remove_call = state.remove_calls + 1;
    require(!wifi_manager_update_power_config(true, WIFI_POLICY_ON_DEMAND,
            true, 5, true, false));
    require(wifi_manager_get_policy() == WIFI_POLICY_ALWAYS_ON);
    require(wifi_manager_get_timeout_min() == 10);
    require(wifi_manager_get_timeout_enabled());
    require(wifi_test_log::last_line.find("rollback incomplete") != std::string::npos);
    state.fail_bool_call = -1;
    state.fail_remove_call = -1;
    state.values["timeout_min"] = "10";

    // A malformed stored bool cannot be safely snapshotted for rollback.
    state.values["timeout_en"] = "2";
    const int writes_before_invalid_bool = state.put_uint_call + state.put_bool_call;
    require(!wifi_manager_set_timeout_enabled(false));
    require(state.put_uint_call + state.put_bool_call == writes_before_invalid_bool);
    state.values["timeout_en"] = "1";

    // STA's second-key write failure rolls back both credentials and never reconnects.
    const int initial_begin_calls = WiFi.begin_calls;
    const int string_call = state.raw_string_sets;
    state.fail_raw_string_set_call = string_call + 2;
    require(!wifi_manager_save_sta_config(String("new-network"), String("new-secret")));
    require(value("ssid") == "old-network" && value("pass") == "old-secret");
    require(wifi_manager_get_sta_ssid() == "old-network");
    require(wifi_manager_get_sta_pass() == "old-secret");
    require(WiFi.begin_calls == initial_begin_calls);
    state.fail_raw_string_set_call = -1;

    // A failed old-value read is rejected before touching either credential.
    const int writes_before_sta_read_failure = state.raw_string_sets;
    state.fail_raw_string_read_call = state.raw_string_reads + 1;
    require(!wifi_manager_save_sta_config(String("unreadable-old"), String("new-secret")));
    require(state.raw_string_sets == writes_before_sta_read_failure);
    require(value("ssid") == "old-network" && value("pass") == "old-secret");
    state.fail_raw_string_read_call = -1;

    // A read-back mismatch is also a failed save, with the old pair restored.
    const int read_call = state.raw_string_reads;
    state.corrupt_raw_string_read_call = read_call + 4; // snapshot, current probe, then write verification
    require(!wifi_manager_save_sta_config(String("bad-readback"), String("new-secret")));
    require(value("ssid") == "old-network" && value("pass") == "old-secret");
    require(WiFi.begin_calls == initial_begin_calls);
    state.corrupt_raw_string_read_call = -1;

    // Valid STA save publishes and starts connecting only after both keys verify.
    require(wifi_manager_save_sta_config(String("new-network"), String("new-secret")));
    require(value("ssid") == "new-network" && value("pass") == "new-secret");
    require(WiFi.begin_calls == initial_begin_calls + 1);

    // An empty-string read-back I/O error is a failed save and restores the old password.
    state.values["ap_pass"] = "before-empty-fault";
    const int reads_before_empty_fault = state.raw_string_reads;
    const int softap_calls = WiFi.softap_calls;
    state.fail_raw_string_read_call = reads_before_empty_fault + 3;
    require(!wifi_manager_save_ap_config(String("")));
    require(value("ap_pass") == "before-empty-fault");
    require(WiFi.softap_calls == softap_calls);
    state.fail_raw_string_read_call = -1;

    // NVS can stage the empty string but reject commit; it must still fail and roll back.
    state.values["ap_pass"] = "before-empty-commit-fault";
    const int commits_before_empty_fault = state.raw_commits;
    const int softap_before_empty_commit_fault = WiFi.softap_calls;
    state.fail_raw_commit_call = commits_before_empty_fault + 1;
    require(!wifi_manager_save_ap_config(String("")));
    require(value("ap_pass") == "before-empty-commit-fault");
    require(WiFi.softap_calls == softap_before_empty_commit_fault);
    state.fail_raw_commit_call = -1;

    // Empty AP password is valid only when the empty value is actually stored under the key.
    require(wifi_manager_save_ap_config(String("")));
    require(state.values.count("ap_pass") == 1 && value("ap_pass").empty());
    require(WiFi.softap_calls == softap_calls + 1);

    // Simulate a Preferences backend which reports a zero-length put but omits the key.
    // The save must fail, restore the prior AP value, and preserve the running AP.
    state.values["ap_pass"] = "restored-secret";
    const int writes_before_ap_read_failure = state.raw_string_sets;
    const int softap_before_ap_read_failure = WiFi.softap_calls;
    state.fail_raw_string_read_call = state.raw_string_reads + 1;
    require(!wifi_manager_save_ap_config(String("new-ap-secret")));
    require(state.raw_string_sets == writes_before_ap_read_failure);
    require(value("ap_pass") == "restored-secret");
    require(WiFi.softap_calls == softap_before_ap_read_failure);
    state.fail_raw_string_read_call = -1;

    state.omit_empty_key = true;
    const int softap_before_failed_ap = WiFi.softap_calls;
    require(!wifi_manager_save_ap_config(String("")));
    require(state.values.count("ap_pass") == 1 && value("ap_pass") == "restored-secret");
    require(WiFi.softap_calls == softap_before_failed_ap);
    return 0;
}
