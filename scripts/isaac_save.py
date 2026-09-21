"""Repentance+ persistent game data (persistentgamedata<slot>.dat): read it, and merge several into the save a lobby shares.

The game's own online does not hand out one player's save. When the host starts the match it builds a shared save out of the
saves every lobby member reported (J460, RVA 0x51a450) and every client loads it over its own for the match: a flag is set
only if every member has it, a counter is the smallest any member has, a bestiary entry survives only if every member has it.
merge() is that rule. The source files are only read.

File layout (loader RVA 0x526f10): a 16-byte header, one word the loader stores unchecked, chunks of (type, size word,
element count, elements), the bestiary (chunk 11: four maps of key and value), a trailing word, and a closing checksum over
everything from byte 16 up to itself."""
import argparse
import struct
import sys
from pathlib import Path

HEADER = b"ISAACNGSAVE09R  "
# Bytes per element, by chunk type. The size word of a chunk is not its length in bytes: it is four times the element
# count for every chunk but the first, and for the bestiary four times the number of entries.
WIDTH = {1: 1, 2: 4, 3: 4, 4: 1, 5: 1, 6: 1, 7: 1, 8: 4, 9: 4, 10: 1}
NAMES = {1: "achievements", 2: "counters", 3: "levelCounters", 4: "collectibles", 5: "minibosses", 6: "bosses", 7: "challenges",
         8: "cutsceneCounters", 9: "gameSettings", 10: "specialSeedCounters"}
CUTSCENES, SETTINGS, SEEDS, BESTIARY = 8, 9, 10, 11
# What the game's shared save carries, in the order of its wire form (writer RVA 0x51b130, reader RVA 0x51adf0). Flags are
# packed one bit each, least significant bit first, into (count >> 3) + 1 bytes (packer RVA 0x51b580) - one byte more than
# needed when the count is a multiple of eight, as with the 104 bosses. Game settings, special seed counters and bestiary
# maps 3 and 4 are not part of it: in the game's online every player keeps their own.
SHARED_ORDER = (1, 2, 4, 3, 5, 6, 8, 7)
SHARED_MAPS = (1, 2)
SHARED_COUNTS = {1: 642, 2: 523, 3: 14, 4: 733, 5: 7, 6: 104, 7: 46, 8: 27}
SHARED_FIXED = sum((count >> 3) + 1 if WIDTH[kind] == 1 else count * 4 for kind, count in SHARED_COUNTS.items())  # 2450 bytes
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
    """dict(word, chunks={type: (size word, [elements])}, bestiary={map: [(key, value), ...]}, trailer)."""
    if len(data) < 24 or data[:16] != HEADER:
        raise ValueError("Not a Repentance+ persistent game data file")
    if checksum(data[16:-4]) != struct.unpack_from("<I", data, len(data) - 4)[0]:
        raise ValueError("Checksum mismatch: the file is damaged or of another game version")
    end, at, chunks = len(data) - 4, 20, {}
    while True:
        if at + 12 > end:
            raise ValueError("The chunks run past the end of the file")
        kind, size, count = struct.unpack_from("<3I", data, at)
        if kind == BESTIARY:
            break
        if kind != len(chunks) + 1 or kind not in WIDTH:
            raise ValueError(f"Unexpected chunk {kind}")
        width = WIDTH[kind]; stop = at + 12 + count * width
        if stop > end:
            raise ValueError(f"Chunk {kind} runs past the end of the file")
        chunks[kind] = (size, list(struct.unpack_from(f"<{count}{'B' if width == 1 else 'I'}", data, at + 12)))
        at = stop
    if sorted(chunks) != sorted(WIDTH):
        raise ValueError("A chunk is missing")
    at += 12; bestiary = {}
    for _ in range(count):
        if at + 8 > end:
            raise ValueError("The bestiary runs past the end of the file")
        which, words = struct.unpack_from("<2I", data, at); entries = words // 4; at += 8
        if which in bestiary or not 1 <= which <= 4 or words % 4 or at + entries * 8 > end:
            raise ValueError("Unexpected bestiary map")
        bestiary[which] = [struct.unpack_from("<iI", data, at + n * 8) for n in range(entries)]; at += entries * 8
    if sorted(bestiary) != [1, 2, 3, 4] or size != 4 * sum(len(entries) for entries in bestiary.values()) or end - at != 4:
        raise ValueError("Unexpected bestiary layout")
    return dict(word=struct.unpack_from("<I", data, 16)[0], chunks=chunks, bestiary=bestiary, trailer=bytes(data[at:end]))


def build(save):
    body = struct.pack("<I", save["word"])
    for kind in sorted(save["chunks"]):
        size, elements = save["chunks"][kind]
        body += struct.pack("<3I", kind, size, len(elements)) + struct.pack(f"<{len(elements)}{'B' if WIDTH[kind] == 1 else 'I'}", *elements)
    body += struct.pack("<3I", BESTIARY, 4 * sum(len(entries) for entries in save["bestiary"].values()), len(save["bestiary"]))
    for which, entries in save["bestiary"].items():  # in the order of the file, which is not ascending
        body += struct.pack("<2I", which, 4 * len(entries)) + b"".join(struct.pack("<iI", key, value) for key, value in entries)
    body += save["trailer"]
    return HEADER + body + struct.pack("<I", checksum(body))


def _signed(value):
    return value - (1 << 32) if value & 0x80000000 else value


def shared_view(save):
    """What a save contributes to a shared save, in the form decode_shared() returns (flags as 0 and 1). This, packed by
    encode_shared(), is all a lobby member sends to the host - as in the game, never the whole file."""
    return dict(chunks={kind: [1 if value else 0 for value in save["chunks"][kind][1]] if WIDTH[kind] == 1 else list(save["chunks"][kind][1])
                        for kind in SHARED_ORDER},
                bestiary={which: list(save["bestiary"][which]) for which in SHARED_MAPS})


def merge_shared(views):
    """The shared save of a lobby, as the game's builder (RVA 0x51a450) makes it from what the members sent: flags by AND,
    counters by the smallest value (cutscene counters compared as signed numbers, as the game does), bestiary maps 1 and 2
    by the smallest value of the entries every member has."""
    if not views:
        raise ValueError("Nothing to merge")
    chunks = {}
    for kind in SHARED_ORDER:
        if any(len(view["chunks"][kind]) != SHARED_COUNTS[kind] for view in views):
            raise ValueError(f"Chunk {kind} has a different layout in one of the saves")
        rows = list(zip(*(view["chunks"][kind] for view in views)))
        if WIDTH[kind] == 1:
            chunks[kind] = [1 if all(row) else 0 for row in rows]
        else:
            chunks[kind] = [min(row, key=_signed) if kind == CUTSCENES else min(row) for row in rows]
    bestiary = {}
    for which in SHARED_MAPS:
        tables = [dict(view["bestiary"][which]) for view in views]
        bestiary[which] = [(key, min(table[key] for table in tables)) for key in sorted(tables[0]) if all(key in table for table in tables)]
    return dict(chunks=chunks, bestiary=bestiary)


def overlay(own, shared):
    """A member's save for the match: the shared save laid over the member's own, as the game does at 'Notify Game Start'
    (RVA 0x50ca40 -> 0x51adf0 over Manager+0x14). What the shared save does not carry - settings, special seed counters,
    bestiary maps 3 and 4, the unchecked word and the trailer - stays the member's own."""
    chunks = {}
    for kind, (size, elements) in own["chunks"].items():
        if kind in shared["chunks"]:
            if len(shared["chunks"][kind]) != len(elements):
                raise ValueError(f"Chunk {kind} of the shared save has a different layout")
            chunks[kind] = (size, list(shared["chunks"][kind]))
        else:
            chunks[kind] = (size, list(elements))
    bestiary = {which: list(shared["bestiary"][which]) if which in shared["bestiary"] else list(entries) for which, entries in own["bestiary"].items()}
    return dict(word=own["word"], chunks=chunks, bestiary=bestiary, trailer=own["trailer"])


def merge(saves):
    """One file for a whole lobby: the shared save of the given saves laid over the first one, the host's."""
    if not saves:
        raise ValueError("Nothing to merge")
    for kind in saves[0]["chunks"]:
        if any(save["chunks"][kind][0] != saves[0]["chunks"][kind][0] or len(save["chunks"][kind][1]) != len(saves[0]["chunks"][kind][1]) for save in saves[1:]):
            raise ValueError(f"Chunk {kind} has a different layout in one of the saves")
    return overlay(saves[0], merge_shared([shared_view(save) for save in saves]))


def decode_shared(raw):
    """A shared save as the game sends and dumps it (online_logs/sessions/<session>/sharedsave_begin.dat): the shared chunks,
    bestiary maps 1 and 2, and the Steam ids of up to four members whose saves went into it. The game writes those dumps
    from buffers it has already released: some hold other memory and do not decode, and the first four bytes (achievements
    0 to 31) of one that does decode may be the allocator's link rather than flags."""
    if len(raw) < SHARED_FIXED + 8 + 32:
        raise ValueError("Too short for a shared save")
    at, chunks = 0, {}
    for kind in SHARED_ORDER:
        count = SHARED_COUNTS[kind]
        if WIDTH[kind] == 1:
            chunks[kind] = [(raw[at + (n >> 3)] >> (n & 7)) & 1 for n in range(count)]; at += (count >> 3) + 1
        else:
            chunks[kind] = list(struct.unpack_from(f"<{count}I", raw, at)); at += count * 4
    bestiary = {}
    for which in SHARED_MAPS:
        if at + 4 > len(raw):
            raise ValueError("The shared save ends before its bestiary")
        entries = struct.unpack_from("<I", raw, at)[0]; at += 4
        if at + entries * 8 > len(raw):
            raise ValueError("A bestiary map of the shared save runs past its end")
        bestiary[which] = [struct.unpack_from("<iI", raw, at + n * 8) for n in range(entries)]; at += entries * 8
    if at + 32 != len(raw):
        raise ValueError(f"A shared save of {len(raw)} bytes does not end where its parts do ({at + 32})")
    return dict(chunks=chunks, bestiary=bestiary, members=list(struct.unpack_from("<4Q", raw, at)))


def encode_shared(shared):
    """The wire form of what decode_shared() returns."""
    raw = b""
    for kind in SHARED_ORDER:
        elements = shared["chunks"][kind]
        if len(elements) != SHARED_COUNTS[kind]:
            raise ValueError(f"Chunk {kind} has {len(elements)} elements, the shared save carries {SHARED_COUNTS[kind]}")
        if WIDTH[kind] == 1:
            block = bytearray((len(elements) >> 3) + 1)
            for n, value in enumerate(elements):
                if value:
                    block[n >> 3] |= 1 << (n & 7)
            raw += bytes(block)
        else:
            raw += struct.pack(f"<{len(elements)}I", *elements)
    for which in SHARED_MAPS:
        entries = shared["bestiary"][which]
        raw += struct.pack("<I", len(entries)) + b"".join(struct.pack("<iI", key, value) for key, value in entries)
    return raw + struct.pack("<4Q", *(list(shared.get("members", [])) + [0] * 4)[:4])


def lacking_unlocks(shared, joiner):
    """Who may come into a match that runs (the user's rule, as in native_state.cpp): a player whose save has at least every
    unlock of the session's shared save. The achievements of the shared view that the joiner's view lacks; none - may join."""
    return [index for index, (ours, theirs) in enumerate(zip(shared["chunks"][1], joiner["chunks"][1])) if ours and not theirs]


def difference(expected, actual):
    """Where two shared views differ: {part: [(index or key, expected, actual), ...]}. Achievements 0 to 31 are reported as
    the part 'firstBytes': in a dump of the game they are the four bytes a released buffer loses to the allocator."""
    rows = {}
    for kind in SHARED_ORDER:
        for index, (a, b) in enumerate(zip(expected["chunks"][kind], actual["chunks"][kind])):
            if a != b:
                rows.setdefault("firstBytes" if kind == 1 and index < 32 else NAMES[kind], []).append((index, a, b))
    for which in SHARED_MAPS:
        a, b = dict(expected["bestiary"][which]), dict(actual["bestiary"][which])
        for key in sorted(set(a) | set(b)):
            if a.get(key) != b.get(key):
                rows.setdefault(f"bestiaryMap{which}", []).append((key, a.get(key), b.get(key)))
    return rows


def summary(save):
    """Numbers of a parsed save, or of a decoded shared save."""
    rows = {}
    for kind, chunk in save["chunks"].items():
        elements = chunk[1] if isinstance(chunk, tuple) else chunk
        rows[NAMES[kind]] = dict(elements=len(elements), set=sum(1 for value in elements if value), total=sum(elements))
    for which, entries in save["bestiary"].items():
        rows[f"bestiaryMap{which}"] = dict(elements=len(entries), set=sum(1 for _, value in entries if value), total=sum(value for _, value in entries))
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    info = commands.add_parser("info", help="what a save has unlocked, in numbers"); info.add_argument("file", type=Path)
    dump = commands.add_parser("shared", help="the same for a shared save dumped by the game (sharedsave_begin.dat)"); dump.add_argument("file", type=Path)
    merged = commands.add_parser("merge", help="the save a lobby shares, by the game's own rule")
    merged.add_argument("--output", type=Path, required=True); merged.add_argument("files", type=Path, nargs="+", help="the host's save first")
    laid = commands.add_parser("overlay", help="a member's save for the match: a shared save laid over the member's own")
    laid.add_argument("--shared", type=Path, required=True, help="the shared save in the game's packed form")
    laid.add_argument("--output", type=Path, required=True); laid.add_argument("file", type=Path, help="the member's own save")
    compared = commands.add_parser("compare", help="a shared save dumped by the game against the merge of its members' saves")
    compared.add_argument("--shared", type=Path, required=True, help="sharedsave_begin.dat of the session")
    compared.add_argument("files", type=Path, nargs="+", help="every member's save as it was before the session")
    joining = commands.add_parser("joinable", help="may a save's player come into a session that runs: has it every unlock the members share")
    joining.add_argument("--joiner", type=Path, required=True); joining.add_argument("files", type=Path, nargs="+", help="the saves of the session's members")
    args = parser.parse_args()
    if args.command == "joinable":
        shared = merge_shared([shared_view(parse(path.read_bytes())) for path in args.files])
        lacking = lacking_unlocks(shared, shared_view(parse(args.joiner.read_bytes())))
        print(f"the session shares {sum(shared['chunks'][1])} unlocks; the joiner lacks {len(lacking)}" + (f": achievements {lacking[:20]}{' ...' if len(lacking) > 20 else ''}" if lacking else " - may join"))
        return 1 if lacking else 0
    if args.command == "overlay":
        if args.output.exists():
            raise SystemExit("The output file exists; a save is never overwritten")
        data = build(overlay(parse(args.file.read_bytes()), decode_shared(args.shared.read_bytes()))); parse(data)
        args.output.write_bytes(data)
        return 0
    if args.command == "compare":
        shared = decode_shared(args.shared.read_bytes()); named = sum(1 for member in shared["members"] if member)
        if named != len(args.files):
            print(f"The shared save names {named} members, {len(args.files)} saves were given: equality cannot be expected")
        rows = difference(shared_view(merge([parse(path.read_bytes()) for path in args.files])), shared)
        for part, found in rows.items():
            print(f"{part:20} {len(found):4} differ, (index or key, merged, game's): {found[:6]}")
        print("EQUAL" if not rows else "equal but for the first four bytes of the dump" if list(rows) == ["firstBytes"] else "DIFFERENT")
        return 0 if not rows or list(rows) == ["firstBytes"] else 1
    if args.command in ("info", "shared"):
        shown = parse(args.file.read_bytes()) if args.command == "info" else decode_shared(args.file.read_bytes())
        for name, row in summary(shown).items():
            print(f"{name:20} {row['set']:4} of {row['elements']:4} set, total {row['total']}")
        if args.command == "shared":  # Steam ids are nobody's business: only how many
            print(f"members              {sum(1 for member in shown['members'] if member)}")
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
