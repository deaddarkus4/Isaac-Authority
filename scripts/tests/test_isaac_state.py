import importlib.util
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location("state_reader", Path(__file__).parents[1] / "Read-IsaacState.py")
reader = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reader)


class Memory:
    base, game, manager, vector, player, room, payload = (
        0x400000, 0x2000000, 0x3000000, 0x4000000, 0x5000000, 0x6000000, 0x7000000)

    def __init__(self):
        self.blocks = {}
        self.put(self.base + reader.GAME_RVA, struct.pack("<I", self.game))
        self.put(self.base + reader.MANAGER_RVA, struct.pack("<I", self.manager))
        self.put(self.manager + 0x4B3E4, struct.pack("<I", 40))
        self.put(self.game + reader.PLAYER_VECTOR, struct.pack("<II", self.vector, self.vector + 4))
        self.put(self.vector, struct.pack("<I", self.player))
        blob = bytearray(0x368)
        struct.pack_into("<III", blob, 0x28, 1, 0, 21)
        struct.pack_into("<ff", blob, reader.POSITION, 300, 240)
        struct.pack_into("<ff", blob, reader.VELOCITY, 1, -2)
        blob[0x172] = 1
        self.put(self.player, blob)
        self.put(self.game + reader.ROOM_POINTER, struct.pack("<I", self.room))
        self.put(self.room + reader.HISTORY_COUNTERS, struct.pack("<II", 31, 2))
        self.put(self.room + reader.HISTORY + 31 * 24,
                 struct.pack("<6I", 39, self.payload, self.payload + 56, self.payload + 56, 123, 456))
        self.put(self.payload, struct.pack("<6I8f", 0, 1, 0, 21, 12, 34,
                                          299, 242, 1, -2, 300, 240, 1, -2))

    def put(self, address, data):
        self.blocks[address] = bytes(data)

    def read(self, address, length):
        value = self.blocks[address]
        assert length == len(value), (length, len(value))
        return value


class ReaderTests(unittest.TestCase):
    def test_observer_packet_is_explicit_wire_v1(self):
        memory = Memory()
        state = reader.sample(memory.read, memory.base)
        self.assertIsNone(reader.observer_packet(state, 123, 1))
        second = dict(state["history"]["entities"][0], index=1)
        state["history"]["entities"].append(second)
        packet = reader.observer_packet(state, 123, 2)
        self.assertEqual(len(packet), 72)
        fields = struct.unpack("<IHHQ6I8f", packet)
        self.assertEqual(fields[:10], (0x48545541, 1, 2, 123, 2, 2, 0, 39, 0, 0))
        self.assertEqual(fields[10:], (300, 240, 1, -2, 300, 240, 1, -2))

    def test_real_layout_and_wrapped_history(self):
        memory = Memory()
        state = reader.sample(memory.read, memory.base)
        self.assertEqual(state["players"][0]["position"], [300, 240])
        self.assertEqual(state["players"][0]["velocity"], [1, -2])
        self.assertEqual(state["history"]["frame"], 39)
        self.assertEqual(state["history"]["entities"][0]["postVelocity"], [1, -2])
        self.assertFalse(state["atomic"])

    def test_vector_invalid_or_huge(self):
        for begin, end in [(0, 4), (0x10000, 0x10001), (0x10000, 0x999999), (0x20000, 0x10000)]:
            with self.subTest(begin=begin, end=end), self.assertRaises(reader.InvalidState):
                reader.vector_count(begin, end, 4, 16)

    def test_nan_rejected(self):
        memory = Memory()
        blob = bytearray(memory.blocks[memory.player])
        struct.pack_into("<f", blob, reader.VELOCITY, float("nan"))
        memory.put(memory.player, blob)
        with self.assertRaises(reader.InvalidState):
            reader.sample(memory.read, memory.base)

    def test_player_removed_during_read(self):
        memory = Memory()
        reads = 0
        def changing(address, length):
            nonlocal reads
            if address == memory.game + reader.PLAYER_VECTOR:
                reads += 1
                if reads > 1:
                    return bytes(8)
            return memory.read(address, length)
        with self.assertRaisesRegex(reader.InvalidState, "changed"):
            reader.sample(changing, memory.base)

    def test_history_changes_during_copy(self):
        memory = Memory()
        reads = 0
        def changing(address, length):
            nonlocal reads
            if address == memory.payload:
                reads += 1
                if reads > 1:
                    return bytes(length)
            return memory.read(address, length)
        with self.assertRaisesRegex(reader.InvalidState, "history changed"):
            reader.sample(changing, memory.base)

    def test_empty_player_list(self):
        memory = Memory()
        memory.put(memory.game + reader.PLAYER_VECTOR, bytes(8))
        memory.put(memory.game + reader.ROOM_POINTER, bytes(4))
        state = reader.sample(memory.read, memory.base)
        self.assertEqual(state["players"], [])
        self.assertIsNone(state["history"])

    def test_duplicate_players(self):
        memory = Memory()
        memory.put(memory.game + reader.PLAYER_VECTOR, struct.pack("<II", memory.vector, memory.vector + 8))
        memory.put(memory.vector, struct.pack("<II", memory.player, memory.player))
        with self.assertRaisesRegex(reader.InvalidState, "duplicate"):
            reader.sample(memory.read, memory.base)


if __name__ == "__main__":
    unittest.main()
