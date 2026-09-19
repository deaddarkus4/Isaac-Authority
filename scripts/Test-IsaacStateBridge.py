"""Live read-only Isaac -> UDP -> native observer integration check."""
import argparse
import datetime
import json
from pathlib import Path
import secrets
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    executable = root / "Binaries/authority-build/Release/IsaacAuthorityLab.exe"
    directory = root / "Binaries/diagnostics/state-reader"
    directory.mkdir(parents=True, exist_ok=True)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d-%H%M%S")
    capture = directory / f"bridge-{stamp}-{secrets.token_hex(4)}.jsonl"
    session = str(secrets.randbits(63) or 1)
    observer = subprocess.Popen([str(executable), "observer", "0", session],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        ready = json.loads(observer.stdout.readline())
        result = subprocess.run([sys.executable, str(root / "scripts/Read-IsaacState.py"),
                                 "--pid", str(args.pid), "--seconds", "5", "--output", str(capture),
                                 "--observer-port", str(ready["port"]), "--session", session],
                                capture_output=True, text=True, timeout=12)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        output, error = observer.communicate(timeout=10)
        if observer.returncode:
            raise RuntimeError(output + error)
        observed = json.loads(output)
        records = [json.loads(line) for line in capture.read_text(encoding="utf-8").splitlines()]
        sent = [record for record in records if "observerSent" in record]
        if not sent:
            raise RuntimeError("No two-player history snapshots available in this run")
        last = sent[-1]
        players = sorted((e for e in last["history"]["entities"] if e["type"] == 1), key=lambda e: e["index"])
        expected = [e["postPosition"] + e["postVelocity"] for e in players]
        if observed["lastFrame"] != last["observerSent"]["frame"]:
            raise RuntimeError("observer did not receive the final transmitted frame")
        if observed["epoch"] != last["observerSent"]["epoch"]:
            raise RuntimeError("observer epoch differs")
        # Observer prints 9 significant digits; compare float32 bits, not rounded decimal doubles.
        import struct
        if any(struct.pack("<4f", *a) != struct.pack("<4f", *b)
               for a, b in zip(expected, observed["players"])):
            raise RuntimeError("native observer state differs from game history")
        summary = {"capture": str(capture), "sampler": records[-1], "observer": observed,
                   "positionsAndVelocitiesMatchFloat32": True, "writesToGame": False,
                   "atomicGameSnapshot": False}
        report = capture.with_suffix(".summary.json")
        report.write_text(json.dumps(summary, indent=2), encoding="utf-8")
        print(json.dumps(summary))
    finally:
        if observer.poll() is None:
            observer.kill()
            observer.wait(timeout=3)


if __name__ == "__main__":
    main()
