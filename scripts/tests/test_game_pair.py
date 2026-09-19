import importlib.util
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location("game_pair", Path(__file__).parents[1] / "Test-IsaacGamePair.py")
pair = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pair)


class SourceSlotTests(unittest.TestCase):
    def blob(self, **changes):
        values = dict(magic=0x43525349, version=1, generation=2, alive=1, sequence=3,
                      seed=42, room=0x10000, player=0x20000, stamp=1000,
                      x=320, y=280, vx=2, vy=-1, thread=12, token=99)
        values.update(changes)
        return pair.SOURCE.pack(*values.values())

    def sample(self, blob, now=1000, generation=None):
        def read(address, size):
            if size == 4:
                return struct.pack("<I", generation) if generation is not None else blob[8:12]
            return blob
        return pair.read_source(read, 0x30000, 99, now)

    def test_game_thread_slot_and_replica_wire(self):
        state = self.sample(self.blob())
        self.assertEqual(state["body"], [320, 280, 2, -1])
        packet = pair.encode(dict(session="123", epoch=1, seed=7), state)
        fields = pair.PACKET.unpack(packet)
        self.assertEqual(fields[:9], (0x50414E53, 1, 2, 123, 1, 7, 3, 0, 1000))
        self.assertEqual(fields[9:13], (320, 280, 2, -1))

    def test_in_progress_or_inconsistent_copy(self):
        self.assertIsNone(self.sample(self.blob(generation=3)))
        self.assertIsNone(self.sample(self.blob(), generation=4))
        count = 0
        def read(address, size):
            nonlocal count
            if size == 4:
                count += 1
                return struct.pack("<I", 2 if count == 1 else 4)
            return self.blob()
        self.assertIsNone(pair.read_source(read, 0x30000, 99, 1000))

    def test_stopped_paused_and_future_source(self):
        self.assertIsNone(self.sample(self.blob(alive=0)))
        self.assertIsNone(self.sample(self.blob(), now=1251))
        self.assertIsNone(self.sample(self.blob(), now=999))

    def test_wrong_attachment_and_invalid_body(self):
        for changes in (dict(token=98), dict(version=2), dict(x=float("nan")), dict(vx=21), dict(y=450)):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                self.sample(self.blob(**changes))


if __name__ == "__main__":
    unittest.main()
