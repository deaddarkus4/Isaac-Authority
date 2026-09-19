import importlib.util
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location("isaac_world", Path(__file__).parents[1] / "isaac_world.py")
world = importlib.util.module_from_spec(spec)
spec.loader.exec_module(world)


class WorldTests(unittest.TestCase):
    def frame(self):
        return dict(session=123, epoch=1, sequence=4, timeMs=1000, room=[1, 0, 84, 0, 1, 2, 1, 99, 1],
                    player=[320, 280, 0, 0], entities=[dict(id=1, seed=17, type=2, variant=0, subtype=0,
                    body=[350, 280, 10, 0], tear=[-20, -1, 0.5, 1])])

    def test_roundtrip_and_complete_rosters(self):
        frame = self.frame()
        packet = world.encode(frame)
        self.assertEqual(len(packet), 152)
        self.assertEqual(world.decode(packet), frame)
        for length in range(len(packet)):
            with self.assertRaises(ValueError):
                world.decode(packet[:length])
        with self.assertRaises(ValueError):
            world.decode(packet + b"\0")
        frame["entities"] = []
        self.assertEqual(world.decode(world.encode(frame))["entities"], [])

    def test_bad_entity_and_reserved_fields(self):
        for offset in (0, 4, 88, 92, 148):
            packet = bytearray(world.encode(self.frame()))
            packet[offset] ^= 0x55
            with self.assertRaises(ValueError):
                world.decode(packet)
        for change in (dict(type=5), dict(variant=1), dict(body=[float("nan"), 0, 0, 0]), dict(seed=0)):
            frame = self.frame()
            frame["entities"][0].update(change)
            with self.assertRaises(ValueError):
                world.encode(frame)

    def test_duplicate_ids_and_overflow(self):
        frame = self.frame()
        frame["entities"] *= 2
        with self.assertRaises(ValueError):
            world.encode(frame)
        frame["entities"] *= 9
        with self.assertRaises(ValueError):
            world.encode(frame)

    def test_slot_generation_session_and_freshness(self):
        packet = world.encode(self.frame())
        blob = world.SLOT.pack(0x31525357, 1, 2, 1, len(packet), 0, 123) + packet + bytes(world.MAX_BYTES - len(packet))
        def read(address, size):
            return blob[8:12] if size == 4 else blob
        descriptor = dict(address=0x10000, session="123")
        self.assertEqual(world.read_slot(read, descriptor, 1000)[0], self.frame())
        self.assertIsNone(world.read_slot(read, descriptor, 1251))
        self.assertIsNone(world.read_slot(read, descriptor, 999))
        with self.assertRaises(ValueError):
            world.read_slot(read, dict(address=0x10000, session="124"), 1000)
        count = 0
        def racing(address, size):
            nonlocal count
            if size == 4:
                count += 1
                return struct.pack("<I", count * 2)
            return blob
        self.assertIsNone(world.read_slot(racing, descriptor, 1000))


if __name__ == "__main__":
    unittest.main()
