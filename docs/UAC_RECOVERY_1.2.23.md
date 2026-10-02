# UAC delivery regression and recovery — 1.2.23

## Captured fault

On 1.2.22, repeated physical voice holds produced complete HID DOWN/UP reports
(170 completed HID reports, only 4 initial failed attempts, no pending release).
ATVV was still receiving approximately 63 frames/s, but the audio ring was full
(8192 samples) and the latest session decoded 198 frames while enqueueing zero
samples. Thus this reproduction is an audio-consumer blockage, not evidence of
another stuck Alt. Windows repeatedly toggled the audio streaming alternate
setting. The saved evidence is in `.cache/uac-fix/*-before.json`.

## Driver constraints

The linked library uses `dcd_esp32sx.c.obj`. Inspection of the built ELF confirms
that it implements `dcd_edpt_close_all` but not individual `dcd_edpt_close` or
ISO activate helpers. Disassembly of `usbd_edpt_clear_stall` confirms it only
clears busy when the endpoint is marked stalled. Previous postmortem claims
about unconditional busy clearing were incorrect for this installed binary.

The upstream [TinyUSB device implementation](https://github.com/hathach/tinyusb/blob/0.16.0/src/device/usbd.c)
also documents deferred task calls and conditional stall clearing. The linked
Espressif driver, rather than generic DWC2 assumptions, determines available APIs.

## Changes

- Pace transfers at 2 ms, but defer each submission to the TinyUSB task. At most
  one service call is queued. Submission is now serialized with alternate-setting
  changes, bus reset, and completion handling.
- Apply pending ring resets from that same consumer even when capture is closed
  or an endpoint cannot be claimed. A full previous session cannot indefinitely
  prevent the next session from enqueueing audio.
- Count only successful 64-byte audio completions as progress. Export completion,
  failure, claim-skip, last-completion and recovery counters through `/api/audio`.
- If an active stream has no completion for 500 ms, end the current hold and
  request a stack-managed USB disconnect/reconnect with a 350 ms detach. Limit
  recovery to once per 30 seconds. No direct GPIO/FIFO/busy-flag manipulation.
- AudioControl accepts only alternate setting zero; its GET_INTERFACE response
  no longer incorrectly returns the streaming interface's active alternate value.

Recovery deliberately interrupts the current recording and may require reopening
capture or pressing voice again. It is a fallback, not proof of an eliminated
driver defect. Repeated recovery events should be investigated, not called success.

## Validation

- N16R8 build passed, application size 1,413,017 bytes; OTA HTTP 200, boot 1.2.23
  from app1 and BLE reconnected.
- `tools/test_usb_capture.py`: 10 Windows WinMM open/capture/close cycles passed.
  Each cycle delivered at least 41,208 bytes to Windows and at least 641 successful
  USB packets. Zero USB audio failures and zero automatic recoveries.
- The test selected only the RemoteMapper capture device and discarded audio.
  It validates USB transport and alternate-setting cycling, not speech content.
- Repeated physical voice holds, sound recognition and fault-triggered recovery
  remain separate acceptance tests.
