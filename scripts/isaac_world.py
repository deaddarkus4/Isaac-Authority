"""Bounded WLD1 codec and coherent published-slot reader for the J460 tear experiment."""
import math
import struct

HEADER = struct.Struct("<IIQIIQ9I4fIII")
ENTITY = struct.Struct("<5I8fI")
SLOT = struct.Struct("<6IQ")
MAX_ENTITIES, MAX_BYTES, SLOT_BYTES = 16, 992, 1024


def validate(frame):
    if not 0 < int(frame["session"]) < 2**64 or not 0 < frame["epoch"] < 2**32 or not 0 < frame["sequence"] < 2**32:
        raise ValueError("Invalid frame identity")
    room = frame["room"]
    if len(room) != 9 or room[0] > 20 or room[1] > 10 or room[3] > 2 or room[4] > 40 or not 1 <= room[6] <= 12:
        raise ValueError("Invalid room")
    def body(values):
        return len(values) == 4 and all(math.isfinite(v) for v in values) and all(-128 <= v <= 2048 for v in values[:2]) and all(-40 <= v <= 40 for v in values[2:])
    if not body(frame["player"]) or len(frame["entities"]) > MAX_ENTITIES:
        raise ValueError("Invalid player or incomplete roster")
    seen = set()
    for entity in frame["entities"]:
        if not entity["id"] or entity["id"] in seen or not entity["seed"]:
            raise ValueError("Invalid or duplicate entity identity")
        seen.add(entity["id"])
        if (entity["type"], entity["variant"], entity["subtype"]) != (2, 0, 0) or not body(entity["body"]):
            raise ValueError("Unsupported entity")
        auxiliary = entity["tear"]
        if len(auxiliary) != 4 or not all(math.isfinite(v) for v in auxiliary) or not (
                -500 <= auxiliary[0] <= 100 and -100 <= auxiliary[1] <= 100 and
                -10 <= auxiliary[2] <= 10 and 0.01 <= auxiliary[3] <= 10):
            raise ValueError("Invalid tear state")


def encode(frame):
    validate(frame)
    packet = HEADER.pack(0x31444C57, 1, int(frame["session"]), frame["epoch"], frame["sequence"], frame["timeMs"],
                         *frame["room"], *frame["player"], len(frame["entities"]), 0, 0)
    return packet + b"".join(ENTITY.pack(entity["id"], entity["seed"], entity["type"], entity["variant"], entity["subtype"],
                                         *entity["body"], *entity["tear"], 0) for entity in frame["entities"])


def decode(packet):
    if len(packet) < HEADER.size:
        raise ValueError("Truncated world header")
    fields = HEADER.unpack_from(packet)
    if fields[:2] != (0x31444C57, 1) or fields[-2:] != (0, 0):
        raise ValueError("Invalid world header")
    count = fields[19]
    if count > MAX_ENTITIES or len(packet) != HEADER.size + count * ENTITY.size:
        raise ValueError("Incomplete or oversized roster")
    frame = dict(session=fields[2], epoch=fields[3], sequence=fields[4], timeMs=fields[5],
                 room=list(fields[6:15]), player=list(fields[15:19]), entities=[])
    for offset in range(HEADER.size, len(packet), ENTITY.size):
        entity = ENTITY.unpack_from(packet, offset)
        if entity[-1]:
            raise ValueError("Reserved entity field is not zero")
        frame["entities"].append(dict(id=entity[0], seed=entity[1], type=entity[2], variant=entity[3], subtype=entity[4],
                                      body=list(entity[5:9]), tear=list(entity[9:13])))
    validate(frame)
    return frame


def read_slot(read, descriptor, now):
    address = descriptor["address"]
    first = struct.unpack("<I", read(address + 8, 4))[0]
    if first & 1:
        return None
    blob = read(address, SLOT_BYTES)
    last = struct.unpack("<I", read(address + 8, 4))[0]
    magic, version, generation, alive, count, reserved, session = SLOT.unpack_from(blob)
    if first != last or generation != last or last & 1:
        return None
    if magic != 0x31525357 or version != 1 or session != int(descriptor["session"]) or reserved:
        raise ValueError("World slot attachment mismatch")
    if not alive:
        return None
    if not HEADER.size <= count <= MAX_BYTES:
        raise ValueError("World slot payload size is invalid")
    packet = blob[SLOT.size:SLOT.size + count]
    frame = decode(packet)
    if frame["session"] != session:
        raise ValueError("World slot session mismatch")
    if frame["timeMs"] > now or now - frame["timeMs"] > 250:
        return None
    return frame, packet


def same_entity(a, b):
    return all(a[key] == b[key] for key in ("id", "seed", "type", "variant", "subtype")) and (
        struct.pack("<8f", *a["body"], *a["tear"]) == struct.pack("<8f", *b["body"], *b["tear"]))
