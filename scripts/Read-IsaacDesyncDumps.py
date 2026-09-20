"""Read-only: the game's own desync dumps (online_logs/desyncs/*/ in the user's save folder) summed up - what the official
online found different when it split a lobby. No user names or ids are read or printed.

- desync_diff.txt names the entities whose frame state differed and the field: a census by entity type and field;
- desync_rng_history.txt logs every roll of the global random number generator in the last frames with a call stack of
  absolute addresses; with the dump's 'Application Address' they become RVAs. Counted by who asked for the number, and
  named by the nearest function at or below the address in the REPENTOGON map resolved for J460
  (Binaries/research/repentogon/zhl-j460.tsv from reverse/Resolve-ZhlSignatures.py; optional) - a rough name, often only
  the neighbourhood."""
import bisect
import collections
import re
from pathlib import Path

root = Path.home() / "Documents/My Games/Binding of Isaac Repentance+/online_logs/desyncs"
names = []
tsv = Path(__file__).resolve().parents[1] / "Binaries/research/repentogon/zhl-j460.tsv"
for line in (tsv.read_text(encoding="utf-8").splitlines()[1:] if tsv.exists() else []):
    file, declaration, matches, rva = line.split("\t")
    if matches == "1":
        short = re.search(r"([A-Za-z_0-9]+::[A-Za-z_0-9~]+|\b[A-Za-z_0-9]+)\s*\(", declaration)
        names.append((int(rva, 16), short.group(1) if short else declaration[:40]))
names.sort(); starts = [n[0] for n in names]


def name(rva):
    at = bisect.bisect_right(starts, rva) - 1
    if at < 0 or rva - starts[at] > 0x3000:
        return f"?{rva:#x}"
    return f"{names[at][1]}+{rva - starts[at]:#x}"


stacks = collections.Counter(); rolls = 0; dumps = 0; kinds = collections.Counter(); fields = collections.Counter(); silent = 0
for folder in sorted(root.iterdir()) if root.exists() else []:
    history = folder / "desync_rng_history.txt"
    if history.exists():
        text = history.read_text(encoding="utf-8", errors="replace"); dumps += 1
        base = int(re.search(r"Application Address = (0x[0-9a-fA-F]+)", text).group(1), 16)
        for block in re.split(r"\*+ Generated random number \d+ on frame \d+ \*+", text)[1:]:
            frames = [int(a, 16) - base for a in re.findall(r"\((0x[0-9a-fA-F]+)\)", block)]
            frames = [f for f in frames if 0x1000 <= f < 0x700000]
            if len(frames) >= 3:
                rolls += 1; stacks[tuple(frames[1:4])] += 1
    diff = folder / "desync_diff.txt"
    if diff.exists():
        body = diff.read_text(encoding="utf-8", errors="replace"); silent += "No Entity Desyncs Detected" in body
        for kind, rest in re.findall(r"- Player 0:\s+Type: \(([0-9.]+)\)(.*)", body):
            kinds[kind.split(".")[0]] += 1
            for field in re.findall(r"([A-Za-z]+): \(", rest):
                fields[field] += 1
print(f"{dumps} dumps ({silent} without any entity named), {rolls} logged rolls of the global generator, {len(stacks)} distinct call paths")
print("entities the diffs named, by type:", dict(kinds)); print("the field that differed:", dict(fields))
by_caller = collections.Counter()
for path, n in stacks.items():
    by_caller[path[0]] += n
print("\nwho asks for the number (the frame above the generator), by count:")
for rva, n in by_caller.most_common(30):
    example = next(p for p in stacks if p[0] == rva)
    print(f"  {n:5}  {name(rva):46} <- {name(example[1]):40} <- {name(example[2])}")
