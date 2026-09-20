"""One game of the local pair through the game's own menus into the lobby, never blind:
after EVERY key a picture of that game's window is saved, and before every key that matters the game's state is checked from
its memory or log - above all that its network service is the LOCALHOST one before anything is pressed inside Online. Any
mismatch stops the run where it is. All pictures end up on one sheet for review.

Enter-IsaacNativeLobby.py host|guest|ready PID intro|title|lobby OUTPUT_SHEET.png

The games come from Start-IsaacNativePair.ps1. Order: host (the first game) - guest (each other game) - ready (the host again).
Look at the game first to tell whether it still shows the intro or already the title. The pictures show one game window
only, cut by its visible frame, and only while that window is in front - never the desktop."""
import ctypes as C
import ctypes.wintypes as W
import importlib.util
import json
import struct
import sys
import time
from pathlib import Path

from PIL import Image, ImageGrab

scripts = Path(__file__).resolve().parent; sys.path.insert(0, str(scripts))
spec = importlib.util.spec_from_file_location("game_pair", scripts / "Test-IsaacGamePair.py"); pair = importlib.util.module_from_spec(spec); spec.loader.exec_module(pair)
spec = importlib.util.spec_from_file_location("state_reader", scripts / "Read-IsaacState.py"); reader = importlib.util.module_from_spec(spec); spec.loader.exec_module(reader)
NAMES = dict(enter=0x0D, space=0x20, esc=0x1B, tab=0x09, up=0x26, down=0x28, left=0x25, right=0x27)
SERVICE, LOCALHOST_TABLE = 0x879A58, 0x7A23A4
C.windll.user32.SetProcessDPIAware()
role, pid, screen, sheet = sys.argv[1], int(sys.argv[2]), sys.argv[3], Path(sys.argv[4])
instances = json.loads((scripts.parent / "Binaries/game-instances/native-pair.json").read_text(encoding="utf-8-sig"))
log_path = Path(next(i["saveDirectory"] for i in instances if i["pid"] == pid)) / "log.txt"
pictures = []


def service():
    process = reader.WindowsProcess(pid)
    try:
        pointer = struct.unpack("<I", process.read(process.base + SERVICE, 4))[0]
        return "none" if not pointer else "LOCALHOST" if struct.unpack("<I", process.read(pointer, 4))[0] - process.base == LOCALHOST_TABLE else "OTHER"
    finally:
        process.close()


def log_has(text, since):
    return text in log_path.read_text(encoding="utf-8", errors="replace")[since:]


def step(control, key, wait, note):
    time.sleep(0.2)
    if key:
        code = NAMES[key]; control.key(chr(code), True); time.sleep(0.08); control.key(chr(code), False)
    time.sleep(wait)
    window = C.windll.user32.GetForegroundWindow(); owner = W.DWORD(); C.windll.user32.GetWindowThreadProcessId(window, C.byref(owner))
    if owner.value != pid:
        raise SystemExit(f"STOP after '{note}': the game window is not in front")
    rect = W.RECT(); C.windll.dwmapi.DwmGetWindowAttribute(W.HWND(window), 9, C.byref(rect), C.sizeof(rect))
    image = ImageGrab.grab(bbox=(rect.left, rect.top, rect.right, rect.bottom), all_screens=True).resize((320, 190))
    pictures.append(image); print(f"{len(pictures):2}. {note}")


def need(condition, message):
    if not condition:
        save(); raise SystemExit("STOP: " + message)


def save():
    if pictures:
        columns = 4; rows = (len(pictures) + columns - 1) // columns
        canvas = Image.new("RGB", (columns * 320, rows * 190), "black")
        for n, image in enumerate(pictures):
            canvas.paste(image, ((n % columns) * 320, (n // columns) * 190))
        canvas.save(sheet)


since = len(log_path.read_text(encoding="utf-8", errors="replace"))
control = pair.HostWindow(pid)
try:
    if role in ("host", "guest"):
        step(control, None, 0.3, "before any key")
        if screen == "intro":
            step(control, "space", 2.0, "space: skip the intro -> title")
        step(control, "enter", 2.0, "enter: title -> file select")
        step(control, "space", 2.0, "space: file 1 -> main menu")
        need(service() == "none", "a network service exists before Online was entered")
        step(control, "down", 1.0, "down: cursor to Online")
        step(control, "space", 2.0, "space: enter Online")
        need(service() == "LOCALHOST", f"the network service is {service()}, not LOCALHOST; nothing more is pressed")
        step(control, "space", 5.0, "space: Quick Match")
        need(service() == "LOCALHOST", "the network service changed")
        if role == "host":
            need(not log_has("Successfully joined lobby", since), "Quick Match joined somebody's lobby instead of offering to create one")
            step(control, "space", 3.0, "space: YES, create a lobby")
            step(control, "down", 1.0, "down"); step(control, "right", 1.0, "right"); step(control, "right", 1.0, "right: cursor on Create!")
            step(control, "space", 4.0, "space: Create!")
            need(log_has("Successfully created lobby", since), "the log does not say the lobby was created")
        else:
            need(log_has("Successfully joined lobby", since), "the log does not say the lobby was joined")
            step(control, "tab", 2.0, "tab: ready")
    elif role == "ready":
        step(control, None, 0.3, "the host's lobby before ready")
        need(service() == "LOCALHOST", "the network service is not LOCALHOST")
        step(control, "tab", 12.0, "tab: ready -> the match starts")
        need(log_has("Start Networked", since), "the log does not say the match started")
finally:
    control.release(); save()
print("sheet:", sheet)
