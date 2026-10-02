"""Measure RemoteMapper PCM levels for a short live voice test; discard audio.

Run with: .venv/Scripts/python.exe -X utf8 tools/measure_usb_voice.py
The script prints only per-second RMS/peak and never writes speech to disk.
"""
import array
import ctypes as c
import math
import time
from ctypes import wintypes as w

from test_usb_capture import Caps, Format, Header, check


def main():
    mm = c.WinDLL('winmm')
    mm.waveInGetDevCapsW.argtypes = [c.c_size_t, c.POINTER(Caps), w.UINT]
    mm.waveInOpen.argtypes = [c.POINTER(c.c_void_p), w.UINT, c.POINTER(Format),
                              c.c_size_t, c.c_size_t, w.DWORD]
    for name in ('waveInPrepareHeader', 'waveInUnprepareHeader', 'waveInAddBuffer'):
        getattr(mm, name).argtypes = [c.c_void_p, c.POINTER(Header), w.UINT]
    for name in ('waveInStart', 'waveInStop', 'waveInReset', 'waveInClose'):
        getattr(mm, name).argtypes = [c.c_void_p]

    device = None
    for i in range(mm.waveInGetNumDevs()):
        caps = Caps()
        check(mm.waveInGetDevCapsW(i, c.byref(caps), c.sizeof(caps)), 'caps')
        if 'RemoteMapper' in caps.name:
            device = i
            break
    if device is None:
        raise RuntimeError('RemoteMapper capture device missing')

    handle = c.c_void_p()
    fmt = Format(1, 1, 16000, 32000, 2, 16, 0)
    storage = c.create_string_buffer(480000)
    header = Header(c.addressof(storage), len(storage), 0, 0, 0, 0, None, 0)
    check(mm.waveInOpen(c.byref(handle), device, c.byref(fmt), 0, 0, 0), 'open')
    try:
        check(mm.waveInPrepareHeader(handle, c.byref(header), c.sizeof(header)), 'prepare')
        check(mm.waveInAddBuffer(handle, c.byref(header), c.sizeof(header)), 'buffer')
        check(mm.waveInStart(handle), 'start')
        print('CAPTURING 12 seconds; press and hold voice key, speak, then release.', flush=True)
        time.sleep(12)
        check(mm.waveInReset(handle), 'reset')
        check(mm.waveInUnprepareHeader(handle, c.byref(header), c.sizeof(header)), 'unprepare')
    finally:
        check(mm.waveInClose(handle), 'close')

    pcm = array.array('h')
    pcm.frombytes(storage.raw[:header.recorded & ~1])
    print(f'captured_bytes={header.recorded}', flush=True)
    for second, start in enumerate(range(0, len(pcm), 16000)):
        block = pcm[start:start + 16000]
        if not block:
            break
        rms = math.sqrt(sum(v * v for v in block) / len(block))
        peak = max(abs(v) for v in block)
        nonzero = sum(v != 0 for v in block)
        print(f'second={second} samples={len(block)} rms={rms:.1f} '
              f'peak={peak} nonzero_pct={100 * nonzero / len(block):.1f}', flush=True)


if __name__ == '__main__':
    main()
