"""Read-only, bounded capture of HID/UAC diagnostics during manual remote tests."""
import argparse
import datetime
import json
import pathlib
import time
import urllib.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="192.168.31.4")
    parser.add_argument("--seconds", type=float, default=180)
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    if args.seconds <= 0:
        parser.error("--seconds must be positive")
    output = args.output or pathlib.Path(".cache/usb-diagnostics") / datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    output.mkdir(parents=True, exist_ok=True)
    print(f"Read-only capture for {args.seconds:g}s -> {output}", flush=True)

    def get(route):
        with urllib.request.urlopen(f"http://{args.host}/api/{route}", timeout=3) as response:
            # Preserve diagnostics even if a log message contains invalid UTF-8.
            return json.loads(response.read().decode("utf-8", errors="replace"))

    deadline = time.monotonic() + args.seconds
    previous_fault = None
    samples = errors = 0
    with (output / "samples.jsonl").open("w", encoding="utf-8") as stream:
        while time.monotonic() < deadline:
            sample = {"time": datetime.datetime.now().astimezone().isoformat()}
            try:
                sample["guard"] = guard = get("guard")
                sample["audio"] = get("audio")
                samples += 1
                fault = guard.get("hid_diagnostics", {}).get("last_fault", {})
                key = (fault.get("count", 0), fault.get("ms", 0))
                if key != previous_fault:
                    if key[0]:
                        try:
                            sample["logs"] = get("logs")
                        except Exception as error:
                            sample["logs_error"] = str(error)
                        (output / f"fault-{samples:06d}.json").write_text(
                            json.dumps(sample, ensure_ascii=False, indent=2), encoding="utf-8")
                        print(f"Saved fault: count={key[0]} reason={fault.get('reason')} ms={key[1]}", flush=True)
                    previous_fault = key
            except Exception as error:
                errors += 1
                sample["error"] = str(error)
            stream.write(json.dumps(sample, ensure_ascii=False) + "\n")
            stream.flush()
            remaining = deadline - time.monotonic()
            if remaining > 0:
                time.sleep(min(1, remaining))
    print(f"Finished: {samples} samples, {errors} request errors. {output}", flush=True)


if __name__ == "__main__":
    main()
