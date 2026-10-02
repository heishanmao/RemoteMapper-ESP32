# HID release recovery — 1.2.22

## Observations and corrected diagnosis

The previous firmware discarded the boolean result of keyboard report sends,
cleared the local held registry before delivery was known, and stopped release
retries after one second. A local held-key timeout cannot repair that state once
the registry is empty. Voice holds are also exempt from the ordinary modifier
timeout; their separate voice timeout remains in effect.

The first 1.2.22 boot log reports **UAC EP3 IN**, not EP1. Arduino reserves CDC
OUT3/IN4/IN5 during interface registration; the allocator prefers an available
IN endpoint paired with an existing OUT endpoint. UAC therefore receives IN3,
and HID subsequently receives IN1 in this configuration. The earlier compact's
UAC=EP1/HID=EP2 explanation was incorrect. The old diagnostic cannot establish
whether a particular release reached Windows because it discarded send results.

`TX starved` also fired with `session_active=false`: this is normal silent audio
while a host capture stream stays open, not evidence of a dead USB endpoint.

## Changes

- Use the framework's shared `USBHID::SendReport` transport directly for keyboard
  and consumer reports. Its success result follows the TinyUSB transfer-complete
  callback, rather than a guessed endpoint's busy flag. This confirms USB transfer
  completion, not speech application's handling of the key.
- Store the latest desired report and retain failed sends indefinitely. Retry
  every 50 ms with bounded 20 ms send waits; pause actual sends while USB is not
  ready. New input replaces stale pending state under the same mutex.
- Release recovery no longer depends on local held-key guard entries. Mount and
  resume send explicit clear reports; remove blanket clear-stall operations on
  unrelated endpoints.
- Expose pending keyboard/consumer state, desired modifier, completed transfers
  and failed attempts through `/api/guard`.
- Remove periodic AUDIO_EXTEND and timer-driven microphone reopen for this HTT
  remote. Clear microphone-open state on stop notifications as well as HOGP UP.
- Restrict UAC empty-ring warnings to an active, non-buffering recording session.
- Preserve saved mappings and guard settings, including the current 60-second
  voice ceiling. A continuous hold longer than that can still trigger the guard.

## Validation

- N16R8 firmware builds successfully; application flash size 1,411,753 bytes.
- Existing Python algorithm checks: 4/4 pass (these do not test the USB transport).
- OTA returns HTTP 200; device boots 1.2.22 from app0 and reconnects to Wi-Fi/BLE.
- Boot keyboard release completes after transient initial send failures;
  keyboard/consumer pending flags are false and desired modifier is zero.
- Device keymap compares equal before and after OTA.

Host USB failure injection, sleep/resume and repeated 10–20-second physical
voice holds still require hardware interaction. Do not treat successful boot
and compilation as proof that every intermittent Alt-stuck trigger is eliminated.

Local pre-update configuration is saved under `.cache/hid-fix-1.2.22/` (ignored
by Git). Do not publish that backup: it can contain device configuration secrets.
