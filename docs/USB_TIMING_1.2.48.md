# USB timing hardening — 1.2.48 validation build

## Scope and release path

This revision prepares one N16R8 validation baseline for the remaining HID
completion timing risk and the new task-core diagnostics. It preserves the
approved web layout and the DWC2 cross-core controller fix documented in
[the 1.2.46 / 1.2.47 investigation](USB_HID_RACE_1.2.46.md).

`esp32s3_n16r8` is the default environment and defines
`REMOTEMAPPER_DWC2_DRIVER=1`. `esp32s3_n16r8_usb_test` inherits the same driver
and adds only `REMOTEMAPPER_USB_TEST_BUILD=1`; both identify as `1.2.48`.
There is one project DWC2 implementation, not two different release drivers.
The test alias remains available for existing stress commands and scripts.

N8R8, N8R2 and N4R2 have not been migrated to this driver in this revision.
Their images identify as `1.2.48-legacy-usb` so that a legacy USB image cannot
be confused with the N16R8 validation baseline. Those hardware profiles need
separate build and hardware validation.

`tools/ota.ps1` defaults to the N16R8 build, prints the resolved image path,
size and SHA-256 before uploading, and accepts an explicit archive path:

```powershell
.\tools\ota.ps1 -Bin .\.cache\firmware-1.2.48\firmware.bin
```

The script does not build or determine that an existing binary is current.
Use the reviewed image and recorded hash when deploying; the version string
alone does not uniquely identify a build.

## HID completion boundary

The earlier DWC2 fix protects task/ISR updates of controller registers and
transfer descriptors. It does not by itself make Arduino's synchronous HID
completion semaphore safe across a timed-out report and a later submission.
The timeout/late-callback acknowledgment risk is separate from the captured
empty-FIFO/TXFE-mask fault, and is not proof that every historical restart had
the same cause.

The boundary implemented in this revision is that a submitted report remains
the outstanding report after its caller times out. A late callback belongs to
that outstanding report and must not acknowledge a new caller. New submission
must remain gated until the outstanding report is retired, with submission
and completion generation checks used to validate the wait result rather than
treating an arbitrary semaphore wakeup as delivery. A semaphore is only a wakeup
mechanism; clearing it alone cannot establish report ownership.

`tools/patches/hid_tx_guard.h` implements the ownership state machine, embedded
by `hid_tx_patch.py` directly into the framework's USBHID source. The sender
retains the mutex and semaphore, checks the completion generation after every
wakeup, and leaves a timed-out slot pending. Completion validates the interface,
report ID, length and complete payload. Rejected native submission stays failed.
Immediate completion before submission returns cannot free the submitting slot.

Synchronous TinyUSB mount/umount hooks cancel the old generation before posting
Arduino's asynchronous event. A lifecycle change during native submission
quarantines an accepted slot until its matching callback or a later lifecycle
boundary, because the submission could still reach the new configuration.
Suspend/resume do not retire a submitted slot. No USB call, wait, logging or
allocation runs under the guard's short critical section.

The patch validates stock/V1 source, refreshes its controlled V2 block on each
build, and fails on unknown baselines. It embeds the exact tested state machine
and needs no project include path in the shared framework package. C++11 uses
ordinary inline methods; C++14 and later permit the compile-time scenario tests.

The USB completion callback does not carry a hardware generation token. This
boundary therefore depends on TinyUSB's one outstanding transfer/unique
completion contract and synchronous lifecycle event ordering. Full payload
comparison cannot distinguish an arbitrary duplicate old callback with exactly
the same payload as a new report. The fix is not a guarantee against any possible
duplicate controller event. An unresolved endpoint still requires coordinated
recovery; the timing change never manufactures a successful keyboard release.

## Recovery submission boundary

The loop closes the HID transport gate under the same mutex used by all senders
before force-release and physical detach. Keyboard/consumer release debt remains
pending; the idle-only stress sender also observes the gate. Rechecking HID debt
and closing the gate share one lock, so a just-completed retry cannot cause an
unnecessary detach through an intervening check/send race.

After the 350 ms detach interval, the loop snapshots a synchronous mount sequence
before `tud_connect()`. Only a new mount and `tud_mounted()` reopen HID submission.
A queued old Arduino Started event cannot reopen the gate or erase the reconnect
deadline. The TinyUSB hook updates an atomic sequence without taking the sender
mutex, avoiding a completion/sender deadlock.

PC wakeup now requests the same coordinator instead of delaying the loop and
overriding D+/D- with GPIO writes. The existing reconnect deadline remains.
`/api/guard` exposes `transport_recovering`; HID diagnostics schema 2 adds
`previous_pending`, `callback_rejected` and `lifecycle_cancelled`. Old persisted
binary fault records with a different size are ignored, rather than interpreted
using the new layout; previous incident captures remain in their archive.

## Task-core diagnostics

`core_diagnostics_json()` copies the fixed-size task registry under its
`portMUX` and releases the lock before calling FreeRTOS task-query functions,
scanning stack headroom, or constructing JSON. This prevents a diagnostics
request from extending the registry's interrupt-off window with stack scans
or dynamic allocation.

Registry entries are restricted to permanent tasks and names whose lifetime
is at least that of the registry. FreeRTOS task-query calls do not validate a
deleted task handle. Temporary tasks therefore require separate lifecycle
coordination before they may be registered; the registry lock alone is not
a task-lifetime guarantee.

`/api/cores` reports only explicitly registered application tasks. It does not
measure CPU utilization or enumerate every Arduino, NimBLE, TinyUSB or system
task. `cpu_load_available` remains false.

## Verification boundary and acceptance

Prior 1.2.47 results remain historical evidence: 21 complete HID/UAC concurrent
cycles and 8,845 successful zero-state HID stress reports, interrupted during
cycle 22 by real user input. They are neither a completed 100-cycle run nor
validation of this new framework timing patch.

The 1.2.48 validation should include:

- A successful build of default N16R8 and the USB-test compatibility alias,
  with exactly one DWC2 implementation in each linked ELF.
- Framework-patch verification and acknowledgment model cases covering late
  callbacks, timeout, immediate completion and recovery boundaries.
- Repeated Windows microphone open/capture/close while zero-state HID reports
  are submitted from the other core; aborted or measurement-invalid attempts
  must not count as successful cycles.
- Normal remote voice-key press, speech and release with the intended input
  software, including many consecutive sessions and the remote's 60-second
  stream limit. Confirm audio, window termination and modifier release together.
- Read `/api/cores` during active capture and inspect HID/audio/recovery counters;
  an API response alone is not a USB health check.
- Continued ordinary use after the repeated-session test. Retain first-fault
  diagnostics if audio stops, a release is missed, or the device restarts.

ISO missed-frame retries/aborts can still occur and must be assessed separately
from a persistent no-progress state. No complete stability claim follows from
model tests or short capture runs alone.

## Build and deployment record

Both N16R8 environments built successfully on 2026-10-04:

| Environment | Image bytes | SHA-256 |
| --- | ---: | --- |
| `esp32s3_n16r8` (validation image) | 1,435,600 | `32d3f0df6fa6e7147143b323339abdff795e15169d83edf9000cc8bb0f340336` |
| `esp32s3_n16r8_usb_test` (compatibility alias) | 1,435,728 | `13f80d0d37b34f95ee770dca828bf4a0b124efe9e3c9a8e144c843f38eeae939` |

The default image is the test baseline. The alias uses the same source/driver;
these are not byte-identical binaries. The linked ELF for each has one strong
definition of `dcd_init`, `dcd_edpt_xfer`, `dcd_int_handler` and the project DWC2
diagnostics, HID lifecycle and project lifecycle hooks. Both binaries contain
version `1.2.48` and `/api/cores`; neither contains the legacy version suffix.
Framework files match the current reproducible V2 transformation.

`python -m unittest discover -s tools/tests -v` passed 39 tests. These include
the actual C++ guard's ten compile-time state-transition assertions, generated
adapter compilation with C++17 and gnu++11, reproducible patch checks, recovery
source contracts, registry checks and the earlier DWC2 interleaving model.
They do not execute FreeRTOS scheduling or exercise a physical USB controller.

The image, ELF, segmented serial-flash files, source snapshots, build/test logs,
link checks and manifest are archived in `.cache/firmware-1.2.48`. The serial
package uses the project's dual-OTA bootloader, not the framework's single-app
bootloader. The initial image was deployed by OTA. Runtime acceptance found that the
new diagnostics handler passed a null JsonObject from a fresh JsonDocument.
The corrected image above uses `doc.to<JsonObject>()` and adds a source
regression check. Initial image and observations remain in
`.cache/firmware-1.2.48-initial`; the corrected image was deployed to app1 and verified after OTA.
No full hardware stress result is claimed for 1.2.48.

Configuration parsing confirms that the default N16R8 target enables the shared
DWC2 driver and the test alias inherits it. PowerShell AST parsing of the OTA
script succeeds. These checks do not compile the firmware or upload an image.

## OTA runtime acceptance

The corrected default image was uploaded with HTTP 200 and success=true, then
ran in app1 with image_state=valid. At 123 seconds uptime its safe-boot marker
was cleared, rollback_armed=false and boot_fail_count=0. BLE was connected.
The core endpoint returned all three registered tasks with matching affinity.
Windows reported both the RemoteMapper microphone endpoint and media device OK.

The cold-start HID trace contains one completion wait timeout and one not-ready
attempt (two failed attempts total), followed by completed release retries.
These counts stayed unchanged from the initial 50-second snapshot through
confirmation; no held keys, pending reports or USB recoveries remained. This
is retry behavior rather than a zero-failure claim. The microphone was idle
during this observation, so it does not replace repeated voice-input testing.
