# Voice-key tap and post-release BLE microphone probe

## Observation

On 1.2.38, a 136 ms physical voice-key tap delivered zero ATVV audio frames. The remote reported AUDIO_STOP on release, and the bridge sent MIC_CLOSE. Sending Right Alt for this tap could put an input method into a recording state without remote audio.

## One-shot probe (1.2.40)

The temporary probe suppressed HID output, sent ATVV MIC_OPEN (`0x0C 0x00`) while the key was released, counted complete ATVV audio frames for two seconds, and sent MIC_CLOSE (`0x0D`). It stored no audio content.

- Idle MIC_OPEN: zero frames over 2000 ms.
- After a single 142 ms tap, MIC_OPEN was sent 204 ms after release: zero frames over 2000 ms. No other physical key press occurred in that measurement window.

This shows that the current remote does not continue streaming in response to MIC_OPEN while the physical voice key is released. It does not prove that every possible vendor command or alternate remote firmware behaves the same way.

## 1.2.41 behavior

The temporary probe API was removed. Audio capture still begins at physical key-down and uses the existing pre-roll buffer. The Right Alt HID report is sent only after at least 300 ms of hold and three decoded audio frames. A shorter or silent press produces no Alt report; a triggered hold releases Alt immediately on physical release. This prevents a tap with no remote audio from toggling a speech application into a silent recording state.

## Known boundary limitation

A 323 ms physical press crossed the 300 ms gate and produced only a 21 ms Right Alt pulse, with no completed USB audio packets. The input method displayed its Alt-tap UI. Raising the gate to 600 ms would suppress this case but would also delay the visible recording UI, which the user does not want. No threshold can perfectly classify a press as short before its release. Sending Alt on initial key-down and then releasing it on a short press still delivers a valid Alt tap to an application that assigns an action to taps. The 600 ms change was not deployed.

## 1.2.42 user-selected behavior

The user chose immediate response and accepts manually pressing Esc if a short tap opens the input method. The bridge now starts audio capture and sends Right Alt on the physical voice-key down, then releases Right Alt immediately on physical release. Audio drain remains independent of HID release, and the existing guard/retry path still handles a failed USB release. A short tap may therefore open the input method without usable remote audio; this is expected behavior, not a firmware attempt to keep the remote microphone open after release.
