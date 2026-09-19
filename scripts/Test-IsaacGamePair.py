"""Real J460 host -> loopback -> second J460 player replica. One empty room only."""
import argparse
import ctypes
import datetime
import importlib.util
import json
import math
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import time
from ctypes import wintypes as W

SOURCE = struct.Struct("<8IQ4f2I")
PACKET = struct.Struct("<IHHQIIIIQ4fII")


class HostWindow:
    def __init__(self, pid):
        self.pid, self.held = pid, set()
        self.u = ctypes.WinDLL("user32", use_last_error=True)
        self.u.GetForegroundWindow.restype = W.HWND
        self.u.GetWindowThreadProcessId.argtypes = [W.HWND, ctypes.POINTER(W.DWORD)]
        self.u.IsWindowVisible.argtypes = [W.HWND]
        self.u.SetForegroundWindow.argtypes = [W.HWND]
        self.u.ShowWindow.argtypes = [W.HWND, ctypes.c_int]
        windows = []
        callback_type = ctypes.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)
        def visit(hwnd, _):
            owner = W.DWORD()
            self.u.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
            if owner.value == pid and self.u.IsWindowVisible(hwnd):
                windows.append(hwnd)
            return True
        callback = callback_type(visit)
        self.u.EnumWindows.argtypes = [callback_type, W.LPARAM]
        self.u.EnumWindows(callback, 0)
        if not windows:
            raise RuntimeError("HOST has no visible window")
        self.hwnd = windows[0]
        self.u.ShowWindow(self.hwnd, 9)
        self.u.SetForegroundWindow(self.hwnd)
        time.sleep(0.1)
        foreground_pid = W.DWORD()
        foreground_thread = self.u.GetWindowThreadProcessId(self.u.GetForegroundWindow(), ctypes.byref(foreground_pid))
        if foreground_pid.value != pid:
            kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel.GetCurrentThreadId.restype = W.DWORD
            current_thread = kernel.GetCurrentThreadId()
            self.u.AttachThreadInput.argtypes = [W.DWORD, W.DWORD, W.BOOL]
            self.u.BringWindowToTop.argtypes = [W.HWND]
            self.u.PeekMessageW.argtypes = [ctypes.c_void_p, W.HWND, W.UINT, W.UINT, W.UINT]
            message = ctypes.create_string_buffer(64)
            self.u.PeekMessageW(message, None, 0, 0, 0)
            attached = current_thread != foreground_thread and self.u.AttachThreadInput(current_thread, foreground_thread, True)
            try:
                self.u.BringWindowToTop(self.hwnd)
                self.u.SetForegroundWindow(self.hwnd)
            finally:
                if attached:
                    self.u.AttachThreadInput(current_thread, foreground_thread, False)
            time.sleep(0.1)
        self.require_focus()
        class Keyboard(ctypes.Structure):
            _fields_ = [("vk", W.WORD), ("scan", W.WORD), ("flags", W.DWORD), ("time", W.DWORD), ("extra", ctypes.c_size_t)]
        class Mouse(ctypes.Structure):
            _fields_ = [("x", W.LONG), ("y", W.LONG), ("data", W.DWORD), ("flags", W.DWORD), ("time", W.DWORD), ("extra", ctypes.c_size_t)]
        class Payload(ctypes.Union):
            _fields_ = [("keyboard", Keyboard), ("mouse", Mouse)]
        class Input(ctypes.Structure):
            _fields_ = [("type", W.DWORD), ("payload", Payload)]
        self.Keyboard, self.Input = Keyboard, Input
        self.u.SendInput.argtypes = [W.UINT, ctypes.POINTER(Input), ctypes.c_int]

    def require_focus(self):
        pid = W.DWORD()
        self.u.GetWindowThreadProcessId(self.u.GetForegroundWindow(), ctypes.byref(pid))
        if pid.value != self.pid:
            raise RuntimeError("HOST lost focus; test input was not sent to another application")

    def key(self, letter, down):
        if down:
            self.require_focus()
        event = self.Input()
        event.type = 1
        virtual_key = ord(letter)
        extended = 1 if 0x21 <= virtual_key <= 0x2E else 0
        event.payload.keyboard = self.Keyboard(0, self.u.MapVirtualKeyW(virtual_key, 0), 8 | extended | (0 if down else 2), 0, 0)
        if self.u.SendInput(1, ctypes.byref(event), ctypes.sizeof(event)) != 1:
            raise RuntimeError("Unable to send HOST test input")
        if down:
            self.held.add(letter)
        else:
            self.held.discard(letter)

    def release(self):
        for letter in tuple(self.held):
            self.key(letter, False)


def uptime_ms():
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.GetTickCount64.restype = ctypes.c_ulonglong
    return kernel.GetTickCount64()


def body_bits(body):
    return struct.pack("<4f", *body)


def read_source(read, address, token, now):
    before = struct.unpack("<I", read(address + 8, 4))[0]
    if before & 1:
        return None
    blob = read(address, SOURCE.size)
    after = struct.unpack("<I", read(address + 8, 4))[0]
    values = SOURCE.unpack(blob)
    if before != after or values[2] != after or after & 1:
        return None
    magic, version, _, alive, sequence, seed, room, player, stamp, *tail = values
    body, thread, actual_token = tail[:4], tail[4], tail[5]
    if magic != 0x43525349 or version != 1 or actual_token != token:
        raise ValueError("Source descriptor does not match live slot")
    if not alive or not sequence:
        return None
    if stamp > now or now - stamp > 250:
        return None
    if not all(math.isfinite(value) for value in body):
        raise ValueError("Nonfinite source state")
    if not (64 <= body[0] <= 576 and 96 <= body[1] <= 400 and abs(body[2]) < 20 and abs(body[3]) < 20):
        raise ValueError("Host left the bounded test area")
    return {"sequence": sequence, "seed": seed, "room": room, "player": player,
            "timeMs": stamp, "body": body, "thread": thread}


def encode(endpoint, state):
    return PACKET.pack(0x50414E53, 1, 2, int(endpoint["session"]), endpoint["epoch"], endpoint["seed"],
                       state["sequence"], 0, state["timeMs"], *state["body"], 0, 0)


def validate(source_records, replica_records, sent, gap, final_source, final_replica):
    source_steps = [r for r in source_records if r["type"] == "player_step"]
    replica_steps = [r for r in replica_records if r["type"] == "player_step"]
    applied = [r for r in replica_steps if r["action"] == 4]
    if len(source_steps) < 120 or len(applied) < 30:
        raise RuntimeError("Insufficient live source updates or applied snapshots")
    for records in (source_records, replica_records):
        if records[-1].get("type") != "stop" or not records[-1].get("vtableRestored") or records[-1]["dropped"]:
            raise RuntimeError("Incomplete restoration or lost log entries")
    if any(r["action"] != 0 for r in source_steps):
        raise RuntimeError("Host observer unexpectedly changed state")
    if any(r["action"] in (1, 2, 5, 6) for r in replica_steps) or replica_records[-1]["networkErrors"]:
        raise RuntimeError("Replica refused a state, lost context or encountered network errors")
    if len({r["thread"] for r in source_steps}) != 1 or len({r["thread"] for r in replica_steps}) != 1:
        raise RuntimeError("Expected one player-update thread per game")
    originals = {r["sourceSequence"]: r for r in source_steps}
    sent_by_sequence = {r["sequence"]: r for r in sent}
    seen = set()
    for application in applied:
        sequence = application["networkSequence"]
        if sequence in seen or sequence not in sent_by_sequence or sequence not in originals:
            raise RuntimeError("Duplicate or untraceable replica snapshot")
        seen.add(sequence)
        expected = sent_by_sequence[sequence]["body"]
        if any(body_bits(body) != body_bits(expected) for body in
               (originals[sequence]["after"], application["requested"], application["after"])):
            raise RuntimeError("Host update, sent packet and replica float32 values differ")
        following = next((r for r in replica_steps if r["sequence"] > application["sequence"] and
                          r["action"] == 3 and r["networkSequence"] == sequence), None)
        if following is None or following["originalCalled"] or body_bits(following["before"]) != body_bits(expected):
            raise RuntimeError("Replica did not hold authoritative state at the next update")
    after_first = [r for r in replica_steps if r["sequence"] > applied[0]["sequence"]]
    if not after_first or any(r["originalCalled"] for r in after_first):
        raise RuntimeError("Client resumed independent player updates during replica mode")
    source_gap = [r for r in source_steps if gap[0] <= r["timeMs"] < gap[1]]
    replica_gap = [r for r in replica_steps if gap[0] + 300 <= r["timeMs"] < gap[1]]
    if len(source_gap) < 15 or len(replica_gap) < 10:
        raise RuntimeError("Both game update loops must continue during the packet gap")
    if len({body_bits(r["after"]) for r in replica_gap}) != 1:
        raise RuntimeError("Replica moved without host snapshots during the packet gap")
    if not any(r["timeMs"] > gap[1] for r in applied):
        raise RuntimeError("Replica did not resume applying snapshots after the packet gap")
    positions = [r["body"] for r in sent]
    span = [max(b[i] for b in positions) - min(b[i] for b in positions) for i in (0, 1)]
    if max(span) < 4:
        raise RuntimeError("No meaningful host movement captured; move HOST a little during the test")
    if abs(final_source[2]) >= 0.01 or abs(final_source[3]) >= 0.01:
        raise RuntimeError("Host must stop before the final comparison")
    if body_bits(final_source) != body_bits(final_replica):
        raise RuntimeError("Final source and replica states differ")
    return {"hostUpdates": len(source_steps), "replicaUpdates": len(replica_steps), "applied": len(applied),
            "hostUpdatesDuringGap": len(source_gap), "replicaUpdatesDuringGap": len(replica_gap),
            "suppressedPlayerUpdates": len(after_first), "motionSpan": span,
            "hostThread": source_steps[0]["thread"], "replicaThread": replica_steps[0]["thread"],
            "allAppliedStatesMatchHostFloat32": True, "finalStatesMatchFloat32": True,
            "replicaHoldsDuringGap": True, "vtableRestoredBoth": True}


def main(args):
    if args.source_pid == args.replica_pid or not 6 <= args.seconds <= 20:
        raise RuntimeError("Two different games and a 6..20 second test are required")
    here = Path(__file__).resolve().parent
    root = here.parent
    binaries = args.binary_directory or (here if (here / "IsaacAuthorityReplica.dll").is_file()
                                        else root / "Binaries/authority-build/Release")
    attach = binaries / "IsaacAuthorityAttach.exe"
    source_dll, replica_dll = binaries / "IsaacAuthoritySource.dll", binaries / "IsaacAuthorityReplica.dll"
    spec = importlib.util.spec_from_file_location("reader", here / "Read-IsaacState.py")
    reader = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(reader)
    directory = Path(os.environ["LOCALAPPDATA"]) / "IsaacAuthority"
    logs = directory / "logs"
    output = args.output_directory or (root / "Binaries/diagnostics/game-pair" if (root / "native").is_dir()
                                      else directory / "verified-pair")
    output.mkdir(parents=True, exist_ok=True)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d-%H%M%S-%f")
    before = {pid: set(logs.glob(f"player-step-{pid}-*.jsonl")) for pid in (args.source_pid, args.replica_pid)}
    processes, started, sent, samples = [], [], [], []
    host_window = None
    failures, endpoint, descriptor, gap = [], None, None, None
    final_source = final_replica = None

    def invoke(action, pid, dll):
        result = subprocess.run([str(attach), action, str(pid), str(dll)], capture_output=True, text=True, timeout=20)
        if result.returncode:
            raise RuntimeError(f"{action} PID {pid}: {result.stdout} {result.stderr}")

    try:
        source = reader.WindowsProcess(args.source_pid); processes.append(source)
        replica = reader.WindowsProcess(args.replica_pid); processes.append(replica)
        host_window = HostWindow(args.source_pid)
        initial = []
        for process in (source, replica):
            state = reader.sample(process.read, process.base)
            if len(state["players"]) != 1:
                raise RuntimeError("Each game must have one player in a solo starting room")
            initial.append(state["players"][0])
        if (initial[0]["variant"], initial[0]["subtype"]) != (initial[1]["variant"], initial[1]["subtype"]):
            raise RuntimeError("Use the same character in both games")
        for i in (0, 1):
            if abs(initial[0]["position"][i] - initial[1]["position"][i]) > 128:
                raise RuntimeError("Initial players are too far apart; use the empty starting rooms")
        invoke("source", args.source_pid, source_dll); started.append((args.source_pid, source_dll))
        invoke("replica", args.replica_pid, replica_dll); started.append((args.replica_pid, replica_dll))
        descriptor = json.loads((directory / f"source-{args.source_pid}.json").read_text(encoding="utf-8"))
        endpoint = json.loads((directory / f"network-{args.replica_pid}.json").read_text(encoding="utf-8"))
        if descriptor["pid"] != args.source_pid or endpoint["pid"] != args.replica_pid or not endpoint["replica"]:
            raise RuntimeError("Unexpected source or replica descriptor")
        time.sleep(0.2)  # Observe multiple real client updates before the first application.
        until = uptime_ms() + 2000
        while read_source(source.read, descriptor["address"], descriptor["token"], uptime_ms()) is None:
            if uptime_ms() >= until:
                raise RuntimeError("HOST has no live player updates; leave pause/menu before testing")
            time.sleep(0.01)
        started_ms = uptime_ms()
        if endpoint["deadlineMs"] - started_ms < (args.seconds + 6) * 1000:
            raise RuntimeError("Not enough receiver session time remains")
        gap = (started_ms + 3000, started_ms + 4000)
        identity, previous = None, 0
        stationary_since = None
        inputs = [(500, "D", True), (650, "D", False), (950, "A", True), (1100, "A", False),
                  (3000, "D", True), (3150, "D", False), (3500, "A", True), (3650, "A", False)] if args.drive_host else []
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            address = ("127.0.0.1", endpoint["port"])
            # Keep relaying for up to five extra seconds while the user releases movement.
            # Require an actual stationary interval, not one lucky near-zero sample.
            while uptime_ms() - started_ms < (args.seconds + 5) * 1000:
                now = uptime_ms()
                if args.drive_host:
                    host_window.require_focus()
                    while inputs and now - started_ms >= inputs[0][0]:
                        _, letter, down = inputs.pop(0)
                        host_window.key(letter, down)
                state = read_source(source.read, descriptor["address"], descriptor["token"], now)
                if state is not None and state["sequence"] != previous:
                    current = (state["seed"], state["room"], state["player"])
                    if identity is None:
                        identity = current
                    if current != identity:
                        raise RuntimeError("Host player or room changed during attachment")
                    previous = state["sequence"]
                    samples.append(state)
                    if not gap[0] <= now < gap[1]:
                        udp.sendto(encode(endpoint, state), address)
                        sent.append(state)
                    if abs(state["body"][2]) < 0.01 and abs(state["body"][3]) < 0.01:
                        if stationary_since is None:
                            stationary_since = now
                    else:
                        stationary_since = None
                    if now - started_ms >= args.seconds * 1000 and stationary_since is not None and now - stationary_since >= 200:
                        break
                time.sleep(0.005)
            if not sent:
                raise RuntimeError("No real host snapshots transmitted")
            final_source = sent[-1]["body"]
            # Keep this host state while checking the independently read client body.
            time.sleep(0.1)
            player_address = int(initial[1]["address"], 16)
            final_replica = list(struct.unpack("<2f", replica.read(player_address + reader.POSITION, 8)) +
                                 struct.unpack("<2f", replica.read(player_address + reader.VELOCITY, 8)))
    except BaseException as error:
        failures.append(str(error))
    finally:
        if host_window is not None:
            try:
                host_window.release()
            except BaseException as error:
                failures.append(f"Input release: {error}")
        for pid, dll in reversed(started):
            try:
                invoke("stop", pid, dll)
            except BaseException as error:
                failures.append(f"Stop failed: {error}")
        for process in processes:
            try:
                actual = struct.unpack("<I", process.read(process.base + 0x76bdd0 + 12, 4))[0]
                if actual != process.base + 0x382af0:
                    failures.append("Original player update slot not restored")
            except BaseException as error:
                failures.append(f"Restoration readback: {error}")
            process.close()
    reports, copied = {}, []
    for pid in (args.source_pid, args.replica_pid):
        new = set(logs.glob(f"player-step-{pid}-*.jsonl")) - before[pid]
        if len(new) != 1 and any(started_pid == pid for started_pid, _ in started):
            failures.append(f"Expected one report for PID {pid}, got {len(new)}")
        for path in new:
            destination = output / path.name
            if destination.exists():
                raise RuntimeError("Report destination already exists")
            shutil.copyfile(path, destination); copied.append(str(destination))
            reports[pid] = [json.loads(line) for line in destination.read_text(encoding="utf-8").splitlines()]
    summary = {"utc": datetime.datetime.now(datetime.timezone.utc).isoformat(), "sourcePid": args.source_pid,
               "replicaPid": args.replica_pid, "source": "game-thread-slot", "sourceSamples": len(samples),
               "sent": len(sent), "packetGapMs": gap, "sourceDescriptor": descriptor, "endpoint": endpoint,
               "reports": copied, "finalSource": final_source, "finalReplica": final_replica,
               "failures": failures, "passed": False, "worldReplication": False}
    summary["hostInput"] = "bounded-A-D-keyboard-pulses" if args.drive_host else "user"
    if not failures:
        try:
            summary.update(validate(reports[args.source_pid], reports[args.replica_pid], sent, gap,
                                    final_source, final_replica), passed=True)
        except RuntimeError as error:
            failures.append(str(error))
    trace = output / f"pair-{stamp}.source.jsonl"
    with trace.open("x", encoding="utf-8") as file:
        for sample in samples:
            file.write(json.dumps(sample) + "\n")
    summary_path = output / f"pair-{stamp}.summary.json"
    with summary_path.open("x", encoding="utf-8") as file:
        json.dump(summary, file, indent=2)
    print(json.dumps({"summary": str(summary_path), **summary}))
    if not summary["passed"]:
        raise SystemExit(1)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-pid", required=True, type=int)
    parser.add_argument("--replica-pid", required=True, type=int)
    parser.add_argument("--seconds", type=float, default=10)
    parser.add_argument("--binary-directory", type=Path)
    parser.add_argument("--output-directory", type=Path)
    parser.add_argument("--drive-host", action="store_true", help="Send four bounded A/D pulses to foreground HOST")
    main(parser.parse_args())
