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
        self.assertEqual(commands.decode(commands.encode(77, 1, 1000, controller=1, buttons=("join", "menuRight")))["buttons"], ["join", "menuRight"])

    def slot(self, packet, generation=2, alive=1, session=77):
        return commands.SLOT.pack(0x31534E49, 1, generation, alive, len(packet), 0, session) + packet

    def test_client_command_slot(self):
        packet = commands.encode(77, 9, 1000, move=(-1, 0), buttons=("bomb",))
        blob = self.slot(packet)
        descriptor = dict(address=0x10000, session="77", bytes=88)
        def read(address, size):
            return blob[8:12] if size == 4 else blob
        command, data = commands.read_slot(read, descriptor, 1000)
        self.assertEqual((command["sequence"], command["controller"], command["move"], command["buttons"], data), (9, 0, [-1, 0], ["bomb"], packet))
        self.assertIsNone(commands.read_slot(read, descriptor, 1251))
        self.assertIsNone(commands.read_slot(read, descriptor, 999))
        for bad in (dict(descriptor, session="78"), dict(descriptor, bytes=3168)):
            with self.assertRaises(ValueError):
                commands.read_slot(read, bad, 1000)
        for blob in (self.slot(packet, generation=3), self.slot(packet, alive=0)):  # being written; stopped
            self.assertIsNone(commands.read_slot(read, descriptor, 1000))
        count = 0
        def racing(address, size):
            nonlocal count
            if size == 4:
                count += 1
                return (count * 2).to_bytes(4, "little")
            return self.slot(packet)
        self.assertIsNone(commands.read_slot(racing, descriptor, 1000))

    def test_the_transport_assigns_session_and_controller(self):
        captured = commands.encode(5, 9, 1000, move=(1, 0), shoot=(0, -1), buttons=("item",))
        routed = commands.decode(commands.route(captured, 77, 1))
        self.assertEqual(routed, dict(session=77, sequence=9, timeMs=1000, controller=1, move=[1, 0], shoot=[0, -1], buttons=["item"]))
        for session, controller in ((0, 1), (77, 8), (77, -1)):
            with self.assertRaises(ValueError):
                commands.route(captured, session, controller)
        with self.assertRaises(ValueError):
            commands.route(captured[:-1], 77, 1)


if __name__ == "__main__":
    unittest.main()
