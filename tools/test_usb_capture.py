"""Windows USB capture cycling test. Captures RemoteMapper only; discards audio.
Run: .venv/Scripts/python.exe -X utf8 tools/test_usb_capture.py
Requires the device's HTTP diagnostics, not third-party Python packages.
"""
import ctypes as c
from ctypes import wintypes as w
import argparse
import json
import time
import urllib.request


class Caps(c.Structure):
    _fields_ = [('mid', w.WORD), ('pid', w.WORD), ('version', w.DWORD),
                ('name', w.WCHAR * 32), ('formats', w.DWORD),
                ('channels', w.WORD), ('reserved', w.WORD)]


class Format(c.Structure):
    _pack_ = 1
    _fields_ = [('tag', w.WORD), ('channels', w.WORD), ('rate', w.DWORD),
                ('bytes_sec', w.DWORD), ('align', w.WORD), ('bits', w.WORD), ('extra', w.WORD)]


class Header(c.Structure):
    _fields_ = [('data', c.c_void_p), ('length', w.DWORD), ('recorded', w.DWORD),
                ('user', c.c_size_t), ('flags', w.DWORD), ('loops', w.DWORD),
                ('next', c.c_void_p), ('reserved', c.c_size_t)]


def audio_status():
    with urllib.request.urlopen('http://192.168.31.4/api/audio', timeout=5) as r:
        return json.load(r)


def check(code, operation):
    if code:
        raise RuntimeError(f'{operation}: WinMM error {code}')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cycles', type=int, default=10)
    args = parser.parse_args()
    mm = c.WinDLL('winmm')
    mm.waveInGetDevCapsW.argtypes = [c.c_size_t, c.POINTER(Caps), w.UINT]
    mm.waveInOpen.argtypes = [c.POINTER(c.c_void_p), w.UINT, c.POINTER(Format),
                            c.c_size_t, c.c_size_t, w.DWORD]
    for name in ('waveInPrepareHeader', 'waveInUnprepareHeader', 'waveInAddBuffer'):
        getattr(mm, name).argtypes = [c.c_void_p, c.POINTER(Header), w.UINT]
    for name in ('waveInStart', 'waveInStop', 'waveInReset', 'waveInClose'):
        getattr(mm, name).argtypes = [c.c_void_p]
    initial = audio_status()
    for cycle in range(1, args.cycles + 1):
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
        storage = c.create_string_buffer(64000)
        header = Header(c.addressof(storage), len(storage), 0, 0, 0, 0, None, 0)
        prepared = False
        before = audio_status()
        check(mm.waveInOpen(c.byref(handle), device, c.byref(fmt), 0, 0, 0), 'open')
        try:
            check(mm.waveInPrepareHeader(handle, c.byref(header), c.sizeof(header)), 'prepare')
            prepared = True
            check(mm.waveInAddBuffer(handle, c.byref(header), c.sizeof(header)), 'buffer')
            check(mm.waveInStart(handle), 'start')
            started = time.perf_counter()
            time.sleep(1.2)
            during = audio_status()
            elapsed = time.perf_counter() - started
            windows_bytes_before_stop = header.recorded
        finally:
            check(mm.waveInReset(handle), 'reset')
            if prepared:
                check(mm.waveInUnprepareHeader(handle, c.byref(header), c.sizeof(header)), 'unprepare')
            check(mm.waveInClose(handle), 'close')
        result = dict(cycle=cycle, windows_bytes=header.recorded,
                      windows_bytes_before_stop=windows_bytes_before_stop,
                      active_seconds=round(elapsed, 3),
                      windows_bytes_per_second=round(header.recorded / elapsed),
                      usb_packets=during['usb_completed']-before['usb_completed'],
                      usb_packets_per_second=round((during['usb_completed']-before['usb_completed']) / elapsed),
                      recoveries=during['usb_recoveries'], failed=during['usb_failed'])
        print(json.dumps(result), flush=True)
        assert result['windows_bytes'] >= 16000, 'Windows capture did not progress'
        assert result['usb_packets'] >= 200, 'USB audio completions stalled / too slow'
        assert 25000 <= result['windows_bytes_per_second'] <= 40000, 'Windows audio rate is wrong'
        assert 350 <= result['usb_packets_per_second'] <= 600, 'USB packet cadence is wrong'
        assert during['usb_recoveries'] == initial['usb_recoveries'], 'Recovery occurred; not a clean pass'
        time.sleep(0.4)
    print(f'PASS: {args.cycles} open/capture/close cycles; audio discarded.', flush=True)


if __name__ == '__main__':
    main()
