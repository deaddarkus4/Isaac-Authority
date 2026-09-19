"""Bounded loopback host -> live J460 application test; not a second game client."""
import argparse
import ctypes
import datetime
import importlib.util
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import time

PACKET = struct.Struct("<IHHQIIIIQ4fII")


def uptime_ms():
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.GetTickCount64.restype = ctypes.c_ulonglong
    return kernel.GetTickCount64()


def packet(endpoint, sequence, body, *, age_ms=0, foreign=False):
    return PACKET.pack(0x50414E53, 1, 1, int(endpoint["session"]) ^ int(foreign),
                       endpoint["epoch"], endpoint["seed"], sequence, 0,
                       uptime_ms() - age_ms, *body, 0, 0)


def float_body(values):
    return list(struct.unpack("<4f", struct.pack("<4f", *values)))


def same_body(first, second):
    return struct.pack("<4f", *first) == struct.pack("<4f", *second)


def send_scenario(endpoint, observe=None, sent=None):
    if endpoint["host"] != "127.0.0.1" or not 0 < endpoint["port"] < 65536:
        raise RuntimeError("Only loopback endpoints are supported")
    if endpoint["deadlineMs"] - uptime_ms() < 4000:
        raise RuntimeError("Receiver endpoint expired or too little session time remains")
    baseline = endpoint["baseline"]
    if not (100 <= baseline[0] <= 500 and 100 <= baseline[1] <= 400
            and abs(baseline[2]) < 0.01 and abs(baseline[3]) < 0.01):
        raise RuntimeError("Stand still near the center of a solo starting room")
    address = (endpoint["host"], endpoint["port"])
    if sent is None:
        sent = []
    # The test host owns absolute states. It does not send a '+8' command to the DLL.
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
        first = float_body([baseline[0] + 2, baseline[1], 0, 0])
        udp.sendto(packet(endpoint, 1, first, foreign=True), address)
        udp.sendto(packet(endpoint, 1, first, age_ms=1000), address)
        for sequence, offset in enumerate((2, 4, 6, 8, 0), 1):
            body = float_body([baseline[0] + offset, baseline[1], 0, 0])
            data = packet(endpoint, sequence, body)
            udp.sendto(data, address)
            udp.sendto(data, address)  # Duplicate must never be applied again.
            sent.append({"sequence": sequence, "body": body})
            if observe is not None:
                deadline = time.monotonic() + 0.75
                while time.monotonic() < deadline:
                    if same_body(observe(), body):
                        break
                    time.sleep(0.01)
                else:
                    raise RuntimeError(f"Game did not reach host state {sequence}; keep it running, unpaused")
            time.sleep(0.2)
        # A delayed old snapshot cannot undo the return to the baseline.
        udp.sendto(packet(endpoint, 2, first), address)
        time.sleep(0.3)
    return sent


def validate_report(records, sent):
    if not records or not records[0].get("network") or records[-1].get("type") != "stop":
        raise RuntimeError("Missing complete network adapter report")
    footer = records[-1]
    steps = [r for r in records if r["type"] == "player_step"]
    applied = [r for r in steps if r["action"] == 4]
    if len({r["thread"] for r in steps}) != 1 or len(applied) != len(sent):
        raise RuntimeError("Expected all host states exactly once on a single game thread")
    if any(r["action"] in (1, 2, 5, 6) for r in steps):
        raise RuntimeError("Unexpected local correction, refused state or expired player context")
    for actual, expected in zip(applied, sent):
        if actual["networkSequence"] != expected["sequence"] or not same_body(actual["requested"], expected["body"]) or not same_body(actual["after"], expected["body"]):
            raise RuntimeError("Applied game state differs from the host float32 payload")
        following = next((r for r in steps if r["sequence"] > actual["sequence"] and
                          r["action"] == 3 and r["networkSequence"] == expected["sequence"]), None)
        if following is None or not same_body(following["before"], expected["body"]):
            raise RuntimeError("Missing next game update preserving authoritative state")
    if not footer["vtableRestored"] or footer["dropped"] or footer["networkErrors"]:
        raise RuntimeError("Restoration, recording or network failure")
    if footer["networkAccepted"] != len(sent) or footer["networkRejected"] < len(sent) + 3:
        raise RuntimeError("Unexpected accepted/rejected packet counts")
    return {"updates": len(steps), "applied": len(applied), "thread": steps[0]["thread"],
            "networkAccepted": footer["networkAccepted"], "networkRejected": footer["networkRejected"],
            "vtableRestored": True, "allStatesMatchFloat32": True, "nextUpdatesVerified": True}


def live(args):
    root = Path(__file__).resolve().parent.parent
    binaries = args.binary_directory or (Path(__file__).parent if Path(__file__).with_name("IsaacAuthorityNetwork.dll").is_file()
                                        else root / "Binaries/authority-build/Release")
    attach, dll = binaries / "IsaacAuthorityAttach.exe", binaries / "IsaacAuthorityNetwork.dll"
    for file in (attach, dll):
        if not file.is_file():
            raise RuntimeError(f"Missing binary: {file}")
    spec = importlib.util.spec_from_file_location("reader", Path(__file__).with_name("Read-IsaacState.py"))
    reader = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(reader)
    process = reader.WindowsProcess(args.pid)
    started = False
    directory = Path(os.environ["LOCALAPPDATA"]) / "IsaacAuthority"
    logs = directory / "logs"
    before = set(logs.glob(f"player-step-{args.pid}-*.jsonl"))
    sent, failure, stop_failure, endpoint = [], None, None, None

    def invoke(action):
        result = subprocess.run([str(attach), action, str(args.pid), str(dll)],
                                text=True, capture_output=True, timeout=20)
        if result.returncode:
            raise RuntimeError(f"{action}: {result.stdout} {result.stderr}")

    try:
        state = reader.sample(process.read, process.base)
        if len(state["players"]) != 1:
            raise RuntimeError("Exactly one player in a solo room is required")
        player = state["players"][0]
        if not (100 <= player["position"][0] <= 500 and 100 <= player["position"][1] <= 400
                and all(abs(v) < 0.01 for v in player["velocity"])):
            raise RuntimeError("Stand still near the center of the solo starting room")
        address = int(player["address"], 16)

        def observe():
            # Independent readback; the DLL also records values on the update thread.
            return list(struct.unpack("<2f", process.read(address + reader.POSITION, 8)) +
                        struct.unpack("<2f", process.read(address + reader.VELOCITY, 8)))

        invoke("network")
        started = True
        endpoint = json.loads((directory / f"network-{args.pid}.json").read_text(encoding="utf-8"))
        if endpoint["pid"] != args.pid:
            raise RuntimeError("Endpoint belongs to another game")
        time.sleep(0.2)
        send_scenario(endpoint, observe, sent)
        if not same_body(observe(), sent[-1]["body"]):
            raise RuntimeError("Final game state drifted after stale packet test")
    except BaseException as error:
        failure = str(error)
    finally:
        if started:
            try:
                invoke("stop")
            except BaseException as error:
                stop_failure = str(error)
        process.close()
    output = args.output_directory or (root / "Binaries/diagnostics/network-state" if (root / "native").is_dir()
                                      else directory / "verified-network")
    output.mkdir(parents=True, exist_ok=True)
    reports = sorted(set(logs.glob(f"player-step-{args.pid}-*.jsonl")) - before)
    copied = []
    for report in reports:
        destination = output / report.name
        if destination.exists():
            raise RuntimeError("Report destination already exists")
        shutil.copyfile(report, destination)
        copied.append(str(destination))
    summary = {"utc": datetime.datetime.now(datetime.timezone.utc).isoformat(), "pid": args.pid,
               "reports": copied, "sent": sent, "endpoint": endpoint,
               "testHost": "bounded-loopback-fixture", "secondGameClient": False,
               "failure": failure, "stopFailure": stop_failure, "passed": False}
    try:
        if failure or stop_failure:
            raise RuntimeError(f"Test failure: {failure}; stop failure: {stop_failure}")
        if len(reports) != 1:
            raise RuntimeError("Expected exactly one new report")
        records = [json.loads(line) for line in reports[0].read_text(encoding="utf-8").splitlines()]
        summary.update(validate_report(records, sent), passed=True)
    except RuntimeError as error:
        summary["failure"] = str(error)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d-%H%M%S-%f")
    summary_path = output / f"network-{args.pid}-{stamp}.summary.json"
    with summary_path.open("x", encoding="utf-8") as file:
        json.dump(summary, file, indent=2)
    print(json.dumps({"summary": str(summary_path), **summary}))
    if not summary["passed"]:
        raise SystemExit(1)


def fixture(executable):
    process = subprocess.Popen([str(executable), "--fixture"], stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True)
    try:
        endpoint = json.loads(process.stdout.readline())
        sent = send_scenario(endpoint)
        output, error = process.communicate(timeout=6)
        if process.returncode:
            raise RuntimeError(output + error)
        result = json.loads(output)
        if result["applied"] != len(sent) or result["refused"] or result["last"] != sent[-1]["sequence"] or result["rejected"] < 8:
            raise RuntimeError(f"Unexpected fixture result: {result}")
        if not same_body(result["body"], sent[-1]["body"]):
            raise RuntimeError("C++ receiver state differs from Python host float32 packet")
        print(json.dumps({"fixturePassed": True, **result}))
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=3)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--pid", type=int)
    mode.add_argument("--fixture", type=Path)
    parser.add_argument("--binary-directory", type=Path)
    parser.add_argument("--output-directory", type=Path)
    options = parser.parse_args()
    if options.fixture:
        fixture(options.fixture)
    else:
        live(options)
