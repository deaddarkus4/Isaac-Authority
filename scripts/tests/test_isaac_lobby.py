import base64
import hashlib
import struct
import sys
import tempfile
import threading
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parents[1]))
import isaac_level as level  # noqa: E402
import isaac_lobby as lobby  # noqa: E402
import isaac_save as save  # noqa: E402

COUNTS = {1: 642, 2: 523, 3: 14, 4: 733, 5: 7, 6: 104, 7: 46, 8: 27, 9: 2, 10: 80}
SEED = "CZH4 8E6W"


def sample(settings=0, bestiary=None, **values):
    chunks = {}
    for kind, count in COUNTS.items():
        elements = [0] * count
        for index, value in values.get(f"chunk_{kind}", {}).items():
            elements[index] = value
        chunks[kind] = (count if kind == 1 else count * 4, elements)
    chunks[9][1][0] = settings
    return dict(word=7, chunks=chunks, bestiary={which: list((bestiary or {}).get(which, [])) for which in (4, 2, 3, 1)}, trailer=b"\x0b\0\0\0")


HOST = sample(settings=1, chunk_1={1: 1, 2: 1, 40: 1}, chunk_2={20: 90}, chunk_4={5: 1}, bestiary={1: [(10, 4), (20, 1)], 3: [(1, 1)]})
GUEST = sample(settings=0, chunk_1={2: 1, 40: 1, 41: 1}, chunk_2={20: 30}, chunk_4={5: 1, 6: 1}, bestiary={1: [(10, 9)], 3: [(2, 2)]})


def packed(own):
    return save.encode_shared(save.shared_view(own))


def facts(flags, seed=SEED, difficulty="hard", rooms="a"):
    return dict(seed=seed, difficulty=difficulty, level=hashlib.sha256(rooms.encode()).hexdigest(), curses=[], rooms=15, flagsSha256=flags)


class LobbyTests(unittest.TestCase):
    def started(self):
        one = lobby.Lobby("J460")
        host, guest = one.join("host", packed(HOST), game="J460"), one.join("guest", packed(GUEST), game="J460")
        one.start(host, "czh48e6w", "hard")
        return one, host, guest

    def test_who_may_join(self):
        one = lobby.Lobby("J460")
        host = one.join(" host ", packed(HOST), game="J460")
        for reason, arguments in (("protocol", dict(name="a", protocol=2, game="J460")), ("game", dict(name="a", game="J273")), ("name", dict(name="host", game="J460")),
                                  ("name", dict(name=" ", game="J460")), ("name", dict(name="x" * 33, game="J460"))):
            with self.assertRaises(lobby.Refused) as refusal:
                one.join(packed=packed(GUEST), **arguments)
            self.assertEqual(refusal.exception.reason, reason)
        for broken in (b"", packed(GUEST)[:-1], packed(GUEST) + b"\0"):
            with self.assertRaises(lobby.Refused) as refusal:
                one.join("a", broken, game="J460")
            self.assertEqual(refusal.exception.reason, "save")
        for name in "bcd":
            one.join(name, packed(GUEST), game="J460")
        with self.assertRaises(lobby.Refused) as refusal:
            one.join("e", packed(GUEST), game="J460")
        self.assertEqual((refusal.exception.reason, [m["name"] for m in one.members], one.members[0]["token"]), ("full", ["host", "b", "c", "d"], host))

    def test_the_host_starts_and_everyone_gets_the_same_shared_save(self):
        one = lobby.Lobby("J460")
        host, guest = one.join("host", packed(HOST), game="J460"), one.join("guest", packed(GUEST), game="J460")
        with self.assertRaises(lobby.Refused) as refusal:
            one.ticket(host)
        self.assertEqual(refusal.exception.reason, "open")
        for reason, arguments in (("host", (guest, SEED, "hard")), ("seed", (host, "AAAA AAAA", "hard")), ("difficulty", (host, SEED, "easy")), ("token", ("nobody", SEED, "hard"))):
            with self.assertRaises(lobby.Refused) as refusal:
                one.start(*arguments)
            self.assertEqual((refusal.exception.reason, one.state), (reason, "open"))
        one.start(host, "czh48e6w", "hard")
        first, second = one.ticket(host), one.ticket(guest)
        self.assertEqual((first["controller"], second["controller"], first["members"], first["seed"], first["difficulty"]), (0, 1, ["host", "guest"], SEED, "hard"))
        self.assertEqual({key: first[key] for key in first if key != "controller"}, {key: second[key] for key in second if key != "controller"})
        shared = save.decode_shared(base64.b64decode(first["shared"]))
        self.assertEqual(([n for n, v in enumerate(shared["chunks"][1]) if v], shared["chunks"][2][20], shared["bestiary"][1]), ([2, 40], 30, [(10, 4)]))
        self.assertEqual((first["sharedSha256"], first["flagsSha256"]), (hashlib.sha256(base64.b64decode(first["shared"])).hexdigest(), lobby.flags_digest(shared)))
        self.assertTrue(0 < int(first["session"]) < 2 ** 64)

    def test_nobody_joins_a_match_that_has_begun(self):
        one, host, guest = self.started()
        for state in ("started", "running"):
            self.assertEqual(one.state, state)
            with self.assertRaises(lobby.Refused) as refusal:
                one.join("late", packed(GUEST), game="J460")
            self.assertEqual(refusal.exception.reason, "started")
            with self.assertRaises(lobby.Refused):
                one.start(host, SEED, "hard")
            if state == "started":
                flags = one.ticket(host)["flagsSha256"]
                self.assertIsNone(one.ready(host, facts(flags)))
                self.assertEqual(one.ready(guest, facts(flags))["type"], "go")
        # Who leaves a running match is gone; the match goes on and the slot stays taken.
        self.assertEqual((one.leave(guest)["type"], one.state, len(one.members)), ("go", "running", 2))
        with self.assertRaises(lobby.Refused):
            one.ready(guest, facts("x"))

    def test_a_game_that_is_not_in_the_hosts_run_stops_the_match(self):
        for wrong, key in ((dict(seed="KAKG ZR44"), "seed"), (dict(difficulty="normal"), "difficulty"), (dict(rooms="b"), "level"), (dict(flags="00"), "flagsSha256")):
            one, host, guest = self.started(); flags = one.ticket(host)["flagsSha256"]
            one.ready(host, facts(flags))
            verdict = one.ready(guest, facts(wrong.get("flags", flags), **{k: v for k, v in wrong.items() if k != "flags"}))
            self.assertEqual((verdict["type"], one.state, list(verdict["differences"]), list(verdict["differences"]["guest"])), ("abort", "aborted", ["guest"], [key]))
        # The host's own game is held to the lobby's seed as well.
        one, host, guest = self.started(); flags = one.ticket(host)["flagsSha256"]
        one.ready(guest, facts(flags))
        self.assertEqual(one.ready(host, facts(flags, seed="KAKG ZR44"))["differences"], {"host": {"seed": ["KAKG ZR44", SEED]}})

    def test_leaving_before_the_match_runs(self):
        one = lobby.Lobby("J460")
        host, guest = one.join("host", packed(HOST), game="J460"), one.join("guest", packed(GUEST), game="J460")
        self.assertIsNone(one.leave(guest))
        self.assertEqual(([m["name"] for m in one.members], one.state), (["host"], "open"))
        self.assertEqual((one.leave(host)["type"], one.state), ("abort", "aborted"))
        one, host, guest = self.started()
        self.assertEqual((one.leave(guest)["type"], one.state), ("abort", "aborted"))

    def test_a_member_plays_from_the_shared_save_laid_over_their_own(self):
        one, host, guest = self.started()
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            files = {name: save.parse(lobby.prepare(folder / name, own, one.ticket(token)).read_bytes()) for name, own, token in (("host", HOST, host), ("guest", GUEST, guest))}
            self.assertEqual(save.shared_view(files["host"]), save.shared_view(files["guest"]))
            self.assertEqual(dict(chunks=save.shared_view(files["host"])["chunks"], bestiary=save.shared_view(files["host"])["bestiary"]), one.shared)
            # Settings and the bestiary maps the game does not share stay each member's own.
            self.assertEqual((files["host"]["chunks"][9][1][0], files["guest"]["chunks"][9][1][0], files["host"]["bestiary"][3], files["guest"]["bestiary"][3]), (1, 0, [(1, 1)], [(2, 2)]))
            self.assertNotIn('"shared"', (folder / "host" / "ticket.json").read_text(encoding="utf-8"))
            with self.assertRaises(ValueError):
                lobby.prepare(folder / "host", HOST, one.ticket(host))
            with self.assertRaises(ValueError):
                lobby.prepare(folder / "other", HOST, dict(one.ticket(host), sharedSha256="0" * 64))

    def test_a_transport_attaches_only_games_of_one_running_match(self):
        go = dict(type="go", session="77", level="ab")
        host, client = dict(verdict=go, pid=10, controller=0), dict(verdict=go, pid=20, controller=1)
        self.assertEqual(lobby.admit(host, client, 10, 20), 1)
        for broken in ((dict(host, verdict=dict(type="abort")), client, 10, 20), (dict(host, verdict=None), client, 10, 20), (host, dict(client, verdict=dict(go, session="78")), 10, 20),
                       (host, dict(client, verdict=dict(go, level="cd")), 10, 20), (client, host, 20, 10), (host, dict(client, controller=4), 10, 20),
                       (host, client, 10, 21), (host, client, 20, 10), (host, dict(client, controller=0), 10, 20)):
            with self.assertRaises(ValueError):
                lobby.admit(*broken)

    def test_facts_of_a_running_game(self):
        game, manager, base = 0x1000000, 0x3000000, 0x400000
        words = {base + level.GAME_RVA: game, base + level.MANAGER_RVA: manager, game: 1, game + 4: 0, game + level.START_SEED: level.seed_value(SEED),
                 game + level.ROOM_INDEX: 84, game + level.DIMENSION: 0, game + level.ROOM_COUNT: 1, game + level.ROOM_TRANSITION: 0,
                 game + level.DIFFICULTY: 1, game + level.CURSES: 1, game + level.ROOMS: 84, game + level.ROOMS + 0xC: 0, game + level.ROOMS + 0x10: 0x2000000,
                 game + level.ROOMS + 0x40: 0, game + level.ROOMS + 0x5C: 11, 0x2000008: 1, 0x200000C: 2, 0x2000010: 0, 0x2000048: 1}
        live = bytearray(0x1000); merged = save.merge([HOST, GUEST])
        for kind, offset in lobby.LIVE_FLAGS.items():
            live[lobby.LIVE_SAVE + offset:lobby.LIVE_SAVE + offset + COUNTS[kind]] = bytes(merged["chunks"][kind][1])

        def read(address, size):
            return bytes(live[address - manager:address - manager + size]) if manager <= address < manager + len(live) else struct.pack("<I", words[address])

        found = lobby.game_facts(read, base)
        self.assertEqual((found["seed"], found["difficulty"], found["curses"], found["rooms"]), (SEED, "hard", ["Darkness"], 1))
        self.assertEqual((found["flagsSha256"], found["level"]), (lobby.flags_digest(save.shared_view(merged)), level.fingerprint(level.snapshot(read, base))))
        live[lobby.LIVE_SAVE + lobby.LIVE_FLAGS[6] + 3] = 1     # a game started from a save with one more boss
        self.assertNotEqual(lobby.game_facts(read, base)["flagsSha256"], found["flagsSha256"])

    def test_over_tcp_a_latecomer_is_told_why(self):
        server = lobby.Server(("127.0.0.1", 0), lobby.Lobby("J460")); threading.Thread(target=server.serve_forever, daemon=True).start()
        try:
            address = server.server_address
            with server.lock:
                host = server.lobby.join("host", packed(HOST), game="J460")
            with self.assertRaises(lobby.Refused) as refusal:
                lobby.Client(address, "guest", packed(GUEST), "J273")
            self.assertEqual(refusal.exception.reason, "game")
            guest = lobby.Client(address, "guest", packed(GUEST), "J460")
            self.assertTrue(server.wait(lambda one: len(one.members) == 2, 5))
            with server.lock:
                server.lobby.start(host, SEED, "hard"); flags = server.lobby.ticket(host)["flagsSha256"]
            server.announce()
            ticket = guest.receive(5, ("start",))
            self.assertEqual((ticket["type"], ticket["controller"], ticket["members"], ticket["flagsSha256"]), ("start", 1, ["host", "guest"], flags))
            with self.assertRaises(lobby.Refused) as refusal:
                lobby.Client(address, "late", packed(GUEST), "J460")
            self.assertEqual(refusal.exception.reason, "started")
            guest.ready(facts(flags))
            with server.lock:
                server.lobby.ready(host, facts(flags))
            server.announce()
            self.assertTrue(server.wait(lambda one: one.verdict is not None, 5))
            self.assertEqual((guest.receive(5, ("go",))["type"], server.lobby.state), ("go", "running"))
            with self.assertRaises(lobby.Refused) as refusal:
                lobby.Client(address, "later", packed(GUEST), "J460")
            self.assertEqual(refusal.exception.reason, "started")
            guest.close()
        finally:
            server.shutdown(); server.server_close()

    def test_over_tcp_a_member_who_drops_before_the_run_stops_the_match(self):
        server = lobby.Server(("127.0.0.1", 0), lobby.Lobby("J460")); threading.Thread(target=server.serve_forever, daemon=True).start()
        try:
            with server.lock:
                host = server.lobby.join("host", packed(HOST), game="J460")
            guest = lobby.Client(server.server_address, "guest", packed(GUEST), "J460")
            self.assertTrue(server.wait(lambda one: len(one.members) == 2, 5))
            with server.lock:
                server.lobby.start(host, SEED, "hard")
            server.announce(); guest.receive(5, ("start",)); guest.stream.close(); guest.socket.close()   # the process died: no goodbye
            self.assertTrue(server.wait(lambda one: one.verdict is not None, 5))
            self.assertEqual((server.lobby.state, server.lobby.verdict["type"]), ("aborted", "abort"))
        finally:
            server.shutdown(); server.server_close()


if __name__ == "__main__":
    unittest.main()
