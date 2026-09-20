import importlib.util
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location("isaac_save", Path(__file__).parents[1] / "isaac_save.py")
save = importlib.util.module_from_spec(spec)
spec.loader.exec_module(save)

# Element counts of a J460 save; the size word of a chunk is four times the count, except for the achievements.
COUNTS = {1: 642, 2: 523, 3: 14, 4: 733, 5: 7, 6: 104, 7: 46, 8: 27, 9: 2, 10: 80}


def sample(word=7, tail=b"\x0b\0\0\0" + bytes(44), **values):
    """A save with everything zero except chunk_<type>={index: value}."""
    chunks = {}
    for kind, count in COUNTS.items():
        elements = [0] * count
        for index, value in values.get(f"chunk_{kind}", {}).items():
            elements[index] = value
        chunks[kind] = (count if kind == 1 else count * 4, elements)
    return dict(word=word, chunks=chunks, tail=tail)


class SaveTests(unittest.TestCase):
    def test_checksum_is_the_games_own_variant_of_crc32(self):
        # Verified against four real J460 saves: the table is built with arithmetic shifts, the seed is 0xFEDCBA76.
        # A standard CRC-32 table would have 0x77073096 and 0x2D02EF8D at indices 1 and 255.
        self.assertEqual((save.TABLE[0], save.TABLE[1], save.TABLE[128], save.TABLE[255]), (0, 0x09073096, 0xEDB88320, 0x0702EF8D))
        self.assertEqual(save.checksum(b""), 0xFEDCBA76)
        self.assertNotEqual(save.checksum(b"\0"), save.checksum(b"\1"))

    def test_roundtrip_and_a_file_of_the_loaders_size(self):
        original = sample(chunk_1={0: 1, 641: 1}, chunk_2={5: 1234567}, chunk_9={0: 1})
        data = save.build(original)
        # Chunks 1 to 10 always end at byte 4016 in a J460 save.
        self.assertEqual(len(data) - 4 - len(original["tail"]), 4016)
        self.assertEqual(save.parse(data), original)
        self.assertEqual(save.build(save.parse(data)), data)

    def test_damage_is_noticed(self):
        data = bytearray(save.build(sample(chunk_1={3: 1})))
        for offset in (0, 16, 40, len(data) - 1):
            broken = bytearray(data); broken[offset] ^= 1
            with self.assertRaises(ValueError):
                save.parse(bytes(broken))
        with self.assertRaises(ValueError):
            save.parse(bytes(data[:-8]))
        # A valid checksum over a chunk list that is out of order is still refused.
        body = bytearray(data[16:-4]); struct.pack_into("<I", body, 4, 2)
        with self.assertRaises(ValueError):
            save.parse(save.HEADER + bytes(body) + struct.pack("<I", save.checksum(bytes(body))))

    def test_a_lobby_shares_what_everybody_has(self):
        host = sample(word=11, tail=b"\x0b\0\0\0host bestiary", chunk_1={0: 1, 1: 1, 2: 1}, chunk_2={7: 600, 8: 3}, chunk_6={4: 1}, chunk_9={0: 1, 1: 5})
        second = sample(word=22, chunk_1={1: 1, 2: 1, 3: 1}, chunk_2={7: 20, 8: 9}, chunk_6={4: 1, 5: 1}, chunk_9={1: 2})
        third = sample(chunk_1={2: 1}, chunk_2={7: 100, 8: 9}, chunk_6={4: 1})
        merged = save.merge([host, second, third])
        self.assertEqual([i for i, v in enumerate(merged["chunks"][1][1]) if v], [2])           # unlocked by all three
        self.assertEqual((merged["chunks"][2][1][7], merged["chunks"][2][1][8]), (20, 3))      # e.g. coins donated to the shop
        self.assertEqual([i for i, v in enumerate(merged["chunks"][6][1]) if v], [4])
        # Settings, the bestiary and the unchecked word are nobody's progress: the host's.
        self.assertEqual((merged["chunks"][9][1], merged["tail"], merged["word"]), ([1, 5], b"\x0b\0\0\0host bestiary", 11))
        self.assertEqual(save.parse(save.build(merged)), merged)
        self.assertEqual(save.merge([host]), host)
        self.assertEqual(save.merge([host, host]), host)
        # The order of the guests does not matter.
        self.assertEqual(save.merge([host, third, second]), merged)

    def test_saves_of_another_layout_are_not_merged(self):
        other = sample(); other["chunks"][1] = (641, other["chunks"][1][1][:-1])
        with self.assertRaises(ValueError):
            save.merge([sample(), other])
        with self.assertRaises(ValueError):
            save.merge([])

    def test_summary_counts_what_is_set(self):
        rows = save.summary(sample(chunk_1={0: 1, 9: 1}, chunk_2={1: 40}))
        self.assertEqual((rows["achievements"], rows["counters"]["total"]), (dict(elements=642, set=2, total=2), 40))


if __name__ == "__main__":
    unittest.main()
