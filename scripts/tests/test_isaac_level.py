import importlib.util
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location("isaac_level", Path(__file__).parents[1] / "isaac_level.py")
level = importlib.util.module_from_spec(spec)
spec.loader.exec_module(level)

BASE, GAME, CONFIGS = 0x400000, 0x1000000, 0x2000000


def memory(seed, index, rooms, difficulty=1, curses=0):
    """Sparse fake process: (grid, type, variant, shape, spawnSeed, visits) per room."""
    cells = {BASE + level.GAME_RVA: GAME, GAME: 1, GAME + 4: 0, GAME + level.START_SEED: seed,
             GAME + level.ROOM_INDEX: index, GAME + level.DIMENSION: 0, GAME + level.ROOM_COUNT: len(rooms),
             GAME + level.ROOM_TRANSITION: 0, GAME + level.DIFFICULTY: difficulty, GAME + level.CURSES: curses}
    for slot, (grid, kind, variant, shape, spawn, visits) in enumerate(rooms):
        descriptor, config = GAME + level.ROOMS + slot * level.ROOM_STRIDE, CONFIGS + slot * 0x100
        cells.update({descriptor: grid, descriptor + 0xC: 0, descriptor + 0x10: config, descriptor + 0x40: visits,
                      descriptor + 0x5C: spawn, config + 8: kind, config + 0xC: variant, config + 0x10: 0, config + 0x48: shape})
    return lambda address, size: struct.pack("<I", cells[address])


ROOMS = [(84, 1, 2, 1, 11, 1), (85, 1, 300, 1, 12, 0), (83, 6, 2037, 1, 13, 0), (97, 1, 297, 4, 14, 0), (71, 1, 70, 1, 15, 0)]


class LevelTests(unittest.TestCase):
    def test_seed_text_matches_game_logs(self):
        # Pairs printed by J460 itself: "RNG Start Seed: CZH4 8E6W (446746862)".
        for text, seed in (("CZH4 8E6W", 446746862), ("2PBN A7CZ", 3433938825)):
            self.assertEqual(level.seed_text(seed), text)
            self.assertEqual(level.seed_value(text), seed)
            self.assertEqual(level.seed_value(text.replace(" ", "").lower()), seed)
        for bad in ("CZH4 8E6X", "CZH4 8E6", "CZH4 8E6I"):
            with self.assertRaises(ValueError):
                level.seed_value(bad)
        with self.assertRaises(ValueError):
            level.seed_text(0)

    def test_same_seed_levels_ignore_visit_history(self):
        host = level.snapshot(memory(446746862, 84, ROOMS), BASE)
        visited = [room[:5] + (room[5] + 3,) for room in reversed(ROOMS)]
        replica = level.snapshot(memory(446746862, 84, visited), BASE)
        self.assertEqual(level.differences(host, replica), [])
        other = level.snapshot(memory(3433938825, 84, ROOMS[:-1] + [(71, 1, 71, 1, 15, 0)]), BASE)
        found = level.differences(host, other)
        self.assertEqual(len(found), 2)
        self.assertTrue(found[0].startswith("startSeed") and found[1].startswith("rooms"))

    def test_one_seed_with_different_saves_shares_only_some_doors(self):
        # Observed with seed CZH4 8E6W: the miniboss sits left of the start for one save and right of it for the other.
        host = [(84, 1, 2, 1, 11, 1), (83, 6, 2037, 1, 21, 0), (85, 1, 1071, 1, 22, 0), (97, 1, 297, 1, 23, 0)]
        replica = [(84, 1, 2, 1, 11, 1), (83, 1, 640, 1, 24, 0), (85, 6, 2037, 1, 21, 0), (97, 1, 297, 1, 23, 5)]
        a, b = level.snapshot(memory(446746862, 84, host), BASE), level.snapshot(memory(446746862, 84, replica), BASE)
        self.assertEqual(level.run_differences(a, b), [])
        self.assertEqual(level.room_differences(a, b), [(83, 0), (85, 0)])
        self.assertEqual(sorted(level.doors(a)), ["down", "right"])
        self.assertEqual({name: room["grid"] for name, room in level.shared_doors(a, b).items()}, {"down": 97})
        self.assertEqual(level.room_differences(a, level.snapshot(memory(446746862, 84, host[:-1]), BASE)), [(97, 0)])

    def test_a_curse_or_a_difficulty_is_part_of_the_run(self):
        # Observed with seed CZH4 8E6W on hard: Curse of Darkness with a progressed save, no curse with a clean one.
        host = level.snapshot(memory(446746862, 84, ROOMS, curses=1), BASE)
        clean = level.snapshot(memory(446746862, 84, ROOMS), BASE)
        self.assertEqual(level.run_differences(host, clean), ["curses: 1 != 0"])
        self.assertEqual(level.run_differences(host, level.snapshot(memory(446746862, 84, ROOMS, curses=1), BASE)), [])
        self.assertEqual(level.run_differences(clean, level.snapshot(memory(446746862, 84, ROOMS, difficulty=0), BASE)), ["difficulty: 1 != 0"])
        self.assertEqual((level.curse_names(0), level.curse_names(1), level.curse_names(0b1000010), level.curse_names(0x100)),
                         ([], ["Darkness"], ["Labyrinth", "Blind"], ["0x100"]))

    def test_doors_only_lead_to_ordinary_single_rooms(self):
        host = level.snapshot(memory(446746862, 84, ROOMS), BASE)
        # 83 is a miniboss room and 97 is a large room; both are left to later stages.
        self.assertEqual({name: room["grid"] for name, room in level.doors(host).items()}, {"right": 85, "up": 71})
        edge = level.snapshot(memory(446746862, 90, [(90, 1, 2, 1, 1, 1), (91, 1, 5, 1, 2, 0), (89, 1, 6, 1, 3, 0)]), BASE)
        self.assertEqual(list(level.doors(edge)), ["left"])

    def test_invalid_layout_is_rejected(self):
        with self.assertRaises(ValueError):
            level.snapshot(memory(1, 84, []), BASE)
        broken = memory(1, 84, ROOMS)
        with self.assertRaises(ValueError):
            level.snapshot(lambda address, size: struct.pack("<I", 0) if address == BASE + level.GAME_RVA else broken(address, size), BASE)


if __name__ == "__main__":
    unittest.main()
