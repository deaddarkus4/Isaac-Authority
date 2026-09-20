"""A lobby for the authoritative host prototype, shaped after the lobby of the game's own online.

In the game a run cannot be joined half way. Members gather in a lobby and each reports the shared part of their save; when
the host starts the match it builds one shared save out of them (isaac_save.merge_shared, the rule of the game's builder)
and every member lays it over their own save. Here the same happens before the games are attached to each other:

    open     members join and report their packed save; the first member is the host
    started  the host started the match: membership is frozen, every member has a ticket - session, seed, difficulty,
             their controller, the shared save - and brings their game into that run
    running  every member proved their game is in the same run on the same shared save; only now may a transport attach
    aborted  a member's game differed or a member left before the match ran; nothing is attached

Anyone who connects after the start is refused: a game that did not start from this shared save and this seed would
generate other rooms, floors and items. Lobby is the host's logic without any socket; Server and Client put it on TCP with
one JSON object per line."""
import argparse
import base64
import hashlib
import json
import secrets
import socket
import socketserver
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

import isaac_level as level
import isaac_save as save

PROTOCOL = 1
MAX_MEMBERS = 4
DIFFICULTIES = ("normal", "hard", "greed", "greedier")
# Where the shared flags live in the running game's PersistentGameData (Manager+0x14), by the game's own deserializer of
# the shared save (RVA 0x51adf0): chunk type -> offset. Counters move as soon as a run is played; flags do not move in the
# first room, so they tell which save a game really runs on.
LIVE_SAVE, LIVE_FLAGS = 0x14, {1: 0x38, 4: 0xAE8, 5: 0xE00, 6: 0xE07, 7: 0xE6F}


class Refused(Exception):
    """The lobby does not take this request; reason is one short word a client can act on."""
    def __init__(self, reason, detail=""):
        super().__init__(f"{reason}: {detail}" if detail else reason)
        self.reason, self.detail = reason, detail


def flags_digest(shared):
    """One value for the flags of a shared view, in the order the game keeps them."""
    return hashlib.sha256(b"".join(bytes(shared["chunks"][kind]) for kind in sorted(LIVE_FLAGS))).hexdigest()


class Lobby:
    def __init__(self, game):
        self.game, self.state, self.members, self.session = game, "open", [], None
        self.seed = self.difficulty = self.shared = self.packed = self.verdict = None

    def _member(self, token):
        for member in self.members:
            if secrets.compare_digest(member["token"], str(token)):
                return member
        raise Refused("token", "not a member of this lobby")

    def join(self, name, packed, protocol=PROTOCOL, game=None):
        """A member's name and packed save; returns the token that proves later messages come from that member."""
        if self.state != "open":
            raise Refused("started", "the match has begun; a run cannot be joined half way")
        if protocol != PROTOCOL:
            raise Refused("protocol", f"this lobby speaks version {PROTOCOL}")
        if game != self.game:
            raise Refused("game", "another build of the game")
        if not isinstance(name, str) or not 0 < len(name.strip()) <= 32 or any(member["name"] == name.strip() for member in self.members):
            raise Refused("name", "empty, too long or taken")
        if len(self.members) >= MAX_MEMBERS:
            raise Refused("full", f"a lobby holds {MAX_MEMBERS}")
        try:
            view = save.decode_shared(packed)
        except (ValueError, struct.error) as error:
            raise Refused("save", str(error)) from None
        member = dict(name=name.strip(), token=secrets.token_hex(16), view=dict(chunks=view["chunks"], bestiary=view["bestiary"]), facts=None)
        self.members.append(member)
        return member["token"]

    def leave(self, token):
        member = self._member(token)
        if self.state == "open":
            hosted = member is self.members[0]
            self.members.remove(member)
            if hosted:
                self.state = "aborted"; self.verdict = dict(type="abort", reason="the host left the lobby")
        elif self.state == "started":
            self.state = "aborted"; self.verdict = dict(type="abort", reason=f"{member['name']} left before the match ran")
        # Once the match runs, a member who leaves is simply gone, as in the game: the slot is not given to anyone else.
        return self.verdict

    def start(self, token, seed, difficulty):
        """Only the host, the first member, starts the match. Freezes the membership and builds the shared save."""
        if self.state != "open":
            raise Refused("started", "the match has already begun")
        if self._member(token) is not self.members[0]:
            raise Refused("host", "only the host starts the match")
        if difficulty not in DIFFICULTIES:
            raise Refused("difficulty", f"one of {', '.join(DIFFICULTIES)}")
        try:
            self.seed = level.seed_text(level.seed_value(seed))
        except (ValueError, KeyError, TypeError) as error:
            raise Refused("seed", str(error)) from None
        self.difficulty, self.session = difficulty, secrets.randbits(64) or 1
        self.shared = save.merge_shared([member["view"] for member in self.members])
        self.packed = save.encode_shared(self.shared)
        self.state = "started"

    def ticket(self, token):
        """What one member needs to bring their game into the match. Controllers follow the order of joining, the host 0."""
        member = self._member(token)
        if self.state == "open":
            raise Refused("open", "the match has not started")
        return dict(type="start", protocol=PROTOCOL, session=str(self.session), seed=self.seed, difficulty=self.difficulty,
                    controller=self.members.index(member), members=[m["name"] for m in self.members],
                    shared=base64.b64encode(self.packed).decode("ascii"), sharedSha256=hashlib.sha256(self.packed).hexdigest(),
                    flagsSha256=flags_digest(self.shared))

    def ready(self, token, facts):
        """A member's proof that their game is in the run: see game_facts(). When the last one arrives the verdict falls:
        the match runs only if every game equals the host's."""
        member = self._member(token)
        if self.state != "started":
            raise Refused(self.state, "no match is waiting for its members")
        member["facts"] = dict(facts)
        if any(m["facts"] is None for m in self.members):
            return None
        expected = dict(self.members[0]["facts"], seed=self.seed, difficulty=self.difficulty, flagsSha256=flags_digest(self.shared))
        differences = {m["name"]: {key: [m["facts"].get(key), value] for key, value in expected.items() if m["facts"].get(key) != value}
                       for m in self.members}
        differences = {name: found for name, found in differences.items() if found}
        if differences:
            self.state, self.verdict = "aborted", dict(type="abort", reason="a member's game is not in the host's run", differences=differences)
        else:
            self.state, self.verdict = "running", dict(type="go", session=str(self.session), level=expected["level"])
        return self.verdict


def game_facts(read, base):
    """What a member proves about their running game: the run (seed, difficulty), everything the level generated, and
    the flags of the save the game really runs on. read(address, size) reads the game's memory."""
    state = level.snapshot(read, base)
    manager = level.u32(read, base + level.MANAGER_RVA)
    first = min(LIVE_FLAGS.values()); last = max(offset + save.SHARED_COUNTS[kind] for kind, offset in LIVE_FLAGS.items())
    block = read(manager + LIVE_SAVE + first, last - first)  # one read from an aligned address; the flag arrays are not all aligned
    flags = b"".join(bytes(1 if value else 0 for value in block[LIVE_FLAGS[kind] - first:LIVE_FLAGS[kind] - first + save.SHARED_COUNTS[kind]])
                     for kind in sorted(LIVE_FLAGS))
    return dict(seed=level.seed_text(state["startSeed"]), difficulty=DIFFICULTIES[state["difficulty"]] if state["difficulty"] < len(DIFFICULTIES) else str(state["difficulty"]),
                level=level.fingerprint(state), curses=level.curse_names(state["curses"]), rooms=len(state["rooms"]), flagsSha256=hashlib.sha256(flags).hexdigest())


def _send(stream, message):
    stream.write((json.dumps(message, separators=(",", ":")) + "\n").encode("utf-8")); stream.flush()


class Server(socketserver.ThreadingTCPServer):
    """The host's lobby on TCP. start() and ready() for the host itself are called on .lobby under .lock by the host's own
    process; everything a remote member sends goes through handle()."""
    allow_reuse_address, daemon_threads = True, True

    def __init__(self, address, lobby):
        super().__init__(address, _Handler)
        self.lobby, self.lock, self.streams, self.changed, self.ticketed = lobby, threading.RLock(), {}, threading.Condition(), set()

    def announce(self):
        """Tell every connected member what the lobby now has for them, and wake whoever waits on the lobby."""
        with self.lock:
            for token, stream in list(self.streams.items()):
                try:
                    if self.lobby.verdict:
                        _send(stream, self.lobby.verdict)
                    elif self.lobby.state == "started":
                        if token not in self.ticketed:
                            self.ticketed.add(token); _send(stream, self.lobby.ticket(token))
                    else:
                        _send(stream, dict(type="members", members=[m["name"] for m in self.lobby.members]))
                except OSError:
                    self.streams.pop(token, None)
        with self.changed:
            self.changed.notify_all()

    def wait(self, condition, timeout):
        with self.changed:
            return self.changed.wait_for(lambda: condition(self.lobby), timeout)


class _Handler(socketserver.StreamRequestHandler):
    def handle(self):
        server, token = self.server, None
        try:
            for line in self.rfile:
                try:
                    message = json.loads(line)
                    with server.lock:
                        if message.get("type") == "hello" and token is None:
                            token = server.lobby.join(message.get("name"), base64.b64decode(message.get("save", ""), validate=True),
                                                      message.get("protocol"), message.get("game"))
                            server.streams[token] = self.wfile
                            _send(self.wfile, dict(type="welcome", token=token))
                        elif message.get("type") == "ready" and token is not None:
                            server.lobby.ready(token, message.get("facts") or {})
                        elif message.get("type") == "leave" and token is not None:
                            break
                        else:
                            raise Refused("message", "unexpected here")
                    server.announce()
                except Refused as refusal:
                    _send(self.wfile, dict(type="refused", reason=refusal.reason, detail=refusal.detail))
                    if token is None:
                        return
                except (ValueError, TypeError) as error:
                    _send(self.wfile, dict(type="refused", reason="message", detail=str(error)))
                    if token is None:
                        return
        except OSError:
            pass
        finally:
            if token is not None:
                with server.lock:
                    server.streams.pop(token, None)
                    if server.lobby.state in ("open", "started"):
                        server.lobby.leave(token)
                server.announce()


class Client:
    """A member's connection: hello, then messages of the host as they come."""
    def __init__(self, address, name, packed, game, timeout=10):
        self.socket = socket.create_connection(address, timeout=timeout)
        self.stream = self.socket.makefile("rwb")
        _send(self.stream, dict(type="hello", protocol=PROTOCOL, game=game, name=name, save=base64.b64encode(packed).decode("ascii")))
        answer = self.receive(timeout)
        if answer.get("type") != "welcome":
            self.close()
            raise Refused(answer.get("reason", "message"), answer.get("detail", ""))

    def receive(self, timeout, kinds=None):
        """The next message, or the next one of the given kinds; a refusal or a lost host always ends the wait."""
        self.socket.settimeout(timeout)
        while True:
            line = self.stream.readline()
            if not line:
                raise ConnectionError("The host closed the lobby")
            message = json.loads(line)
            if kinds is None or message.get("type") in kinds or message.get("type") in ("refused", "abort"):
                return message

    def ready(self, facts):
        _send(self.stream, dict(type="ready", facts=facts))

    def close(self):
        try:
            _send(self.stream, dict(type="leave")); self.stream.close()
        except (OSError, ValueError):
            pass
        self.socket.close()


def admit(host, client, source_pid, replica_pid):
    """The gate of a transport: two session.json files of one running match and the two games they were written for.
    Returns the controller the lobby gave the client, or raises ValueError naming what is wrong. A game that was not
    admitted by the lobby must not be attached: it may be in another run or on another save."""
    for role, session in (("host", host), ("client", client)):
        if (session.get("verdict") or {}).get("type") != "go":
            raise ValueError(f"The {role}'s lobby did not let the match run")
    if host["verdict"].get("session") != client["verdict"].get("session") or host["verdict"].get("level") != client["verdict"].get("level"):
        raise ValueError("The two games belong to different matches")
    if host.get("controller") != 0 or not 0 < client.get("controller", 0) < MAX_MEMBERS:
        raise ValueError("The first game must be the lobby's host and the second one of its guests")
    if (host.get("pid"), client.get("pid")) != (source_pid, replica_pid):
        raise ValueError("These are not the games the lobby checked")
    return client["controller"]


def prepare(folder, own, ticket):
    """The member's files for the match: lobby.dat, their own save with the shared save laid over it, and ticket.json."""
    folder.mkdir(parents=True, exist_ok=True)
    packed = base64.b64decode(ticket["shared"])
    if hashlib.sha256(packed).hexdigest() != ticket["sharedSha256"]:
        raise ValueError("The shared save of the ticket is damaged")
    target = folder / "lobby.dat"
    if target.exists():
        raise ValueError("The folder already holds a lobby save; a save is never overwritten")
    data = save.build(save.overlay(own, save.decode_shared(packed))); save.parse(data)
    target.write_bytes(data)
    (folder / "ticket.json").write_text(json.dumps({key: value for key, value in ticket.items() if key != "shared"}, indent=2), encoding="utf-8")
    return target


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("role", choices=("host", "join"))
    parser.add_argument("--save", type=Path, required=True, help="the member's own save; only read")
    parser.add_argument("--name", required=True)
    parser.add_argument("--folder", type=Path, required=True, help="where this member's lobby.dat, ticket.json and session.json go")
    parser.add_argument("--address", default="127.0.0.1:47460", help="where the host listens")
    parser.add_argument("--game", default="J460", help="the build every member must run")
    parser.add_argument("--members", type=int, default=2, help="host: start the match when this many members, the host included, are in")
    parser.add_argument("--seed", help="host: the seed of the run"); parser.add_argument("--difficulty", default="hard", choices=DIFFICULTIES)
    parser.add_argument("--launch", action="store_true", help="start an isolated game from lobby.dat (Start-IsaacReplica.ps1)")
    parser.add_argument("--pid", type=int, help="a game that was already started from this member's lobby.dat")
    parser.add_argument("--wait", type=float, default=900, help="seconds to wait for the other members and for the run")
    args = parser.parse_args()
    try:
        return play(args)
    except Refused as refusal:
        raise SystemExit(f"The lobby refused: {refusal}") from None


def play(args):
    host, _, port = args.address.rpartition(":")
    own = save.parse(args.save.read_bytes()); packed = save.encode_shared(save.shared_view(own))
    server = client = None
    try:
        if args.role == "host":
            if not args.seed:
                raise SystemExit("The host names the seed")
            server = Server((host, int(port)), Lobby(args.game)); threading.Thread(target=server.serve_forever, daemon=True).start()
            with server.lock:
                token = server.lobby.join(args.name, packed, PROTOCOL, args.game)
            print(f"lobby open on {args.address}; waiting for {args.members} members", flush=True)
            if not server.wait(lambda lobby: len(lobby.members) >= args.members or lobby.state != "open", args.wait):
                raise SystemExit("Not enough members came")
            with server.lock:
                server.lobby.start(token, args.seed, args.difficulty); ticket = server.lobby.ticket(token)
            server.announce()
        else:
            client = Client((host, int(port)), args.name, packed, args.game)
            print("joined; waiting for the host to start the match", flush=True)
            ticket = client.receive(args.wait, ("start",))
            if ticket.get("type") != "start":
                raise SystemExit(f"No match: {ticket}")
        target = prepare(args.folder, own, ticket)
        print(f"match {ticket['session']}: members {ticket['members']}, controller {ticket['controller']}, seed {ticket['seed']}, {ticket['difficulty']}; save {target}", flush=True)
        pid = args.pid
        if args.launch:
            script = Path(__file__).with_name("Start-IsaacReplica.ps1")
            launched = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(script), "-PersistentGameData", str(target),
                                       "-WindowPosX", str(960 * (ticket["controller"] % 2)), "-WindowPosY", str(540 * (ticket["controller"] // 2))],
                                      capture_output=True, text=True, check=True)
            pid = json.loads(launched.stdout)["launch"]["pid"]
            print(f"game started, pid {pid}: enter the run with seed {ticket['seed']}, {ticket['difficulty']}", flush=True)
        if pid is None:
            raise SystemExit("Start a game from the lobby save and name it with --pid, or use --launch")
        import importlib.util
        spec = importlib.util.spec_from_file_location("state_reader", Path(__file__).with_name("Read-IsaacState.py"))
        reader = importlib.util.module_from_spec(spec); spec.loader.exec_module(reader)
        process = reader.WindowsProcess(pid); deadline = time.monotonic() + args.wait; facts = None
        try:
            while facts is None:
                try:
                    facts = game_facts(process.read, process.base)
                except (ValueError, OSError) as error:
                    if time.monotonic() > deadline:
                        raise SystemExit(f"The game did not enter a run in time: {error}")
                    time.sleep(1)
        finally:
            process.close()
        print(f"in the run: {facts}", flush=True)
        if server:
            with server.lock:
                server.lobby.ready(token, facts)
            server.announce()
            if not server.wait(lambda lobby: lobby.verdict is not None, args.wait):
                raise SystemExit("The other members did not get ready")
            verdict = server.lobby.verdict
        else:
            client.ready(facts); verdict = client.receive(args.wait, ("go",))
        (args.folder / "session.json").write_text(json.dumps(dict(verdict=verdict, pid=pid, name=args.name, controller=ticket["controller"],
                                                                  members=ticket["members"], seed=ticket["seed"], facts=facts), indent=2), encoding="utf-8")
        print(json.dumps(verdict), flush=True)
        if server and verdict.get("type") == "go":
            # The lobby stays up while the match runs so that whoever knocks now is told why they cannot come in.
            print("match running; the lobby refuses newcomers until this process ends", flush=True)
            time.sleep(args.wait)
        return 0 if verdict.get("type") == "go" else 1
    finally:
        if client:
            client.close()
        if server:
            server.shutdown(); server.server_close()


if __name__ == "__main__":
    sys.exit(main())
