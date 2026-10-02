# Long voice holds — 1.2.24

- Explicitly updated this device's persisted `guard_conf.voice_ms` from 60000
  to 900000 using `/api/guard`, as requested. Other saved guard settings remain
  unchanged. The firmware default was already 15 minutes; changing defaults
  alone would not have changed the existing NVS value. No global migration
  overwrites other users' intentional settings.
- Voice holds still bypass ordinary modifier/key-idle limits. Physical release
  ends a session normally; the configured absolute ceiling remains a safety net.
- Add a five-second complete-ATVV-frame timeout with five seconds of startup
  grace. Silence samples count as live data, as do frames received when USB
  backpressure prevents enqueueing. Acoustic silence is not a timeout signal.
- Read the cross-core frame timestamp atomically before sampling the clock;
  this avoids false unsigned-age underflow when a frame arrives during a guard
  tick. Unsigned subtraction supports timer wraparound.
- A forced stop requests MIC_CLOSE from the BLE task. A newly active recording
  supersedes a pending stop request. USB key release remains independently
  retried by the HID transport.
- `/api/guard` exposes `voice_rx_gap_ms`; the settings page explains the two
  independent protections. Setting the absolute limit to zero does not disable
  the data-loss timeout.

Validation: N16R8 firmware build succeeded. On-device OTA/config readback is
checked after deployment. A physical hold over 60 seconds and an interrupted
audio-link scenario are separate hardware acceptance tests, not established by
compilation alone.
