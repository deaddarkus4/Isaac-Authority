"""INP1: one client's input for one player of the host's game. The client never sends positions."""
import math
import struct

COMMAND = struct.Struct("<IIQIiQ4fII")
# Bit n of the button mask; the game's ButtonAction numbers are 8, 9, 10, 11, 19, 14, 15, 20, 21, 22, 23.
BUTTONS = ("bomb", "item", "pillCard", "drop", "join", "menuConfirm", "menuBack", "menuLeft", "menuRight", "menuUp", "menuDown")


def encode(session, sequence, time_ms, controller=0, move=(0.0, 0.0), shoot=(0.0, 0.0), buttons=()):
    axes = (*move, *shoot)
    if not 0 < int(session) < 2**64 or not 0 < sequence < 2**32 or not 0 <= controller <= 7:
        raise ValueError("Invalid command identity")
    if len(axes) != 4 or not all(math.isfinite(v) and -1 <= v <= 1 for v in axes):
        raise ValueError("An axis is a finite value from -1 to 1")
    mask = 0
    for button in buttons:
        mask |= 1 << BUTTONS.index(button)
    return COMMAND.pack(0x31504E49, 1, int(session), sequence, controller, time_ms, *axes, mask, 0)


SLOT = struct.Struct("<6IQ")
SLOT_BYTES = SLOT.size + COMMAND.size


def read_slot(read, descriptor, now):
    """The client module's latest captured command, or None while it is being written, absent or stale."""
    address = descriptor["address"]
    if descriptor.get("bytes") != SLOT_BYTES:
        raise ValueError("Command slot size is not one this reader knows")
    first = struct.unpack("<I", read(address + 8, 4))[0]
    if first & 1:
        return None
    blob = read(address, SLOT_BYTES)
    last = struct.unpack("<I", read(address + 8, 4))[0]
    magic, version, generation, alive, count, reserved, session = SLOT.unpack_from(blob)
    if first != last or generation != last:
        return None
    if magic != 0x31534E49 or version != 1 or session != int(descriptor["session"]) or reserved:
        raise ValueError("Command slot attachment mismatch")
    if not alive:
        return None
    if count != COMMAND.size:
        raise ValueError("Command slot payload size is invalid")
    packet = blob[SLOT.size:]
    command = decode(packet)
    if command["session"] != session:
        raise ValueError("Command slot session mismatch")
    if command["timeMs"] > now or now - command["timeMs"] > 250:
        return None
    return command, packet


def route(packet, session, controller):
    """What the transport does to a client's command: the host's session, and the controller the host gave that client.
    The client does not choose whose player it drives."""
    if not 0 < int(session) < 2**64 or not 0 <= controller <= 7:
        raise ValueError("Invalid route")
    routed = bytearray(packet)
    struct.pack_into("<Q", routed, 8, int(session)); struct.pack_into("<i", routed, 20, controller)
    decode(bytes(routed))
    return bytes(routed)


def decode(packet):
    if len(packet) != COMMAND.size:
        raise ValueError("Wrong command size")
    magic, version, session, sequence, controller, time_ms, mx, my, sx, sy, mask, reserved = COMMAND.unpack(packet)
    if (magic, version, reserved) != (0x31504E49, 1, 0) or mask >= 1 << len(BUTTONS):
        raise ValueError("Invalid command header")
    encode(session, sequence, time_ms, controller, (mx, my), (sx, sy))
    return dict(session=session, sequence=sequence, timeMs=time_ms, controller=controller, move=[mx, my], shoot=[sx, sy],
                buttons=[name for bit, name in enumerate(BUTTONS) if mask >> bit & 1])
