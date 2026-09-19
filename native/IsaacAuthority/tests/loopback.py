"""Two real processes, loopback UDP. No game process is opened or modified."""
import json
import secrets
import subprocess
import sys


def main():
    executable = sys.argv[1]
    session = str(secrets.randbits(63) or 1)
    processes = []
    try:
        host = subprocess.Popen([executable, "host", "0", session], stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, text=True)
        processes.append(host)
        ready = json.loads(host.stdout.readline())
        client = subprocess.Popen([executable, "client", str(ready["port"]), session],
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        processes.append(client)
        client_out, client_err = client.communicate(timeout=10)
        host_out, host_err = host.communicate(timeout=3)
        if host.returncode or client.returncode:
            raise RuntimeError(f"host={host.returncode}: {host_out} {host_err}; "
                               f"client={client.returncode}: {client_out} {client_err}")
        h, c = json.loads(host_out), json.loads(client_out)
        assert h["ticks"] == c["hostTick"] == 400
        assert c["pausedTicks"] == 80 and c["pending"] == 0
        assert h["remote"] == c["predicted"], (h, c)
        assert c["droppedInputs"] > 0 and h["withheldSnapshots"] > 0
        assert c["corrections"] > 0 and h["receivedInputs"] > 20
        # Generous scheduling tolerance; algorithmic independence is checked in core tests.
        assert 3900 <= h["elapsedMs"] < 8000, h
        print(json.dumps({"host": h, "client": c, "converged": True}))
    finally:
        for process in processes:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=3)


if __name__ == "__main__":
    main()
