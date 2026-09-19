"""Read-only J460 state sampler. External reads are best-effort, not atomic game snapshots."""
import argparse
import ctypes as C
from ctypes import wintypes as W
import datetime
import hashlib
import json
import math
import os
import socket
from pathlib import Path
import struct
import time

J460_SHA256 = "3bdfc8bae0dc7e334b76009d0ad45dfbb16ee5f00c06ffbc3a0094e34d44616b"
GAME_RVA = 0x871678
MANAGER_RVA = 0x87169C
PLAYER_VECTOR = 0x1BAA8
POSITION = 0x33C
VELOCITY = 0x360
ROOM_POINTER = 0x18300
HISTORY = 0x1310
HISTORY_COUNTERS = 0x1610
GET_PLAYER_RVA = 0x17870
GET_PLAYER_PREFIX = bytes.fromhex("558bec5156578bf98b97acba01008bc28bb7a8ba0100")


class InvalidState(ValueError):
    pass


def pointer(value):
    if not 0x10000 <= value < 0xFFF00000 or value % 4:
        raise InvalidState("invalid x86 pointer")
    return value


def u32(data, offset=0):
    return struct.unpack_from("<I", data, offset)[0]


def finite_pair(data, offset, limit):
    values = struct.unpack_from("<ff", data, offset)
    if not all(math.isfinite(v) and abs(v) <= limit for v in values):
        raise InvalidState("invalid position/velocity")
    return list(values)


def vector_count(begin, end, stride, maximum):
    if begin == end == 0:
        return 0
    pointer(begin)
    if end < begin or (end - begin) % stride or (end - begin) // stride > maximum:
        raise InvalidState("invalid vector bounds")
    return (end - begin) // stride


def player_fields(data):
    if len(data) != 0x368 or u32(data, 0x28) != 1:
        raise InvalidState("not a player entity")
    return {"index": u32(data, 0x20), "type": 1, "variant": u32(data, 0x2C),
            "subtype": u32(data, 0x30), "existsByte": data[0x172],
            "position": finite_pair(data, POSITION, 100000),
            "velocity": finite_pair(data, VELOCITY, 10000)}


def frame_entities(data):
    if len(data) % 0x38 or len(data) > 4096 * 0x38:
        raise InvalidState("invalid history payload size")
    result = []
    for at in range(0, len(data), 0x38):
        result.append({"index": u32(data, at), "type": u32(data, at + 4),
                       "variant": u32(data, at + 8), "subtype": u32(data, at + 12),
                       "checksum": u32(data, at + 16), "rngChecksum": u32(data, at + 20),
                       "prePosition": finite_pair(data, at + 24, 100000),
                       "preVelocity": finite_pair(data, at + 32, 10000),
                       "postPosition": finite_pair(data, at + 40, 100000),
                       "postVelocity": finite_pair(data, at + 48, 10000)})
    return result


def observer_packet(state, session, epoch):
    """Project two historical players into lab wire v1; never a complete world snapshot."""
    history = state.get("history")
    if not history:
        return None
    players = sorted((e for e in history["entities"] if e["type"] == 1), key=lambda e: e["index"])
    if len(players) != 2 or len({e["index"] for e in players}) != 2:
        return None
    values = []
    for player in players:
        position, velocity = player["postPosition"], player["postVelocity"]
        if not all(math.isfinite(v) and abs(v) <= 10000 for v in position):
            return None
        if not all(math.isfinite(v) and abs(v) <= 100 for v in velocity):
            return None
        values.extend(position + velocity)
    return struct.pack("<IHHQ6I8f", 0x48545541, 1, 2, session, epoch, 2, 0,
                       history["frame"], 0, 0, *values)


def sample(read, base):
    game_pointer = read(base + GAME_RVA, 4)
    game = pointer(u32(game_pointer))
    manager_pointer = read(base + MANAGER_RVA, 4)
    manager = pointer(u32(manager_pointer))
    frame_before = read(manager + 0x4B3E4, 4)
    header = read(game + PLAYER_VECTOR, 8)
    begin, end = struct.unpack("<II", header)
    count = vector_count(begin, end, 4, 16)
    addresses = read(begin, count * 4) if count else b""
    objects = []
    seen = set()
    for offset in range(0, len(addresses), 4):
        address = pointer(u32(addresses, offset))
        if address in seen:
            raise InvalidState("duplicate player pointer")
        seen.add(address)
        blob = read(address, 0x368)
        fields = player_fields(blob)
        fields.update(slot=offset // 4, address=hex(address))
        objects.append((address, fields))
    room_pointer = read(game + ROOM_POINTER, 4)
    history = None
    if u32(room_pointer):
        room = pointer(u32(room_pointer))
        counters = read(room + HISTORY_COUNTERS, 8)
        start, entries = struct.unpack("<II", counters)
        if entries > 32:
            raise InvalidState("invalid frame history count")
        # Skip newest ring entry; it may still be under construction this tick.
        if entries >= 2:
            entry = room + HISTORY + ((start + entries - 2) & 31) * 24
            record = read(entry, 24)
            frame, first, last, _, checksum, rng = struct.unpack("<6I", record)
            length = vector_count(first, last, 0x38, 4096) * 0x38
            payload = read(first, length) if length else b""
            history = {"frame": frame, "checksum": checksum, "rngChecksum": rng,
                       "entities": frame_entities(payload)}
            if (read(entry, 24) != record or (length and read(first, length) != payload)
                    or read(room + HISTORY_COUNTERS, 8) != counters):
                raise InvalidState("frame history changed while reading")
    # Compare selected fields; animations and unrelated data can legitimately change.
    for address, fields in objects:
        current = player_fields(read(address, 0x368))
        if any(current[key] != fields[key] for key in current):
            raise InvalidState("player changed while reading")
    if (read(game + PLAYER_VECTOR, 8) != header
            or (count and read(begin, count * 4) != addresses)
            or read(game + ROOM_POINTER, 4) != room_pointer
            or read(base + GAME_RVA, 4) != game_pointer
            or read(base + MANAGER_RVA, 4) != manager_pointer
            or read(manager + 0x4B3E4, 4) != frame_before):
        raise InvalidState("game/frame changed while reading")
    return {"type": "state", "networkFrameObserved": u32(frame_before),
            "gameAddress": hex(game), "roomAddress": hex(u32(room_pointer)),
            "players": [fields for _, fields in objects], "history": history,
            "atomic": False}


class WindowsProcess:
    def __init__(self, pid):
        if os.name != "nt":
            raise RuntimeError("Windows is required for process sampling")
        self.k = C.WinDLL("kernel32", use_last_error=True)
        self.k.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]
        self.k.OpenProcess.restype = W.HANDLE
        self.k.CloseHandle.argtypes = [W.HANDLE]
        self.k.ReadProcessMemory.argtypes = [W.HANDLE, C.c_void_p, C.c_void_p, C.c_size_t, C.POINTER(C.c_size_t)]
        self.k.ReadProcessMemory.restype = W.BOOL
        self.k.QueryFullProcessImageNameW.argtypes = [W.HANDLE, W.DWORD, W.LPWSTR, C.POINTER(W.DWORD)]
        self.k.QueryFullProcessImageNameW.restype = W.BOOL
        self.k.CreateToolhelp32Snapshot.argtypes = [W.DWORD, W.DWORD]
        self.k.CreateToolhelp32Snapshot.restype = W.HANDLE
        # QUERY_LIMITED_INFORMATION | VM_READ. No write, allocation or thread rights.
        self.handle = self.k.OpenProcess(0x1010, False, pid)
        if not self.handle:
            raise C.WinError(C.get_last_error())
        try:
            path = C.create_unicode_buffer(32768); size = W.DWORD(len(path))
            if not self.k.QueryFullProcessImageNameW(self.handle, 0, path, C.byref(size)):
                raise C.WinError(C.get_last_error())
            self.path = Path(path.value)
            image = self.path.read_bytes()
            if hashlib.sha256(image).hexdigest() != J460_SHA256:
                raise InvalidState("only the original J460 executable is supported")
            class Module(C.Structure):
                _fields_ = [("size", W.DWORD), ("moduleId", W.DWORD), ("pid", W.DWORD),
                            ("globalUsage", W.DWORD), ("processUsage", W.DWORD),
                            ("base", C.c_void_p), ("bytes", W.DWORD), ("module", W.HMODULE),
                            ("name", W.WCHAR * 256), ("path", W.WCHAR * 260)]
            self.k.Module32FirstW.argtypes = [W.HANDLE, C.POINTER(Module)]
            self.k.Module32NextW.argtypes = [W.HANDLE, C.POINTER(Module)]
            modules = self.k.CreateToolhelp32Snapshot(0x18, pid)
            if modules == C.c_void_p(-1).value:
                raise C.WinError(C.get_last_error())
            try:
                module = Module(); module.size = C.sizeof(module)
                ok = self.k.Module32FirstW(modules, C.byref(module))
                self.base = None
                while ok:
                    if os.path.normcase(module.path) == os.path.normcase(str(self.path)):
                        self.base = module.base; break
                    ok = self.k.Module32NextW(modules, C.byref(module))
                if self.base is None:
                    raise InvalidState("executable module not found; use 64-bit Python")
            finally:
                self.k.CloseHandle(modules)
            if self.read(self.base + GET_PLAYER_RVA, len(GET_PLAYER_PREFIX)) != GET_PLAYER_PREFIX:
                raise InvalidState("loaded GetPlayer bytes differ from J460")
        except BaseException:
            self.close()
            raise

    def read(self, address, length):
        if not 0 < length <= 4096 * 0x38:
            raise InvalidState("read size outside bounds")
        pointer(address)
        if address + length > 0x100000000:
            raise InvalidState("read exceeds x86 address space")
        blob = C.create_string_buffer(length); got = C.c_size_t()
        if not self.k.ReadProcessMemory(self.handle, address, blob, length, C.byref(got)) or got.value != length:
            raise C.WinError(C.get_last_error())
        return blob.raw

    def close(self):
        if self.handle:
            self.k.CloseHandle(self.handle)
            self.handle = None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--seconds", type=float, default=10)
    parser.add_argument("--hz", type=int, default=30)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--observer-port", type=int, help="optional localhost UDP lab observer")
    parser.add_argument("--session", type=int, help="observer session number")
    args = parser.parse_args()
    if not 0 < args.seconds <= 300 or not 1 <= args.hz <= 120:
        parser.error("seconds must be (0,300], hz 1..120")
    if args.observer_port is not None and (not 1 <= args.observer_port <= 65535
            or args.session is None or not 0 < args.session < 2**64):
        parser.error("observer requires a valid port and nonzero u64 session")
    process = WindowsProcess(args.pid)
    accepted = rejected = 0
    sent, epoch, previous_frame = 0, 1, None
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM) if args.observer_port is not None else None
    started = time.monotonic()
    try:
        # Never overwrite an existing capture.
        with args.output.open("x", encoding="utf-8") as output:
            header = {"type": "start", "schemaVersion": 1, "pid": args.pid,
                      "utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                      "gameSha256": J460_SHA256, "imageBase": hex(process.base),
                      "readOnly": True, "atomic": False, "gameThreadHook": False}
            header["observerBridge"] = udp is not None
            output.write(json.dumps(header) + "\n")
            while time.monotonic() - started < args.seconds:
                tick = time.monotonic()
                try:
                    record = sample(process.read, process.base)
                    record["elapsedSeconds"] = tick - started
                    if udp is not None and record["history"]:
                        frame = record["history"]["frame"]
                        if previous_frame is not None and frame < previous_frame:
                            epoch += 1
                        packet = observer_packet(record, args.session, epoch)
                        if packet is not None and frame != previous_frame:
                            udp.sendto(packet, ("127.0.0.1", args.observer_port))
                            sent += 1
                            record["observerSent"] = {"frame": frame, "epoch": epoch}
                        previous_frame = frame
                    output.write(json.dumps(record, allow_nan=False) + "\n")
                    accepted += 1
                except (InvalidState, OSError) as error:
                    rejected += 1
                    output.write(json.dumps({"type": "rejected", "reason": str(error),
                                             "elapsedSeconds": tick - started}) + "\n")
                time.sleep(max(0, 1 / args.hz - (time.monotonic() - tick)))
            summary = {"type": "stop", "accepted": accepted, "rejected": rejected,
                       "observerPackets": sent}
            output.write(json.dumps(summary) + "\n")
            print(json.dumps(summary))
        if not accepted:
            raise SystemExit("No state samples accepted; inspect rejection reasons")
    finally:
        if udp is not None:
            udp.close()
        process.close()


if __name__ == "__main__":
    main()
