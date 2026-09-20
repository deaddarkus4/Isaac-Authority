"""Two players of the host's run on the replica. The host publishes every player with its controller index: the first is
driven by the HOST keyboard, the second by client commands over UDP. The replica, which created its own second player the
same way, shows both. One shared first room, no door.

With --loop the commands are not scripted: keys are pressed in the client's own window, its input module captures them, and
the client sees its player move only because the host simulated it and sent the world back."""
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
import isaac_input as commands
import isaac_link as link
import isaac_world as world


def module(name, filename):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(filename))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


rooms = module("room_test", "Test-IsaacRoom.py")
pair, reader, tears = rooms.pair, rooms.reader, rooms.tears
CONTROLLER_INDEX = 0x1618
SLOTS = ((0x76BDD0 + 12, 0x382AF0, "player update"), (0x764EAC + 12, 0x2670F0, "tear update"), (0x782950 + 29 * 4, 0x620FB0, "input manager"))
UP, RIGHT = chr(0x26), chr(0x27)
# The first player: (ms, key, down). It walks left while the second walks right, shoots up, walks back.
KEYS = [(1000, "A", True), (1700, "A", False), (3400, UP, True), (4000, UP, False), (5200, "D", True), (5900, "D", False)]
# The second player: (from ms, move, shoot) until the next row. The last row is rest, so both games end standing still.
# A restarted run puts the players near the bottom wall of the first room, so the vertical walk goes up.
CLIENT = [(0, (0, 0), (0, 0)), (1000, (1, 0), (0, 0)), (1700, (0, 0), (0, 0)), (2600, (0, -1), (0, 0)), (3100, (0, 0), (0, 0)),
          (4200, (0, 0), (1, 0)), (4800, (0, 0), (0, 0)), (5200, (-1, 0), (0, 0)), (5900, (0, 0), (0, 0))]
DURATION = 7500
# What each player must have done on the host, from its own frames: (name, from ms, to ms, player, axis, sign or 0 for "stays").
EXPECTED = [("first walks left", 1000, 1900, 0, 0, -1), ("second walks right", 1000, 1900, 1, 0, 1),
            ("second walks up", 2600, 3300, 1, 1, -1), ("first stays", 2600, 3300, 0, 1, 0),
            ("first walks right", 5200, 6100, 0, 0, 1), ("second walks left", 5200, 6100, 1, 0, -1)]
# The closed loop (--loop): every key is pressed in the CLIENT's window and reaches the host only as commands captured by the
# client module. Nobody is at the host's keyboard, so the host's own player never moves.
LOOP_KEYS = [(1000, "D", True), (1700, "D", False), (2600, "W", True), (3100, "W", False), (3800, RIGHT, True), (4400, RIGHT, False),
             (5200, "A", True), (5900, "A", False)]
LOOP_EXPECTED = [("second walks right", 1000, 1900, 1, 0, 1), ("first stays", 1000, 1900, 0, 0, 0),
                 ("second walks up", 2600, 3300, 1, 1, -1), ("second walks left", 5200, 6100, 1, 0, -1),
                 ("first still stays", 5200, 6100, 0, 0, 0)]


def team(process):
    """Players in list order with the controller index that drives each."""
    return [dict(position=p["position"], velocity=p["velocity"],
                 controller=struct.unpack("<i", process.read(int(p["address"], 16) + CONTROLLER_INDEX, 4))[0])
            for p in reader.sample(process.read, process.base)["players"]]


def join(pid, role, controller, invoke, dll, directory):
    """The game creates the second player itself: a free controller presses the co-op join button and confirms the character."""
    process = reader.WindowsProcess(pid)
    try:
        before = team(process)
        if len(before) == 2 and before[1]["controller"] == controller:
            return "present"
        if len(before) != 1:
            raise RuntimeError(f"{role} has {len(before)} players; restart its run")
        control = pair.HostWindow(pid)  # focus only: the join is a menu of the focused game
        rooms.wait_running(process, role, control)
        invoke("input", pid, dll)
        try:
            endpoint = json.loads((directory / f"input-host-{pid}.json").read_text())
            address, sequence = ("127.0.0.1", endpoint["port"]), 0
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
                for name, seconds in (("before", 0.5), ("join", 0.3), ("choice", 1.2), ("menuConfirm", 0.3), ("after", 2.0)):
                    until = time.monotonic() + seconds
                    while time.monotonic() < until:
                        control.require_focus()
                        sequence += 1
                        udp.sendto(commands.encode(endpoint["session"], sequence, pair.uptime_ms(), controller, (0, 0), (0, 0),
                                                   (name,) if name in commands.BUTTONS else ()), address)
                        time.sleep(1 / 60)
        finally:
            invoke("input-stop", pid, dll)
        after = team(process)
        if len(after) != 2 or after[1]["controller"] != controller:
            raise RuntimeError(f"{role} did not create a second player for controller {controller}: {after}")
        return "joined"
    finally:
        process.close()


def run(args):
    if not args.source_pid or not args.replica_pid or args.source_pid == args.replica_pid:
        raise RuntimeError("Specify different --source-pid and --replica-pid")
    if args.restart_runs:
        rooms.restart(args.replica_pid, "REPLICA"); rooms.restart(args.source_pid, "HOST")
        time.sleep(2.0)
    report, _ = rooms.check(args)
    if args.check or report["problems"]:
        print(json.dumps(report, indent=2))
        if report["problems"]:
            raise SystemExit("Both games need the run seed %s, the same difficulty and the first room; a level or a curse that still "
                             "differs means different save files: start the other instance with "
                             "Start-IsaacReplica.ps1 -PersistentGameData <the host's save>" % report["hostSeed"])
        return
    here = Path(__file__).resolve().parent; root = here.parent
    binaries = args.binary_directory or (here if (here / "installation.json").is_file() else root / "Binaries/authority-build/Release")
    names = dict(source="IsaacAuthorityCoopSource.dll", replica="IsaacAuthorityCoopReplica.dll", host="IsaacAuthorityInput.dll",
                 client="IsaacAuthorityInputClient.dll", predictSource="IsaacAuthorityPredictSource.dll",
                 predictReplica="IsaacAuthorityPredictReplica.dll", predictClient="IsaacAuthorityInputPredict.dll",
                 steppedHost="IsaacAuthorityInputSteppedHost.dll", steppedClient="IsaacAuthorityInputSteppedClient.dll")
    if (binaries / "installation.json").is_file():
        names.update(json.loads((binaries / "installation.json").read_text(encoding="utf-8")).get("modules", {}))
    attach = binaries / "IsaacAuthorityAttach.exe"
    directory = Path(os.environ["LOCALAPPDATA"]) / "IsaacAuthority"; logs = directory / "logs"
    output = args.output_directory or (root / "Binaries/diagnostics/coop" if (root / "native").is_dir() else directory / "verified-coop")
    output.mkdir(parents=True, exist_ok=True)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d-%H%M%S-%f")
    # Prediction is its own set of modules: the source acknowledges consumed commands, the replica lets the client's own
    # player run, the client's input module walks that player with the command it sends.
    stage = "predict" if args.predict else "coop"
    source_dll, replica_dll, client_dll = (binaries / names[role] for role in (
        ("predictSource", "predictReplica", "predictClient") if args.predict else ("source", "replica", "client")))
    # Stepped: the host plays one command per step out of a buffer and the client captures one per step of its own player.
    # Joining still goes through the plain host module: a controller without a player is never stepped.
    drive_dll = binaries / names["steppedHost" if args.stepped else "host"]
    if args.stepped:
        client_dll = binaries / names["steppedClient"]
    before = set(logs.glob(stage + "-*.jsonl")) | set(logs.glob("input-*.jsonl"))
    processes, started, failures, transmitted, joined, ends = {}, [], [], [], {}, {}
    control = None; start_ms = None; sent_commands = 0; pressed_ms = {}; network = {}
    impaired = bool(args.delay_ms or args.jitter_ms or args.loss)
    # The keyboard belongs to the focused game. In the closed loop that is the client; otherwise it is the host.
    keyboard_pid, keyboard_role = (args.replica_pid, "CLIENT") if args.loop else (args.source_pid, "HOST")
    script_keys, expected = (LOOP_KEYS, LOOP_EXPECTED) if args.loop else (KEYS, EXPECTED)

    def invoke(action, pid, dll):
        result = subprocess.run([str(attach), action, str(pid), str(dll)], capture_output=True, text=True, timeout=20)
        if result.returncode:
            raise RuntimeError(f"{action} PID {pid}: {result.stdout} {result.stderr}")

    try:
        # The game with the keyboard joins last and keeps the focus; a game that pauses on focus loss must be that one.
        order = (("host", args.source_pid), ("replica", args.replica_pid)) if args.loop else (("replica", args.replica_pid), ("host", args.source_pid))
        for role, pid in order:
            joined[role] = join(pid, role.upper(), args.controller, invoke, binaries / names["host"], directory)
        replica = reader.WindowsProcess(args.replica_pid); processes[args.replica_pid] = replica
        source = reader.WindowsProcess(args.source_pid); processes[args.source_pid] = source
        control = pair.HostWindow(keyboard_pid)
        rooms.wait_running(processes[keyboard_pid], keyboard_role, control)
        rooms.wait_running(replica if keyboard_pid == args.source_pid else source, "the game without the keyboard")
        invoke("input", args.source_pid, drive_dll); started.append(("input-stop", args.source_pid, drive_dll))
        client = json.loads((directory / f"input-host-{args.source_pid}.json").read_text())
        invoke("world", args.source_pid, source_dll); started.append(("world-stop", args.source_pid, source_dll))
        source_endpoint = json.loads((directory / f"{stage}-source-{args.source_pid}.json").read_text())
        tears.wait_frame(source, source_endpoint)
        invoke("world", args.replica_pid, replica_dll); started.append(("world-stop", args.replica_pid, replica_dll))
        endpoint = json.loads((directory / f"{stage}-replica-{args.replica_pid}.json").read_text())
        captured = None
        if args.loop:
            invoke("input", args.replica_pid, client_dll); started.append(("input-stop", args.replica_pid, client_dll))
            captured = json.loads((directory / f"input-client-{args.replica_pid}.json").read_text())
        address, client_address = ("127.0.0.1", endpoint["port"]), ("127.0.0.1", client["port"])
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            # The network between the two games, one impaired path each way. A snapshot counts as transmitted when it
            # leaves the path, not when the host offers it.
            uplink = link.Link(lambda packet, _: udp.sendto(packet, client_address), args.delay_ms, args.jitter_ms, args.loss, args.network_seed)
            downlink = link.Link(lambda packet, frame: (udp.sendto(packet, address), transmitted.append(frame)),
                                 args.delay_ms, args.jitter_ms, args.loss, args.network_seed + 1)
            start_ms = pair.uptime_ms(); keys = list(script_keys); previous, last_valid, last_command = None, start_ms, 0
            command_sequence = 0
            while True:
                now = pair.uptime_ms(); control.require_focus()
                if now - start_ms >= DURATION:
                    break
                while keys and now - start_ms >= keys[0][0]:
                    _, key, down = keys.pop(0); control.key(key, down)
                    if down:
                        pressed_ms.setdefault(key, pair.uptime_ms())
                if args.loop:
                    # The only way from the client's keyboard to the host: what the client module captured, carried as is.
                    # The transport, not the client, says which of the host's controllers this client drives.
                    sample = commands.read_slot(replica.read, captured, now)
                    if sample is not None and sample[0]["sequence"] != last_command:
                        last_command = sample[0]["sequence"]
                        uplink.send(commands.route(sample[1], client["session"], args.controller), now)
                elif now - last_command >= 10:
                    _, move, shoot = [row for row in CLIENT if row[0] <= now - start_ms][-1]
                    command_sequence += 1; last_command = now
                    uplink.send(commands.encode(client["session"], command_sequence, now, args.controller, move, shoot), now)
                state = world.read_slot(source.read, source_endpoint, now)
                if state is not None:
                    frame, data = state; last_valid = now
                    key = (frame["epoch"], frame["sequence"])
                    if key != previous:
                        previous = key
                        packet = bytearray(data); struct.pack_into("<Q", packet, 8, int(endpoint["session"]))
                        downlink.send(bytes(packet), now, frame)
                elif now - last_valid > 1000:
                    raise RuntimeError("Source stopped publishing world frames")
                uplink.flush(now); downlink.flush(now)
                time.sleep(0.005)
            sent_commands = uplink.delivered
            network = dict(seed=args.network_seed, clientToHost=uplink.report(), hostToClient=downlink.report())
        # Both games stand still now and the replica is still held: read them from outside before anything is stopped.
        ends = dict(host=team(source), replica=team(replica))
    except BaseException as error:
        failures.append(str(error))
    finally:
        if control is not None:
            try:
                control.release()
            except BaseException as error:
                failures.append(f"Input release: {error}")
        for action, pid, dll in reversed(started):
            try:
                invoke(action, pid, dll)
            except BaseException as error:
                failures.append(f"Cleanup: {error}")
        for process in processes.values():
            try:
                for slot, original, name in SLOTS:
                    if struct.unpack("<I", process.read(process.base + slot, 4))[0] != process.base + original:
                        failures.append(f"The {name} slot was not restored")
            except BaseException as error:
                failures.append(f"Restoration readback: {error}")
            process.close()
    reports, inputs, copied = {}, [], []
    # Oldest first: the last input session is the one that drove the second player.
    for file in sorted((set(logs.glob(stage + "-*.jsonl")) | set(logs.glob("input-*.jsonl"))) - before, key=lambda f: f.stat().st_mtime):
        target = output / file.name
        if target.exists():
            raise RuntimeError("Report already exists")
        shutil.copyfile(file, target); copied.append(str(target))
        records = [json.loads(line) for line in target.read_text().splitlines()]
        if file.name.startswith("input-"):
            inputs.append(records[-1])
        else:
            reports[records[0]["role"]] = records
    summary = dict(utc=datetime.datetime.now(datetime.timezone.utc).isoformat(), sourcePid=args.source_pid, replicaPid=args.replica_pid,
                   scope="standard-tears-rooms-enemies-players", realRoomTransition=False, runSeed=report["hostSeed"], room=report["room"],
                   mode="closed-loop-predicted-stepped" if args.stepped else "closed-loop-predicted" if args.predict else "closed-loop" if args.loop else "scripted-client", keyboardIn=keyboard_role,
                   clientController=args.controller, secondPlayer=joined, commandsSent=sent_commands, impairedNetwork=impaired, network=network,
                   **({} if args.loop else dict(keysPressedForSecondPlayer=0)),
                   transmitted=len(transmitted), reports=copied, failures=failures, passed=False)
    if not failures:
        try:
            target = reports["replica"]; footer = target[-1]; host_records = reports["source"]; host_footer = host_records[-1]
            if footer["type"] != "stop" or footer["faults"] or footer["dropped"] or footer["networkErrors"] or footer["remaining"] \
                    or footer["levelMismatches"] or footer["traveling"] or footer["contextChanges"] or footer["rosterMismatches"] \
                    or footer["transitions"] or footer["selfMoves"] or footer["playerShielded"] or not footer["slotsRestored"]:
                raise RuntimeError("Incomplete lifecycle, recording or restoration on the replica")
            if host_footer["faults"] or host_footer["dropped"] or host_footer["lastFault"] or not host_footer["slotsRestored"]:
                raise RuntimeError("Host observation failed")
            if (host_footer["players"], footer["players"]) != (2, 2):
                raise RuntimeError("A module did not see two players")
            drive = [f for f in inputs if f["role"] == "input-host"][-1]
            capture = [f for f in inputs if f["role"] == "input-client"]
            if any(not f["slotRestored"] or f["networkErrors"] for f in inputs) or drive["accepted"] < sent_commands * (0.15 if args.jitter_ms else 0.9):
                raise RuntimeError("The input module failed or did not receive the client's commands")
            if args.loop and (len(capture) != 1 or capture[0]["samples"] < 300 or not capture[0]["activeSamples"]):
                raise RuntimeError(f"The client module did not capture the keyboard: {capture}")
            sent = {(f["epoch"], f["sequence"]): f for f in transmitted}
            host_frames = {(r["epoch"], r["sequence"]): r for r in host_records if r["type"] == "frame"}
            applied = [r for r in target if r["type"] == "frame" and r["action"] == 1]
            copies = set(); born = {0: set(), 1: set()}
            for frame in applied:
                key = (frame["epoch"], frame["sequence"])
                if key not in sent or not tears.equal_frame(sent[key], frame):
                    raise RuntimeError("Applied world differs from its full transmitted snapshot")
                if key not in host_frames or not tears.equal_frame(host_frames[key], frame):
                    raise RuntimeError("Replica state cannot be traced to the original host update")
                listed = world.players(frame)
                if [p["controller"] for p in listed] != [0, args.controller]:
                    raise RuntimeError(f"A frame does not list the keyboard player and the client's player: {listed}")
                for entity in frame["entities"]:
                    if (frame["epoch"], entity["id"]) not in copies:
                        # A tear first appears next to the player who fired it.
                        copies.add((frame["epoch"], entity["id"]))
                        born[min((0, 1), key=lambda i: (listed[i]["body"][0] - entity["body"][0]) ** 2 +
                                 (listed[i]["body"][1] - entity["body"][1]) ** 2)].add(entity["id"])
            # In the closed loop nobody touches the host's keyboard: only the client's player fires.
            # An impaired path loses snapshots, and of two that arrive out of order or together only the newer is applied.
            enough = 100 if impaired else 200
            if len(applied) < enough or not born[1] or bool(born[0]) == args.loop:
                raise RuntimeError(f"Too few applied frames ({len(applied)}) or unexpected tears: first player {len(born[0])}, second {len(born[1])}")
            if footer["created"] != len(copies) or footer["removed"] + footer["vanished"] != footer["created"]:
                raise RuntimeError("Unexpected respawn or unremoved entity")
            moved = {}
            for name, begin, end, player, axis, sign in expected:
                rows = [world.players(f)[player]["body"] for f in applied if begin <= f["observedMs"] - start_ms <= end]
                if len(rows) < 10:
                    raise RuntimeError(f"Too few frames applied while the {name}")
                shift = [rows[-1][i] - rows[0][i] for i in (0, 1)]; moved[name] = [round(v, 1) for v in shift]
                if (sign and (shift[axis] * sign < 25 or abs(shift[1 - axis]) > 12)) or (not sign and max(map(abs, shift)) > 1.0):
                    raise RuntimeError(f"On the replica the {name}: moved by {shift}")
            apart = [((a["position"][0] - b["position"][0]) ** 2 + (a["position"][1] - b["position"][1]) ** 2) ** 0.5
                     for a, b in zip(ends["host"], ends["replica"])]
            if len(apart) != 2 or max(apart) > 0.5:
                raise RuntimeError(f"The games disagree on where the players stand at the end: {apart}")
            summary.update(applied=len(applied), playersPerFrame=2, movedOnReplica=moved,
                           tearsOfFirstPlayer=len(born[0]), tearsOfSecondPlayer=len(born[1]), created=footer["created"],
                           removed=footer["removed"], vanished=footer["vanished"], remaining=footer["remaining"],
                           playerUpdatesHeld=footer["playerHolds"], rejected=footer["rejected"],
                           hostUpdateOrderSurprises=host_footer["updateOrderSurprises"],
                           replicaUpdateOrderSurprises=footer["updateOrderSurprises"],
                           commandsAccepted=drive["accepted"], playerDistanceAtEnd=[round(d, 3) for d in apart],
                           allStatesMatchFloat32=True, slotsRestored=True)
            if args.loop:
                # From the key going down in the client's window to the client's player moving: on the host, then on the client's screen.
                pressed = pressed_ms[script_keys[0][1]]
                def reaction(records):
                    return min(r["observedMs"] for r in records if r["observedMs"] >= pressed and abs(world.players(r)[1]["body"][2]) > 0.01) - pressed
                summary.update(keysPressedOnHost=0, capturedSamples=capture[0]["samples"], capturedActiveSamples=capture[0]["activeSamples"],
                               localAnswersWithheld=capture[0]["withheld"], keyToHostPlayerMovesMs=reaction(host_frames.values()),
                               keyToClientSeesItMs=reaction(applied))
            if args.predict:
                # The own player ran its own update; beside every applied snapshot the replica recorded where it really stood
                # and how far that was from the host after the same command.
                own = [r["own"] for r in applied if "own" in r]
                matched = sorted(o["error"] for o in own if o["matched"])
                acks = [world.players(r)[1]["ack"] for r in applied]
                if footer["ownIndex"] != 1 or len(own) != len(applied) or len(matched) < enough or footer["predictRemembered"] < 300:
                    raise RuntimeError(f"The replica did not predict its own player: {len(matched)} matched of {len(own)}")
                if sum(a > 0 for a in acks) < enough or any(b < a for a, b in zip(acks, acks[1:])) or not capture[0]["predicts"]:
                    raise RuntimeError("The host did not acknowledge consumed commands in order, or the client module does not predict")
                # A client that entered the run stands wherever its own game put it, a door after Continue for one: the first
                # snapshot may teleport its player to the host's place. Any later teleport means prediction broke down.
                late_snaps = sum(o["mend"] == 3 for o in own[1:])
                if late_snaps or matched[-1] > 48:
                    raise RuntimeError(f"Prediction left the host by {matched[-1]} or was teleported {late_snaps} times after the first alignment")
                local = {}
                for name, begin, end, player, axis, sign in expected:
                    if player == 1:
                        rows = [r["own"]["local"] for r in applied if begin <= r["observedMs"] - start_ms <= end]
                        shift = [rows[-1][i] - rows[0][i] for i in (0, 1)]; local[name] = [round(v, 1) for v in shift]
                        if shift[axis] * sign < 25 or abs(shift[1 - axis]) > 12:
                            raise RuntimeError(f"On the client's own screen the {name}: moved by {shift}")
                def own_reaction():
                    return min(r["observedMs"] for r in applied if r["observedMs"] >= pressed and abs(r["own"]["local"][2]) > 0.01) - pressed
                summary.update(ownPlayerIndex=1, movedOnClientsOwnScreen=local, keyToOwnPlayerMovesLocallyMs=own_reaction(),
                               predictionSamples=len(own), predictionMatched=len(matched),
                               predictionErrorMean=round(sum(matched) / len(matched), 3), predictionErrorMedian=round(matched[len(matched) // 2], 3),
                               predictionErrorP95=round(matched[int(len(matched) * 0.95)], 3), predictionErrorMax=round(matched[-1], 3),
                               mendsSettled=footer["predictSettles"], mendsShifted=footer["predictShifts"], mendsSnapped=footer["predictSnaps"],
                               firstAlignment=dict(mend=("none", "settle", "shift", "snap")[own[0]["mend"]], distance=round(own[0]["error"], 1)),
                               snapsAfterFirstAlignment=late_snaps,
                               acknowledgedFrames=sum(a > 0 for a in acks), ownUpdatesRemembered=footer["predictRemembered"])
            if args.stepped:
                if not drive["stepped"] or not capture[0]["stepped"] or not drive["playoutStarts"]:
                    raise RuntimeError("The stepped input modules did not step")
                summary.update(playout=dict(starts=drive["playoutStarts"], substituted=drive["playoutSubstituted"], starved=drive["playoutStarved"],
                                            skipped=drive["playoutSkipped"], late=drive["playoutLate"]))
        except (RuntimeError, KeyError, IndexError, TypeError, ValueError) as error:
            failures.append(str(error))
    # Decided once, after the last check: a later check that fails must never leave an earlier "passed" standing.
    summary["passed"] = not failures
    with (output / f"{stage}-{stamp}.sent.jsonl").open("x", encoding="utf-8") as file:
        for frame in transmitted:
            file.write(json.dumps(frame) + "\n")
    summary_path = output / f"{stage}-{stamp}.summary.json"
    with summary_path.open("x", encoding="utf-8") as file:
        json.dump(summary, file, indent=2)
    print(json.dumps(dict(summary=str(summary_path), **summary)))
    if not summary["passed"]:
        raise SystemExit(1)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-pid", type=int)
    parser.add_argument("--replica-pid", type=int)
    parser.add_argument("--controller", type=int, default=1, help="free controller index that joins as the second player")
    parser.add_argument("--delay-ms", type=int, default=0, help="one-way delay of the path between the games, each direction")
    parser.add_argument("--jitter-ms", type=int, default=0, help="extra uniform delay per packet; reorders packets")
    parser.add_argument("--loss", type=float, default=0.0, help="percent of packets lost, each direction")
    parser.add_argument("--network-seed", type=int, default=1, help="seed of the path's random losses and jitter")
    parser.add_argument("--stepped", action="store_true",
                        help="with --predict: one command per step of the client's player on both sides (playout buffer on the host)")
    parser.add_argument("--predict", action="store_true",
                        help="closed loop with prediction: the client walks its own player at once and the host mends it (implies --loop)")
    parser.add_argument("--loop", action="store_true",
                        help="closed loop: keys go to the game of --replica-pid (the client, focused) and drive the second player of "
                             "--source-pid (the host, which must keep running without focus)")
    parser.add_argument("--check", action="store_true", help="only report whether both games share the run seed and room")
    parser.add_argument("--restart-runs", action="store_true", help="first hold R in both games to restart their seeded runs")
    parser.add_argument("--binary-directory", type=Path)
    parser.add_argument("--output-directory", type=Path)
    options = parser.parse_args()
    options.predict = options.predict or options.stepped
    options.loop = options.loop or options.predict
    if options.predict and options.controller != 1:
        parser.error("the prediction modules know the client's own player as controller 1")
    run(options)
