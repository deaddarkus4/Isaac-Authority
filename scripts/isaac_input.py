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


def decode(packet):
    if len(packet) != COMMAND.size:
        raise ValueError("Wrong command size")
    magic, version, session, sequence, controller, time_ms, mx, my, sx, sy, mask, reserved = COMMAND.unpack(packet)
    if (magic, version, reserved) != (0x31504E49, 1, 0) or mask >= 1 << len(BUTTONS):
        raise ValueError("Invalid command header")
    encode(session, sequence, time_ms, controller, (mx, my), (sx, sy))
    return dict(session=session, sequence=sequence, timeMs=time_ms, controller=controller, move=[mx, my], shoot=[sx, sy],
                buttons=[name for bit, name in enumerate(BUTTONS) if mask >> bit & 1])
