#include "usb/uac_microphone.h"
#include "usb/usb_composite.h"
#include "app_config.h"
#include "core_diagnostics.h"
#include "esp32-hal-tinyusb.h"
#include "led_indicator.h"
#include "audio/audio_pipeline.h"
#include "wifi/wifi_manager.h"
#include "log/app_log.h"
#include "tusb.h"
#include "device/usbd_pvt.h"
#include "soc/usb_struct.h"
#include "soc/usb_reg.h"
#include "usb/dwc2_diagnostics.h"

#define UAC_DESC_TOTAL_LEN  108
#define UAC_TX_BLOCK_SAMPLES 32
#define UAC_TX_BLOCK_BYTES   (UAC_TX_BLOCK_SAMPLES * 2)
#define UAC_TX_BLOCKS        2
#define UAC_TX_PERIOD_MS     2
#define UAC_STALL_TIMEOUT_MS 500

static uint8_t s_uac_ep_in   = 0;
static uint8_t s_uac_itf_ac  = 0;
static uint8_t s_uac_itf_as  = 0;
static uint8_t s_uac_str_idx = 0;
static uint8_t s_uac_alt     = 0;
static volatile bool s_uac_streaming   = false;
static bool          s_uac_initialized = false;
static bool          s_uac_interface_enabled = false;
static bool          s_audio_pipeline_initialized = false;
static TaskHandle_t  s_uac_push_task = nullptr;

// Controls
static uint8_t  s_mic_mute   = 0;
static int16_t  s_mic_volume = 0x0000;

// Submit from TinyUSB task context, serialized with SET_INTERFACE/reset and
// transfer completion. The pacing task only queues one deferred service call.
static DRAM_ATTR int16_t s_tx_buf[UAC_TX_BLOCKS][UAC_TX_BLOCK_SAMPLES] __attribute__((aligned(4)));
static uint32_t s_tx_cur = 0;
static volatile uint32_t s_xfer_cb_count = 0;
static volatile uint32_t s_xfer_fail_count = 0;
static volatile uint32_t s_claim_skip_count = 0;
static volatile uint32_t s_last_complete_ms = 0;
static volatile uint32_t s_stream_start_ms = 0;
static volatile bool s_service_queued = false;
static volatile bool s_recovery_requested = false;
static uint32_t s_last_progress_ms = 0;
static uint32_t s_seen_completions = 0;
static volatile uint32_t s_fifo_rearms = 0;
static volatile uint32_t s_fifo_rearm_completions = 0;
static volatile bool s_fifo_rearm_waiting = false;
static uint32_t s_last_fifo_rearm_ms = 0;
static volatile uint32_t s_pcm_samples = 0;
static volatile uint32_t s_pcm_nonzero = 0;
static volatile uint64_t s_pcm_absolute_sum = 0;
static volatile uint16_t s_pcm_peak = 0;
#if defined(REMOTEMAPPER_DWC2_DRIVER)
static tusb_desc_endpoint_t s_uac_ep_desc = {};
static bool s_uac_ep_open = false;
#endif

static void uac_service(void*);
static void queue_uac_service(void) {
    if (!__atomic_exchange_n(&s_service_queued, true, __ATOMIC_ACQ_REL)) {
        usbd_defer_func(uac_service, nullptr, false);
    }
}

static void uac_service(void*) {
    // Even if the endpoint is stuck or Windows closed capture, honor resets on
    // the ring's one consumer. Otherwise a full old ring blocks new sessions.
    audio_pipeline_consume_pending_clear(&g_audio_pipeline);
    const uint32_t now = millis();
#if defined(REMOTEMAPPER_DWC2_DRIVER)
    if (remotemapper_dwc2_controller_faulted() && !s_recovery_requested) {
        s_recovery_requested = true;
        usb_composite_request_recovery(USB_RECOVERY_AUDIO);
        app_log("UAC", "DWC2 endpoint disable failed; requesting USB recovery");
    }
#endif
    if (!s_uac_streaming || !tud_ready() || s_recovery_requested || !s_uac_ep_in) {
        s_last_progress_ms = now;
        __atomic_store_n(&s_service_queued, false, __ATOMIC_RELEASE);
        return;
    }
    if (s_seen_completions != s_xfer_cb_count) {
        s_seen_completions = s_xfer_cb_count;
        s_last_progress_ms = now;
    }
    // The bundled ESP32-S3 DCD sets this FIFO-empty mask in dcd_edpt_xfer()
    // and clears it in the IN ISR using separate read/modify/write operations.
    // If an ISR clear wins over a new transfer's set, a full packet remains
    // pending with an empty FIFO and no enabled TXFE interrupt. The saved fault
    // had exactly this state (EP3, DIEPTSIZ=0x80040, DIEPINT TXFE, mask=0).
    // Re-arm only that precise state, from the serialized TinyUSB task, and
    // leave the 500 ms full-recovery path intact if it does not make progress.
#if !defined(REMOTEMAPPER_DWC2_DRIVER)
    if (now - s_last_progress_ms >= 30 &&
            now - s_last_fifo_rearm_ms >= 40 &&
            usbd_edpt_busy(0, s_uac_ep_in | 0x80)) {
        const uint32_t bit = 1u << s_uac_ep_in;
        const auto& ep = USB0.in_ep_reg[s_uac_ep_in];
        if (!(USB0.dtknqr4_fifoemptymsk & bit) &&
                (ep.diepctl & USB_D_EPENA1_M) &&
                (ep.diepint & USB_D_TXFEMP0_M) &&
                (ep.dieptsiz & USB_D_XFERSIZE1_M) == UAC_TX_BLOCK_BYTES &&
                ep.dtxfsts >= UAC_TX_BLOCK_BYTES / 4) {
            s_last_fifo_rearm_ms = now;
            s_fifo_rearm_waiting = true;
            ++s_fifo_rearms;
            USB0.dtknqr4_fifoemptymsk |= bit;
        }
    }
#endif
    // A missing completion for this long requires stack-managed USB recovery.
    if (now - s_last_progress_ms >= UAC_STALL_TIMEOUT_MS) {
        s_recovery_requested = true;
        usb_composite_request_recovery(USB_RECOVERY_AUDIO);
        app_log("UAC", "No USB audio completions for %ums: ep=0x%02X busy=%d skips=%u; requesting USB reconnect",
                UAC_STALL_TIMEOUT_MS,
                s_uac_ep_in | 0x80, usbd_edpt_busy(0, s_uac_ep_in | 0x80),
                (unsigned)s_claim_skip_count);
        __atomic_store_n(&s_service_queued, false, __ATOMIC_RELEASE);
        return;
    }
    const uint8_t ep = s_uac_ep_in | 0x80;
    if (usbd_edpt_claim(0, ep)) {
        const uint8_t block = s_tx_cur & 1;
        audio_pipeline_read_for_usb(&g_audio_pipeline, s_tx_buf[block], UAC_TX_BLOCK_SAMPLES);
        if (usbd_edpt_xfer(0, ep, (uint8_t*)s_tx_buf[block], UAC_TX_BLOCK_BYTES)) {
            if (audio_pipeline_is_active(&g_audio_pipeline)) {
                for (unsigned i = 0; i < UAC_TX_BLOCK_SAMPLES; ++i) {
                    const int32_t v = s_tx_buf[block][i];
                    const uint32_t amplitude = v < 0 ? (uint32_t)-v : (uint32_t)v;
                    s_pcm_samples++;
                    s_pcm_nonzero += amplitude != 0;
                    s_pcm_absolute_sum += amplitude;
                    if (amplitude > s_pcm_peak) s_pcm_peak = amplitude;
                }
            }
            s_tx_cur++;
        } else {
            s_xfer_fail_count++;
            usbd_edpt_release(0, ep);
        }
    } else {
        s_claim_skip_count++;
    }
    __atomic_store_n(&s_service_queued, false, __ATOMIC_RELEASE);
}

static void uac_push_task(void*) {
    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t interval = pdMS_TO_TICKS(UAC_TX_PERIOD_MS);
    while (1) {
        vTaskDelayUntil(&last_wake, interval);
        if (!tud_inited()) continue;
        queue_uac_service();
    }
}

// ---------------------------------------------------------------------------
// Descriptor
// ---------------------------------------------------------------------------
static uint16_t uac_load_descriptor(uint8_t *dst, uint8_t *itf) {
    if (!dst || !itf) return 0;

    s_uac_itf_ac = *itf;
    s_uac_itf_as = *itf + 1;
    *itf += 2;

    s_uac_str_idx = tinyusb_add_string_descriptor("RemoteMapper Wireless Microphone");

    uint8_t desc[UAC_DESC_TOTAL_LEN] = {
        // 1. IAD — 8 bytes
        0x08, 0x0B, s_uac_itf_ac, 0x02, 0x01, 0x00, 0x00, s_uac_str_idx,
        // 2. Standard AC Interface — 9 bytes
        0x09, 0x04, s_uac_itf_ac, 0x00, 0x00, 0x01, 0x01, 0x00, s_uac_str_idx,
        // 3. CS AC Header — 9 bytes
        0x09, 0x24, 0x01, 0x00, 0x01, 0x27, 0x00, 0x01, s_uac_itf_as,
        // 4. Input Terminal (Microphone) — 12 bytes
        0x0C, 0x24, 0x02, 0x01, 0x01, 0x02, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00,
        // 5. Feature Unit (Mute + Volume) — 9 bytes
        0x09, 0x24, 0x06, 0x02, 0x01, 0x01, 0x01, 0x02, 0x00,
        // 6. Output Terminal (USB Streaming) — 9 bytes
        0x09, 0x24, 0x03, 0x03, 0x01, 0x01, 0x00, 0x02, 0x00,
        // 7. Standard AS Interface Alt 0 (Zero BW) — 9 bytes
        0x09, 0x04, s_uac_itf_as, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
        // 8. Standard AS Interface Alt 1 (Active 16kHz) — 9 bytes
        0x09, 0x04, s_uac_itf_as, 0x01, 0x01, 0x01, 0x02, 0x00, 0x00,
        // 9. CS AS General — 7 bytes
        0x07, 0x24, 0x01, 0x03, 0x01, 0x01, 0x00,
        // 10. Format Type I (16kHz, 16-bit, Mono) — 11 bytes
        0x0B, 0x24, 0x02, 0x01, 0x01, 0x02, 0x10, 0x01, 0x80, 0x3E, 0x00,
        // 11. Isochronous Endpoint — 9 bytes (64 bytes each 2 ms frame).
        0x09, 0x05, (uint8_t)(s_uac_ep_in | 0x80), 0x05, 0x40, 0x00, 0x02, 0x00, 0x00,
        // 12. CS Endpoint General — 7 bytes
        0x07, 0x25, 0x01, 0x00, 0x00, 0x00, 0x00
    };

    memcpy(dst, desc, sizeof(desc));
    return sizeof(desc);
}

// ---------------------------------------------------------------------------
// TinyUSB Custom Class Driver
// ---------------------------------------------------------------------------
static void uac_driver_init(void) {}

static void uac_driver_reset(uint8_t rhport) {
    (void)rhport;
    s_uac_streaming = false;
    s_uac_alt = 0;
    s_last_progress_ms = millis();
    s_recovery_requested = false;
#if defined(REMOTEMAPPER_DWC2_DRIVER)
    s_uac_ep_open = false;
#endif
    app_log("UAC", "USB Bus Reset detected -> UAC state reset");
}

static uint16_t uac_driver_open(uint8_t rhport, tusb_desc_interface_t const *desc_intf, uint16_t max_len) {
    if (desc_intf->bInterfaceClass != TUSB_CLASS_AUDIO) return 0;
    uint8_t const *p = (uint8_t const *)desc_intf;
    uint16_t len = 0;
    while (len < max_len) {
        if (p[1] == TUSB_DESC_INTERFACE) {
            if (((tusb_desc_interface_t const*)p)->bInterfaceClass != TUSB_CLASS_AUDIO) break;
        } else if (p[1] == TUSB_DESC_ENDPOINT) {
#if defined(REMOTEMAPPER_DWC2_DRIVER)
            // Alternate setting 0 has no audio endpoint. Save its descriptor
            // and activate only when the host selects alternate setting 1.
            memcpy(&s_uac_ep_desc, p, sizeof(s_uac_ep_desc));
#else
            usbd_edpt_open(rhport, (tusb_desc_endpoint_t const *)p);
#endif
        }
        len += p[0]; p += p[0];
    }
    return len;
}

#if defined(REMOTEMAPPER_DWC2_DRIVER)
static bool uac_activate_endpoint(uint8_t rhport) {
    if (s_uac_ep_open) return true;
    if (s_recovery_requested) return false;
    app_log("UAC", "Opening ISO EP 0x%02X size=%u interval=%u",
            s_uac_ep_desc.bEndpointAddress,
            (unsigned)tu_edpt_packet_size(&s_uac_ep_desc),
            (unsigned)s_uac_ep_desc.bInterval);
    if (!usbd_edpt_open(rhport, &s_uac_ep_desc)) {
        app_log("UAC", "ISO EP open failed at alt=1");
        return false;
    }
    s_uac_ep_open = true;
    return true;
}

static void uac_deactivate_endpoint(uint8_t rhport) {
    if (!s_uac_ep_open) return;
    s_uac_streaming = false;
    usbd_edpt_close(rhport, s_uac_ep_in | 0x80);
    s_uac_ep_open = false;
    s_fifo_rearm_waiting = false;
    if (remotemapper_dwc2_close_failed()) {
        s_recovery_requested = true;
        usb_composite_request_recovery(USB_RECOVERY_AUDIO);
        app_log("UAC", "ISO endpoint did not disable in 2ms; requesting USB recovery");
    }
}
#endif

static bool uac_driver_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                         tusb_control_request_t const *req) {
    if (stage != CONTROL_STAGE_SETUP) return true;

    // 1. Standard USB Requests
    if (req->bmRequestType_bit.type == TUSB_REQ_TYPE_STANDARD) {
        if (req->bRequest == TUSB_REQ_SET_INTERFACE) {
            uint8_t itf = (uint8_t)req->wIndex;
            if (itf != s_uac_itf_as && itf != s_uac_itf_ac) {
                return false; // Crucial: Let other class drivers (HID, CDC) handle their own interfaces!
            }
            uint8_t alt = (uint8_t)req->wValue;
            if (itf == s_uac_itf_ac) {
                return alt == 0 && tud_control_status(rhport, req);
            }
            if (req->wValue > 1) return false;
#if defined(REMOTEMAPPER_DWC2_DRIVER)
            if (alt == 0) uac_deactivate_endpoint(rhport);
            else if (!uac_activate_endpoint(rhport)) return false;
#endif
            if (alt == 1 && s_uac_alt == 0) {
                s_stream_start_ms = millis();
                s_last_progress_ms = s_stream_start_ms;
                s_seen_completions = s_xfer_cb_count;
                s_fifo_rearm_waiting = false;
            }
            if (alt != s_uac_alt) {
                app_log("UAC", alt ? "Stream ARMED by host (alt=1, 16kHz mono)" : "Stream STOPPED by host (alt=0)");
            }
            s_uac_alt       = alt;
            s_uac_streaming = (alt == 1);

            // The legacy driver opens once at enumeration; DWC2 opens only for
            // alt 1 and closes at alt 0. Both use the same paced TX service.
            return tud_control_status(rhport, req);
        } else if (req->bRequest == TUSB_REQ_GET_INTERFACE) {
            uint8_t itf = (uint8_t)req->wIndex;
            if (itf != s_uac_itf_as && itf != s_uac_itf_ac) {
                return false; // Crucial: Let other class drivers handle their own interfaces!
            }
            static uint8_t ac_alt = 0;
            return tud_control_xfer(rhport, req,
                    itf == s_uac_itf_ac ? &ac_alt : &s_uac_alt, 1);
        }
        return false;
    }

    // 2. Class-Specific USB Requests
    if (req->bmRequestType_bit.type == TUSB_REQ_TYPE_CLASS) {
        uint8_t target_itf = (uint8_t)req->wIndex;
        if (target_itf != s_uac_itf_ac && target_itf != s_uac_itf_as && target_itf != 0x02) {
            return false; // Crucial: Let HID / CDC class drivers handle their own class requests!
        }
        uint8_t r  = req->bRequest;
        uint8_t cs = (uint8_t)(req->wValue >> 8);
        if (r == 0x81) {   // GET_CUR
            if (cs == 0x01) return tud_control_xfer(rhport, req, &s_mic_mute,   1);
            else             return tud_control_xfer(rhport, req, &s_mic_volume, 2);
        }
        if (r == 0x01) {   // SET_CUR
            if (cs == 0x01) return tud_control_xfer(rhport, req, &s_mic_mute,   1);
            else             return tud_control_xfer(rhport, req, &s_mic_volume, 2);
        }
        static int16_t vol_min = -32768, vol_max = 0, vol_res = 256;
        if (r == 0x82) return tud_control_xfer(rhport, req, &vol_min, 2);
        if (r == 0x83) return tud_control_xfer(rhport, req, &vol_max, 2);
        if (r == 0x84) return tud_control_xfer(rhport, req, &vol_res, 2);
        return false;
    }
    return false;
}

static bool uac_driver_xfer_cb(uint8_t rhport, uint8_t ep_addr,
                                 xfer_result_t result, uint32_t xferred_bytes) {
    (void)rhport;
    if (ep_addr == (uint8_t)(s_uac_ep_in | 0x80)) {
        if (result == XFER_RESULT_SUCCESS && xferred_bytes == UAC_TX_BLOCK_BYTES) {
            s_xfer_cb_count++;
            s_last_complete_ms = millis();
            if (s_fifo_rearm_waiting) {
                s_fifo_rearm_waiting = false;
                ++s_fifo_rearm_completions;
            }
            // Schedule the next packet from the USB completion, keeping its
            // cadence locked to host polls. The 2 ms task remains a fallback
            // when an endpoint is busy or a completion is missed.
#if !defined(REMOTEMAPPER_DWC2_DRIVER)
            if (s_uac_streaming) queue_uac_service();
#endif
        } else {
            s_xfer_fail_count++;
        }
        return true;
    }
    return true;
}

static usbd_class_driver_t const s_uac_driver = {
#if CFG_TUSB_DEBUG >= CFG_TUD_LOG_LEVEL
    .name             = "UAC_MIC",
#endif
    .init             = uac_driver_init,
    .reset            = uac_driver_reset,
    .open             = uac_driver_open,
    .control_xfer_cb  = uac_driver_control_xfer_cb,
    .xfer_cb          = uac_driver_xfer_cb,
    .sof              = NULL
};

usbd_class_driver_t const* usbd_app_driver_get_cb(uint8_t* driver_count) {
    *driver_count = 1;
    return &s_uac_driver;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
extern "C" {

bool uac_microphone_init(void) {
    if (s_uac_initialized) return true;
    // UAC owns pipeline startup. Keep this guard so a retry after a partial
    // interface/task failure never resets the live ring buffer or leaks it.
    if (!s_audio_pipeline_initialized) {
        audio_pipeline_init(&g_audio_pipeline);
        s_audio_pipeline_initialized = true;
    }

    if (s_uac_ep_in == 0) {
        s_uac_ep_in = tinyusb_get_free_in_endpoint();
    }
    if (s_uac_ep_in == 0) {
        app_log("UAC", "Failed to allocate microphone IN endpoint");
        return false;
    }

    // Register the interface before starting its worker. On retry, retain the
    // successful registration and only retry task creation; never leave a
    // temporary worker running after an interface-registration failure.
    if (!s_uac_interface_enabled) {
        esp_err_t err = tinyusb_enable_interface(USB_INTERFACE_CUSTOM, UAC_DESC_TOTAL_LEN, uac_load_descriptor);
        if (err != ESP_OK) {
            app_log("UAC", "Failed to enable UAC interface: %d", err);
            return false;
        }
        s_uac_interface_enabled = true;
    }

    // Spawn the 500Hz TX pump once. Failed creation leaves init unready and
    // permits a later retry without duplicating the interface or worker.
    if (s_uac_push_task == nullptr && xTaskCreatePinnedToCore(uac_push_task, "uac_push", 4096, NULL,
            PRIO_TASK_USB, &s_uac_push_task, TASK_CORE_USB) != pdPASS) {
        s_uac_push_task = nullptr;
        app_log("UAC", "Failed to spawn TX pump task");
        return false;
    }
    if (!s_uac_initialized) {
        core_diagnostics_register("uac_push", s_uac_push_task, TASK_CORE_USB);
        s_uac_initialized = true;
    }
    app_log("UAC", "UAC 1.0 Microphone ready (EP %d IN, 500Hz serialized TX + recovery)", s_uac_ep_in);
    return true;
}

bool uac_microphone_is_ready(void) {
    return s_uac_initialized && s_uac_interface_enabled && s_uac_push_task != nullptr;
}

void uac_microphone_task(void) {
    static uint32_t last_activity_ms = 0;
    const uint32_t now = millis();
    if (s_uac_streaming && now - last_activity_ms >= 500) {
        wifi_manager_mark_activity();
        last_activity_ms = now;
    }
}

void uac_microphone_get_control(uint8_t* mute, int16_t* volume) {
    if (mute)   *mute   = s_mic_mute;
    if (volume) *volume = s_mic_volume;
}

uint8_t uac_microphone_get_alt(void) {
    return s_uac_alt;
}

uint32_t uac_microphone_get_stream_start_ms(void) {
    return s_stream_start_ms;
}

bool uac_microphone_is_streaming(void) {
    return s_uac_streaming;
}

} // extern "C"
void uac_microphone_get_stats(uac_tx_stats_t* stats) {
    if (!stats) return;
    stats->completed = s_xfer_cb_count;
    stats->failed = s_xfer_fail_count;
    stats->claim_skips = s_claim_skip_count;
    stats->recoveries = usb_composite_recovery_count();
    stats->last_complete_ms = s_last_complete_ms;
    stats->endpoint = s_uac_ep_in | 0x80;
    stats->recovery_pending = s_recovery_requested;
}

void uac_microphone_get_fifo_rearm_stats(uint32_t* attempts, uint32_t* completions) {
    if (attempts) *attempts = s_fifo_rearms;
    if (completions) *completions = s_fifo_rearm_completions;
}

void uac_microphone_get_pcm_stats(uint32_t* samples, uint32_t* nonzero,
                                  uint64_t* absolute_sum, uint16_t* peak) {
    if (samples) *samples = s_pcm_samples;
    if (nonzero) *nonzero = s_pcm_nonzero;
    if (absolute_sum) *absolute_sum = s_pcm_absolute_sum;
    if (peak) *peak = s_pcm_peak;
}
