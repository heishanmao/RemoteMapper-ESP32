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
typedef enum {
    APP_LOG_OUTPUT_UART = 0,
    APP_LOG_OUTPUT_CDC = 1
} app_log_output_t;
// Queue one complete CLI payload (up to 16 KB). On failure it is rejected
// whole and a fixed error notice is scheduled for that output route.
bool app_log_queue_cli_text(app_log_output_t output, const char* text, size_t length);
// Compatibility wrapper for callers that only target CDC.
bool app_log_queue_cdc_text(const char* text, size_t length);
String app_log_get_json(void);
void app_log_clear(void);
void app_log_set_cdc_enabled(bool enabled);
bool app_log_get_cdc_enabled(void);
void app_log_set_console_enabled(bool enabled);
bool app_log_get_console_enabled(void);
uint32_t app_log_get_mirror_dropped(void);
uint32_t app_log_get_cli_dropped(app_log_output_t output);

#ifdef __cplusplus
}
#endif
