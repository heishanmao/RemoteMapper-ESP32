# Independent HID delivery watchdog — 1.2.25

## Captured failure

1.2.24 received the voice release at uptime 155.903 s and stopped the audio
session. The keyboard clear report did not complete. At uptime 238 s,
`keyboard_pending=true`, `desired_modifier=0`, `held_count=0`, and HID had
1,574 failed attempts. UAC had 51,747 successful completions and zero failures.
This is an independent HID transport blockage: live audio cannot be used as
evidence that keyboard releases are healthy. Raw evidence is saved locally in
`.cache/stuck-1.2.24/`.

## Recovery change

- Track continuous pending HID time independently of the local held registry
  and microphone activity. New key actions do not restart this deadline.
- After one second pending while USB is ready, request composite USB recovery.
- Audio and HID share one recovery coordinator: force-release local state,
  close a running mic session, detach for 350 ms, and reconnect via TinyUSB.
- Do not initiate HID recovery while suspended/unmounted. Rate-limit repeated
  recovery to once per ten seconds; cancel a delayed HID request if its reports
  have since completed. Reuse the four-second mount/restart fallback.
- Expose `pending_ms` and `usb_recoveries` in `/api/guard`. `/api/audio`'s recovery
  count now includes composite recovery triggered by either class.

This is bounded recovery from a confirmed fault, not a demonstrated elimination
of its underlying USB driver/host trigger. A recovery interrupts the recording;
the host speech application may require closing its dialog and starting again.
The 60-second remote recording limit is a separate issue.
