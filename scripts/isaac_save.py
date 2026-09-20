"""Repentance+ persistent game data (persistentgamedata<slot>.dat): read it, and merge several into the save a lobby shares.

The game's own online does not hand out one player's save. What nobody may meet unless everybody has unlocked it - items,
bosses, the shop's level and the coins donated to it - is settled when the lobby starts, so every participant, the host
included, plays from the same restricted save. merge() builds that save: every flag and every counter becomes the smallest
value any participant has. The source files are only read.

Layout (J460 loader, RVA 0x526f10): a 16-byte header, one word the loader stores unchecked, chunks of (type, size word,
element count, elements), the bestiary, and a closing checksum over everything from byte 16 up to itself."""
import argparse
import struct
import sys
from pathlib import Path

HEADER = b"ISAACNGSAVE09R  "
# Bytes per element, by chunk type: achievements, counters, level counters, collectibles, minibosses, bosses, challenges,
# cutscene counters, game settings, special seed counters. The size word of a chunk is not its length in bytes.
WIDTH = {1: 1, 2: 4, 3: 4, 4: 1, 5: 1, 6: 1, 7: 1, 8: 4, 9: 4, 10: 1}
NAMES = {1: "achievements", 2: "counters", 3: "levelCounters", 4: "collectibles", 5: "minibosses", 6: "bosses", 7: "challenges",
         8: "cutsceneCounters", 9: "gameSettings", 10: "specialSeedCounters"}
SETTINGS, BESTIARY = 9, 11
MASK = 0xFFFFFFFF


def _table():
    """CRC-32 table as the game builds it (RVA 0x283410): the first shift is logical, the other seven are arithmetic."""
    table = []
    for n in range(256):
        c = ((0xEDB88320 if n & 1 else 0) ^ (n >> 1)) & MASK
        for _ in range(7):
            signed = c - (1 << 32) if c & 0x80000000 else c
            c = ((0xEDB88320 if c & 1 else 0) ^ (signed >> 1)) & MASK
        table.append(c)
    return table


TABLE = _table()


def checksum(body):
    c = ~0xFEDCBA76 & MASK
    for byte in body:
        c = (c >> 8) ^ TABLE[(byte ^ c) & 0xFF]
    return ~c & MASK


def parse(data):
    """dict(word, chunks={type: (size word, [elements])}, tail): the tail is the bestiary, kept as bytes."""
    if len(data) < 24 or data[:16] != HEADER:
        raise ValueError("Not a Repentance+ persistent game data file")
    if checksum(data[16:-4]) != struct.unpack_from("<I", data, len(data) - 4)[0]:
        raise ValueError("Checksum mismatch: the file is damaged or of another game version")
    at, chunks = 20, {}
    while True:
        if at + 12 > len(data) - 4:
            raise ValueError("The chunks run past the end of the file")
        kind, size, count = struct.unpack_from("<3I", data, at)
        if kind == BESTIARY:
            break
        if kind != len(chunks) + 1 or kind not in WIDTH:
            raise ValueError(f"Unexpected chunk {kind}")
        width = WIDTH[kind]; end = at + 12 + count * width
        if end > len(data) - 4:
            raise ValueError(f"Chunk {kind} runs past the end of the file")
        chunks[kind] = (size, list(struct.unpack_from(f"<{count}{'B' if width == 1 else 'I'}", data, at + 12)))
        at = end
    if sorted(chunks) != sorted(WIDTH):
        raise ValueError("A chunk is missing")
    return dict(word=struct.unpack_from("<I", data, 16)[0], chunks=chunks, tail=bytes(data[at:-4]))


def build(save):
    body = struct.pack("<I", save["word"])
    for kind in sorted(save["chunks"]):
        size, elements = save["chunks"][kind]
        body += struct.pack("<3I", kind, size, len(elements)) + struct.pack(f"<{len(elements)}{'B' if WIDTH[kind] == 1 else 'I'}", *elements)
    body += save["tail"]
    return HEADER + body + struct.pack("<I", checksum(body))


def merge(saves):
    """The save of a lobby: element by element the smallest value any participant has. Game settings, the bestiary and the
    unchecked word are nobody's progress and come from the first save, the host's."""
    if not saves:
        raise ValueError("Nothing to merge")
    first = saves[0]; chunks = {}
    for kind, (size, elements) in first["chunks"].items():
        others = [save["chunks"][kind] for save in saves[1:]]
        if any(other[0] != size or len(other[1]) != len(elements) for other in others):
            raise ValueError(f"Chunk {kind} has a different layout in one of the saves")
        chunks[kind] = (size, list(elements) if kind == SETTINGS else [min(values) for values in zip(elements, *(other[1] for other in others))])
    return dict(word=first["word"], chunks=chunks, tail=first["tail"])


def summary(save):
    return {NAMES[kind]: dict(elements=len(elements), set=sum(1 for value in elements if value), total=sum(elements))
            for kind, (_, elements) in save["chunks"].items()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    info = commands.add_parser("info", help="what a save has unlocked, in numbers"); info.add_argument("file", type=Path)
    merged = commands.add_parser("merge", help="the save a lobby shares: the smallest of every flag and counter")
    merged.add_argument("--output", type=Path, required=True); merged.add_argument("files", type=Path, nargs="+", help="the host's save first")
    args = parser.parse_args()
    if args.command == "info":
        for name, row in summary(parse(args.file.read_bytes())).items():
            print(f"{name:20} {row['set']:4} of {row['elements']:4} set, total {row['total']}")
        return
    if args.output.exists():
        raise SystemExit("The output file exists; a save is never overwritten")
    result = merge([parse(path.read_bytes()) for path in args.files])
    data = build(result); parse(data)
    args.output.write_bytes(data)
    for name, row in summary(result).items():
        print(f"{name:20} {row['set']:4} of {row['elements']:4} set, total {row['total']}")


if __name__ == "__main__":
    sys.exit(main())
