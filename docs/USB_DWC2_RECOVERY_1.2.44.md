# Investigation and unflashed candidate: 1.2.44-usbtest5

These are the original candidate notes. TEST 5 was subsequently flashed with
user authorization; see `USB_INCIDENT_1.2.44_2026-10-02.md`. The 2026-10-02 UI
release `1.2.45-usbtest5` retains the same USB implementation; deployment and
remaining runtime observations are recorded in `WEB_LAYOUT_RELEASE_1.2.45.md`.

The 2026-10-02 incident on TEST 3 received BLE audio but stopped USB audio
completions. A HID release stall preceded USB reconnect, after which two
microphone opens stalled. The third reconnect plus reopening Windows capture
restored operation. The initial trigger of the HID stall remains unproven.

## Source defects and relevance

1. TinyUSB 0.16's DWC2 driver leaves incomplete ISO IN processing commented out
   and does not enable that interrupt. The saved TEST 3 fault has GINTSTS bit
   20 set. A missed frame can consequently leave TinyUSB's transfer busy state
   waiting indefinitely for completion. The register was captured during later
   recovery, so it does not establish the origin of the first HID failure.
2. Bus reset clears software descriptors but does not disable old IN transfers,
   reset TX-empty interrupt masks, or flush RX/TX FIFOs. That is a recovery-path
   gap: resetting software state alone cannot guarantee removal of stale
   hardware transfers. TinyUSB 0.20 performs those cleanup steps.
3. The 0.16 allocator gives each TX FIFO one maximum packet, while initialization
   does not explicitly select a fully-empty TX interrupt threshold. Upstream
   issue #2049 reports interrupt starvation when a one-packet FIFO is paired
   with a half-empty threshold. Our incident snapshots lack GAHBCFG, so the
   actual threshold during the initial stall is not established.
4. Two old ISR register references use GOTGINT instead of GINTSTS for SOF
   clearing and the RX FIFO drain condition. The newer driver uses GINTSTS.

Primary references:

- [TinyUSB 0.20 DWC2 source](https://github.com/hathach/tinyusb/blob/0.20.0/src/portable/synopsys/dwc2/dcd_dwc2.c):
  `handle_incomplete_iso_in`, `handle_bus_reset`, interrupt handler.
- [FIFO interrupt issue #2049](https://github.com/hathach/tinyusb/issues/2049)
  and [upstream correction #2050](https://github.com/hathach/tinyusb/pull/2050).

## Candidate changes

TEST 5 keeps the Arduino/TinyUSB 0.16 stack and backports the targeted DWC2
handling rather than replacing the entire USB stack:

- Enable/handle incomplete ISO IN interrupts. Retry a transfer up to the
  endpoint interval, then disable it and emit a failed completion. The class
  can clear busy state and schedule another packet; the retry does not refill
  the FIFO or rewind its PCM buffer.
- Disable old IN transfers on bus reset, clear TX-empty mask and EP0 pending
  counters, flush all FIFOs with bounded waits, and clear stale incomplete-ISO
  status before enabling its interrupt.
- Explicitly set TXFELVL to fully empty, matching the one-packet FIFO allocation.
- Correct the two GINTSTS references and reject transfers on closed endpoints.
- Add `/api/audio`'s experimental `dwc2` counters for resets, reset-flush
  failures, incomplete ISO events, retries and aborts. Retain the first affected
  ISO endpoint's control/size/frame registers before any recovery, rather than
  overwriting that snapshot on later incidents. Counters are cumulative per
  boot. `first_iso_ms` is a publication marker based on uptime milliseconds + 1.

The source also contains the unflashed TEST 4 refactoring. Its binary/ELF were
preserved in `.cache/firmware-1.2.44-usbtest4/` before building TEST 5.
The user's device has not been upgraded from TEST 3.

## Validation limits

TEST 5 and the default N16R8 build compile successfully. The default build
retains the bundled controller and 1.2.43 version; the experimental environment
builds TEST 5. Whitespace checks pass. No host C compiler is available for a
register replay harness, and no firmware was flashed, so the new ISR, retry and
reset paths remain unvalidated on hardware.

Before deployment, retain rollback images, then verify enumeration, correct
32 KB/s microphone rate, repeated capture open/close, HID press/release during
voice use, and recovery after a controlled interruption. Longer normal use is
still required: the previous 100-cycle check did not cover this multi-hour
incident. This candidate addresses demonstrated source gaps; it does not prove
that the earliest HID stall or every application's capture-session recovery is
resolved.
