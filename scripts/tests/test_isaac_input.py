import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("isaac_input", Path(__file__).parents[1] / "isaac_input.py")
commands = importlib.util.module_from_spec(spec)
spec.loader.exec_module(commands)


class InputTests(unittest.TestCase):
    def test_roundtrip(self):
        packet = commands.encode(77, 5, 1000, controller=1, move=(1, 0), shoot=(0, -0.75), buttons=("bomb", "drop"))
        self.assertEqual(len(packet), 56)
        self.assertEqual(commands.decode(packet), dict(session=77, sequence=5, timeMs=1000, controller=1, move=[1, 0],
                                                       shoot=[0, -0.75], buttons=["bomb", "drop"]))
        for length in range(len(packet)):
            with self.assertRaises(ValueError):
                commands.decode(packet[:length])
        for offset in (0, 4, 52):
            bad = bytearray(packet); bad[offset] ^= 0x55
            with self.assertRaises(ValueError):
                commands.decode(bad)

    def test_a_client_cannot_ask_for_more_than_full_tilt(self):
        for bad in (dict(move=(1.5, 0)), dict(shoot=(0, float("nan"))), dict(controller=8), dict(controller=-1), dict(sequence=0)):
            arguments = dict(session=77, sequence=1, time_ms=1000); arguments.update(bad)
            with self.assertRaises(ValueError):
                commands.encode(**arguments)
        with self.assertRaises(ValueError):
            commands.encode(77, 1, 1000, buttons=("pause",))


if __name__ == "__main__":
    unittest.main()
