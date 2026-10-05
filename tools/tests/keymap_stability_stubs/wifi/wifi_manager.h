#pragma once
#include "Arduino.h"
#include <stdint.h>
typedef enum { WIFI_POLICY_ALWAYS_ON = 0, WIFI_POLICY_ON_DEMAND = 1, WIFI_POLICY_DISABLED = 2 } wifi_policy_t;
#ifndef WIFI_DEFAULT_POLICY
#define WIFI_DEFAULT_POLICY WIFI_POLICY_ON_DEMAND
#endif
#define WIFI_DEFAULT_TIMEOUT_MIN 5
#define WIFI_DEFAULT_TIMEOUT_ENABLED 0
extern "C" {
wifi_policy_t wifi_manager_get_policy(void);
uint32_t wifi_manager_get_timeout_min(void);
bool wifi_manager_get_timeout_enabled(void);
bool wifi_manager_validate_backup(const String&, const String&, const String&, wifi_policy_t, uint32_t);
bool wifi_manager_restore_backup(const String&, const String&, const String&, wifi_policy_t, uint32_t, bool, bool);
void wifi_manager_apply_backup_state(const String&, wifi_policy_t, uint32_t, bool);
const char* wifi_manager_policy_str(wifi_policy_t);
}
String wifi_manager_get_sta_ssid(void);
String wifi_manager_get_sta_pass(void);
String wifi_manager_get_ap_pass(void);
