#pragma once
static constexpr int ESP_COEX_PREFER_BT = 1;
inline int esp_coex_preference_set(int) { return 0; }
