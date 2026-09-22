"""Live test of the game's own online changed from inside (IsaacAuthorityNative.dll) on a running local match.

Start the games with Start-IsaacNativePair.ps1, bring them through the game's own lobby into one match (host: Online ->
Quick Match -> create; guest: Online -> Quick Match; ready in both), then:

    python Test-IsaacNative.py [--no-relay] [--delay-ms N] [--jitter-ms N] [--loss P] [--seconds S]
    python Test-IsaacNative.py --play [MINUTES] [--delay-ms N ...]    a person plays; no key is pressed by the test

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
PUBLISHED = struct.Struct("<II4I")
COUNTERS = ("published", "received", "applied", "stale", "rejected", "otherRoom", "worldPublished", "worldReceived", "worldApplied", "npcMatched", "npcOnlyHost",
            "npcOnlyLocal", "npcKilled", "hitPointFixes", "deathsHeld", "npcPaired", "npcRemoved",
            "healthFixes", "copyDamageIgnored", "copyDeaths", "copyRevivalsMissed", "roomFollows", "roomFollowFailures", "stateFixes", "npcSpawned", "npcSpawnFailures",
            "clearsHeld", "clearsFromHost", "copyRevivals", "copyTouchesIgnored", "taken", "takenApplied", "takenMissed", "animationFixes", "gridHeld", "gridFixes", "gridMismatch", "fireHeld",
            "projectilesMade", "projectilesEnded", "projectilesDropped", "tearsSent", "tearsMade", "tearsEnded", "tearsDropped", "fireHolds",
            "dropsMade", "dropsRemoved", "dropsMorphed", "dropsSkipped", "counterFixes", "doorFixes", "doorMismatch", "bombsMade", "bombsEnded", "bombsDropped",
            "enemyBombsMade", "enemyBombsEnded", "enemyBombsDropped", "hurtTaken", "otherFloor",
            "slotFixes", "slotsMade", "slotTouchesSent", "slotTouchesPlayed", "slotTouchesIgnored",
            "npcPartsLeft", "petsSent", "petsSet", "petFireHolds", "gridBorn", "gridRemoved", "longFrames", "frameMaxMs", "bytesSent", "bytesReceived", "framesBroken", "copyHitsPlayed",
            "gameFaults", "faultRules", "floorFollows", "floorFollowFailures", "floorsElsewhere",
            "npcFaults", "ownBehaviourKinds", "needleLeaps",
            "inputsHeard", "inputsPlayed", "inputsRepeated", "inputsSkipped", "inputsStale", "inputsLate", "inputsMisread",
            "joinsAdmitted", "joinsRefused", "joinsUnjudged", "joinLacking",
            "roomFollowsRefused", "dealsHeld", "dealDoorsMade", "dealDoorFailures", "dealElsewhere", "dealOtherKind",
            "blowsSent", "blowsPlayed", "blowsMissed", "blowsLost", "copyBlowsIgnored",
            "worldsLate", "worldsRebased", "worldCushion",
            "visibleFixes", "flameFixes", "deathsHeldAtUpdate", "copyGhostRevivals", "takenItemFixes", "joinsHeld",
            "ownTouchesHeld", "takenNoRecord", "takenElsewhere", "takenNoTwin", "dropsNotMadeAsTaken", "npcPairsDropped",
            "senderFramePlayed", "ownFrameThen",
            "summonsHeld", "summonsPaired", "summonsRemoved", "pedestalsTakenTwice")   # rejected: datagrams refused; gameFaults: exceptions on the game's thread - FAULTS; faultRules: the rules switched off after them
# The module's rules, each of which can be left out (--without): the same bits as in native_adapter.cpp.
RULES = dict(follow=1, behaviour=2, clear=4, taken=8, grid=16, fire=32, projectiles=64, tears=128, drops=256, counters=512, doors=1024, traps=2048, bombs=4096, hurt=8192, slots=16384, pets=32768, lead=65536, look=131072, join=262144, gate=524288, deal=1048576, hits=2097152, steady=4194304, maze=8388608, summons=16777216)
STATS = struct.Struct(f"<{len(COUNTERS)}I4f"); SUM = len(COUNTERS)   # four floats follow: correctionSum, correctionMax, npcCorrectionSum, npcCorrectionMax
folder = Path(os.environ["LOCALAPPDATA"]) / "IsaacAuthority"
FIRST_PORT = 27460   # the local pair: one port per game
release = root / "Binaries/authority-build/Release"


def attach(instances, mask, restart, carried, auto, installed, cheats):
    """Tell each game's module which lobby device is its local player - the 'Setting controller ID to N' that follows
    'Adding local player' in the game's own log - and attach it. A game that already runs the module is left as it is.

    A game that is in the lobby of a match it has not been let into has no such line at all: it still gets its cfg and its
    place among the ports, so that its module starts by itself if the others ever let it in, and nothing is read from it
    meanwhile. Returns those games' pids. Only the module that follows the log by itself (--auto, --installed) can be told
    this little; told the old way it would need the device number the log does not have yet."""
    ports = {instance["pid"]: FIRST_PORT + n for n, instance in enumerate(instances)}
    waiting = set(); seen = {}
    for instance in instances:
        log = (Path(instance["saveDirectory"]) / "log.txt").read_text(encoding="utf-8", errors="replace")
        start = log.rfind("Start Networked"); local = log.find("Adding local player", start)
        found = re.findall(r"Setting controller ID to (\d+), \(Prev: 0\)", log[local:local + 600]) if start >= 0 and local >= 0 else []
        if not found:
            # Told apart by the match itself, not by the device number: a game whose "Start Networked" is there has been
            # let into the match, and its number is only late in the log (the game writes it a moment after). Taking that
            # for a game sitting in the lobby would drop a real player out of everybody's neighbours and out of every
            # measurement, silently. A game that never was in this match has no such line at all - that one waits.
            if start >= 0:
                raise SystemExit(f"{instance['title']}: the match is in its log, but the device number is not there yet - run this again in a moment")
            if not (auto or installed):
                raise SystemExit(f"{instance['title']}: no running match of the game's own online in its log")
            waiting.add(instance["pid"])
        # The game that made the lobby is the host: the authority over the enemies.
        seen[instance["pid"]] = (found[0] if found else None, "Successfully created lobby" in log)
    # Only the games of the match are neighbours. A game waiting in the lobby answers no hello, and a module that counted
    # it among its neighbours would wait for that hello to the end of the match: on the stand of four every module of the
    # three playing said "neighbours answered 2 of 3" and none of them ever went live. It is given the others' addresses
    # all the same - they cost it nothing and it will need them - but that alone does not make it heard: a module takes
    # datagrams from named neighbours only, and the three playing are not told of it. So a newcomer that is really let in
    # is a run of this script again, with it among the playing games, and not a matter of the addresses handed out now.
    playing = [pid for pid in ports if pid not in waiting]
    for instance in instances:
        pid = instance["pid"]
        found, hosting = seen[pid]
        # The modules send to each other by themselves (the way two machines would) unless a process outside is to carry the slots.
        network = "" if carried else f" listen={ports[pid]} " + " ".join(f"peer=127.0.0.1:{ports[other]}" for other in playing if other != pid)
        # Installed or --auto: the module reads the game's log by itself - device, host and the end of the match; it is told
        # only that this test instance may be touched, the rules, and the way to the neighbours (no Steam here).
        # --cheats: the tester's help of the module, in every game of the stand (no damage to its own player, twentyfold
        # damage to enemies; F6 and F7 turn them off and on in whichever window has the focus, F8 kills the room's enemies
        # on the host, whose enemies they are). A guest's blow is told to the host and played there with the damage as this
        # game asked for it - past the hook, so the twentyfold is counted once, not twice. The module takes the word in an
        # isolated test instance only.
        network += " cheats" if cheats else ""
        (folder / f"native-{pid}.cfg").write_text(f"auto {mask:x}{network}" if auto or installed else f"{found} {'host' if hosting else 'guest'} {mask:x}{network}", encoding="ascii")
        if installed:   # the game has loaded the module itself, through the version.dll beside it
            continue
        if restart:   # the same build, other rules: stop the running module first
            subprocess.run([str(release / "IsaacAuthorityAttach.exe"), "native-stop", str(pid), str(release / "IsaacAuthorityNative.dll")], capture_output=True, text=True)
        done = subprocess.run([str(release / "IsaacAuthorityAttach.exe"), "native-auto" if auto else "native", str(pid), str(release / "IsaacAuthorityNative.dll")], capture_output=True, text=True)
        result = json.loads(done.stdout)["result"] if done.stdout.strip().startswith("{") else done.stderr.strip()
        if result not in (0, 1247):   # 1247: already attached
            raise SystemExit(f"{instance['title']}: the module did not start: {result}")
    return waiting


parser = argparse.ArgumentParser()
parser.add_argument("--no-relay", action="store_true"); parser.add_argument("--delay-ms", type=int, default=0)
parser.add_argument("--carried", action="store_true", help="carry the published slots between the games from outside (the link with delay, jitter and loss) instead of letting the modules send to each other")
parser.add_argument("--auto", action="store_true", help="start the module the installed way: it follows the game's log and starts and stops itself per match")
parser.add_argument("--installed", action="store_true", help="attach nothing: the games have loaded the module themselves (version.dll beside the game); only tell it that it may run here")
parser.add_argument("--jitter-ms", type=int, default=0); parser.add_argument("--loss", type=float, default=0.0)
parser.add_argument("--seconds", type=float, default=1.2)
parser.add_argument("--without", default="", metavar="RULES", help="comma-separated rules to leave out: " + ", ".join(RULES))
# The word goes to every game of the stand by default, and what it allows starts off in all of them: the tester turns it
# on with F6/F7 in whichever window he plays in. --no-cheats leaves the word out altogether, for a run that is to be honest.
parser.add_argument("--cheats", action=argparse.BooleanOptionalAction, default=True,
                    help="the tester's help in every game of the stand: F6 makes the own player take no damage, F7 makes enemies take twentyfold, F8 kills the room's enemies (on the host). Every window is turned on and off by itself, and all of it starts off; the module is given the word in every game by default")
parser.add_argument("--restart", action="store_true", help="stop the module in each game first, so that it starts again with these rules")
parser.add_argument("--play", type=float, nargs="?", const=120.0, default=None, metavar="MINUTES",
                    help="only attach and relay while a person plays; a line of state every ten seconds, until the games close")
args = parser.parse_args()

instances = json.loads((root / "Binaries/game-instances/native-pair.json").read_text(encoding="utf-8-sig"))
left_out = [name for name in args.without.split(",") if name]
if any(name not in RULES for name in left_out):
    raise SystemExit("unknown rule; known: " + ", ".join(RULES))
waiting = attach(instances, 0x1FFFFFF & ~sum(RULES[name] for name in left_out), args.restart, args.carried, args.auto, args.installed, args.cheats)
folder = Path(os.environ["LOCALAPPDATA"]) / "IsaacAuthority"
games = []
for instance in instances:
    if instance["pid"] in waiting:
        print(f"{instance['title']}: in the lobby, not in this match - its cfg is written, nothing is read from it", flush=True)
        continue
    described = folder / f"native-{instance['pid']}.json"; waited = time.monotonic()
    while (args.auto or args.installed) and not described.exists() and time.monotonic() - waited < 20:   # the module starts itself some two seconds into the match
        time.sleep(0.25)
    descriptor = json.loads(described.read_text())
    games.append(dict(pid=instance["pid"], title=instance["title"], descriptor=descriptor, process=reader.WindowsProcess(instance["pid"]),
                      relay=reader.WindowsProcess(instance["pid"]), last=0, lastWorld=0))
lock = threading.Lock(); stop = threading.Event(); sent = [0]


def published(game):
    blob = game["relay"].read(game["descriptor"]["published"], game["descriptor"]["publishedBytes"])
    magic, generation, body_magic, controller, sequence, _ = PUBLISHED.unpack_from(blob)
    return None if generation & 1 or magic != 0x31524C50 or not sequence else (sequence, blob[8:])


def published_world(game):
    """The host's enemies: read twice around the body, like any seqlock reader."""
    address = game["descriptor"]["publishedWorld"]
    first = struct.unpack("<II", game["relay"].read(address, 8)); blob = game["relay"].read(address + 8, game["descriptor"]["publishedWorldBytes"] - 8)
    last = struct.unpack("<II", game["relay"].read(address, 8)); sequence = struct.unpack_from("<I", blob, 4)[0]
    return None if first != last or first[1] & 1 or not sequence else (sequence, blob)


def published_shots(game):
    """A player's own tears: the same seqlock reading; nothing from a module that does not publish them."""
    address = game["descriptor"].get("publishedShots")
    if not address:
        return None
    first = struct.unpack("<II", game["relay"].read(address, 8)); blob = game["relay"].read(address + 8, game["descriptor"]["publishedShotsBytes"] - 8)
    last = struct.unpack("<II", game["relay"].read(address, 8)); sequence = struct.unpack_from("<I", blob, 8)[0]
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
            try:
                shots = published_shots(game)
            except OSError:
                shots = None
            if shots and shots[0] != game.get("lastShots"):
                game["lastShots"] = shots[0]
                for other in games:
                    if other is not game:
                        paths[(game["pid"], other["pid"])].send(shots[1], now)
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


def room_of(game):
    """The floor and the room a game is in: a copy's gap means something only between games of one room."""
    u32 = lambda a: struct.unpack("<I", game["process"].read(a, 4))[0]
    at = u32(game["process"].base + reader.GAME_RVA)
    return u32(at), u32(at + 4), u32(at + 0x18304), u32(at + 0x1830c)


def stats(game):
    if game["descriptor"]["statsBytes"] != STATS.size:
        raise RuntimeError(f"The module counts {game['descriptor']['statsBytes']} bytes of statistics, this script expects {STATS.size}")
    names = COUNTERS + ("correctionSum", "correctionMax", "npcCorrectionSum", "npcCorrectionMax")
    return dict(zip(names, STATS.unpack(game["process"].read(game["descriptor"]["stats"], STATS.size))))


def walk(game, key, seconds):
    """Hold a key in one game; returns key-to-own-movement in ms and, per sample, how far the other games' copy of this
    player is from the owner's own."""
    own = game["descriptor"]["ownController"]; control = pair.HostWindow(game["pid"]); gaps = []; first = None; apart = 0
    try:
        time.sleep(0.4); before = positions(); start = time.perf_counter(); control.key(key, True)
        try:
            while time.perf_counter() - start < seconds:
                # The held key can walk the player through a door, and the games change room some frames apart: a sample
                # taken then measures the two rooms' doors, not the copy (274 px in the first live run of 0.1.3). The rooms
                # are read on both sides of the places, so a change in between is seen as well.
                rooms = [room_of(g) for g in games]; now = positions(); rooms += [room_of(g) for g in games]; mine = now[game["pid"]][own]
                if first is None and mine != before[game["pid"]][own]:
                    first = (time.perf_counter() - start) * 1000
                if len(set(rooms)) != 1:
                    apart += 1
                else:
                    for other in games:
                        if other is not game:
                            theirs = now[other["pid"]][own]; gaps.append(((mine[0] - theirs[0]) ** 2 + (mine[1] - theirs[1]) ** 2) ** 0.5)
                time.sleep(0.004)
        finally:
            control.key(key, False)
        time.sleep(1.2); end = positions(); together = len({room_of(g) for g in games}) == 1
    finally:
        control.release()
    mine = end[game["pid"]][own]; rest = max(((mine[0] - end[o["pid"]][own][0]) ** 2 + (mine[1] - end[o["pid"]][own][1]) ** 2) ** 0.5 for o in games if o is not game)
    return dict(game=game["title"], key=key, keyToOwnMovementMs=round(first) if first is not None else None,
                copyGapWhileMoving=dict(median=round(statistics.median(gaps), 2), p95=round(sorted(gaps)[int(len(gaps) * 0.95)], 2), max=round(max(gaps), 2)) if gaps else None,
                samplesInOtherRooms=apart, copyGapAtRest=round(rest, 3) if together else None)


label = ["idle"]; big = []


def monitor():
    """The largest single corrections and when they happened: a second reader per game, so the walk is not disturbed."""
    watch = [(g, reader.WindowsProcess(g["pid"])) for g in games]; previous = None
    while not stop.is_set():
        now = [STATS.unpack(process.read(g["descriptor"]["stats"], STATS.size)) for g, process in watch]
        if previous:
            for (g, _), a, b in zip(watch, now, previous):
                if a[SUM] - b[SUM] > 12:
                    big.append(dict(game=g["title"], during=label[0], corrections=a[2] - b[2], distance=round(a[SUM] - b[SUM], 1)))
        previous = now; time.sleep(0.02)
    for _, process in watch:
        process.close()


worker = None
if args.carried and not args.no_relay:
    worker = threading.Thread(target=relay, daemon=True); worker.start(); threading.Thread(target=monitor, daemon=True).start(); time.sleep(0.5)
before = [stats(g) for g in games]
results = []
if args.play is not None:
    # No window is touched here: the relay and the modules run, the person plays in whichever window has the focus.
    deadline = time.monotonic() + args.play * 60; previous = before
    print(f"playing: relay on for {args.play:g} min or until a game closes (Ctrl+C ends it too)", flush=True)
    try:
        while time.monotonic() < deadline:
            time.sleep(10)
            now = [stats(g) for g in games]; line = []
            for g, a, b in zip(games, now, previous):
                line.append(f"{g['title'][-1]}: FAULTS {a['gameFaults']}{(' (rules switched off: %x)' % a['faultRules']) if a['faultRules'] else ''}, ENEMY UPDATES THROWN {a['npcFaults']} (kinds left to themselves {a['ownBehaviourKinds']}), needle leaps set up {a['needleLeaps']}, TWINS shown or hidden {a['visibleFixes']} / deaths held at the update {a['deathsHeldAtUpdate']} / flames sized {a['flameFixes']}, BLOWS told {a['blowsSent']} / played {a['blowsPlayed']} / no target {a['blowsMissed']} / never came {a['blowsLost']} / of copies ignored {a['copyBlowsIgnored']}, DEAL rolls held {a['dealsHeld']} / doors made {a['dealDoorsMade']} (failed {a['dealDoorFailures']}, elsewhere {a['dealElsewhere']}, other kind {a['dealOtherKind']}), ROOMS NOT FOLLOWED INTO {a['roomFollowsRefused']}, WORLDS dropped as overtaken {a['worldsLate'] - b['worldsLate']} of {a['worldReceived'] - b['worldReceived']} (cushion {a['worldCushion']} frames, clock anew {a['worldsRebased']}), REMOTE INPUT frames that did not wait {a['inputsRepeated'] - b['inputsRepeated']} of {a['inputsRepeated'] + a['inputsPlayed'] - b['inputsRepeated'] - b['inputsPlayed']} (heard {a['inputsHeard'] - b['inputsHeard']}, too late {a['inputsStale'] - b['inputsStale']}, jumped {a['inputsSkipped'] - b['inputsSkipped']}, late let through {a['inputsLate'] - b['inputsLate']}, MISREAD {a['inputsMisread']}), newcomers let in {a['joinsAdmitted']} / refused {a['joinsRefused']} (lacking {a['joinLacking']}) / not judged {a['joinsUnjudged']} / HELD IN THE LOBBY {a['joinsHeld']}, datagrams refused {a['rejected']}, sent {(a['bytesSent'] - b['bytesSent']) / 10240:.1f} KB/s, received {(a['bytesReceived'] - b['bytesReceived']) / 10240:.1f} KB/s, frames lost in pieces {a['framesBroken']}, frames over 45 ms {a['longFrames'] - b['longFrames']} of {a['published'] - b['published']} (longest ever {a['frameMaxMs']} ms), bodies {a['applied'] - b['applied']} (max fix {a['correctionMax']:.1f}), enemies matched {a['npcMatched'] - b['npcMatched']}, pairings let go {a['npcPairsDropped']}, "
                            f"paired {a['npcPaired']}, only host {a['npcOnlyHost'] - b['npcOnlyHost']}, removed {a['npcRemoved']}, killed {a['npcKilled']}, held {a['deathsHeld']}, other room {a['otherRoom'] - b['otherRoom']}, OTHER FLOOR {a['otherFloor'] - b['otherFloor']}, floors followed {a['floorFollows']} (failed {a['floorFollowFailures']}, arrived elsewhere {a['floorsElsewhere']}); health fixes {a['healthFixes'] - b['healthFixes']}, blows to copies ignored {a['copyDamageIgnored']}, "
                            f"copy deaths {a['copyDeaths']}, revived {a['copyRevivals']} (from the ghost {a['copyGhostRevivals']}), revivals missed {a['copyRevivalsMissed'] - b['copyRevivalsMissed']}, taken {a['taken']} / applied from others {a['takenApplied']} / missed {a['takenMissed']} (no record {a['takenNoRecord']}, elsewhere {a['takenElsewhere']}, nothing to take {a['takenNoTwin']}; own touches held {a['ownTouchesHeld']}, host's pickups not made as taken {a['dropsNotMadeAsTaken']}) (pedestals given the owner's item first {a['takenItemFixes']}; copies' touches ignored {a['copyTouchesIgnored']}), rooms followed {a['roomFollows']} (failed {a['roomFollowFailures']}), behaviour fixes {a['stateFixes'] - b['stateFixes']}, animations started {a['animationFixes'] - b['animationFixes']}, enemies created {a['npcSpawned']} (failed {a['npcSpawnFailures']}), clears held {a['clearsHeld'] - b['clearsHeld']}, clears from host {a['clearsFromHost']}, grid held {a['gridHeld']} / set from host {a['gridFixes']} / mismatch {a['gridMismatch'] - b['gridMismatch']} / cells made {a['gridBorn']} / removed {a['gridRemoved']}, blows to fireplaces held {a['fireHeld']}; projectiles made {a['projectilesMade']} / ended {a['projectilesEnded']} / own dropped {a['projectilesDropped']}, tears sent {a['tearsSent'] - b['tearsSent']} / made {a['tearsMade']} / ended {a['tearsEnded']} / copy's dropped {a['tearsDropped']}, fire holds {a['fireHolds'] - b['fireHolds']}; pickups made {a['dropsMade']} / removed {a['dropsRemoved']} / items set {a['dropsMorphed']} / skipped {a['dropsSkipped']}, frames: host {a['senderFramePlayed']} played at own {a['ownFrameThen']}, counter fixes {a['counterFixes']}, door fixes {a['doorFixes']} (mismatch {a['doorMismatch'] - b['doorMismatch']}), bombs made {a['bombsMade']} / set off {a['bombsEnded']} / copy's dropped {a['bombsDropped']}, enemy bombs made {a['enemyBombsMade']} / set off {a['enemyBombsEnded']} / own dropped {a['enemyBombsDropped']}, red heart damage learnt {a['hurtTaken']}, machines set {a['slotFixes']} / made {a['slotsMade']}, touches sent {a['slotTouchesSent']} / played {a['slotTouchesPlayed']} / of copies ignored {a['slotTouchesIgnored']}; pets sent {a['petsSent'] - b['petsSent']} / placed {a['petsSet'] - b['petsSet']} / fire holds {a['petFireHolds'] - b['petFireHolds']}, parts of enemies left alone {a['npcPartsLeft'] - b['npcPartsLeft']}, copies' hits played {a['copyHitsPlayed'] - b['copyHitsPlayed']}")
            print(time.strftime('%H:%M:%S'), ' | '.join(line), flush=True); previous = now
    except (OSError, KeyboardInterrupt) as error:
        print('stopped:', type(error).__name__, error, flush=True)
    games_for_walks = ()
elif len(games) < 2:
    raise SystemExit("the walks need two games of the match; with one, play in it yourself (--play)")
else:
    games_for_walks = ((games[0], "D"), (games[1], "W"), (games[0], "A"), (games[1], "S"))
for game, key in games_for_walks:
    label[0] = f"{game['title'][-1]} holds {key}"; results.append(walk(game, key, args.seconds)); label[0] = "idle"
stop.set()
if worker:
    worker.join(2)
try:
    after = [stats(g) for g in games]
except OSError:
    after = before   # a game was closed
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
