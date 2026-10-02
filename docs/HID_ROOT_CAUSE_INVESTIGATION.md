# HID release stall: evidence and remaining verification

Date: 2026-09-30. Running firmware: 1.2.25. This investigation did not flash another firmware.

## Confirmed failure boundary

The saved 1.2.24 incident in `.cache/stuck-1.2.24` shows:

- Voice release, ATVV release and HOGP UP reached the firmware; MIC_CLOSE followed.
- `held_count=0`, `desired_modifier=0`, `keyboard_pending=true`.
- HID `tx_complete=23`, `tx_failed=1574`; no corresponding successful release completion.
- UAC `usb_completed=51747`, `usb_failed=0`, `session_active=false`, still streaming silence to the open host capture endpoint.

This places the observed failure after release handling, in HID delivery/completion. It does not prove Windows received a release, nor prove which layer lost progress. The remote's 60-second limit and acoustic silence do not explain this captured incident. The SendReport boolean alone cannot distinguish submission rejection, endpoint busy, mutex timeout, or completion timeout.

## Concrete driver concurrency candidate

Inspected the actual linked `.pio/build/esp32s3_n16r8/firmware.elf`, not a different upstream DWC2 implementation. The map identifies `dcd_esp32sx.c.obj` from the packaged `libarduino_tinyusb.a`.

In this ELF:

- `usbd_edpt_xfer` sets software busy and invokes `dcd_edpt_xfer` without a surrounding critical section in that function.
- `dcd_edpt_xfer`, addresses 0x4207e990–0x4207e99e, reads, ORs and writes the shared register at USB base + 0x834 (IN FIFO-empty interrupt mask). No critical section is present in this function.
- `_dcd_int_handler`, addresses 0x4207e2b6–0x4207e2c3, reads, ANDs and writes the same register to clear an endpoint bit.
- HID submission executes in application contexts; UAC submission is deferred to the USB task. The project HID mutex does not serialize UAC or the USB ISR. The framework creates an unpinned USB task.

A possible interleaving is: the ISR reads the old mask to clear the audio bit; HID enables its bit; the ISR writes its old value with only the audio bit cleared, losing the new HID bit. A report may then remain software-busy without being loaded into its FIFO. Concurrent submissions can also overwrite another endpoint's mask update. This is a concrete unprotected read/modify/write pattern and a strong candidate, **not yet proof that it caused the captured incident**. Memory barriers do not make these multi-instruction updates atomic.

Disassembly outputs are retained in `.cache/dcd-xfer.txt` and `.cache/dcd-isr-range.txt`. Addresses apply only to this ELF.

## Evidence needed before declaring the root cause fixed

1. Capture a bounded diagnostic snapshot before the recovery disconnect, with the actual descriptor-derived HID endpoint, HID ready/busy state, transfer submission and completion sequence numbers, core/task identity, and endpoint control, transfer-size, FIFO-space and interrupt-mask/status registers. Only read non-destructive registers; do not pop FIFO/status registers.
2. Distinguish a report that never submitted, a queued report never loaded into FIFO, a transmitted report whose completion event was lost, and a completed report whose waiting semaphore was mishandled. A cleared FIFO-empty bit alone is normal after FIFO loading and cannot prove the candidate.
3. If the mask race is confirmed, protect all relevant task/ISR register updates or use a verified driver implementation that does so. Serializing only HID and UAC task calls does not by itself protect against the ISR. Do not block inside the USB task waiting for a completion callback that requires that same task.
4. Validate with simultaneous audio capture and repeated HID press/release, including short presses, near-60-second holds and automatic remote cutoff. Audio capture-only tests are insufficient. Require matched submission/completion sequences and no recovery-count increase; automatic reconnect must not hide failures in the test result.

The Espressif issue https://github.com/espressif/esp-idf/issues/9691 documents a different USB reset/submission race. It is relevant background, not evidence that our failure is the same bug.

## Current status

1.2.25 provides recovery from an observed stalled HID path. User-visible recovery is confirmed by the user; root-cause elimination is not confirmed. No driver synchronization change was made on the basis of this hypothesis alone.
