# HID / UAC concurrency investigation — 1.2.46 / 1.2.47 USB TEST6

## Fault evidence

The running `1.2.45-usbtest5` captured a HID stall at uptime 179445 ms.
The current fault is separate from the historical, persisted TEST3 audio fault.
Its keyboard IN endpoint was enabled with one 9-byte packet pending
(`DIEPTSIZ=0x80009`), a completely empty FIFO (`DTXFSTS=16` words), TXFE set,
and `DIEPEMPMSK=0`. The state was also present at the completion timeout
178378 ms, and subsequent HID attempts only returned not-ready. Audio had
completed a packet at 179443 ms, two milliseconds before the fault capture.
Thus this incident was a stalled HID endpoint while UAC continued, rather
than an application mutex deadlock or a missing completion notification alone.

The original driver enabled TXFE with `diepempmsk |= bit` during task-side
submission and disabled it with `diepempmsk &= ~bit` in the ISR. Neither side
used a shared cross-core critical section. HID can submit from BLE on core 0;
UAC and the USB interrupt run on core 1. An ISR can read the old UAC bit,
a HID submission can add its bit, and the ISR can write its stale result,
removing the HID bit. This mechanism is sufficient to produce the observed
pending packet, empty FIFO and disabled interrupt. Register snapshots alone
do not prove the exact instruction interleaving, or explain every historical
restart.

Arduino's `USBHID::SendReport` calls `tud_hid_n_report` in its caller's task.
The TinyUSB 0.16 endpoint submission path calls the DCD directly; its endpoint
claim mutex does not serialize different endpoints' controller registers.
The existing non-experimental UAC driver had a narrowly targeted TXFE rearm
workaround, which was excluded from the experimental driver. TEST5 retained
the unsafe read/modify/write operations.

## Fix and boundaries

TEST6 uses a single ESP32 `portMUX` shared by task-side controller updates
and their ISR counterparts. It protects transfer descriptors together with
the register operations that publish them, including TXFE mask updates.
Endpoint shutdown and bus reset prevent new transfers from using a descriptor
being disabled or cleared. Hardware polling remains outside the cross-core
critical section; completion events are queued after unlocking.

This follows the need for critical sections documented in the
[official TinyUSB 0.20 DWC2 driver](https://github.com/hathach/tinyusb/blob/0.20.0/src/portable/synopsys/dwc2/dcd_dwc2.c),
but pairs the task and ISR protection explicitly for this dual-core port.
It retains TEST5's ISO frame selection, bounded hardware waits, ISO retry,
and bus-reset cleanup. FIFO loading timing is unchanged in this revision.
The normal firmware target remains `1.2.43`; the fix is in the opt-in USB target.

## Verification

`tools/tests/test_dwc2_mask_race.py` enumerates independent endpoint mask
updates. It finds both lost-enable and lost-disable counterexamples without
mutual exclusion, shows that locking only one participant is insufficient,
and checks 416 legal schedules with the shared lock. These are synchronization
model tests, not a simulation of the USB controller or proof of firmware
lock coverage.

Hardware validation must include HID reports while Windows opens, captures
and closes the UAC microphone. The previous microphone-only cycling test
did not exercise concurrent HID submissions. The experimental CLI stress
sender emits only zero keyboard and consumer reports on core 0, exits when
a real user action appears, and never changes desired key state or guard
release debt. Host capture discards the microphone samples.

Build, deployment and measured hardware results will be recorded below.

## CDC control discovered during validation

The first automated attempts did not start a HID worker, so they are not
successful concurrency tests. The tool initially selected a USB-UART port;
it now requires the RemoteMapper native CDC VID/PID `303A:8089` and refuses
other UART devices. Arduino's CDC boolean also depends on both modem-control
lines and an esptool line-state machine. Received commands were left queued
behind the application's `if (USBSerial)` receive gate, and executed when a
later terminal connection met that gate. `1.2.47-usbtest6` keeps the same DCD
fix and drains received CDC bytes independently of that boolean. Output still
uses the existing connected-state gate. This is a separate command-reception
defect, not evidence that CDC caused the original 179-second HID incident.

The first active combined run stopped at cycle 2 due to a measurement failure:
the main loop was delayed by synchronous CDC reply writes while the test was
using HTTP diagnostics. Its single 2-second capture buffer filled during that
delay, invalidating the bytes/second measurement. A later quiet-CDC attempt
also encountered HTTP latency beyond its capture buffer capacity. HID reports
continued and there was no USB recovery. The final test keeps native CDC
DTR/RTS deasserted, stops capture before reading HTTP, and provides a 10-second
buffer for scheduler delays. Its capture rate uses only the actual recording
interval; HTTP response time is excluded. Failed attempts are retained
separately; they are not counted as successful cycles.

The tool also supports an explicitly owned, already-running HID worker via
`--observe-existing-hid`; in that mode it does not open a serial port and its
caller is responsible for stopping the worker. This mode was not needed for
the final combined run.

## Deployment and measured results

Both `esp32s3_n16r8_usb_test` and the normal `esp32s3_n16r8` target built
successfully. The running image is `1.2.47-usbtest6` in `app1`, confirmed valid
with rollback disarmed and boot failure count zero. OTA returned HTTP 200 and
`success:true`. Firmware size is 1,432,608 bytes; SHA-256 is
`8bb42a5d40aa184c2f6512871d049fd0e490ce331308e0b1712e93c151c25cf9`.
The independently reviewed DCD source hash is
`faa637ab12668b08c74f92cfe9556a9c4dd74c6e966bb84b01163eaf5b89a233`.
Images, ELF files, exact source snapshots and manifests are retained in
`.cache/firmware-1.2.46-usbtest6` and `.cache/firmware-1.2.47-usbtest6`.
The known TEST5 rollback image remains in
`.cache/firmware-1.2.45-usbtest5-ui/firmware.bin`.

The sampling-corrected combined run completed 21 full validation cycles and
captured cycle 22 before real remote input stopped the idle-only HID worker.
It completed 8,845 stress HID reports with zero failures, zero USB recoveries,
and continued HID callbacks. Audio measured approximately 31.7–31.9 KB/s and
500–547 USB packets/s. Its deliberate `user-input` stop is neither a device
stall nor a completed 100-cycle test. Full raw diagnostics and the explicit
interruption are in `.cache/firmware-1.2.47-usbtest6/stress-sampling-fixed`.
The stress worker is inactive after that run, and the microphone returned to
alternate setting 0. The five synchronization model tests pass.

The served HTML still matches the approved source exactly and configuration
matches the pre-upgrade export after excluding firmware metadata and
redacting secrets. No long-duration stability claim follows from these short
runs. ISO missed-frame retries/aborts still occur and are separately counted;
this change targets the demonstrated HID TXFE race.

Follow-up code risk: the Arduino synchronous HID sender can accept a stale
semaphore wakeup if an earlier timed-out report's completion arrives between
a new caller's readiness check and wait. This is a separate acknowledgment
generation issue, not the captured empty-FIFO stall. It has not been changed
in this DCD revision; completion-timeout diagnostics remain available to
identify whether it needs a dedicated sender redesign.
