import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("isaac_link", Path(__file__).parents[1] / "isaac_link.py")
link = importlib.util.module_from_spec(spec)
spec.loader.exec_module(link)


class LinkTests(unittest.TestCase):
    def path(self, **options):
        out = []
        return link.Link(lambda packet, tag: out.append((packet, tag)), **options), out

    def test_a_perfect_path_delivers_at_once_and_in_order(self):
        path, out = self.path()
        for n in range(5):
            self.assertTrue(path.send(bytes([n]), 1000 + n, tag=n))
            self.assertEqual(path.flush(1000 + n), 1)
        self.assertEqual([tag for _, tag in out], [0, 1, 2, 3, 4])
        self.assertEqual(path.report(), dict(delayMs=0, jitterMs=0, lossPercent=0, offered=5, dropped=0, delivered=5, reordered=0, inFlightAtEnd=0))

    def test_delay_holds_a_packet_until_its_time(self):
        path, out = self.path(delay_ms=80)
        path.send(b"a", 1000)
        self.assertEqual((path.flush(1079), out), (0, []))
        self.assertEqual((path.flush(1080), out), (1, [(b"a", None)]))
        path.send(b"b", 2000)
        self.assertEqual(path.report()["inFlightAtEnd"], 1)

    def test_jitter_reorders_and_loss_drops_reproducibly(self):
        def run(seed):
            path, out = self.path(delay_ms=50, jitter_ms=40, loss_percent=20, seed=seed)
            for n in range(400):
                path.send(b"", 1000 + n * 16, tag=n)
                path.flush(1000 + n * 16)
            path.flush(10**9)
            return path.report(), [tag for _, tag in out]
        report, order = run(7)
        self.assertEqual(run(7), (report, order))
        self.assertNotEqual(run(8)[1], order)
        self.assertEqual(report["offered"], 400)
        self.assertEqual(report["dropped"] + report["delivered"], 400)
        self.assertTrue(50 < report["dropped"] < 110 and report["reordered"] > 10 and order != sorted(order))
        self.assertEqual(sorted(order), sorted(set(order)))

    def test_the_whole_delay_stays_inside_the_modules_freshness_window(self):
        for bad in (dict(delay_ms=150, jitter_ms=60), dict(delay_ms=-1), dict(jitter_ms=-1), dict(loss_percent=100), dict(loss_percent=-1)):
            with self.assertRaises(ValueError):
                link.Link(lambda *_: None, **bad)
        link.Link(lambda *_: None, delay_ms=150, jitter_ms=50, loss_percent=99.9)


if __name__ == "__main__":
    unittest.main()
