#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <ArduinoJson.h>

// Captures before recovery mutates USB state; survives USB re-enumeration.
void hid_diagnostics_capture(uint32_t reason);
void hid_diagnostics_persist_first_audio_fault();
void hid_diagnostics_json(JsonObject out);

// Record a SendReport attempt's outcome and exact 8-byte keyboard payload.
// completed=false means completion was not confirmed; it does not prove that
// the host never received the report.
// Stress reports use stress_source=true and id/epoch zero.
void hid_diagnostics_record_keyboard_report(uint32_t ms, uint32_t command_id,
        uint32_t epoch, const uint8_t report[8], bool completed, bool stress_source);
