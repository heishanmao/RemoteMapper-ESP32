#include "app_log.h"
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tusb.h"

#define STATIC_LOG_LINES 250
#define LOG_LINE_MAX_LEN 160
#define LOG_RING_BYTES   (STATIC_LOG_LINES * LOG_LINE_MAX_LEN)
#define MIRROR_QUEUE_DEPTH 8
#define MIRROR_ROUTE_COUNT 2
#define MIRROR_CHUNK_BYTES 32
#define MIRROR_TEXT_MAX_BYTES 16384

// app_log() is called by BLE and USB callbacks. Its only shared state is
// fixed-size storage protected by a short critical section; serial I/O is
// deferred to app_log_task() in the main loop.
static char* s_log_lines = NULL;
static uint64_t s_log_slot_sequence[STATIC_LOG_LINES] = {};
static uint64_t s_log_next_sequence = 0;
static size_t s_log_head = 0;
static size_t s_log_count = 0;

struct MirrorRecord {
    char line[LOG_LINE_MAX_LEN];
    char* owned_payload;
    uint16_t length;
    uint16_t offset;
};

// Per-route static bounded queues: callback-side logging never allocates,
// waits for a consumer, or writes to a serial transport. New mirror messages
// are dropped on the full route; the in-memory log ring still records them.
static MirrorRecord s_mirror_queue[MIRROR_ROUTE_COUNT][MIRROR_QUEUE_DEPTH] = {};
static size_t s_mirror_head[MIRROR_ROUTE_COUNT] = {};
static size_t s_mirror_count[MIRROR_ROUTE_COUNT] = {};
static uint32_t s_mirror_dropped = 0;
static bool s_cdc_log_enabled = false;
static bool s_console_log_enabled = true;
static uint32_t s_uart_cli_dropped = 0;
static uint32_t s_cdc_cli_dropped = 0;
static bool s_uart_overflow_notice_pending = false;
static bool s_cdc_overflow_notice_pending = false;
static uint16_t s_uart_overflow_notice_offset = 0;
static uint16_t s_cdc_overflow_notice_offset = 0;

static portMUX_TYPE s_log_mux = portMUX_INITIALIZER_UNLOCKED;

void app_log_init(void) {
    if (!s_log_lines) {
        s_log_lines = (char*)heap_caps_malloc(LOG_RING_BYTES, MALLOC_CAP_SPIRAM);
        if (!s_log_lines) {
            s_log_lines = (char*)malloc(LOG_RING_BYTES); // internal-RAM fallback
        }
    }

    char* abandoned[MIRROR_ROUTE_COUNT * MIRROR_QUEUE_DEPTH] = {};
    memset(s_log_slot_sequence, 0, sizeof(s_log_slot_sequence));
    taskENTER_CRITICAL(&s_log_mux);
    size_t abandoned_count = 0;
    for (size_t route = 0; route < MIRROR_ROUTE_COUNT; ++route) {
        for (size_t i = 0; i < s_mirror_count[route]; ++i) {
            const size_t slot = (s_mirror_head[route] + i) % MIRROR_QUEUE_DEPTH;
            abandoned[abandoned_count++] = s_mirror_queue[route][slot].owned_payload;
        }
    }
    s_log_head = 0;
    s_log_count = 0;
    s_log_next_sequence = 0;
    memset(s_mirror_head, 0, sizeof(s_mirror_head));
    memset(s_mirror_count, 0, sizeof(s_mirror_count));
    s_mirror_dropped = 0;
    s_uart_cli_dropped = 0;
    s_cdc_cli_dropped = 0;
    s_uart_overflow_notice_pending = false;
    s_cdc_overflow_notice_pending = false;
    s_uart_overflow_notice_offset = 0;
    s_cdc_overflow_notice_offset = 0;
    taskEXIT_CRITICAL(&s_log_mux);
    for (size_t i = 0; i < abandoned_count; ++i) free(abandoned[i]);
}

void app_log(const char* tag, const char* format, ...) {
    // app_log_init() runs before tasks start. If allocation failed or logging
    // arrives early, drop safely instead of allocating in a callback.
    if (!s_log_lines || !tag || !format) {
        return;
    }

    char msg_buf[128];
    va_list args;
    va_start(args, format);
    vsnprintf(msg_buf, sizeof(msg_buf), format, args);
    va_end(args);

    uint32_t now_ms = millis();
    uint32_t sec = now_ms / 1000;
    uint32_t ms = now_ms % 1000;

    char full_line[LOG_LINE_MAX_LEN];
    snprintf(full_line, sizeof(full_line), "[%04u.%03u] [%s] %s",
             (unsigned int)sec, (unsigned int)ms, tag, msg_buf);
    size_t line_len = strlen(full_line);

    taskENTER_CRITICAL(&s_log_mux);
    const uint64_t sequence = s_log_next_sequence++;
    char* line = s_log_lines + s_log_head * LOG_LINE_MAX_LEN;
    memcpy(line, full_line, line_len + 1);
    s_log_slot_sequence[s_log_head] = sequence;
    s_log_head = (s_log_head + 1) % STATIC_LOG_LINES;
    if (s_log_count < STATIC_LOG_LINES) {
        s_log_count++;
    }

    const bool mirror_enabled[MIRROR_ROUTE_COUNT] = {
        s_console_log_enabled && !s_uart_overflow_notice_pending,
        s_cdc_log_enabled && !s_cdc_overflow_notice_pending
    };
    for (size_t route = 0; route < MIRROR_ROUTE_COUNT; ++route) {
        if (!mirror_enabled[route]) continue;
        if (s_mirror_count[route] >= MIRROR_QUEUE_DEPTH) {
            s_mirror_dropped++;
            continue;
        }
        const size_t tail = (s_mirror_head[route] + s_mirror_count[route]) % MIRROR_QUEUE_DEPTH;
        MirrorRecord* record = &s_mirror_queue[route][tail];
        // Reserve both the newline and terminator even when snprintf
        // filled the complete ring line. The ring retains its own copy.
        const size_t mirror_len = line_len < LOG_LINE_MAX_LEN - 2
                ? line_len : LOG_LINE_MAX_LEN - 2;
        memcpy(record->line, full_line, mirror_len);
        record->line[mirror_len] = '\n';
        record->line[mirror_len + 1] = '\0';
        record->owned_payload = NULL;
        record->length = (uint16_t)(mirror_len + 1);
        record->offset = 0;
        s_mirror_count[route]++;
    }
    taskEXIT_CRITICAL(&s_log_mux);
}

void app_log_task(void) {
    static const char error_text[] = "\r\n{\"error\":\"output_queue_full\"}\r\n";
    for (size_t route = 0; route < MIRROR_ROUTE_COUNT; ++route) {
        MirrorRecord record = {};
        bool queued_record = false;
        bool notice_record = false;

        taskENTER_CRITICAL(&s_log_mux);
        if (s_mirror_count[route]) {
            record = s_mirror_queue[route][s_mirror_head[route]];
            queued_record = true;
        } else if (route == APP_LOG_OUTPUT_CDC ? s_cdc_overflow_notice_pending
                                               : s_uart_overflow_notice_pending) {
            notice_record = true;
            record.offset = route == APP_LOG_OUTPUT_CDC
                    ? s_cdc_overflow_notice_offset : s_uart_overflow_notice_offset;
            record.length = sizeof(error_text) - 1;
        }
        taskEXIT_CRITICAL(&s_log_mux);
        if (!queued_record && !notice_record) continue;

        const bool cdc = route == APP_LOG_OUTPUT_CDC;
        const bool disconnected = cdc && !tud_cdc_n_connected(0);
        const char* payload = notice_record ? error_text
                : (record.owned_payload ? record.owned_payload : record.line);
        if (disconnected) {
            record.offset = record.length;
        } else if (record.offset < record.length) {
            size_t amount = record.length - record.offset;
            if (amount > MIRROR_CHUNK_BYTES) amount = MIRROR_CHUNK_BYTES;
            size_t sent = 0;
            if (cdc) {
                uint32_t accepted = tud_cdc_n_write(0, payload + record.offset, (uint32_t)amount);
                sent = accepted > amount ? amount : accepted;
                if (sent) tud_cdc_n_write_flush(0);
            } else if (Serial) {
                const int available = Serial.availableForWrite();
                if (available > 0) {
                    if (amount > (size_t)available) amount = (size_t)available;
                    if (amount) {
                        sent = Serial.write((const uint8_t*)payload + record.offset, amount);
                        if (sent > amount) sent = amount;
                    }
                }
            }
            record.offset = (uint16_t)(record.offset + sent);
        }

        char* released_payload = NULL;
        taskENTER_CRITICAL(&s_log_mux);
        if (queued_record && s_mirror_count[route]) {
            MirrorRecord* queued = &s_mirror_queue[route][s_mirror_head[route]];
            queued->offset = record.offset;
            if (queued->offset >= queued->length) {
                released_payload = queued->owned_payload;
                queued->owned_payload = NULL;
                s_mirror_head[route] = (s_mirror_head[route] + 1) % MIRROR_QUEUE_DEPTH;
                s_mirror_count[route]--;
            }
        } else if (notice_record) {
            uint16_t* offset = cdc ? &s_cdc_overflow_notice_offset : &s_uart_overflow_notice_offset;
            bool* pending = cdc ? &s_cdc_overflow_notice_pending : &s_uart_overflow_notice_pending;
            *offset = record.offset;
            if (*offset >= record.length) {
                *pending = false;
                *offset = 0;
            }
        }
        taskEXIT_CRITICAL(&s_log_mux);
        free(released_payload);
    }
}
static bool app_log_queue_cli_text_route(app_log_output_t output, const char* text, size_t length) {
    const bool cdc = output == APP_LOG_OUTPUT_CDC;
    if (!text || !length || length > MIRROR_TEXT_MAX_BYTES) {
        taskENTER_CRITICAL(&s_log_mux);
        if (cdc) {
            s_cdc_cli_dropped++;
            if (!s_cdc_overflow_notice_pending) s_cdc_overflow_notice_offset = 0;
            s_cdc_overflow_notice_pending = true;
        } else {
            s_uart_cli_dropped++;
            if (!s_uart_overflow_notice_pending) s_uart_overflow_notice_offset = 0;
            s_uart_overflow_notice_pending = true;
        }
        taskEXIT_CRITICAL(&s_log_mux);
        return false;
    }

    char* payload = (char*)heap_caps_malloc(length, MALLOC_CAP_SPIRAM);
    if (!payload) payload = (char*)malloc(length);
    if (!payload) {
        taskENTER_CRITICAL(&s_log_mux);
        if (cdc) {
            s_cdc_cli_dropped++;
            if (!s_cdc_overflow_notice_pending) s_cdc_overflow_notice_offset = 0;
            s_cdc_overflow_notice_pending = true;
        } else {
            s_uart_cli_dropped++;
            if (!s_uart_overflow_notice_pending) s_uart_overflow_notice_offset = 0;
            s_uart_overflow_notice_pending = true;
        }
        taskEXIT_CRITICAL(&s_log_mux);
        return false;
    }
    memcpy(payload, text, length);

    taskENTER_CRITICAL(&s_log_mux);
    const bool route_blocked = cdc ? s_cdc_overflow_notice_pending : s_uart_overflow_notice_pending;
    const size_t route = cdc ? APP_LOG_OUTPUT_CDC : APP_LOG_OUTPUT_UART;
    if (s_mirror_count[route] >= MIRROR_QUEUE_DEPTH || route_blocked) {
        if (cdc) {
            s_cdc_cli_dropped++;
            if (!s_cdc_overflow_notice_pending) s_cdc_overflow_notice_offset = 0;
            s_cdc_overflow_notice_pending = true;
        } else {
            s_uart_cli_dropped++;
            if (!s_uart_overflow_notice_pending) s_uart_overflow_notice_offset = 0;
            s_uart_overflow_notice_pending = true;
        }
        taskEXIT_CRITICAL(&s_log_mux);
        free(payload);
        return false;
    }
    const size_t tail = (s_mirror_head[route] + s_mirror_count[route]) % MIRROR_QUEUE_DEPTH;
    MirrorRecord* record = &s_mirror_queue[route][tail];
    record->owned_payload = payload;
    record->length = (uint16_t)length;
    record->offset = 0;
    s_mirror_count[route]++;
    taskEXIT_CRITICAL(&s_log_mux);
    return true;
}

bool app_log_queue_cli_text(app_log_output_t output, const char* text, size_t length) {
    if (output != APP_LOG_OUTPUT_UART && output != APP_LOG_OUTPUT_CDC) return false;
    return app_log_queue_cli_text_route(output, text, length);
}

bool app_log_queue_cdc_text(const char* text, size_t length) {
    return app_log_queue_cli_text_route(APP_LOG_OUTPUT_CDC, text, length);
}

String app_log_get_json(void) {
    static const size_t MAX_LINES = 120;
    static char snapshot[MAX_LINES][LOG_LINE_MAX_LEN];
    uint64_t first_sequence = 0;
    uint64_t end_sequence = 0;

    taskENTER_CRITICAL(&s_log_mux);
    const size_t n = (s_log_count < MAX_LINES) ? s_log_count : MAX_LINES;
    end_sequence = s_log_next_sequence;
    first_sequence = end_sequence - n;
    taskEXIT_CRITICAL(&s_log_mux);

    // Copy one line per short critical section. Sequence tags make the result
    // a point-in-time snapshot with overwritten entries skipped; a concurrent
    // clear only affects snapshots whose initial count was taken afterwards.
    size_t copied = 0;
    for (uint64_t sequence = first_sequence; sequence < end_sequence; ++sequence) {
        const size_t slot = (size_t)(sequence % STATIC_LOG_LINES);
        bool valid = false;
        taskENTER_CRITICAL(&s_log_mux);
        if (s_log_lines && s_log_slot_sequence[slot] == sequence) {
            memcpy(snapshot[copied], s_log_lines + slot * LOG_LINE_MAX_LEN, LOG_LINE_MAX_LEN);
            snapshot[copied][LOG_LINE_MAX_LEN - 1] = '\0';
            valid = true;
        }
        taskEXIT_CRITICAL(&s_log_mux);
        if (valid) copied++;
    }

    JsonDocument doc;
    JsonArray arr = doc["logs"].to<JsonArray>();
    for (size_t i = 0; i < copied; i++) {
        // BLE advertised names can contain raw C0 bytes. ArduinoJson's string
        // writer does not escape every C0 value; keep exported JSON valid.
        for (size_t j = 0; snapshot[i][j]; ++j) {
            if ((unsigned char)snapshot[i][j] < 0x20) snapshot[i][j] = '?';
        }
        arr.add(snapshot[i]);
    }

    String out;
    out.reserve(copied * 64 + 24);
    serializeJson(doc, out);
    return out;
}

void app_log_clear(void) {
    taskENTER_CRITICAL(&s_log_mux);
    s_log_count = 0;
    taskEXIT_CRITICAL(&s_log_mux);
}

void app_log_set_cdc_enabled(bool enabled) {
    taskENTER_CRITICAL(&s_log_mux);
    s_cdc_log_enabled = enabled;
    taskEXIT_CRITICAL(&s_log_mux);
}

bool app_log_get_cdc_enabled(void) {
    taskENTER_CRITICAL(&s_log_mux);
    const bool enabled = s_cdc_log_enabled;
    taskEXIT_CRITICAL(&s_log_mux);
    return enabled;
}

void app_log_set_console_enabled(bool enabled) {
    taskENTER_CRITICAL(&s_log_mux);
    s_console_log_enabled = enabled;
    taskEXIT_CRITICAL(&s_log_mux);
}

bool app_log_get_console_enabled(void) {
    taskENTER_CRITICAL(&s_log_mux);
    const bool enabled = s_console_log_enabled;
    taskEXIT_CRITICAL(&s_log_mux);
    return enabled;
}

uint32_t app_log_get_mirror_dropped(void) {
    taskENTER_CRITICAL(&s_log_mux);
    const uint32_t dropped = s_mirror_dropped;
    taskEXIT_CRITICAL(&s_log_mux);
    return dropped;
}

uint32_t app_log_get_cli_dropped(app_log_output_t output) {
    taskENTER_CRITICAL(&s_log_mux);
    const uint32_t dropped = output == APP_LOG_OUTPUT_CDC
            ? s_cdc_cli_dropped : s_uart_cli_dropped;
    taskEXIT_CRITICAL(&s_log_mux);
    return dropped;
}
