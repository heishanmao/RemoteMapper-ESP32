#pragma once
#include <ArduinoJson.h>

// Captures before recovery mutates USB state; survives USB re-enumeration.
void hid_diagnostics_capture(uint32_t reason);
void hid_diagnostics_persist_first_audio_fault();
void hid_diagnostics_json(JsonObject out);
