# TEST 3 voice-input stall, 2026-10-02

Firmware: `1.2.44-usbtest3`. TEST 4 was not flashed. The user reported stuck
voice input during ordinary use after approximately 3 h 33 min of uptime.
Raw snapshots and the log ring are saved locally under
`.cache/usb-diagnostics/20261002-live-stall/incident.json` and
`after-capture.json`.

The log retained this sequence (times are seconds since boot):

- 12722.405: voice-key release; the HID release remained pending.
- 12723.453: HID stall guard forced release; USB recovery #1 detached/reconnected.
- 12724.106: host mounted the device again; a keyboard release completed.
- 12728.335: host opened audio alt 1. Only one subsequent audio completion was
  recorded (last completion at 12728.341); the 500 ms stall detector requested
  recovery.
- Voice sessions 60/61 received 57/36 BLE audio frames with zero USB progress.
- 12733.455: USB recovery #2. A new capture at 12735.375 also stalled; session 62
  received 85 BLE frames, zero USB completions. Session 63 received 170 BLE
  frames and filled the 32768-sample ring without USB progress.
- 12743.456: USB recovery #3, followed by host mount at 12744.097.

The initial observation had `usb_completed=419825`, `usb_recoveries=3`, no
active voice session, no held key and no pending keyboard release. No full MCU
restart occurred during the incident (`boot_usb_recovery_restart=false`,
uptime remained continuous). `usb_failed=0` does not exclude a stall: that
counter counts rejected/failed transfers, not a missing completion.

A one-cycle Windows WinMM open/capture/close probe then passed: 40248 bytes
in 1.272 s, 606 USB completions, about 31643 bytes/s and 476 packets/s.
Recovery count stayed at 3. Audio was discarded, not saved. The user exited
the old voice-input window with Esc and started a new voice session, confirming
recognition and normal release. No manual reboot or firmware update was needed.

The evidence shows the last USB reconnect restored device-side capture by the
time of the probe. The previous application capture session needed reopening;
the probe itself also opened the endpoint, so its contribution cannot be
separated from recovery #3. It is not evidence that automatic recovery alone
restores every application session.

The last-fault register snapshot has GINTSTS bit 20 (`IISOIXFR`, incomplete ISO
IN transfer) set. The vendored TinyUSB 0.16 DWC2 source does not implement an
incomplete-ISO handler. This warrants investigation of ISO missed-frame
handling and the HID stall that preceded the reconnect, but does not prove
which event initiated the fault. The saved endpoint registers were captured
later during recovery, not at the first stalled transfer. The separately
persisted first-audio-fault structure at this initial observation belonged to
the older incident and must not be interpreted as a new TEST 3 snapshot.

The previous 100-cycle and nearly-30-session checks were short regression
checks. This incident establishes that TEST 3 remains vulnerable during longer
use. TEST 4 refactoring/hardening is not yet a verified fix for this incident.

## Subsequent automatic restart

The user subsequently reported another stall followed by a restart. The new
snapshot is saved in
`.cache/usb-diagnostics/20261002-repeat-restart/incident.json`.
At observation, uptime was 567 seconds, `boot_reset_reason=3` (software
restart) and `boot_usb_recovery_restart=true`. Together these identify the
firmware's USB recovery restart path; they do not indicate a watchdog reset,
brownout or a panic.

The restart path persisted fresh first-audio-fault evidence from TEST 3 before
reboot. That snapshot supersedes the older persisted snapshot described above:

- Fault time: 12733453 ms; last audio completion: 12728341 ms.
- Audio completions: 419825; failed submissions: 0.
- Audio IN endpoint 0x83 remained enabled with 64 bytes/one packet pending.
- TX FIFO had four free words (16 bytes); FIFO-empty mask was 8.
- GINTSTS bit 20 (incomplete ISO IN) was set.

This supports investigation of a pending ISO transfer that did not complete;
the snapshot still does not establish the original cause of the preceding HID
stall. Register reads are sequential and are not an atomic controller snapshot.

Within approximately nine minutes after reboot, the device had already counted
seven USB recoveries. The latest recovery, at 519497 ms, was requested by the
HID stall path (reason 2). HID endpoint 0x81 was not ready and had nine bytes
pending. Thus the restart restored operation temporarily but did not remove
the underlying instability, which affects both HID and audio.

The subsequent voice session 23 lasted 6.346 seconds and recorded 412 BLE
frames, 3145 USB completions and 100704 PCM samples (82201 nonzero). Its key
release completed. This verifies renewed device-side audio transmission and
release during that session, not sustained stability or application recognition.

No firmware was flashed during either investigation. TEST 5 source contains
targeted DWC2 incomplete-ISO and bus-reset cleanup changes and has compiled
successfully, but remains untested on this device. The initial HID stall still
requires separate diagnosis before treating those changes as a complete fix.

## TEST 5 deployment authorized by user

The user subsequently authorized flashing. The experimental N16R8 environment
was rebuilt successfully and its firmware uploaded to `/api/ota/upload`.
The device confirmed `1.2.44-usbtest5`, running in app0, with
`boot_usb_recovery_restart=false`; BLE reached connected state 3.

Three Windows WinMM open/capture/close cycles passed, delivering approximately
31619–31791 bytes/s; audio was discarded. USB recovery count stayed zero.
After the probe, counters showed 79 incomplete ISO events, 56 retries and 23
aborts, matching 23 failed-transfer callbacks. FIFO reset failures remained
zero. These events are now handled and observable rather than silently left
pending; their frequency still requires investigation. This short probe
verifies host capture transport, not actual remote speech recognition or
long-term stability. The user will continue real voice-input testing.
