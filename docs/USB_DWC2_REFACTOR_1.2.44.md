# USB controller hardening after the 1.2.44 trial

The device is still running `1.2.44-usbtest3`. This source change builds as
`1.2.44-usbtest4`; it has **not** been flashed. The default N16R8 environment
still builds the legacy `1.2.43` controller, so a normal build is not a
replacement for the trial image.

The UAC alternate-setting handler now delegates DWC2 endpoint activation and
deactivation to small functions. The 2 ms transmit cadence and 500 ms missing
completion threshold are named constants, with their values unchanged.

The experimental DWC2 driver now uses one bounded register-wait helper for
endpoint disable. The two OUT endpoint waits, which previously had unbounded
loops, also time out after 2 ms. A timeout clears global OUT NAK if needed and
sets a sticky controller-fault flag. The UAC service task notices that flag and
requests the existing stack-managed USB recovery even when the microphone is
not streaming. The fault flag clears on USB bus reset. FIFO reclamation checks
that the closing IN endpoint owns the last allocated FIFO; an unexpected order
requests recovery instead of changing the allocator count.

Checks: `pio run -e esp32s3_n16r8_usb_test` and `pio run -e esp32s3_n16r8`
both passed. The link maps show the local DWC2 driver in the trial build and
the bundled `dcd_esp32sx` driver in the default build. `git diff --check`
found no whitespace errors. `pio test -e native_test` cannot run because this
repository has no `test/test_custom_runner.py`; its current Python test suite
is not a direct test of the USB hardware path.

Before deploying this revision, compare it against the running test3 image on
the same Windows open/close workload and repeated Chatterfly voice-key cycles.
Watch `usb_failed`, `usb_recoveries`, `any_held`, and `keyboard_pending`. The
hardware timeout and FIFO-order failure branches have not been exercised on a
real fault, so this is source-level hardening rather than a demonstrated fix
for a new incident.
