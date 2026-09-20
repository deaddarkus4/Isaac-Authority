import importlib.util
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location("isaac_save", Path(__file__).parents[1] / "isaac_save.py")
save = importlib.util.module_from_spec(spec)
spec.loader.exec_module(save)

# Element counts of a J460 save; the size word of a chunk is four times the count, except for the achievements.
COUNTS = {1: 642, 2: 523, 3: 14, 4: 733, 5: 7, 6: 104, 7: 46, 8: 27, 9: 2, 10: 80}
# A real file keeps its four bestiary maps in this order, not in ascending one.
MAP_ORDER = (4, 2, 3, 1)


def sample(word=7, trailer=b"\x0b\0\0\0", bestiary=None, **values):
    """A save with everything zero except chunk_<type>={index: value} and bestiary={map: [(key, value), ...]}."""
    chunks = {}
    for kind, count in COUNTS.items():
        elements = [0] * count
        for index, value in values.get(f"chunk_{kind}", {}).items():
            elements[index] = value
        chunks[kind] = (count if kind == 1 else count * 4, elements)
    return dict(word=word, chunks=chunks, bestiary={which: list((bestiary or {}).get(which, [])) for which in MAP_ORDER}, trailer=trailer)


class SaveTests(unittest.TestCase):
    def test_checksum_is_the_games_own_variant_of_crc32(self):
        # Verified against a hundred real J460 saves: the table is built with arithmetic shifts, the seed is 0xFEDCBA76.
        # A standard CRC-32 table would have 0x77073096 and 0x2D02EF8D at indices 1 and 255.
        self.assertEqual((save.TABLE[0], save.TABLE[1], save.TABLE[128], save.TABLE[255]), (0, 0x09073096, 0xEDB88320, 0x0702EF8D))
        self.assertEqual(save.checksum(b""), 0xFEDCBA76)
        self.assertNotEqual(save.checksum(b"\0"), save.checksum(b"\1"))

    def test_roundtrip_and_a_file_of_the_loaders_size(self):
        original = sample(chunk_1={0: 1, 641: 1}, chunk_2={5: 1234567}, chunk_9={0: 1},
                          bestiary={4: [(0x00A00000, 2)], 1: [(0x00A00000, 3), (0x00B00000, 1)], 2: [(-5, 9)]})
        data = save.build(original)
        # Chunks 1 to 10 always end at byte 4016 in a J460 save; then the bestiary header, four map headers, four entries.
        self.assertEqual(len(data), 4016 + 12 + 4 * 8 + 4 * 8 + len(original["trailer"]) + 4)
        parsed = save.parse(data)
        self.assertEqual(parsed, original)
        # The maps stay in the order of the file, or the rebuilt file would not be the same bytes.
        self.assertEqual(list(parsed["bestiary"]), list(MAP_ORDER))
        self.assertEqual(save.build(parsed), data)
        # The size word of the bestiary is four times the number of its entries.
        self.assertEqual(struct.unpack_from("<3I", data, 4016), (save.BESTIARY, 16, 4))

    def test_damage_is_noticed(self):
        data = bytearray(save.build(sample(chunk_1={3: 1}, bestiary={1: [(7, 1)]})))
        for offset in (0, 16, 40, len(data) - 1):
            broken = bytearray(data); broken[offset] ^= 1
            with self.assertRaises(ValueError):
                save.parse(bytes(broken))
        with self.assertRaises(ValueError):
            save.parse(bytes(data[:-8]))

        def resealed(body):
            return save.HEADER + bytes(body) + struct.pack("<I", save.checksum(bytes(body)))

        # A valid checksum over a chunk list that is out of order is still refused,
        body = bytearray(data[16:-4]); struct.pack_into("<I", body, 4, 2)
        with self.assertRaises(ValueError):
            save.parse(resealed(body))
        # and so is one over a bestiary that names a map twice.
        body = bytearray(data[16:-4]); struct.pack_into("<I", body, 4016 - 16 + 12 + 8, 4)
        with self.assertRaises(ValueError):
            save.parse(resealed(body))

    def test_a_lobby_shares_what_everybody_has(self):
        host = sample(word=11, trailer=b"host", chunk_1={0: 1, 1: 1, 2: 1}, chunk_2={7: 600, 8: 3}, chunk_6={4: 1}, chunk_9={0: 1, 1: 5},
                      chunk_10={3: 2}, bestiary={1: [(10, 5), (20, 1), (30, 8)], 2: [(10, 2)], 3: [(10, 4)], 4: [(99, 1)]})
        second = sample(word=22, chunk_1={1: 1, 2: 1, 3: 1}, chunk_2={7: 20, 8: 9}, chunk_6={4: 1, 5: 1}, chunk_9={1: 2},
                        bestiary={1: [(10, 7), (30, 2), (40, 1)], 2: [(10, 1), (11, 1)], 3: [(10, 1)]})
        third = sample(chunk_1={2: 1}, chunk_2={7: 100, 8: 9}, chunk_6={4: 1}, bestiary={1: [(10, 6), (30, 3)], 2: [(10, 3)]})
        merged = save.merge([host, second, third])
        self.assertEqual([i for i, v in enumerate(merged["chunks"][1][1]) if v], [2])           # unlocked by all three
        self.assertEqual((merged["chunks"][2][1][7], merged["chunks"][2][1][8]), (20, 3))      # e.g. coins donated to the shop
        self.assertEqual([i for i, v in enumerate(merged["chunks"][6][1]) if v], [4])
        # A bestiary entry survives only if every member has it, with the smallest value; maps 3 and 4 are not shared.
        self.assertEqual((merged["bestiary"][1], merged["bestiary"][2]), ([(10, 5), (30, 2)], [(10, 1)]))
        self.assertEqual((merged["bestiary"][3], merged["bestiary"][4]), ([(10, 4)], [(99, 1)]))
        # Settings, special seeds and the words around the chunks are nobody's progress: the host's.
        self.assertEqual((merged["chunks"][9][1], merged["chunks"][10][1][3], merged["trailer"], merged["word"]), ([1, 5], 2, b"host", 11))
        self.assertEqual(save.parse(save.build(merged)), merged)
        self.assertEqual(save.merge([host]), host)
        self.assertEqual(save.merge([host, host]), host)
        # The order of the guests does not matter.
        self.assertEqual(save.merge([host, third, second]), merged)

    def test_counters_compare_as_the_game_compares_them(self):
        # The game takes the smallest counter as an unsigned number, and the smallest cutscene counter as a signed one.
        minus = 0xFFFFFFFF
        merged = save.merge([sample(chunk_2={0: minus}, chunk_3={0: minus}, chunk_8={0: minus}), sample(chunk_2={0: 5}, chunk_3={0: 5}, chunk_8={0: 5})])
        self.assertEqual((merged["chunks"][2][1][0], merged["chunks"][3][1][0], merged["chunks"][8][1][0]), (5, 5, minus))

    def test_saves_of_another_layout_are_not_merged(self):
        other = sample(); other["chunks"][1] = (641, other["chunks"][1][1][:-1])
        with self.assertRaises(ValueError):
            save.merge([sample(), other])
        with self.assertRaises(ValueError):
            save.merge([])

    def test_shared_save_wire_form(self):
        first = sample(chunk_1={0: 1, 9: 1, 641: 1}, chunk_2={21: 6}, chunk_4={732: 1}, chunk_5={6: 1}, chunk_6={0: 1, 103: 1}, chunk_7={45: 1},
                       chunk_8={26: 3}, chunk_9={0: 1}, bestiary={1: [(0x00A00000, 3), (0x00B00000, 1)], 2: [(0x00A00000, 2)], 3: [(1, 1)]})
        view = save.shared_view(first)
        raw = save.encode_shared(dict(view, members=[5, 6]))
        # Flags take (count >> 3) + 1 bytes: the 104 bosses fourteen, not thirteen. 2450 bytes before the bestiary.
        self.assertEqual((save.SHARED_FIXED, len(raw)), (2450, 2450 + 4 + 2 * 8 + 4 + 8 + 32))
        self.assertEqual((raw[0], raw[1], raw[80]), (0b00000001, 0b00000010, 0b00000010))
        self.assertEqual(struct.unpack_from("<I", raw, 81 + 21 * 4)[0], 6)
        decoded = save.decode_shared(raw)
        self.assertEqual((decoded["chunks"], decoded["bestiary"], decoded["members"]), (view["chunks"], view["bestiary"], [5, 6, 0, 0]))
        # What is not shared is not in it: settings, special seeds, bestiary maps 3 and 4.
        self.assertEqual((sorted(decoded["chunks"]), sorted(decoded["bestiary"])), ([1, 2, 3, 4, 5, 6, 7, 8], [1, 2]))
        # The shared save of a lobby is the shared part of the merged save.
        second = sample(chunk_1={9: 1}, chunk_2={21: 2}, bestiary={1: [(0x00B00000, 4)]})
        merged = save.shared_view(save.merge([first, second]))
        self.assertEqual(([i for i, v in enumerate(merged["chunks"][1]) if v], merged["chunks"][2][21], merged["bestiary"]), ([9], 2, {1: [(0x00B00000, 1)], 2: []}))
        for broken in (raw[:-1], raw + b"\0", raw[:2000], b""):
            with self.assertRaises(ValueError):
                save.decode_shared(broken)

    def test_summary_counts_what_is_set(self):
        one = sample(chunk_1={0: 1, 9: 1}, chunk_2={1: 40}, bestiary={1: [(3, 2), (4, 0)]})
        rows = save.summary(one)
        self.assertEqual((rows["achievements"], rows["counters"]["total"], rows["bestiaryMap1"]), (dict(elements=642, set=2, total=2), 40, dict(elements=2, set=1, total=2)))
        self.assertEqual(save.summary(save.decode_shared(save.encode_shared(save.shared_view(one))))["achievements"], rows["achievements"])


if __name__ == "__main__":
    unittest.main()
