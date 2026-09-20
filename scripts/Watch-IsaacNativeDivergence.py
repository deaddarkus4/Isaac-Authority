"""Read-only: watch two games of one match and say where their worlds differ, so that a new kind of divergence shows up in a
log before it shows up on the screen. Nothing is attached, written or pressed.

Once a second the current room of each game is read - enemies, pickups, the breakable grid and doors, the team's counters,
every player's hearts - and compared by what identifies a thing in both games (the seed of an entity, the index of a grid
cell, the controller of a player). A difference is reported when it has stayed the same for --seconds (three by default):
the guest sees the host's world a few frames late, and what settles by itself is not a divergence. While the games are in
different rooms only that is said.

Watch-IsaacNativeDivergence.py [--pids HOST GUEST] [--seconds 3] [--minutes 180] [--log FILE]
Without --pids the games come from Binaries/game-instances/native-pair.json (the first is the host)."""
import argparse
import importlib.util
import json
import struct
import sys
import time
from pathlib import Path

scripts = Path(__file__).resolve().parent; root = scripts.parent; sys.path.insert(0, str(scripts))
import isaac_level as level
spec = importlib.util.spec_from_file_location("state_reader", scripts / "Read-IsaacState.py"); reader = importlib.util.module_from_spec(spec); spec.loader.exec_module(reader)

NPC, PICKUP, PLAYER, SLOT = 0x767468, 0x767f24, 0x76bdd0, 0x764c50
GRID = {0x768738: "rock", 0x768648: "poop", 0x769300: "tnt", 0x769558: "web", 0x768698: "door"}
HEARTS = (0x1340, 0x1344, 0x1348, 0x134c, 0x1350, 0x1d88, 0x1da4, 0x194c)


def snapshot(process):
    u32 = lambda a: struct.unpack("<I", process.read(a, 4))[0]
    game = u32(process.base + level.GAME_RVA); room = u32(game + 0x18300); state = dict(room=u32(game + 0x18304), enemies={}, pickups={}, slots={}, grid={}, players={}, counters=None)
    if not room:
        return state
    descriptor = u32(room + 4); state["clear"] = u32(descriptor + 0x44) & 1 if descriptor else None
    data, count = u32(room + 0x125c), u32(room + 0x1264)
    for n in range(min(count, 700)):
        entity = u32(data + n * 4); blob = process.read(entity, 0x548); table = struct.unpack_from("<I", blob)[0] - process.base
        if not blob[0x172] or blob[0x173]:
            continue
        kind, variant, subtype = struct.unpack_from("<3I", blob, 0x28); seed = struct.unpack_from("<I", blob, 0x3ec)[0]; x, y = struct.unpack_from("<2f", blob, 0x33c)
        if table == NPC:
            state["enemies"][seed] = (f"{kind}.{variant}.{subtype}", round(struct.unpack_from("<f", blob, 0x380)[0], 1), round(x / 40), round(y / 40))
        elif table == PICKUP:
            state["pickups"][seed] = (f"5.{variant}.{subtype}", struct.unpack_from("<i", blob, 0x534)[0])
        elif table == SLOT:
            state["slots"][seed] = (f"6.{variant}.{subtype}", struct.unpack_from("<i", blob, 0x410)[0])
    for index, cell in enumerate(struct.unpack("<448I", process.read(room + 0x24, 448 * 4))):
        if cell:
            blob = process.read(cell, 0x394); name = GRID.get(struct.unpack_from("<I", blob)[0] - process.base)
            if name:
                kind, variant, value = struct.unpack_from("<3i", blob, 4); state["grid"][index] = (name, kind, variant if name == "door" else 0, value, blob[0x391] if name == "door" else 0)
    first, last = u32(game + 0x1BAA8), u32(game + 0x1BAAC)
    for n in range(min(8, (last - first) // 4)):
        player = u32(first + n * 4); blob = process.read(player, 0x1dac); controller = struct.unpack_from("<i", blob, 0x1618)[0]
        state["players"][controller] = tuple(struct.unpack_from("<i", blob, o)[0] for o in HEARTS)
        state["counters"] = struct.unpack_from("<i", blob, 0x1368)[0], struct.unpack_from("<i", blob, 0x1364)[0], struct.unpack_from("<i", blob, 0x135c)[0]
    return state


def differences(host, guest):
    """A set of short statements; equal statements across checks are what counts as lasting."""
    if host["room"] != guest["room"]:
        return {f"rooms: host in {host['room']}, guest in {guest['room']}"}
    found = set()
    if host.get("clear") != guest.get("clear"):
        found.add(f"room clear: host {host.get('clear')}, guest {guest.get('clear')}")
    if host["counters"] != guest["counters"]:
        found.add(f"coins/bombs/keys: host {host['counters']}, guest {guest['counters']}")
    for controller in sorted(set(host["players"]) | set(guest["players"])):
        if host["players"].get(controller) != guest["players"].get(controller):
            found.add(f"player {controller} hearts: host {host['players'].get(controller)}, guest {guest['players'].get(controller)}")
    for title, key in (("enemies", "enemies"), ("pickups", "pickups"), ("slots", "slots")):
        a, b = host[key], guest[key]
        only_host = sorted(a[s][0] for s in a if s not in b); only_guest = sorted(b[s][0] for s in b if s not in a)
        if only_host or only_guest:
            found.add(f"{title}: only at host {only_host[:8]}, only at guest {only_guest[:8]}")
        for seed in a.keys() & b.keys():
            if key == "enemies" and (a[seed][0] != b[seed][0] or abs(a[seed][1] - b[seed][1]) > 0.05):
                found.add(f"enemy {a[seed][0]}: hit points host {a[seed][1]}, guest {b[seed][1]}" if a[seed][0] == b[seed][0] else f"enemy of one seed: host {a[seed][0]}, guest {b[seed][0]}")
            elif key != "enemies" and a[seed] != b[seed]:
                found.add(f"{title[:-1]} of one seed: host {a[seed]}, guest {b[seed]}")
    for index in sorted(set(host["grid"]) | set(guest["grid"])):
        if host["grid"].get(index) != guest["grid"].get(index):
            found.add(f"grid cell {index}: host {host['grid'].get(index)}, guest {guest['grid'].get(index)}")
    return found


parser = argparse.ArgumentParser()
parser.add_argument("--pids", type=int, nargs=2, metavar=("HOST", "GUEST")); parser.add_argument("--seconds", type=int, default=3)
parser.add_argument("--minutes", type=float, default=180.0); parser.add_argument("--log", type=Path)
args = parser.parse_args()
pids = args.pids or [g["pid"] for g in json.loads((root / "Binaries/game-instances/native-pair.json").read_text(encoding="utf-8-sig"))[:2]]
processes = [reader.WindowsProcess(pid) for pid in pids]; lasting = {}; said = set(); checks = 0; end = time.time() + args.minutes * 60


def say(text):
    line = time.strftime("%H:%M:%S ") + text
    print(line, flush=True)
    if args.log:
        with args.log.open("a", encoding="utf-8") as handle:
            handle.write(line + "\n")


say(f"watching games {pids[0]} (host) and {pids[1]} (guest); a difference is said when it has lasted {args.seconds} s")
try:
    while time.time() < end:
        try:
            now = differences(snapshot(processes[0]), snapshot(processes[1])); checks += 1
        except OSError:
            say("a game is gone"); break
        except Exception as error:   # a list that moved under the read: the next second is another try
            now = None
        if now is not None:
            lasting = {text: lasting.get(text, 0) + 1 for text in now}
            for text, age in lasting.items():
                if age == args.seconds and text not in said:
                    said.add(text); say("DIFFERS " + text)
            for text in [t for t in said if t not in now]:
                said.discard(text); say("settled " + text)
        time.sleep(1.0)
finally:
    for process in processes:
        process.close()
    say(f"{checks} checks")
