#pragma once

#include <stdint.h>
#include <stddef.h>
#include <Arduino.h>

#ifdef __cplusplus
extern "C" {
#endif

void app_log_init(void);
void app_log(const char* tag, const char* format, ...);
// Drain a bounded amount of queued UART/CDC output from the main-loop task.
void app_log_task(void);
// Queue one complete CDC payload (up to 16 KB) from the main-loop task.
// Returns false without enqueueing any bytes if it cannot be accepted.
bool app_log_queue_cdc_text(const char* text, size_t length);
String app_log_get_json(void);
void app_log_clear(void);
void app_log_set_cdc_enabled(bool enabled);
bool app_log_get_cdc_enabled(void);
void app_log_set_console_enabled(bool enabled);
bool app_log_get_console_enabled(void);
uint32_t app_log_get_mirror_dropped(void);

#ifdef __cplusplus
}
#endif
