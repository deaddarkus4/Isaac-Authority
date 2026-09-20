"""Live test of the game's own online changed from inside (IsaacAuthorityNative.dll) on a running local match.

Start the games with Start-IsaacNativePair.ps1, bring them through the game's own lobby into one match (host: Online ->
Quick Match -> create; guest: Online -> Quick Match; ready in both), then:

    python Test-IsaacNative.py [--no-relay] [--delay-ms N] [--jitter-ms N] [--loss P] [--seconds S]

The module is attached to every game of the pair (each game's log tells which lobby device is its local player). A relay
reads every game's published player body and the host's published enemies and sends each new one as a PLR1 or WLN1
datagram to the other games' modules, optionally through the impaired path of isaac_link.py. Then a key is held in one game at a time - the windows take the focus - while
both games are sampled: how soon the own player moves, and how far the other side's copy of it is from the owner's."""
import argparse
import importlib.util
import json
import os
import re
import socket
import statistics
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import isaac_link as link  # noqa: E402

spec = importlib.util.spec_from_file_location("coop", Path(__file__).with_name("Test-IsaacCoop.py")); coop = importlib.util.module_from_spec(spec); spec.loader.exec_module(coop)
reader, pair = coop.reader, coop.pair
PUBLISHED = struct.Struct("<II4I4f"); STATS = struct.Struct("<17I4f"); WORLD_BYTES = 20 + 48 * 32 + 16 * 4
folder = Path(os.environ["LOCALAPPDATA"]) / "IsaacAuthority"
release = root / "Binaries/authority-build/Release"


def attach(instances):
    """Tell each game's module which lobby device is its local player - the 'Setting controller ID to N' that follows
    'Adding local player' in the game's own log - and attach it. A game that already runs the module is left as it is."""
    for instance in instances:
        pid = instance["pid"]
        log = (Path(instance["saveDirectory"]) / "log.txt").read_text(encoding="utf-8", errors="replace")
        start = log.rfind("Start Networked"); local = log.find("Adding local player", start)
        found = re.findall(r"Setting controller ID to (\d+), \(Prev: 0\)", log[local:local + 600]) if start >= 0 and local >= 0 else []
        if not found:
            raise SystemExit(f"{instance['title']}: no running match of the game's own online in its log")
        # The game that made the lobby is the host: the authority over the enemies.
        hosting = "Successfully created lobby" in log
        (folder / f"native-{pid}.cfg").write_text(found[0] + (" host" if hosting else ""), encoding="ascii")
        done = subprocess.run([str(release / "IsaacAuthorityAttach.exe"), "native", str(pid), str(release / "IsaacAuthorityNative.dll")], capture_output=True, text=True)
        result = json.loads(done.stdout)["result"] if done.stdout.strip().startswith("{") else done.stderr.strip()
        if result not in (0, 1247):   # 1247: already attached
            raise SystemExit(f"{instance['title']}: the module did not start: {result}")


parser = argparse.ArgumentParser()
parser.add_argument("--no-relay", action="store_true"); parser.add_argument("--delay-ms", type=int, default=0)
parser.add_argument("--jitter-ms", type=int, default=0); parser.add_argument("--loss", type=float, default=0.0)
parser.add_argument("--seconds", type=float, default=1.2)
args = parser.parse_args()

instances = json.loads((root / "Binaries/game-instances/native-pair.json").read_text(encoding="utf-8-sig"))
attach(instances)
folder = Path(os.environ["LOCALAPPDATA"]) / "IsaacAuthority"
games = []
for instance in instances:
    descriptor = json.loads((folder / f"native-{instance['pid']}.json").read_text())
    games.append(dict(pid=instance["pid"], title=instance["title"], descriptor=descriptor, process=reader.WindowsProcess(instance["pid"]),
                      relay=reader.WindowsProcess(instance["pid"]), last=0, lastWorld=0))
lock = threading.Lock(); stop = threading.Event(); sent = [0]


def published(game):
    blob = game["relay"].read(game["descriptor"]["published"], PUBLISHED.size)
    magic, generation, body_magic, controller, sequence, *_ = PUBLISHED.unpack(blob)
    return None if generation & 1 or magic != 0x31524C50 or not sequence else (sequence, blob[8:])


def published_world(game):
    """The host's enemies: read twice around the body, like any seqlock reader."""
    address = game["descriptor"]["publishedWorld"]
    first = struct.unpack("<II", game["relay"].read(address, 8)); blob = game["relay"].read(address + 8, WORLD_BYTES)
    last = struct.unpack("<II", game["relay"].read(address, 8)); sequence = struct.unpack_from("<I", blob, 4)[0]
    return None if first != last or first[1] & 1 or not sequence else (sequence, blob)


def relay():
    out = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    def to(port):
        def deliver(packet, tag):
            out.sendto(packet, ("127.0.0.1", port)); sent[0] += 1
        return deliver
    paths = {(a["pid"], b["pid"]): link.Link(to(b["descriptor"]["port"]), args.delay_ms, args.jitter_ms, args.loss, seed=7 + n)
             for n, (a, b) in enumerate((a, b) for a in games for b in games if a is not b)}
    while not stop.is_set():
        now = time.monotonic() * 1000
        for game in games:
            try:
                found = published(game)
            except OSError:
                continue
            if found and found[0] != game["last"]:
                game["last"] = found[0]
                for other in games:
                    if other is not game:
                        paths[(game["pid"], other["pid"])].send(found[1], now)
            if game["descriptor"]["role"] == "native-host":
                try:
                    world = published_world(game)
                except OSError:
                    world = None
                if world and world[0] != game["lastWorld"]:
                    game["lastWorld"] = world[0]
                    for other in games:
                        if other is not game:
                            paths[(game["pid"], other["pid"])].send(world[1], now)
        for path in paths.values():
            path.flush(now)
        time.sleep(0.002)


def positions():
    for _ in range(50):
        try:
            return {g["pid"]: {q["controller"]: tuple(q["position"]) for q in coop.team(g["process"])} for g in games}
        except ValueError:
            continue
    raise RuntimeError("No clean sample")


def stats(game):
    names = ("published", "received", "applied", "stale", "rejected", "otherRoom", "worldPublished", "worldReceived", "worldApplied", "npcMatched", "npcOnlyHost",
             "npcOnlyLocal", "npcKilled", "hitPointFixes", "deathsHeld", "npcPaired", "npcRemoved", "correctionSum", "correctionMax", "npcCorrectionSum", "npcCorrectionMax")
    return dict(zip(names, STATS.unpack(game["process"].read(game["descriptor"]["stats"], STATS.size))))


def walk(game, key, seconds):
    """Hold a key in one game; returns key-to-own-movement in ms and, per sample, how far the other games' copy of this
    player is from the owner's own."""
    own = game["descriptor"]["ownController"]; control = pair.HostWindow(game["pid"]); gaps = []; first = None
    try:
        time.sleep(0.4); before = positions(); start = time.perf_counter(); control.key(key, True)
        try:
            while time.perf_counter() - start < seconds:
                now = positions(); mine = now[game["pid"]][own]
                if first is None and mine != before[game["pid"]][own]:
                    first = (time.perf_counter() - start) * 1000
                for other in games:
                    if other is not game:
                        theirs = now[other["pid"]][own]; gaps.append(((mine[0] - theirs[0]) ** 2 + (mine[1] - theirs[1]) ** 2) ** 0.5)
                time.sleep(0.004)
        finally:
            control.key(key, False)
        time.sleep(1.2); end = positions()
    finally:
        control.release()
    mine = end[game["pid"]][own]; rest = max(((mine[0] - end[o["pid"]][own][0]) ** 2 + (mine[1] - end[o["pid"]][own][1]) ** 2) ** 0.5 for o in games if o is not game)
    return dict(game=game["title"], key=key, keyToOwnMovementMs=round(first) if first is not None else None,
                copyGapWhileMoving=dict(median=round(statistics.median(gaps), 2), p95=round(sorted(gaps)[int(len(gaps) * 0.95)], 2), max=round(max(gaps), 2)),
                copyGapAtRest=round(rest, 3))


label = ["idle"]; big = []


def monitor():
    """The largest single corrections and when they happened: a second reader per game, so the walk is not disturbed."""
    watch = [(g, reader.WindowsProcess(g["pid"])) for g in games]; previous = None
    while not stop.is_set():
        now = [STATS.unpack(process.read(g["descriptor"]["stats"], STATS.size)) for g, process in watch]
        if previous:
            for (g, _), a, b in zip(watch, now, previous):
                if a[17] - b[17] > 12:
                    big.append(dict(game=g["title"], during=label[0], corrections=a[2] - b[2], distance=round(a[17] - b[17], 1)))
        previous = now; time.sleep(0.02)
    for _, process in watch:
        process.close()


worker = None
if not args.no_relay:
    worker = threading.Thread(target=relay, daemon=True); worker.start(); threading.Thread(target=monitor, daemon=True).start(); time.sleep(0.5)
before = [stats(g) for g in games]
results = []
for game, key in ((games[0], "D"), (games[1], "W"), (games[0], "A"), (games[1], "S")):
    label[0] = f"{game['title'][-1]} holds {key}"; results.append(walk(game, key, args.seconds)); label[0] = "idle"
stop.set()
if worker:
    worker.join(2)
after = [stats(g) for g in games]
report = dict(relay=not args.no_relay, network=dict(delayMs=args.delay_ms, jitterMs=args.jitter_ms, lossPercent=args.loss), datagrams=sent[0], walks=results,
              modules=[{k: (round(a[k] - b[k], 2) if not k.endswith("Max") else round(a[k], 2)) for k in a} for a, b in zip(after, before)])
for w in results:
    print(w)
print('corrections above 12 px:', big)
for g, m in zip(games, report["modules"]):
    mean = m["correctionSum"] / m["applied"] if m["applied"] else 0
    print(g["title"], m, "mean correction", round(mean, 2))
output = folder / f"native-{int(time.time())}.summary.json"; output.write_text(json.dumps(report, indent=2)); print("saved", output.name)
for g in games:
    g["process"].close(); g["relay"].close()
