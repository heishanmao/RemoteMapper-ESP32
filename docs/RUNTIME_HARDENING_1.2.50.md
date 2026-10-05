# Runtime hardening in 1.2.50

This N16R8 release continues the USB controller and completion fixes in
[1.2.48](USB_TIMING_1.2.48.md) and the checked startup/logging changes in
1.2.49. The approved web layout is retained.

## Owners and timing boundaries

- BLE key callbacks enqueue HID commands under a short critical section. The
  permanent `hid_sender` task owns normal keyboard/consumer report sends,
  completion waits, tap dwell and retries. Ordinary press/release commands
  remain FIFO. Queue overflow, BLE disconnect and USB recovery cancel the old
  command epoch and retain emergency zero reports until successful delivery.
- USB recovery closes the command gate before taking the sender mutex and
  detaching. A new synchronous TinyUSB mount reopens submission. The existing
  framework completion guard and shared DWC2 controller lock remain in place.
- Voice start, release and deferred drain completion use a separate short
  state lock. The loop rechecks the voice sequence and drain condition while
  holding it, so an old drain decision cannot stop a newly started session.
- BLE owns decoder/filter/AGC/resampler state. Other tasks publish session reset
  requests; old decoded frames cannot commit after the session epoch changes.
  Resample settings expose requested/applied values and take effect at the next
  producer session boundary. Ring clearing uses a fixed head watermark so its
  deferred acknowledgement preserves samples published after the request.
- The LED task alone writes the LED peripheral. BLE and other callers update a
  bounded state snapshot and notify that task without waiting for LED output.
- The main loop starts and polls asynchronous Wi-Fi scans, caches at most 32
  results, and enforces a deadline. HTTP returns scan state immediately; the UI
  polls and inserts SSIDs as text.
- CLI responses go to their input route (UART or native CDC), with separate
  input buffers. The common output worker sends bounded chunks. Oversize/full
  queue failures reject whole responses, increment route counters, and emit a
  reserved error notice after earlier output on that route drains.

The remote's microphone limit remains 60 seconds. The voice-held upper bound is
62 seconds, including a two-second release tolerance; disabled or excessive
legacy settings migrate to this bound. The immediate long-press behavior is
retained, including manual Esc cancellation of an unintended short press.

## Diagnostics and verification limits

The task registry includes permanent application tasks and the known permanent
`usbd`/`nimble_host` tasks. Framework USB has no required core affinity.
Stack and affinity queries occur outside the short registry lock and are live,
non-atomic observations. The prebuilt framework has runtime statistics disabled:
`cpu_load_available=false` and a reason are reported; loop timing is not a CPU
utilization percentage.

Host tests execute the production logger, audio pipeline/ring and HID queue
helpers. Audio interleavings are deterministic tests with RTOS lock stubs, not
a simulation of target cross-core execution. The suite also checks framework
completion/lifecycle patch behavior, recovery ordering and startup contracts.
The N16R8 default and compatibility test alias must both build, with one strong
definition of each project controller/lifecycle hook in the ELF.

Hardware validation opens/closes Windows capture repeatedly while idle zero
HID reports run, checks transfer progress and restart/recovery counters, and
discards captured audio. This exercises USB transport; actual remote speech and
input-method recognition still require ordinary use or a manual voice test.
The release archive records the exact image hash, build/test results and OTA
status. Short tests cannot establish indefinite fault-free operation.

## Deployment

Deploy the reviewed default N16R8 image explicitly:

```powershell
.\tools\ota.ps1 -Bin .\.cache\firmware-1.2.50\firmware.bin
```

The archive preserves the existing dual-OTA serial bootloader and checks that
the partition hash is unchanged. Serial offsets are `0x0` (bootloader),
`0x8000` (partitions), `0xe000` (boot_app0), and `0x10000` (firmware); NVS is
preserved. Other hardware profiles retain the `-legacy-usb` version suffix and
require their own build and hardware validation.
