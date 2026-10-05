# HID command sender and voice safety ceiling

HID actions from BLE callbacks now enter a bounded FIFO and return after a
short queue operation. A dedicated sender task owns TinyUSB `SendReport` calls
and tap timing. A normal press and its release remain FIFO ordered, and a tap
is one command containing both reports. Two queue entries are reserved for
ordinary keyboard and consumer releases. If the queue still overflows, the
current epoch is discarded and durable zero reports are retained in separate
mailboxes until the sender confirms them.

Disconnect, force-release, and USB recovery invalidate queued commands by
epoch. Recovery rejects new presses until a fresh synchronous host mount. The
sender and recovery path share a short quiescence barrier before USB detach;
the report completion timeout remains 20 ms, matching the HID lifecycle
guard. The worker, rather than NimBLE, performs remote wake waits and the
15 ms tap dwell.

The voice hold ceiling is 62 seconds: the remote's 60-second capture limit
plus two seconds for the final transport/drain. User-selected values from 1 to
62 seconds remain available. A zero value or a persisted value above 62 seconds,
including the former 15-minute setting, migrates to 62 seconds. Nonzero values
below one second are clamped to one second. Sanitized values are written back.
If that write fails, the current boot still enforces the sanitized limit and
logs that persistence failed.
