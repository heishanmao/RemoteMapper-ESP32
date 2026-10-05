"""Cycle RemoteMapper capture while its experimental CLI sends idle HID reports.

Audio is discarded. The firmware stops HID stress when real remote input appears.
Run with Windows device access, e.g.:
  .venv/Scripts/python.exe tools/test_usb_hid_audio.py --cycles 100
The native CDC port is selected by exact RemoteMapper VID/PID, never by UART name.
"""
import argparse
import ctypes as c
from ctypes import wintypes as w
import json
import pathlib
import sys
import time
import urllib.request

import serial
from serial.tools import list_ports
from test_usb_capture import Caps, Format, Header, check


def get_json(host, route):
    with urllib.request.urlopen(f"http://{host}/api/{route}", timeout=5) as response:
        result = json.loads(response.read().decode("utf-8", errors="replace"))
        if route == "status":
            result.pop("ap_pass", None)
        return result


def remote_cdc_port(requested=None):
    ports = list(list_ports.comports())
    if requested:
        matches = [port for port in ports if port.device.casefold() == requested.casefold()]
        if len(matches) != 1 or matches[0].vid != 0x303A or matches[0].pid != 0x8089:
            raise RuntimeError(f"{requested} is not RemoteMapper native CDC "
                               "(required VID 303A / PID 8089); UART ports are not supported")
        return matches[0].device
    matches = [port for port in ports if port.vid == 0x303A and port.pid == 0x8089]
    if len(matches) != 1:
        raise RuntimeError("Expected one RemoteMapper native CDC port "
                           f"(VID 303A / PID 8089), found {len(matches)}: "
                           + ", ".join(port.device for port in matches))
    return matches[0].device


def setup_winmm():
    mm = c.WinDLL("winmm")
    mm.waveInGetDevCapsW.argtypes = [c.c_size_t, c.POINTER(Caps), w.UINT]
    mm.waveInOpen.argtypes = [c.POINTER(c.c_void_p), w.UINT, c.POINTER(Format),
                            c.c_size_t, c.c_size_t, w.DWORD]
    for name in ("waveInPrepareHeader", "waveInUnprepareHeader", "waveInAddBuffer"):
        getattr(mm, name).argtypes = [c.c_void_p, c.POINTER(Header), w.UINT]
    for name in ("waveInStart", "waveInReset", "waveInClose"):
        getattr(mm, name).argtypes = [c.c_void_p]
    return mm


def capture_once(mm, host):
    device = None
    for index in range(mm.waveInGetNumDevs()):
        caps = Caps()
        check(mm.waveInGetDevCapsW(index, c.byref(caps), c.sizeof(caps)), "caps")
        if "RemoteMapper" in caps.name:
            device = index
            break
    if device is None:
        raise RuntimeError("RemoteMapper capture device missing")
    handle = c.c_void_p()
    fmt = Format(1, 1, 16000, 32000, 2, 16, 0)
    storage = c.create_string_buffer(320000)
    header = Header(c.addressof(storage), len(storage), 0, 0, 0, 0, None, 0)
    before = get_json(host, "audio")
    check(mm.waveInOpen(c.byref(handle), device, c.byref(fmt), 0, 0, 0), "open")
    prepared = False
    try:
        check(mm.waveInPrepareHeader(handle, c.byref(header), c.sizeof(header)), "prepare")
        prepared = True
        check(mm.waveInAddBuffer(handle, c.byref(header), c.sizeof(header)), "buffer")
        check(mm.waveInStart(handle), "start")
        started = time.perf_counter()
        time.sleep(1.2)
        elapsed = time.perf_counter() - started
    finally:
        # Always attempt close, including failures in reset/unprepare. Preserve
        # the original exception if capture already failed.
        primary_error = sys.exc_info()[0] is not None
        cleanup_errors = []
        for operation, code in (
                ("reset", mm.waveInReset(handle)),
                ("unprepare", mm.waveInUnprepareHeader(handle, c.byref(header), c.sizeof(header))
                 if prepared else 0),
                ("close", mm.waveInClose(handle))):
            if code:
                cleanup_errors.append(f"{operation}: WinMM error {code}")
        if cleanup_errors:
            if primary_error:
                print("Cleanup: " + "; ".join(cleanup_errors), file=sys.stderr, flush=True)
            else:
                raise RuntimeError("; ".join(cleanup_errors))
    # Stop capture before querying HTTP. Network/HTTP latency is independent
    # of capture cadence and must not extend the sampling denominator or
    # leave a finite capture buffer full while the microphone stays open.
    during = get_json(host, "audio")
    packets = during["usb_completed"] - before["usb_completed"]
    return dict(windows_bytes=header.recorded, active_seconds=round(elapsed, 3),
                windows_bytes_per_second=round(header.recorded / elapsed),
                usb_packets=packets, usb_packets_per_second=round(packets / elapsed),
                audio=during)


def trace(guard):
    return guard.get("hid_diagnostics", {}).get("live", {})


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="192.168.31.4")
    parser.add_argument("--port", help="RemoteMapper native CDC port; default auto-selects the unique "
                        "VID 303A / PID 8089 port (UART ports are rejected)")
    parser.add_argument("--cycles", type=int, default=30)
    parser.add_argument("--seconds", type=int, default=300)
    parser.add_argument("--minimum-seconds", type=float, default=0,
                        help="Continue capture cycles until this total duration is reached")
    parser.add_argument("--observe-existing-hid", action="store_true",
                        help="Observe a separately started HID worker; its owner must stop it")
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    if args.cycles < 1 or not 1 <= args.seconds <= 300 or args.minimum_seconds < 0:
        parser.error("cycles must be positive; seconds must be 1..300; minimum-seconds cannot be negative")
    if max(args.cycles * 1.6, args.minimum_seconds) + 15 > args.seconds:
        parser.error("HID duration is too short for the requested capture cycles and cleanup margin")
    port = remote_cdc_port(args.port) if not args.observe_existing_hid else None
    mm = setup_winmm()
    initial_guard = get_json(args.host, "guard")
    initial_audio = get_json(args.host, "audio")
    initial_status = get_json(args.host, "status")
    require("usb_stress" in initial_guard, "Firmware has no experimental HID stress channel")
    require(initial_guard["usb_stress"]["active"] == args.observe_existing_hid,
            "Expected an active HID worker" if args.observe_existing_hid else "Another HID test is running")
    require("callback" in trace(initial_guard) and "completion_timeout" in trace(initial_guard),
            "Firmware lacks HID callback diagnostics")
    require(not initial_guard["any_held"] and not initial_audio["session_active"],
            "Remote must be idle before test")
    previous_id = initial_guard["usb_stress"]["test_id"]
    output = args.output or pathlib.Path(".cache/usb-hid-audio-stress") / time.strftime("%Y%m%d-%H%M%S")
    output.mkdir(parents=True, exist_ok=True)
    (output / "before.json").write_text(json.dumps(dict(guard=initial_guard, audio=initial_audio,
            status=initial_status), ensure_ascii=False, indent=2), encoding="utf-8")

    # The fixed CLI drains received bytes without requiring modem control.
    # Leave DTR/RTS deasserted so synchronous CDC replies cannot delay the
    # HTTP diagnostics while this test only consumes their structured data.
    uart = serial.Serial(port=None, baudrate=115200, timeout=0.1, write_timeout=2,
                         rtscts=False, dsrdtr=False) if port else None
    if uart:
        uart.dtr = False
        uart.rts = False
        uart.port = port
        uart.open()
        print(f"RemoteMapper native CDC {port}: receive-only CLI control; no UART fallback.", flush=True)
    test_id = initial_guard["usb_stress"]["test_id"] if args.observe_existing_hid else None
    test_started_ms = initial_guard["usb_stress"]["started_ms"] if args.observe_existing_hid else None
    start_sent = False
    failure = None
    final_guard = None
    wall_started = time.monotonic()
    completed_cycles = 0
    try:
        if uart:
            uart.write(f"usb stress {args.seconds}\n".encode("ascii"))
            uart.flush()
            start_sent = True
        deadline = time.monotonic() + 5
        while test_id is None and time.monotonic() < deadline:
            # Drain CLI replies so its synchronous CDC writes cannot block
            # the main loop that serves the HTTP diagnostics.
            if uart: uart.read(max(1, uart.in_waiting))
            guard = get_json(args.host, "guard")
            stress = guard["usb_stress"]
            if stress["test_id"] != previous_id:
                test_id = stress["test_id"]
                test_started_ms = stress["started_ms"]
                require(stress["active"], f"HID test stopped immediately: {stress}")
                break
            time.sleep(0.1)
        require(test_id is not None, "CLI did not start HID stress; verify native CDC handshake")
        print(f"HID test {test_id} active; {args.cycles} capture cycles -> {output}", flush=True)
        last_completed = 0
        last_callback = trace(initial_guard)["callback"]
        with (output / "cycles.jsonl").open("w", encoding="utf-8") as stream:
            while completed_cycles < args.cycles or time.monotonic() - wall_started < args.minimum_seconds:
                cycle = completed_cycles + 1
                result = capture_once(mm, args.host)
                if uart: uart.read(max(1, uart.in_waiting))
                guard = get_json(args.host, "guard")
                stress = guard["usb_stress"]
                result.update(cycle=cycle, guard=guard)
                stream.write(json.dumps(result, ensure_ascii=False) + "\n")
                stream.flush()
                print(json.dumps(dict(cycle=cycle,
                      windows_bytes_per_second=result["windows_bytes_per_second"],
                      usb_packets_per_second=result["usb_packets_per_second"],
                      hid_completed=stress["completed"], hid_failed=stress["failed"],
                      recoveries=guard["usb_recoveries"])), flush=True)
                require(result["windows_bytes"] >= 16000, "Windows capture did not progress")
                require(25000 <= result["windows_bytes_per_second"] <= 40000, "Wrong audio rate")
                require(350 <= result["usb_packets_per_second"] <= 600, "Wrong USB packet cadence")
                require(stress["test_id"] == test_id and stress["active"],
                        f"HID test stopped before all cycles: {stress}")
                require(stress["failed"] == 0 and stress["completed"] > last_completed,
                        "HID reports failed or stopped progressing")
                require(not stress["user_aborted"] and trace(guard)["callback"] > last_callback,
                        "HID callback stopped progressing or real input interrupted the test")
                require(guard["usb_recoveries"] == initial_guard["usb_recoveries"],
                        "USB recovery occurred; not a clean pass")
                require(trace(guard)["completion_timeout"] ==
                        trace(initial_guard)["completion_timeout"], "HID callback timed out")
                require(not guard["keyboard_pending"] and not guard["consumer_pending"],
                        "Unexpected HID release debt")
                last_completed = stress["completed"]
                last_callback = trace(guard)["callback"]
                completed_cycles = cycle
                time.sleep(0.4)
    except BaseException as error:
        failure = error
    finally:
        try:
            if start_sent:
                uart.write(b"usb stress stop\n")
                uart.flush()
                deadline = time.monotonic() + 3
                while time.monotonic() < deadline:
                    uart.read(max(1, uart.in_waiting))
                    final_guard = get_json(args.host, "guard")
                    if not final_guard["usb_stress"]["active"]:
                        break
                    time.sleep(0.1)
                require(final_guard is not None and not final_guard["usb_stress"]["active"],
                        "HID worker did not acknowledge stop")
        except BaseException as error:
            if failure is None:
                failure = error
            else:
                print(f"Stop cleanup: {error}", file=sys.stderr, flush=True)
        finally:
            if uart: uart.close()
        try:
            final = dict(guard=final_guard or get_json(args.host, "guard"),
                         audio=get_json(args.host, "audio"), status=get_json(args.host, "status"),
                         logs=get_json(args.host, "logs"),
                         completed_cycles=completed_cycles,
                         elapsed_seconds=round(time.monotonic() - wall_started, 3),
                         error=str(failure) if failure else None)
            (output / "after.json").write_text(json.dumps(final, ensure_ascii=False, indent=2),
                                               encoding="utf-8")
            if failure is None:
                require(final["status"]["version"] == initial_status["version"],
                        "Firmware version changed during test")
                require(final["status"]["uptime_sec"] - initial_status["uptime_sec"] >=
                        time.monotonic() - wall_started - 3,
                        "Device restarted during test")
                require(not final["audio"]["streaming"], "UAC did not return to alternate setting 0")
                require(final["guard"]["usb_stress"]["failed"] == 0, "HID failed during final close")
                require(not final["guard"]["usb_stress"]["user_aborted"], "Real input interrupted the test")
                require(final["guard"]["usb_stress"]["test_id"] == test_id and
                        final["guard"]["usb_stress"]["started_ms"] == test_started_ms,
                        "HID test state changed or device restarted")
                require(final["guard"]["usb_recoveries"] == initial_guard["usb_recoveries"],
                        "USB recovered during final close")
                require(trace(final["guard"])["completion_timeout"] ==
                        trace(initial_guard)["completion_timeout"], "HID callback timed out during final close")
        except BaseException as error:
            if failure is None:
                failure = error
            else:
                print(f"Final diagnostics: {error}", file=sys.stderr, flush=True)
    if "final" in locals():
        final["error"] = str(failure) if failure else None
        (output / "after.json").write_text(json.dumps(final, ensure_ascii=False, indent=2),
                                           encoding="utf-8")
    if failure is not None:
        raise failure
    print(f"PASS: {completed_cycles} UAC cycles with {final['guard']['usb_stress']['completed']} "
          "completed idle HID reports; audio discarded.", flush=True)


if __name__ == "__main__":
    main()
