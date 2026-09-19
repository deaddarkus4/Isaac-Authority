"""Client input on the host: commands sent over UDP must drive the real J460 player; this test presses no game key."""
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
import isaac_level as level


def module(name, filename):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(filename))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


rooms = module("room_test", "Test-IsaacRoom.py")
pair, reader = rooms.pair, rooms.reader
MANAGER_TABLE, WITH_DEVICE = 0x782950, 0x620FB0
# (name, seconds, move, shoot, send). The last phase keeps "right" as the final command and then goes silent.
SCRIPT = [("idle", 0.5, (0, 0), (0, 0), True), ("right", 0.8, (1, 0), (0, 0), True), ("rest", 0.7, (0, 0), (0, 0), True),
          ("left", 0.8, (-1, 0), (0, 0), True), ("rest2", 0.5, (0, 0), (0, 0), True), ("up", 0.5, (0, -1), (0, 0), True),
          ("down", 0.5, (0, 1), (0, 0), True), ("rest3", 0.5, (0, 0), (0, 0), True), ("shoot", 0.9, (0, 0), (1, 0), True),
          ("rest4", 0.6, (0, 0), (0, 0), True), ("right2", 0.4, (1, 0), (0, 0), True), ("silence", 1.2, (1, 0), (0, 0), False)]


def observe(process, controller=0):
    """The player driven by this controller index (Entity_Player+0x1618), plus how far every other player is from its start."""
    state = reader.sample(process.read, process.base)
    owners = {struct.unpack("<i", process.read(int(p["address"], 16) + 0x1618, 4))[0]: p for p in state["players"]}
    if controller not in owners:
        raise ValueError("No player is bound to this controller")
    player = owners[controller]
    observe.others = {index: p["position"] for index, p in owners.items() if index != controller}
    tears = [e for e in level.entities(process.read, process.base) if e["type"] == 2]
    return player["position"], player["velocity"], tears


def run(args):
    here = Path(__file__).resolve().parent; root = here.parent
    binaries = args.binary_directory or (here if (here / "installation.json").is_file() else root / "Binaries/authority-build/Release")
    names = dict(host="IsaacAuthorityInput.dll")
    if (binaries / "installation.json").is_file():
        names.update(json.loads((binaries / "installation.json").read_text(encoding="utf-8")).get("modules", {}))
    attach, dll = binaries / "IsaacAuthorityAttach.exe", binaries / names["host"]
    directory = Path(os.environ["LOCALAPPDATA"]) / "IsaacAuthority"; logs = directory / "logs"
    output = args.output_directory or (root / "Binaries/diagnostics/input" if (root / "native").is_dir() else directory / "verified-input")
    output.mkdir(parents=True, exist_ok=True)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d-%H%M%S-%f")
    before = set(logs.glob("input-*.jsonl"))
    if args.restart_run:
        rooms.restart(args.pid, "HOST"); time.sleep(2.0)
    failures, samples, started, process, control, census = [], [], False, None, None, []

    def invoke(action):
        result = subprocess.run([str(attach), action, str(args.pid), str(dll)], capture_output=True, text=True, timeout=20)
        if result.returncode:
            raise RuntimeError(f"{action}: {result.stdout} {result.stderr}")

    script = [("observe", args.observe, (0, 0), (0, 0), False)] if args.observe else SCRIPT
    if args.join:
        # A free controller index asks to join the host's run; afterwards it stays neutral while the game is watched.
        # The join opens a character choice for that controller; confirming it spawns the player.
        script = [("before", 0.5, (0, 0), (0, 0), True), ("join", 0.3, (0, 0), (0, 0), True), ("choice", 1.2, (0, 0), (0, 0), True),
                  ("menuConfirm", 0.3, (0, 0), (0, 0), True), ("after", args.join, (0, 0), (0, 0), True)]
    try:
        process = reader.WindowsProcess(args.pid)
        control = pair.HostWindow(args.pid)  # focus only: a game paused by focus loss simulates nothing
        rooms.wait_running(process, "HOST", control)
        first = reader.sample(process.read, process.base)["players"]
        if not first or tuple(process.read(int(first[0]["address"], 16) + 0x170, 4)[2:4]) != (1, 0):  # exists, not dead
            raise RuntimeError("A living first player in a run is required")
        if not args.join and not args.observe:
            observe(process, args.controller)  # the driven controller must already own a player
        invoke("input"); started = True
        endpoint = json.loads((directory / f"input-host-{args.pid}.json").read_text())
        address, sequence = ("127.0.0.1", endpoint["port"]), 0
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            for name, seconds, move, shoot, send in script:
                until = time.monotonic() + seconds
                while time.monotonic() < until:
                    control.require_focus()
                    if control.held:
                        raise RuntimeError("The test itself must not hold any key")
                    if send:
                        sequence += 1
                        udp.sendto(commands.encode(endpoint["session"], sequence, pair.uptime_ms(), args.controller, move, shoot,
                                                   (name,) if name in commands.BUTTONS else ()), address)
                    try:
                        census.append((name, len(reader.sample(process.read, process.base)["players"])))
                    except (reader.InvalidState, OSError, ValueError, struct.error):
                        pass
                    try:
                        position, velocity, tears = observe(process, 0 if args.join else args.controller)
                        samples.append(dict(phase=name, ms=pair.uptime_ms(), position=position, velocity=velocity, others=observe.others,
                                            tears=[[t["position"][0], t["velocity"][0]] for t in tears]))
                    except (reader.InvalidState, OSError, ValueError, struct.error):
                        pass
                    time.sleep(1 / 60)
    except BaseException as error:
        failures.append(str(error))
    finally:
        if started:
            try:
                invoke("input-stop")
            except BaseException as error:
                failures.append(f"Cleanup: {error}")
        if process is not None:
            try:
                pointer = struct.unpack("<I", process.read(process.base + MANAGER_TABLE + 29 * 4, 4))[0]
                if pointer != process.base + WITH_DEVICE:
                    failures.append("Input manager slot was not restored")
            except BaseException as error:
                failures.append(f"Restoration readback: {error}")
            process.close()
    footer, copied = None, []
    for file in sorted(set(logs.glob("input-*.jsonl")) - before):
        target = output / file.name
        shutil.copyfile(file, target); copied.append(str(target))
        footer = json.loads(target.read_text().splitlines()[-1])
    summary = dict(utc=datetime.datetime.now(datetime.timezone.utc).isoformat(), pid=args.pid, controller=args.controller,
                   keysPressedByTest=0, commandsSent=sequence if started else 0, reports=copied, failures=failures, passed=False)
    if args.join and not failures:
        counts = {name: sorted({n for phase, n in census if phase == name}) for name, *_ in script}
        asked = {}
        for controller, kind, action, count in (footer or {}).get("polls", []):
            if controller == args.controller:
                asked.setdefault(("value", "pressed", "triggered")[kind], {})[action] = count
        summary.update(passed=bool(footer and footer["slotRestored"]), playersSeen=counts, pollsOfJoiningController=asked,
                       triggeredReads=(footer or {}).get("triggeredReads"), accepted=(footer or {}).get("accepted"))
    elif args.observe and not failures:
        # Nothing is injected: only report what the game asks the input manager, per controller and action.
        asked = {}
        for controller, kind, action, count in (footer or {}).get("polls", []):
            asked.setdefault(str(controller), {}).setdefault(("value", "pressed", "triggered")[kind], {})[action] = count
        summary.update(passed=bool(footer and footer["slotRestored"]), observedSeconds=args.observe, polls=asked,
                       passedThrough=(footer or {}).get("passedThrough"))
    elif not failures:
        try:
            if not footer or footer["networkErrors"] or not footer["slotRestored"] or not footer["accepted"] or not footer["valueReads"]:
                raise RuntimeError("The host module accepted or injected nothing")
            by = {name: [s for s in samples if s["phase"] == name] for name, *_ in SCRIPT}
            if any(len(rows) < 5 for rows in by.values()):
                raise RuntimeError("Too few observations of the player")
            moved = {name: [rows[-1]["position"][axis] - rows[0]["position"][axis] for axis in (0, 1)] for name, rows in by.items()}
            speed = {name: max(abs(v) for v in rows[-1]["velocity"]) for name, rows in by.items()}
            expect = dict(right=(0, 1), left=(0, -1), up=(1, -1), down=(1, 1), right2=(0, 1))
            for name, (axis, sign) in expect.items():
                if moved[name][axis] * sign < 25 or abs(moved[name][1 - axis]) > 12:
                    raise RuntimeError(f"Command {name} moved the player by {moved[name]}")
            for name in ("idle", "rest", "rest2", "rest3", "rest4"):
                if speed[name] > 0.5:
                    raise RuntimeError(f"The player did not come to rest in {name}")
            fired = [t for s in by["shoot"] + by["rest4"] for t in s["tears"]]
            if not fired or any(t[1] <= 0 for t in fired) or any(s["tears"] for s in by["idle"] + by["right"]):
                raise RuntimeError("Shooting right did not produce tears flying right, or tears appeared without a command")
            # A silent client is a neutral client: the last command said "right", yet the player must stop.
            quiet = [s for s in by["silence"] if s["ms"] - by["silence"][0]["ms"] > 700]
            if not quiet or max(abs(v) for s in quiet for v in s["velocity"]) > 0.5:
                raise RuntimeError("The player kept moving on a stale command")
            # Players of other controllers belong to other people: commands for this controller must not move them.
            strayed = {str(index): max(abs(s["others"][index][axis] - samples[0]["others"][index][axis]) for s in samples if index in s["others"]
                                       for axis in (0, 1)) for index in samples[0]["others"]}
            if any(distance > 1.0 for distance in strayed.values()):
                raise RuntimeError(f"A player of another controller moved: {strayed}")
            summary.update(passed=True, otherPlayersMoved=strayed, movedByPhase={k: [round(v, 1) for v in d] for k, d in moved.items()},
                           tearsObserved=len({round(t[0]) for t in fired}), accepted=footer["accepted"], rejected=footer["rejected"],
                           valueReads=footer["valueReads"], pressedReads=footer["pressedReads"], triggeredReads=footer["triggeredReads"],
                           passedThrough=footer["passedThrough"], stoppedOnSilence=True, slotRestored=True)
        except (RuntimeError, KeyError, IndexError, TypeError) as error:
            failures.append(str(error))
    with (output / f"input-{stamp}.samples.jsonl").open("x", encoding="utf-8") as file:
        for sample in samples:
            file.write(json.dumps(sample) + "\n")
    summary_path = output / f"input-{stamp}.summary.json"
    with summary_path.open("x", encoding="utf-8") as file:
        json.dump(summary, file, indent=2)
    print(json.dumps(dict(summary=str(summary_path), **summary)))
    if not summary["passed"]:
        raise SystemExit(1)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--controller", type=int, default=0, help="controller index of the player to drive")
    parser.add_argument("--join", type=float, help="press the co-op join button for --controller, then watch for this many seconds")
    parser.add_argument("--observe", type=float, help="inject nothing for this many seconds and report what the game polls")
    parser.add_argument("--restart-run", action="store_true", help="first hold R so the player starts in the middle of the first room")
    parser.add_argument("--binary-directory", type=Path)
    parser.add_argument("--output-directory", type=Path)
    run(parser.parse_args())
