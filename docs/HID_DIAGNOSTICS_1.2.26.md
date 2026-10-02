# 1.2.26 — diagnostics before USB recovery

This is a diagnostic release, not a claim that the HID stall is fixed. Transmission logic and the 1.2.25 recovery policy are unchanged.

`GET /api/guard` now includes `hid_diagnostics`:

- `live`: distinct counts for attempts, uninitialized transport, mutex timeout, not-ready endpoint, accepted/rejected submission, completion timeout, successful semaphore wait, and TinyUSB completion callback.
- `live.events`: the last 24 observations, including millisecond time, report ID, and CPU core. A callback can occur before the submission function returns; chronological hook order is not necessarily USB wire order.
- `live.timeout_registers`: the most recent completion-timeout observation, plus `timeout_ms`. Zero values before the first timeout mean no observation.
- `last_fault`: frozen counters/events and both HID/UAC endpoint observations before the recovery path force-releases or disconnects. Reason 1 is audio, 2 is HID. The latest recovery replaces the prior saved fault; `count` tracks captures.

The HID endpoint is read from the active configuration descriptor rather than assumed to be IN1. Register reads are non-destructive and sequential; this is not an atomic hardware snapshot. The `hid_ready` field always describes HID, including inside the audio register object. A cleared FIFO-empty mask bit alone does not diagnose a bug: it is normal after loading the FIFO.

Snapshots survive USB re-enumeration, but not power loss or MCU restart. Use `tools/capture_usb_diagnostics.py --seconds 180` during testing to archive them to `.cache/usb-diagnostics/`. This reads only diagnostics, does not generate keyboard input, and does not record audio. The HTTP reads can span state transitions, so the frozen fault object is the preferred reference over correlations across separate responses.

The existing reproducible build patcher adds an optional weak hook to Arduino USBHID. It keeps the framework semaphore and return behavior, validates source anchors before writing, and fails the build on an incompatible source. On normal traffic the hook uses fixed-size RAM and a short critical section; no logging, allocation, register access, or waiting occurs. Instrumentation can still affect timing, so absence of reproduction is not proof of repair.

Before OTA, the running 1.2.25 reported one recovery with `last_reason=uac-stalled` at 525124 ms. Pre-update status was saved in `.cache/before-1.2.26/`. The prior binary and ELF are in `.cache/firmware-1.2.25/` for rollback/reference.

Manual acceptance sequence: five holds of 10–20 seconds with release, one hold through the remote's 60-second cutoff, then another short hold/release. Verify Windows exits recording and subsequent keyboard behavior is normal; also check recovery counts and submission/completion progress. A recovery is a reproduced failure, even if the user-visible state automatically becomes normal.

## Initial deployed verification

OTA returned HTTP 200 and `/api/status` confirmed 1.2.26. The diagnostic hook is linked and active. At startup a keyboard submission at 997 ms timed out at 1017 ms, then completed at 1024 ms; two subsequent reports completed successfully. Thus one SendReport failure was a late completion, not a persistent stuck endpoint. At that timeout the FIFO-empty mask was zero and the transfer-size register was 524288; this is also a concrete example of why the mask bit alone cannot establish the suspected race. No recovery was triggered.

The Windows WinMM test passed all 10 open/capture/close cycles with 40,248–44,728 bytes received per cycle, zero USB failed completions and zero recoveries. This validates audio regression only. It does not validate physical release handling or eliminate the concurrency candidate. A bounded 180-second read-only capture was started for the manual sequence.

That capture completed with 147 samples and zero request errors; 89 samples observed an active voice session. At the final read there were 77 accepted HID submissions and 77 callbacks, no pending release and zero recoveries. The sole completion timeout remained the startup event at 1017 ms. No persistent HID stall was reproduced in this window; host recognition behavior and the complete 60-second boundary sequence still require user confirmation. Data is in `.cache/usb-diagnostics/1.2.26-first-test/`.

## 1.2.27 follow-up: confirmed log export defect

The final `/api/logs` capture contained valid log messages followed by invalid UTF-8 and raw memory fragments. Inspection found `app_log_get_json()` reading `(start_idx + i)` without wrapping the index modulo the 250-line ring. When the selected 120 lines crossed the ring boundary, the exporter read outside its allocation. Version 1.2.27 wraps each read index. This repairs diagnostic integrity; it is an out-of-bounds read and is not established as the cause of the HID stall. The malformed original response is retained locally as evidence and must not be presented as valid device log entries.

Validation: extracted the corrected indexing expression from the source and checked it against an independent chronological list of the latest 120 entries over 1,000 sequential writes. All snapshots matched, including 357 boundary-crossing snapshots; the old expression would have made 21,420 out-of-bounds reads in those cases. This is a host-side indexing check, not a full firmware execution test. The N16R8 firmware build also passed.

On-device 1.2.27 startup verification then exposed a separate JSON problem: a nearby BLE device advertised a name containing byte 0x06, which the JSON string writer emitted literally. Version 1.2.28 replaces raw C0 control bytes in the export snapshot with `?`, leaving the in-memory log unchanged. This is diagnostic-output sanitization and does not change HID/audio behavior.

Final deployment: 1.2.28 OTA returned HTTP 200. At uptime 26 seconds, `/api/status` confirmed the version, HID had three callbacks, no pending release and zero recoveries. `/api/logs` contained 93 entries and passed strict UTF-8 JSON parsing. Boot responses are archived in `.cache/usb-diagnostics/1.2.28-boot/`. The 180-second desktop collector has finished; firmware-side fault capture remains enabled.
