"""Bounded WLD1 codec and coherent published-slot reader: version 1 carries tears, version 2 adds the room's enemies."""
import math
import struct

HEADER = struct.Struct("<IIQIIQ9I4fIII")
ENTITY = struct.Struct("<5I8fI")
NPC = struct.Struct("<4I8fi3I")
SLOT = struct.Struct("<6IQ")
MAX_ENTITIES, MAX_NPCS = 16, 32
MAX_BYTES = HEADER.size + MAX_ENTITIES * ENTITY.size + MAX_NPCS * NPC.size
SLOT_BYTES = SLOT.size + MAX_BYTES


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
    npcs = frame.get("npcs", [])
    if len(npcs) > MAX_NPCS:
        raise ValueError("Incomplete enemy roster")
    identities = set()
    for npc in npcs:
        identity = (npc["type"], npc["variant"], npc["subtype"], npc["seed"])
        if not 10 <= npc["type"] < 1000 or not npc["seed"] or identity in identities:
            raise ValueError("Invalid or duplicate enemy identity")
        identities.add(identity)
        values = npc["body"] + npc["target"] + npc["hp"]
        if len(values) != 8 or not all(math.isfinite(v) for v in values) or npc["flags"][0] > 1 or len(npc["flags"]) != 3:
            raise ValueError("Invalid enemy state")


def encode(frame):
    validate(frame)
    npcs = frame.get("npcs", [])
    # A frame without enemies is version 1, byte for byte what the tear stage sent.
    packet = HEADER.pack(0x31444C57, 2 if npcs else 1, int(frame["session"]), frame["epoch"], frame["sequence"], frame["timeMs"],
                         *frame["room"], *frame["player"], len(frame["entities"]), len(npcs), 0)
    tail = b"".join(NPC.pack(npc["type"], npc["variant"], npc["subtype"], npc["seed"], *npc["body"], *npc["target"], *npc["hp"],
                             npc["state"], *npc["flags"]) for npc in npcs)
    return packet + b"".join(ENTITY.pack(entity["id"], entity["seed"], entity["type"], entity["variant"], entity["subtype"],
                                         *entity["body"], *entity["tear"], 0) for entity in frame["entities"]) + tail


def decode(packet):
    if len(packet) < HEADER.size:
        raise ValueError("Truncated world header")
    fields = HEADER.unpack_from(packet)
    version, enemies = fields[1], fields[-2]
    if fields[0] != 0x31444C57 or fields[-1] or (enemies != 0 if version == 1 else version != 2 or not 0 < enemies <= MAX_NPCS):
        raise ValueError("Invalid world header")
    count = fields[19]
    if count > MAX_ENTITIES or len(packet) != HEADER.size + count * ENTITY.size + enemies * NPC.size:
        raise ValueError("Incomplete or oversized roster")
    frame = dict(session=fields[2], epoch=fields[3], sequence=fields[4], timeMs=fields[5],
                 room=list(fields[6:15]), player=list(fields[15:19]), entities=[])
    for offset in range(HEADER.size, HEADER.size + count * ENTITY.size, ENTITY.size):
        entity = ENTITY.unpack_from(packet, offset)
        if entity[-1]:
            raise ValueError("Reserved entity field is not zero")
        frame["entities"].append(dict(id=entity[0], seed=entity[1], type=entity[2], variant=entity[3], subtype=entity[4],
                                      body=list(entity[5:9]), tear=list(entity[9:13])))
    if enemies:
        frame["npcs"] = []
        for offset in range(HEADER.size + count * ENTITY.size, len(packet), NPC.size):
            npc = NPC.unpack_from(packet, offset)
            frame["npcs"].append(dict(type=npc[0], variant=npc[1], subtype=npc[2], seed=npc[3], body=list(npc[4:8]),
                                      target=list(npc[8:10]), hp=list(npc[10:12]), state=npc[12], flags=list(npc[13:16])))
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


def same_npc(a, b):
    return all(a[key] == b[key] for key in ("type", "variant", "subtype", "seed", "state", "flags")) and (
        struct.pack("<8f", *a["body"], *a["target"], *a["hp"]) == struct.pack("<8f", *b["body"], *b["target"], *b["hp"]))
