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

    def test_enemy_section_is_version_two(self):
        frame = self.frame()
        tears_only = world.encode(frame)
        frame["npcs"] = [dict(type=244, variant=0, subtype=0, seed=2078110152, body=[80, 160, 0, 0], target=[80, 160],
                              hp=[10, 10], state=8, flags=[1, 5, 4]),
                         dict(type=244, variant=0, subtype=0, seed=2403336305, body=[560, 160, 0.5, -0.25], target=[0, 0],
                              hp=[7.5, 10], state=4, flags=[0, 5, 0])]
        packet = world.encode(frame)
        self.assertEqual((tears_only[4], packet[4], len(packet)), (1, 2, len(tears_only) + 2 * world.NPC.size))
        self.assertEqual(tears_only[8:88], packet[8:88])
        self.assertEqual(world.decode(packet), frame)
        self.assertNotIn("npcs", world.decode(tears_only))
        self.assertTrue(world.same_npc(world.decode(packet)["npcs"][1], frame["npcs"][1]))
        for length in range(len(packet)):
            with self.assertRaises(ValueError):
                world.decode(packet[:length])
        for offset, value in ((4, 1), (88, 3), (92, 1)):
            bad = bytearray(packet); bad[offset] = value
            with self.assertRaises(ValueError):
                world.decode(bad)
        bad = bytearray(tears_only); bad[4] = 2
        with self.assertRaises(ValueError):
            world.decode(bad)
        for change in (dict(seed=2078110152), dict(type=2), dict(type=1000), dict(hp=[float("inf"), 10]), dict(flags=[2, 0, 0])):
            broken = self.frame(); broken["npcs"] = [dict(npc) for npc in frame["npcs"]]
            broken["npcs"][1].update(change)
            with self.assertRaises(ValueError):
                world.encode(broken)
        crowded = self.frame()
        crowded["npcs"] = [dict(frame["npcs"][0], seed=seed) for seed in range(1, world.MAX_NPCS + 2)]
        with self.assertRaises(ValueError):
            world.encode(crowded)
        self.assertEqual((world.MAX_BYTES, world.SLOT_BYTES), (3040, 3072))

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
