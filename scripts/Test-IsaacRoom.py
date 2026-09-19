"""Real room transition: the J460 host walks through a door and the replica loads the same room."""
import argparse
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
import isaac_level as level
import isaac_world as world


def module(name, filename):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(filename))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


pair = module("game_pair", "Test-IsaacGamePair.py")
reader = module("state_reader", "Read-IsaacState.py")
tears = module("world_test", "Test-IsaacWorld.py")


def check(args):
    """Read-only precondition report; nothing is attached and no input is sent."""
    levels = {}
    for role, pid in (("host", args.source_pid), ("replica", args.replica_pid)):
        process = reader.WindowsProcess(pid)
        try:
            levels[role] = level.snapshot(process.read, process.base)
        finally:
            process.close()
    host, replica = levels["host"], levels["replica"]
    problems = level.run_differences(host, replica)
    here = [level.find(state, state["index"], state["dimension"]) for state in (host, replica)]
    if None in here or level.place(here[0]) != level.place(here[1]):
        problems.append(f"current room differs: {host['index']} and {replica['index']}")
    if host["transition"] or replica["transition"]:
        problems.append("a room transition is still running")
    # Different save files change parts of a level even with one run seed. The replica verifies each room it
    # is sent to, so only the rooms on the route have to match; the rest is reported, never hidden.
    doors = level.shared_doors(host, replica)
    if not problems and not doors:
        problems.append("no door of the current room leads to the same ordinary 1x1 room in both games")
    return dict(hostSeed=level.seed_text(host["startSeed"]), replicaSeed=level.seed_text(replica["startSeed"]),
                room=host["index"], doors={name: room["grid"] for name, room in doors.items()},
                divergentRooms=[cell[0] for cell in level.room_differences(host, replica)], problems=problems), levels


def alive(process):
    """The single player exists and is not dead (Entity+0x172, +0x173)."""
    game = level.u32(process.read, process.base + level.GAME_RVA)
    begin, end = level.u32(process.read, game + 0x1BAA8), level.u32(process.read, game + 0x1BAAC)
    if end - begin != 4:
        return False
    flags = process.read(level.u32(process.read, begin) + 0x170, 4)
    return flags[2] == 1 and flags[3] == 0


def wait_running(process, role, control=None):
    """Attach only to a running game. With PauseOnFocusLost a game may open its pause menu when another window is
    focused and keeps it open afterwards. Escape closes that menu, but it also leaves a death screen for the main
    menu, so it is sent only when the pause menu itself is read as open inside a run."""
    game = level.u32(process.read, process.base + level.GAME_RVA)
    manager = level.u32(process.read, process.base + level.MANAGER_RVA)

    first = level.u32(process.read, game + level.FRAME_COUNT)
    until, next_escape, escapes = time.monotonic() + 12, 0.0, 0
    state = menu = 0
    while time.monotonic() < until:
        time.sleep(0.05)
        if level.u32(process.read, game + level.FRAME_COUNT) != first:
            return
        state, menu = level.u32(process.read, manager + level.MANAGER_STATE), level.u32(process.read, game + level.PAUSE_MENU)
        over = level.u32(process.read, game + level.GAME_OVER)
        # On the death screen Escape means "exit to the main menu": never send it there or for a dead player.
        if control is not None and state == level.IN_RUN and menu and not over and alive(process) and escapes < 2 \
                and time.monotonic() >= next_escape:
            control.key(chr(0x1B), True); time.sleep(0.1); control.key(chr(0x1B), False)
            escapes += 1; next_escape = time.monotonic() + 3
    raise RuntimeError(f"{role} is not running a level (game state {state}, pause menu {menu}, Escape sent {escapes}x): "
                       "a death screen or a menu needs the player")


def restart(pid, role):
    """Hold R in one game until its run restarts. A run started from a typed seed keeps that seed."""
    process = reader.WindowsProcess(pid)
    control = None
    try:
        control = pair.HostWindow(pid)
        game = level.u32(process.read, process.base + level.GAME_RVA)
        manager = level.u32(process.read, process.base + level.MANAGER_RVA)
        if level.u32(process.read, manager + level.MANAGER_STATE) == level.IN_RUN and \
                (level.u32(process.read, game + level.GAME_OVER) or not alive(process)):
            # Death screen: Space restarts the run; R does nothing there and Escape would leave for the main menu.
            time.sleep(0.5); control.key(" ", True); time.sleep(0.15); control.key(" ", False)
            until = time.monotonic() + 8
            while not alive(process):
                if time.monotonic() > until:
                    raise RuntimeError(f"{role} did not restart from its death screen")
                time.sleep(0.05)
            return
        wait_running(process, role, control)
        before = level.u32(process.read, game + level.FRAME_COUNT)
        control.key("R", True)
        until = time.monotonic() + 6
        while level.u32(process.read, game + level.FRAME_COUNT) >= before:
            if time.monotonic() > until:
                raise RuntimeError(f"{role} did not restart its run")
            control.require_focus(); time.sleep(0.02)
    finally:
        if control is not None:
            control.release()
        process.close()


def run(args):
    if not args.source_pid or not args.replica_pid or args.source_pid == args.replica_pid:
        raise RuntimeError("Specify different --source-pid and --replica-pid")
    if args.restart_runs:
        restart(args.replica_pid, "REPLICA"); restart(args.source_pid, "HOST")
        time.sleep(2.0)
    report, initial = check(args)
    if args.check or report["problems"]:
        print(json.dumps(report, indent=2))
        if report["problems"]:
            raise SystemExit("Start the REPLICA run with the HOST seed %s, both in the first room" % report["hostSeed"])
        return
    direction = args.direction or next(iter(report["doors"]))
    if direction not in report["doors"]:
        raise RuntimeError(f"The {direction} door has no room shared by both games; available: {sorted(report['doors'])}")
    destination = level.find(initial["host"], report["doors"][direction])
    _, move, shoot = level.DOORS[direction]

    here = Path(__file__).resolve().parent; root = here.parent
    binaries = args.binary_directory or (here if (here / "IsaacAuthorityRoomReplica.dll").is_file()
                                        else root / "Binaries/authority-build/Release")
    attach = binaries / "IsaacAuthorityAttach.exe"
    names = dict(source="IsaacAuthorityRoomSource.dll", replica="IsaacAuthorityRoomReplica.dll")
    if (binaries / "installation.json").is_file():
        # Installed modules carry a content hash in their name so a new build can load beside an old one.
        names.update(json.loads((binaries / "installation.json").read_text(encoding="utf-8")).get("modules", {}))
    replica_dll, source_dll = binaries / names["replica"], binaries / names["source"]
    directory = Path(os.environ["LOCALAPPDATA"]) / "IsaacAuthority"
    logs = directory / "logs"
    output = args.output_directory or (root / "Binaries/diagnostics/room" if (root / "native").is_dir() else directory / "verified-room")
    output.mkdir(parents=True, exist_ok=True)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d-%H%M%S-%f")
    before = set(logs.glob("room-*.jsonl"))
    processes, started, failures, transmitted = {}, [], [], []
    control = None; left_ms = None; final_rooms = {}; withheld = 0; census = {}

    def survey(*games):
        result = {}
        for role, process in zip(("host", "replica"), games):
            try:
                result[role] = level.entities(process.read, process.base)
            except (OSError, ValueError, struct.error) as error:
                result[role] = str(error)
        return result

    def invoke(action, pid, dll):
        result = subprocess.run([str(attach), action, str(pid), str(dll)], capture_output=True, text=True, timeout=20)
        if result.returncode:
            raise RuntimeError(f"{action} PID {pid}: {result.stdout} {result.stderr}")

    try:
        replica = reader.WindowsProcess(args.replica_pid); processes[args.replica_pid] = replica
        source = reader.WindowsProcess(args.source_pid); processes[args.source_pid] = source
        tears.scene(replica); tears.scene(source)
        control = pair.HostWindow(args.source_pid)
        wait_running(source, "HOST", control); wait_running(replica, "REPLICA")
        invoke("world", args.source_pid, source_dll); started.append((args.source_pid, source_dll))
        source_endpoint = json.loads((directory / f"room-source-{args.source_pid}.json").read_text())
        tears.wait_frame(source, source_endpoint)
        invoke("world", args.replica_pid, replica_dll); started.append((args.replica_pid, replica_dll))
        endpoint = json.loads((directory / f"room-replica-{args.replica_pid}.json").read_text())
        address = ("127.0.0.1", endpoint["port"])
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            start_ms = pair.uptime_ms()
            # Tears in the first room, the walk through the door, then tears in the room behind it.
            inputs = [(1200, shoot, True), (1800, shoot, False)]
            # A door is a narrow gap in the middle of its wall: line the player up with it before walking.
            axis, centre, sideways = (0, 320.0, ("D", "A")) if direction in ("up", "down") else (1, 280.0, ("S", "W"))
            phase = "wait"
            # "request": packets are lost while the host crosses the last stretch to the door, so the replica is
            # never held in the doorway and has to ask its game for the room. "door": nothing is withheld.
            along = 1 - axis; limit = dict(down=400.0, up=160.0, right=560.0, left=80.0)[direction]
            outward = 1 if direction in ("down", "right") else -1
            previous, last_valid = None, start_ms
            while True:
                now = pair.uptime_ms(); control.require_focus()
                while inputs and now - start_ms >= inputs[0][0]:
                    _, key, down = inputs.pop(0); control.key(key, down)
                if phase == "wait" and now - start_ms >= 3000:
                    phase = "align"
                state = world.read_slot(source.read, source_endpoint, now)
                if state is not None:
                    frame, data = state; last_valid = now
                    key = (frame["epoch"], frame["sequence"])
                    if key != previous:
                        previous = key
                        near_door = left_ms is None and phase == "walk" and (frame["player"][along] - limit) * outward > 0
                        if args.route == "request" and near_door:
                            withheld += 1
                        else:
                            packet = bytearray(data); struct.pack_into("<Q", packet, 8, int(endpoint["session"]))
                            udp.sendto(packet, address); transmitted.append(frame)
                        if left_ms is None and frame["room"][2] == destination["grid"]:
                            left_ms = now; phase = "arrived"; control.release()
                            inputs = [(now - start_ms + 1500, shoot, True), (now - start_ms + 2100, shoot, False)]
                        elif phase == "align":
                            error = frame["player"][axis] - centre
                            wanted = None if abs(error) <= 10 else sideways[1] if error > 0 else sideways[0]
                            for held in sideways:
                                if held != wanted and held in control.held:
                                    control.key(held, False)
                            if wanted is not None and wanted not in control.held:
                                control.key(wanted, True)
                            if wanted is None and abs(frame["player"][axis + 2]) < 0.3:
                                phase = "walk"; control.key(move, True)
                # The host publishes nothing while its own transition plays; only a long silence is a failure.
                elif now - last_valid > 3000:
                    raise RuntimeError("Source stopped publishing world frames")
                # Observation for the next stage: what each game spawned in the shared room, before the two
                # independent simulations drift apart. Read-only and best effort.
                if left_ms is not None and "arrival" not in census and \
                        level.u32(replica.read, level.u32(replica.read, replica.base + level.GAME_RVA) + level.ROOM_INDEX) == destination["grid"]:
                    census["arrival"] = survey(source, replica); census["series"] = []; sampled = now
                elif "arrival" in census and now - sampled >= 500:
                    sampled = now; census["series"].append(dict(ms=now - left_ms, **survey(source, replica)))
                if left_ms is None and now - start_ms > 12000:
                    raise RuntimeError(f"HOST did not reach room {destination['grid']} through the {direction} door ({phase})")
                if left_ms is not None and now - left_ms > 4000:
                    break
                time.sleep(0.005)
        census["end"] = survey(source, replica)
        for role, process in (("host", source), ("replica", replica)):
            final_rooms[role] = level.snapshot(process.read, process.base)
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
    for file in sorted(set(logs.glob("room-*.jsonl")) - before):
        target = output / file.name
        if target.exists():
            raise RuntimeError("Report already exists")
        shutil.copyfile(file, target); copied.append(str(target))
        records = [json.loads(line) for line in target.read_text().splitlines()]
        reports[records[0]["role"]] = records
    summary = dict(utc=datetime.datetime.now(datetime.timezone.utc).isoformat(), sourcePid=args.source_pid,
                   replicaPid=args.replica_pid, scope="standard-tears-rooms", realRoomTransition=True,
                   runSeed=report["hostSeed"], route=args.route, direction=direction, fromRoom=report["room"], toRoom=destination["grid"],
                   divergentRoomsFromDifferentSaves=report["divergentRooms"],
                   transmitted=len(transmitted), reports=copied, failures=failures, passed=False)
    if not failures:
        try:
            target = reports["replica"]; footer = target[-1]; host_records = reports["source"]
            if footer["type"] != "stop" or footer["faults"] or footer["dropped"] or footer["networkErrors"] or footer["remaining"] \
                    or footer["levelMismatches"] or footer["traveling"] or not footer["slotsRestored"]:
                raise RuntimeError("Incomplete lifecycle, recording or restoration")
            if host_records[-1]["faults"] or host_records[-1]["dropped"] or not host_records[-1]["slotsRestored"]:
                raise RuntimeError("Host observation failed")
            frames = [r for r in target if r["type"] == "frame"]
            requested = [r for r in frames if r["action"] == 3]
            reached = [r for r in frames if r["action"] == (4 if args.route == "request" else 5)]
            moves = (footer["transitions"], footer["arrivals"], footer["selfMoves"])
            if moves != ((1, 1, 0) if args.route == "request" else (0, 0, 1)) or len(reached) != 1:
                raise RuntimeError(f"Unexpected route to the room: requested, arrived, moved by own game = {moves}")
            if args.route == "request" and not withheld:
                raise RuntimeError("No snapshot was withheld near the door")
            wanted = level.place(destination)
            for record in requested + reached:
                room = record["room"]
                # stage, stageType, index, dimension, type, variant, shape, spawnSeed, visits
                if (room[2], room[3], room[4], room[5], room[6], room[7]) != (wanted[0], wanted[1], wanted[2], wanted[3], wanted[5], wanted[6]):
                    raise RuntimeError("Replica travelled to a room other than the host's")
            sent = {(f["epoch"], f["sequence"]): f for f in transmitted}
            host_frames = {(r["epoch"], r["sequence"]): r for r in host_records if r["type"] == "frame"}
            applied = [r for r in frames if r["action"] == 1]
            copies = {"before": set(), "after": set()}; counts = {"before": 0, "after": 0}
            for frame in applied:
                key = (frame["epoch"], frame["sequence"])
                if key not in sent or not tears.equal_frame(sent[key], frame):
                    raise RuntimeError("Applied world differs from its full transmitted snapshot")
                if key not in host_frames or not tears.equal_frame(host_frames[key], frame):
                    raise RuntimeError("Replica state cannot be traced to the original host update")
                side = "after" if frame["observedMs"] > reached[0]["observedMs"] else "before"
                if (frame["room"][2] == destination["grid"]) != (side == "after"):
                    raise RuntimeError("A snapshot was applied in the wrong room")
                counts[side] += 1
                copies[side].update((frame["epoch"], entity["id"]) for entity in frame["entities"])
            if min(counts.values()) < 20 or not copies["before"] or not copies["after"]:
                raise RuntimeError("Tear replication was not observed on both sides of the door")
            # Copies alive at the moment of an own-game room change are destroyed by that game, not by the module.
            if footer["created"] != len(copies["before"] | copies["after"]) or footer["removed"] + footer["vanished"] != footer["created"]:
                raise RuntimeError("Unexpected respawn or unremoved entity")
            for role, state in final_rooms.items():
                if (state["index"], state["dimension"]) != (destination["grid"], 0) or level.place(level.find(state, state["index"])) != wanted:
                    raise RuntimeError(f"{role} did not finish in the destination room")
            if any(level.differences(initial[role], final_rooms[role]) for role in final_rooms):
                raise RuntimeError("A level layout changed during the transition")
            host_gap = [r["observedMs"] for r in host_frames.values()]
            summary.update(passed=True, appliedBefore=counts["before"], appliedAfter=counts["after"],
                           tearsBefore=len(copies["before"]), tearsAfter=len(copies["after"]),
                           created=footer["created"], removed=footer["removed"], vanished=footer["vanished"],
                           remaining=footer["remaining"], settledStaleSnapshots=footer["settled"], withheldNearDoor=withheld,
                           replicaTravelMs=reached[0]["observedMs"] - requested[0]["observedMs"] if requested else None,
                           hostLongestSilenceMs=max(b - a for a, b in zip(host_gap, host_gap[1:])),
                           rejected=footer["rejected"], allStatesMatchFloat32=True, slotsRestored=True)
        except (RuntimeError, KeyError, IndexError, TypeError) as error:
            failures.append(str(error))
    if census:
        with (output / f"room-{stamp}.entities.json").open("x", encoding="utf-8") as file:
            json.dump(census, file, indent=1)
    trace = output / f"room-{stamp}.sent.jsonl"
    with trace.open("x", encoding="utf-8") as file:
        for frame in transmitted:
            file.write(json.dumps(frame) + "\n")
    summary_path = output / f"room-{stamp}.summary.json"
    with summary_path.open("x", encoding="utf-8") as file:
        json.dump(summary, file, indent=2)
    print(json.dumps(dict(summary=str(summary_path), **summary)))
    if not summary["passed"]:
        raise SystemExit(1)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-pid", type=int)
    parser.add_argument("--replica-pid", type=int)
    parser.add_argument("--check", action="store_true", help="only report whether both games share the run seed and room")
    parser.add_argument("--restart-runs", action="store_true", help="first hold R in both games to restart their seeded runs")
    parser.add_argument("--direction", choices=sorted(level.DOORS))
    parser.add_argument("--route", choices=("door", "request"), default="door",
                        help="door: the replica is carried through by its own game; request: snapshots near the door are lost")
    parser.add_argument("--binary-directory", type=Path)
    parser.add_argument("--output-directory", type=Path)
    run(parser.parse_args())
