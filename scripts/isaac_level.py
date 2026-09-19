"""Read-only J460 level layout: run seed, room descriptors and the doors of the current room."""
import struct

GAME_RVA = 0x871678
START_SEED = 0x1BB88
ROOM_INDEX, DIMENSION = 0x18304, 0x1830C
ROOM_COUNT, ROOMS, ROOM_STRIDE, ROOM_LOOKUP = 0x182CC, 0x14, 0xB8, 0x17ADC
ROOM_TRANSITION = 0x1B83C
FRAME_COUNT = 0x264F8
PAUSE_MENU = 0x1D520  # 0 during play; 2 observed both with the pause menu open and on the death screen
MANAGER_RVA, MANAGER_STATE, IN_RUN = 0x87169C, 8, 2  # 1 observed in the main menu
GRID, MAX_ROOMS = 13, 527
PLACE = ("grid", "dimension", "type", "variant", "subtype", "shape", "spawnSeed")
# Direction name -> (grid step, movement key, shooting key as a virtual-key character).
DOORS = {"right": (1, "D", chr(0x27)), "left": (-1, "A", chr(0x25)), "down": (GRID, "S", chr(0x28)), "up": (-GRID, "W", chr(0x26))}


SEED_ALPHABET = "ABCDEFGHJKLMNPQRSTWXYZ01234V6789"


def seed_checksum(value):
    total = 0
    while value:
        total = (total + (value & 0xFF)) & 0xFF
        total = ((total << 1) | (total >> 7)) & 0xFF
        value >>= 5
    return total


def seed_text(seed):
    """The run seed as typed in the game's seed menu (J460 routine at RVA 0x5eb6b0, inverted)."""
    if not 0 < seed < 2**32:
        raise ValueError("Invalid run seed")
    value = seed ^ 0xFEF7FFD
    digits = [(value >> shift) & 31 for shift in (27, 22, 17, 12, 7, 2)]
    check = seed_checksum(seed)
    digits += [(value & 3) << 3 | check >> 5, check & 31]
    text = "".join(SEED_ALPHABET[digit] for digit in digits)
    return text[:4] + " " + text[4:]


def seed_value(text):
    digits = [SEED_ALPHABET.find(letter) for letter in text.replace(" ", "").upper()]
    if len(digits) != 8 or min(digits) < 0:
        raise ValueError("A seed has eight characters of the game's alphabet")
    value = 0
    for digit in digits[:6]:
        value = value << 5 | digit
    seed = (value << 2 | digits[6] >> 3) ^ 0xFEF7FFD
    if not seed or seed_checksum(seed) != ((digits[6] & 7) << 5 | digits[7]):
        raise ValueError("Seed checksum mismatch")
    return seed


def u32(read, address):
    return struct.unpack("<I", read(address, 4))[0]


def snapshot(read, base):
    game = u32(read, base + GAME_RVA)
    if game < 0x10000:
        raise ValueError("No game object; start a run first")
    count = u32(read, game + ROOM_COUNT)
    if not 0 < count <= MAX_ROOMS:
        raise ValueError("Invalid room count")
    rooms = []
    for slot in range(count):
        descriptor = game + ROOMS + slot * ROOM_STRIDE
        config = u32(read, descriptor + 0x10)
        if config < 0x10000:
            raise ValueError("Room descriptor has no layout")
        rooms.append(dict(grid=u32(read, descriptor), dimension=u32(read, descriptor + 0xC),
                          type=u32(read, config + 8), variant=u32(read, config + 0xC), subtype=u32(read, config + 0x10),
                          shape=u32(read, config + 0x48), spawnSeed=u32(read, descriptor + 0x5C),
                          visits=u32(read, descriptor + 0x40)))
    return dict(startSeed=u32(read, game + START_SEED), stage=u32(read, game), stageType=u32(read, game + 4),
                index=u32(read, game + ROOM_INDEX), dimension=u32(read, game + DIMENSION),
                transition=u32(read, game + ROOM_TRANSITION), rooms=rooms)


def place(room):
    return tuple(room[key] for key in PLACE)


def run_differences(a, b):
    return [f"{key}: {a[key]} != {b[key]}" for key in ("startSeed", "stage", "stageType") if a[key] != b[key]]


def room_differences(a, b):
    """Grid cells whose generated room differs; one run seed still diverges when the save files unlock different content."""
    first = {(room["grid"], room["dimension"]): place(room) for room in a["rooms"]}
    second = {(room["grid"], room["dimension"]): place(room) for room in b["rooms"]}
    return sorted(cell for cell in set(first) | set(second) if first.get(cell) != second.get(cell))


def differences(a, b):
    """Visit counters are local history; everything else the run generates is compared."""
    cells = room_differences(a, b)
    return run_differences(a, b) + ([f"rooms: {len(cells)} grid cells differ: {[cell[0] for cell in cells]}"] if cells else [])


def shared_doors(host, replica):
    """Doors of the host's room that lead to the same generated room in both games."""
    return {name: room for name, room in doors(host).items()
            if (there := find(replica, room["grid"])) is not None and place(there) == place(room)}


def find(level, index, dimension=0):
    for room in level["rooms"]:
        if room["grid"] == index and room["dimension"] == dimension:
            return room
    return None


def doors(level):
    """Ordinary 1x1 neighbours of an ordinary 1x1 current room; such rooms are always joined by a door."""
    here = find(level, level["index"], level["dimension"])
    if here is None or here["shape"] != 1 or level["dimension"] != 0:
        return {}
    result = {}
    for name, (step, _, _) in DOORS.items():
        index = level["index"] + step
        if not 0 <= index < GRID * GRID or (abs(step) == 1 and index // GRID != level["index"] // GRID):
            continue
        room = find(level, index)
        if room is not None and room["type"] == 1 and room["shape"] == 1:
            result[name] = room
    return result
