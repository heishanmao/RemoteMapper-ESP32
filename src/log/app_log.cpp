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
#define MIRROR_QUEUE_DEPTH 16
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
    uint16_t console_offset;
    uint16_t cdc_offset;
    bool send_console;
    bool send_cdc;
};

// Static bounded queue: callback-side logging never allocates, waits for a
// consumer, or writes to a serial transport. New mirror messages are dropped
// when full; the in-memory log ring continues recording them.
static MirrorRecord s_mirror_queue[MIRROR_QUEUE_DEPTH] = {};
static size_t s_mirror_head = 0;
static size_t s_mirror_count = 0;
static uint32_t s_mirror_dropped = 0;
static bool s_cdc_log_enabled = false;
static bool s_console_log_enabled = true;

static portMUX_TYPE s_log_mux = portMUX_INITIALIZER_UNLOCKED;

void app_log_init(void) {
    if (!s_log_lines) {
        s_log_lines = (char*)heap_caps_malloc(LOG_RING_BYTES, MALLOC_CAP_SPIRAM);
        if (!s_log_lines) {
            s_log_lines = (char*)malloc(LOG_RING_BYTES); // internal-RAM fallback
        }
    }

    char* abandoned[MIRROR_QUEUE_DEPTH] = {};
    memset(s_log_slot_sequence, 0, sizeof(s_log_slot_sequence));
    taskENTER_CRITICAL(&s_log_mux);
    for (size_t i = 0; i < s_mirror_count; ++i) {
        const size_t slot = (s_mirror_head + i) % MIRROR_QUEUE_DEPTH;
        abandoned[i] = s_mirror_queue[slot].owned_payload;
    }
    s_log_head = 0;
    s_log_count = 0;
    s_log_next_sequence = 0;
    s_mirror_head = 0;
    s_mirror_count = 0;
    s_mirror_dropped = 0;
    taskEXIT_CRITICAL(&s_log_mux);
    for (size_t i = 0; i < MIRROR_QUEUE_DEPTH; ++i) free(abandoned[i]);
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

    const bool mirror_console = s_console_log_enabled;
    const bool mirror_cdc = s_cdc_log_enabled;
    if (mirror_console || mirror_cdc) {
        if (s_mirror_count < MIRROR_QUEUE_DEPTH) {
            const size_t tail = (s_mirror_head + s_mirror_count) % MIRROR_QUEUE_DEPTH;
            MirrorRecord* record = &s_mirror_queue[tail];
            // Reserve both the newline and terminator even when snprintf
            // filled the complete ring line. The ring retains its own copy.
            const size_t mirror_len = line_len < LOG_LINE_MAX_LEN - 2
                    ? line_len : LOG_LINE_MAX_LEN - 2;
            memcpy(record->line, full_line, mirror_len);
            record->line[mirror_len] = '\n';
            record->line[mirror_len + 1] = '\0';
            record->owned_payload = NULL;
            record->length = (uint16_t)(mirror_len + 1);
            record->console_offset = 0;
            record->cdc_offset = 0;
            record->send_console = mirror_console;
            record->send_cdc = mirror_cdc;
            s_mirror_count++;
        } else {
            s_mirror_dropped++;
        }
    }
    taskEXIT_CRITICAL(&s_log_mux);
}

void app_log_task(void) {
    MirrorRecord record;
    taskENTER_CRITICAL(&s_log_mux);
    if (!s_mirror_count) {
        taskEXIT_CRITICAL(&s_log_mux);
        return;
    }
    record = s_mirror_queue[s_mirror_head];
    taskEXIT_CRITICAL(&s_log_mux);

    // Limit each transport to a small chunk per main-loop call. HardwareSerial
    // has a bounded TX ring; availability is checked before writing, and all
    // application Serial writes are made by this same loop task.
    if (record.send_console && record.console_offset < record.length && Serial) {
        int available = Serial.availableForWrite();
        if (available > 0) {
            size_t remaining = record.length - record.console_offset;
            size_t amount = (size_t)available;
            if (amount > MIRROR_CHUNK_BYTES) amount = MIRROR_CHUNK_BYTES;
            if (amount > remaining) amount = remaining;
            if (amount) {
                Serial.write((const uint8_t*)record.line + record.console_offset, amount);
                record.console_offset = (uint16_t)(record.console_offset + amount);
            }
        }
    }

    // TinyUSB's tud_cdc_n_write() is a single bounded FIFO write (unlike
    // USBCDC::write(), which loops until all bytes are accepted). The main
    // loop is the only application-side CDC writer: cli_manager_task() also
    // runs there. This single write reports the bytes accepted; the USB stack
    // consumes the FIFO asynchronously and this function never retries a full
    // FIFO in a loop.
    if (record.send_cdc && record.cdc_offset < record.length && !tud_cdc_n_connected(0)) {
        // Disconnected CDC has no consumer; discard this mirror copy so it
        // cannot pin the bounded queue indefinitely.
        record.cdc_offset = record.length;
    } else if (record.send_cdc && record.cdc_offset < record.length) {
        const size_t remaining = record.length - record.cdc_offset;
        const uint32_t amount = remaining > MIRROR_CHUNK_BYTES
                                    ? MIRROR_CHUNK_BYTES
                                    : (uint32_t)remaining;
        const char* payload = record.owned_payload ? record.owned_payload : record.line;
        uint32_t sent = tud_cdc_n_write(0, payload + record.cdc_offset, amount);
        if (sent > amount) sent = amount;
        record.cdc_offset = (uint16_t)(record.cdc_offset + sent);
        if (sent) {
            tud_cdc_n_write_flush(0);
        }
    }

    char* released_payload = NULL;
    taskENTER_CRITICAL(&s_log_mux);
    if (s_mirror_count) {
        MirrorRecord* queued = &s_mirror_queue[s_mirror_head];
        queued->console_offset = record.console_offset;
        queued->cdc_offset = record.cdc_offset;
        const bool console_done = !queued->send_console || queued->console_offset >= queued->length;
        const bool cdc_done = !queued->send_cdc || queued->cdc_offset >= queued->length;
        if (console_done && cdc_done) {
            released_payload = queued->owned_payload;
            s_mirror_head = (s_mirror_head + 1) % MIRROR_QUEUE_DEPTH;
            s_mirror_count--;
        }
    }
    taskEXIT_CRITICAL(&s_log_mux);
    free(released_payload);
}

bool app_log_queue_cdc_text(const char* text, size_t length) {
    if (!text || !length || length > MIRROR_TEXT_MAX_BYTES) {
        taskENTER_CRITICAL(&s_log_mux);
        s_mirror_dropped++;
        taskEXIT_CRITICAL(&s_log_mux);
        return false;
    }

    char* payload = (char*)heap_caps_malloc(length, MALLOC_CAP_SPIRAM);
    if (!payload) payload = (char*)malloc(length);
    if (!payload) {
        taskENTER_CRITICAL(&s_log_mux);
        s_mirror_dropped++;
        taskEXIT_CRITICAL(&s_log_mux);
        return false;
    }
    memcpy(payload, text, length);

    taskENTER_CRITICAL(&s_log_mux);
    if (s_mirror_count >= MIRROR_QUEUE_DEPTH) {
        s_mirror_dropped++;
        taskEXIT_CRITICAL(&s_log_mux);
        free(payload);
        return false;
    }
    const size_t tail = (s_mirror_head + s_mirror_count) % MIRROR_QUEUE_DEPTH;
    MirrorRecord* record = &s_mirror_queue[tail];
    record->owned_payload = payload;
    record->length = (uint16_t)length;
    record->console_offset = 0;
    record->cdc_offset = 0;
    record->send_console = false;
    record->send_cdc = true;
    s_mirror_count++;
    taskEXIT_CRITICAL(&s_log_mux);
    return true;
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
