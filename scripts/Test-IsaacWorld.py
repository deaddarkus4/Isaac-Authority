"""Standard-tear lifecycle replication in two empty J460 starting rooms."""
import argparse
import copy
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
import isaac_world as world


def module(name, filename):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(filename))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


pair = module("game_pair", "Test-IsaacGamePair.py")
reader = module("state_reader", "Read-IsaacState.py")


def scene(process):
    state = reader.sample(process.read, process.base)
    game = int(state["gameAddress"], 16)
    def u32(address):
        return struct.unpack("<I", process.read(address, 4))[0]
    index, dimension = u32(game + 0x18304), u32(game + 0x1830C)
    if index >= 169 or dimension > 2 or len(state["players"]) != 1:
        raise RuntimeError("A single player in a normal local starting room is required")
    offset = u32(game + 0x17ADC + (dimension * 169 + index) * 4)
    if offset >= 527:
        raise RuntimeError("Invalid room descriptor")
    desc = game + 0x14 + offset * 0xB8
    config = u32(desc + 0x10)
    room = [u32(game), u32(game + 4), index, dimension, u32(config + 8), u32(config + 12),
            u32(config + 0x48), u32(desc + 0x5C), u32(desc + 0x40)]
    if room[4:7] != [1, 2, 1]:
        raise RuntimeError("Only the empty standard starting room is supported")
    player = state["players"][0]
    return room, player["position"] + player["velocity"]


def wait_frame(process, endpoint, sequence=None, timeout=1.5):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        state = world.read_slot(process.read, endpoint, pair.uptime_ms())
        if state is not None and (sequence is None or state[0]["sequence"] >= sequence):
            return state
        time.sleep(0.005)
    raise RuntimeError("No fresh world frame; game may be paused or adapter stopped")


def equal_frame(expected, actual):
    if expected["epoch"] != actual["epoch"] or expected["room"] != actual["room"]:
        return False
    if struct.pack("<4f", *expected["player"]) != struct.pack("<4f", *actual["player"]) or not world.same_players(expected, actual):
        return False
    a = {entity["id"]: entity for entity in expected["entities"]}
    b = {entity["id"]: entity for entity in actual["entities"]}
    return a.keys() == b.keys() and all(world.same_entity(a[key], b[key]) for key in a)


def example(session, room=None, player=None):
    return dict(session=int(session), epoch=1, sequence=1, timeMs=pair.uptime_ms(),
                room=room or [1, 0, 84, 0, 1, 2, 1, 99, 1], player=player or [320, 280, 0, 0], entities=[])


def tear(entity_id, seed, player):
    return dict(id=entity_id, seed=seed, type=2, variant=0, subtype=0,
                body=[player[0] + 24, player[1], 0, 0], tear=[-20, 0, 0, 1])


def fixture(executable):
    process = subprocess.Popen([str(executable), "--fixture"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        endpoint = json.loads(process.stdout.readline())
        frame = example(endpoint["session"])
        address = ("127.0.0.1", endpoint["port"])
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            for sequence in range(1, 6):
                frame["sequence"], frame["timeMs"] = sequence, pair.uptime_ms()
                if sequence == 1:
                    frame["entities"] = [tear(1, 99, frame["player"])]
                elif sequence == 2:
                    frame["entities"][0]["body"][0] += 8
                elif sequence == 3:
                    frame["entities"].append(tear(2, 100, frame["player"]))
                    # Version 2 as encoded here must be accepted by the native decoder; a rejection changes the counts.
                    frame["npcs"] = [dict(type=244, variant=0, subtype=0, seed=2078110152, body=[80, 160, 0.5, -0.25],
                                          target=[80, 160], hp=[7.5, 10], state=8, flags=[1, 5, 4])]
                elif sequence == 4:
                    frame["entities"].pop(0); frame.pop("npcs")
                else:
                    frame["entities"] = []
                data = world.encode(frame)
                udp.sendto(data, address)
                if sequence == 2:
                    udp.sendto(data, address)
                time.sleep(0.1)
            for kind in ("reserved", "foreign", "old"):
                bad = copy.deepcopy(frame)
                bad["sequence"] = 6
                bad["timeMs"] = pair.uptime_ms() - (1000 if kind == "old" else 0)
                if kind == "foreign":
                    bad["session"] ^= 1
                data = bytearray(world.encode(bad))
                if kind == "reserved":
                    data[88] = 1
                udp.sendto(data, address)
            old = copy.deepcopy(frame)
            frame["epoch"], frame["sequence"], frame["timeMs"] = 2, 1, pair.uptime_ms()
            frame["room"][2] += 1
            udp.sendto(world.encode(frame), address)
            time.sleep(0.1)
            old["sequence"], old["timeMs"] = 999, pair.uptime_ms()
            udp.sendto(world.encode(old), address)
        output, error = process.communicate(timeout=5)
        if process.returncode:
            raise RuntimeError(output + error)
        result = json.loads(output)
        if result != dict(creates=2, removes=2, updates=3, roomChanges=1, accepted=6, rejected=5, errors=0):
            raise RuntimeError(f"Unexpected world fixture: {result}")
        print(json.dumps(dict(passed=True, **result)))
    finally:
        if process.poll() is None:
            process.terminate(); process.wait(timeout=3)


def run(args):
    if not args.replica_pid or (not args.probe and (not args.source_pid or args.source_pid == args.replica_pid)):
        raise RuntimeError("Specify replica PID and a different source PID, or --probe")
    here = Path(__file__).resolve().parent; root = here.parent
    binaries = args.binary_directory or (here if (here / "IsaacAuthorityWorldReplica.dll").is_file()
                                        else root / "Binaries/authority-build/Release")
    attach = binaries / "IsaacAuthorityAttach.exe"
    replica_dll, source_dll = binaries / "IsaacAuthorityWorldReplica.dll", binaries / "IsaacAuthorityWorldSource.dll"
    directory = Path(os.environ["LOCALAPPDATA"]) / "IsaacAuthority"
    logs = directory / "logs"
    output = args.output_directory or (root / "Binaries/diagnostics/world" if (root / "native").is_dir() else directory / "verified-world")
    output.mkdir(parents=True, exist_ok=True)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d-%H%M%S-%f")
    before = set(logs.glob("world-*.jsonl"))
    processes, started, failures, transmitted = {}, [], [], []
    control = None; gap = None; epoch_probe = False; endpoint = None

    def invoke(action, pid, dll):
        result = subprocess.run([str(attach), action, str(pid), str(dll)], capture_output=True, text=True, timeout=20)
        if result.returncode:
            raise RuntimeError(f"{action} PID {pid}: {result.stdout} {result.stderr}")

    try:
        replica = reader.WindowsProcess(args.replica_pid); processes[args.replica_pid] = replica
        local_room, local_player = scene(replica)
        # Probe has no keyboard input; the isolated replica already runs in the background.
        control = None if args.probe else pair.HostWindow(args.source_pid)
        if not args.probe:
            source = reader.WindowsProcess(args.source_pid); processes[args.source_pid] = source
            source_room, _ = scene(source)
            if source_room[4:7] != local_room[4:7]:
                raise RuntimeError("Room layouts differ")
            invoke("world", args.source_pid, source_dll); started.append((args.source_pid, source_dll))
            source_endpoint = json.loads((directory / f"world-source-{args.source_pid}.json").read_text())
            wait_frame(source, source_endpoint)
        invoke("world", args.replica_pid, replica_dll); started.append((args.replica_pid, replica_dll))
        endpoint = json.loads((directory / f"world-replica-{args.replica_pid}.json").read_text())
        address = ("127.0.0.1", endpoint["port"])
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            if args.probe:
                frame = example(endpoint["session"], local_room, local_player)
                frame["entities"] = [tear(1, 0x13579, local_player)]
                for sequence in range(1, 4):
                    frame["sequence"], frame["timeMs"] = sequence, pair.uptime_ms()
                    if sequence == 2:
                        frame["entities"][0]["body"][0] += 8
                    if sequence == 3:
                        frame["entities"] = []
                    udp.sendto(world.encode(frame), address); transmitted.append(copy.deepcopy(frame))
                    observed, _ = wait_frame(replica, endpoint, sequence)
                    if not equal_frame(frame, observed):
                        raise RuntimeError("Spawn/update/remove readback differs from requested state")
                    time.sleep(0.2)
            else:
                start_ms = pair.uptime_ms(); gap = [start_ms + 2000, start_ms + 3000]
                # First burst leaves live tears at the packet gap; second tests a new generation.
                inputs = [(1600, chr(0x26), True), (2200, chr(0x26), False),
                          (3900, chr(0x27), True), (4500, chr(0x27), False)]
                previous, last_valid = None, start_ms
                while pair.uptime_ms() - start_ms < 8000:
                    now = pair.uptime_ms(); control.require_focus()
                    while inputs and now - start_ms >= inputs[0][0]:
                        _, key, down = inputs.pop(0); control.key(key, down)
                    state = world.read_slot(source.read, source_endpoint, now)
                    if state is not None:
                        frame, data = state; last_valid = now
                        key = (frame["epoch"], frame["sequence"])
                        if key != previous:
                            previous = key
                            if not epoch_probe and not gap[0] <= now < gap[1]:
                                packet = bytearray(data); struct.pack_into("<Q", packet, 8, int(endpoint["session"]))
                                udp.sendto(packet, address); transmitted.append(frame)
                            # Fault-inject a new room epoch only after a real live-tear frame is applied.
                            if not epoch_probe and now - start_ms >= 4200 and frame["entities"]:
                                observed, _ = wait_frame(replica, endpoint, frame["sequence"])
                                if observed["entities"]:
                                    changed = copy.deepcopy(frame)
                                    changed["session"] = int(endpoint["session"])
                                    changed["epoch"] += 1; changed["sequence"] += 1
                                    changed["room"][2] += 1; changed["timeMs"] = pair.uptime_ms()
                                    udp.sendto(world.encode(changed), address)
                                    time.sleep(0.05)
                                    for _ in range(3):
                                        udp.sendto(packet, address)
                                    epoch_probe = True
                    elif now - last_valid > 1000:
                        raise RuntimeError("Source stopped publishing world frames")
                    time.sleep(0.005)
                if not epoch_probe:
                    raise RuntimeError("No live tear available for the room-epoch teardown probe")
    except BaseException as error:
        failures.append(str(error))
    finally:
        if control is not None:
            try:
                control.release()
            except BaseException as error:
                failures.append(f"Input release: {error}")
        for pid, dll in reversed(started):
            try:
                invoke("world-stop", pid, dll)
            except BaseException as error:
                failures.append(f"Cleanup: {error}")
        for process in processes.values():
            try:
                for table, update in ((0x76BDD0, 0x382AF0), (0x764EAC, 0x2670F0)):
                    pointer = struct.unpack("<I", process.read(process.base + table + 12, 4))[0]
                    if pointer != process.base + update:
                        failures.append("Player or tear update slot was not restored")
            except BaseException as error:
                failures.append(f"Restoration readback: {error}")
            process.close()
    reports, copied = {}, []
    for file in sorted(set(logs.glob("world-*.jsonl")) - before):
        destination = output / file.name
        if destination.exists():
            raise RuntimeError("Report already exists")
        shutil.copyfile(file, destination); copied.append(str(destination))
        records = [json.loads(line) for line in destination.read_text().splitlines()]
        reports[records[0]["role"]] = records
    summary = dict(utc=datetime.datetime.now(datetime.timezone.utc).isoformat(), probe=args.probe,
                   sourcePid=None if args.probe else args.source_pid, replicaPid=args.replica_pid,
                   scope="standard-tears", realRoomTransition=False, syntheticRoomEpochProbe=epoch_probe,
                   packetGapMs=gap, transmitted=len(transmitted), reports=copied, failures=failures, passed=False)
    if not failures:
        try:
            target = reports["replica"]; footer = target[-1]
            if footer["type"] != "stop" or footer["faults"] or footer["dropped"] or footer["networkErrors"] or footer["remaining"] or not footer["slotsRestored"]:
                raise RuntimeError("Incomplete lifecycle, recording or restoration")
            applied = [r for r in target if r["type"] == "frame" and r["action"] == 1]
            sent = {(f["epoch"], f["sequence"]): f for f in transmitted}
            source_records = reports.get("source", [])
            source_frames = {(r["epoch"], r["sequence"]): r for r in source_records if r["type"] == "frame"}
            observed_ids = set()
            for frame in applied:
                key = (frame["epoch"], frame["sequence"])
                if key not in sent or not equal_frame(sent[key], frame):
                    raise RuntimeError("Applied world differs from its full transmitted snapshot")
                if not args.probe and (key not in source_frames or not equal_frame(source_frames[key], frame)):
                    raise RuntimeError("Replica state cannot be traced to the original host update")
                observed_ids.update(entity["id"] for entity in frame["entities"])
            if footer["created"] != len(observed_ids) or footer["removed"] != footer["created"] or not footer["tearHolds"]:
                raise RuntimeError("Unexpected respawn, unremoved entity or independent tear simulation")
            if args.probe:
                if len(applied) != 3 or footer["created"] != 1 or applied[-1]["entities"]:
                    raise RuntimeError("Expected one native tear spawn, update and removal")
            else:
                if len(applied) < 60 or len(observed_ids) < 2 or footer["contextChanges"] != 1 or footer["rejected"] < 1:
                    raise RuntimeError("Incomplete real-tear lifecycle or room epoch rejection")
                if source_records[-1]["faults"] or source_records[-1]["dropped"] or not source_records[-1]["slotsRestored"]:
                    raise RuntimeError("Host observation failed")
                held = [r for r in target if r["type"] == "frame" and r["action"] == 2 and r["entities"] and gap[0] + 300 <= r["observedMs"] < gap[1]]
                if len(held) < 10 or any(not equal_frame(held[0], frame) for frame in held):
                    raise RuntimeError("Tear replica did not hold its live roster through the packet gap")
                if not any(r["observedMs"] >= gap[1] for r in applied):
                    raise RuntimeError("Tear updates did not resume after packet gap")
                summary["liveTearHoldFramesDuringGap"] = len(held)
                summary["hostFramesDuringGap"] = sum(gap[0] <= r["observedMs"] < gap[1] for r in source_frames.values())
            summary.update(passed=True, applied=len(applied), created=footer["created"], removed=footer["removed"],
                           tearUpdatesSuppressed=footer["tearHolds"], remaining=footer["remaining"],
                           rejected=footer["rejected"], allStatesMatchFloat32=True, slotsRestored=True)
        except (RuntimeError, KeyError, IndexError) as error:
            failures.append(str(error))
    trace = output / f"world-{stamp}.sent.jsonl"
    with trace.open("x", encoding="utf-8") as file:
        for frame in transmitted:
            file.write(json.dumps(frame) + "\n")
    summary_path = output / f"world-{stamp}.summary.json"
    with summary_path.open("x", encoding="utf-8") as file:
        json.dump(summary, file, indent=2)
    print(json.dumps(dict(summary=str(summary_path), **summary)))
    if not summary["passed"]:
        raise SystemExit(1)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", type=Path)
    parser.add_argument("--source-pid", type=int)
    parser.add_argument("--replica-pid", type=int)
    parser.add_argument("--probe", action="store_true")
    parser.add_argument("--binary-directory", type=Path)
    parser.add_argument("--output-directory", type=Path)
    options = parser.parse_args()
    fixture(options.fixture) if options.fixture else run(options)
