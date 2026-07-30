#!/usr/bin/env python3
"""Fuzz http-server with hostile request targets and connection patterns.

Usage: test/fuzz.py ./http-server-asan [iterations]

Meant to run against a build with -fsanitize=address,undefined, where a memory
error aborts the process and is reported. Checks that the server survives, that
nothing outside the document root is ever served, that descriptors do not grow
without bound, and that no sanitizer diagnostic appears.

The generator is seeded, so a failure is reproducible.
"""
import os
import random
import socket
import subprocess
import sys
import tempfile
import threading
import time

CANARY = b"SECRET-CANARY-DO-NOT-SERVE"

# Fragments assembled into request targets. The point is to hit the boundaries
# of path resolution: separators, dot segments, escapes both valid and broken,
# and lengths either side of what fits in file_path.
FRAGMENTS = [
    b"/", b"//", b"///", b".", b"..", b"...", b"....",
    b"%2e", b"%2E", b"%2e%2e", b"%2f", b"%2F", b"%5c", b"%5C",
    b"%00", b"%", b"%2", b"%zz", b"%%", b"%41", b"%252e", b"%%2e",
    b"a", b"ab", b"sub", b"index.html", b"big.bin", b"nope",
    b"+", b"%20", b" ", b"?x=..", b"?", b"#", b"#%2e",
    b"\xff", b"%ff", b"%c0%af", b"\x01\x02",
    b"..%2f", b"..%5c", b".%2e", b"a" * 250, b"a" * 255,
]

METHODS = [b"GET", b"HEAD", b"POST", b"FROB", b"", b"G" * 300]

# Connection-level patterns, aimed at the ownership of a connection: a second
# request or garbage arriving while a response is streaming, and clients that
# vanish mid-transfer.
PAYLOADS = [
    b"GET /big.bin HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n",
    b"GET /index.html HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n",
    b"GET /nope HTTP/1.1\r\nHost: x\r\n\r\n",
    b"HEAD /big.bin HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n",
    b"GET /../secret.txt HTTP/1.1\r\nHost: x\r\n\r\n",
    b"GET /%2e%2e%2fsecret.txt HTTP/1.1\r\nHost: x\r\n\r\n",
    b"GET /" + b"A" * 5000 + b" HTTP/1.1\r\nHost: x\r\n\r\n",
    b"GET / HTTP/1.1\r\nHost: x\r\n" + b"H: v\r\n" * 40 + b"\r\n",
    b"GARBAGE\r\n\r\n",
    b"\x00\x01\x02\x03",
    b"GET /index.html HTTP/1.1\r\nHost: x\r\n",   # deliberately incomplete
    b"GET ",
    b"\r\n\r\n",
]

failures = []


def make_target(rng):
    target = b"".join(rng.choice(FRAGMENTS) for _ in range(rng.randint(1, 25)))
    if rng.random() < 0.12:
        target += b"A" * rng.randint(4070, 4110)
    if not target.startswith(b"/"):
        target = b"/" + target
    return target[:60000]


def fd_count(pid):
    path = os.path.join("/proc", str(pid), "fd")
    try:
        return len(os.listdir(path))
    except OSError:
        return None


def one_shot(port, raw, read_bytes=None):
    """Send raw bytes, read what comes back, return it."""
    s = socket.socket()
    s.settimeout(2)
    try:
        s.connect(("127.0.0.1", port))
        s.sendall(raw)
        data = b""
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
            if read_bytes is not None and len(data) >= read_bytes:
                break
        return data
    except OSError:
        return b""
    finally:
        s.close()


def phase_targets(port, iterations, rng):
    print(f"targets: {iterations} randomized request targets")
    for i in range(iterations):
        method = rng.choice(METHODS)
        target = make_target(rng)
        raw = method + b" " + target + b" HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"
        data = one_shot(port, raw)
        if CANARY in data:
            failures.append(f"served the canary for {target[:120]!r}")
            return
        if i and i % 1000 == 0:
            print(f"  {i}")


def phase_connections(port, rounds, rng):
    print(f"connections: {rounds} rounds x 8 concurrent hostile clients")
    seeds = [rng.randrange(1 << 30) for _ in range(8)]

    def worker(seed):
        r = random.Random(seed)
        for _ in range(rounds):
            s = socket.socket()
            s.settimeout(1.5)
            try:
                s.connect(("127.0.0.1", port))
                for _ in range(r.randint(1, 4)):
                    s.sendall(r.choice(PAYLOADS))
                    if r.random() < 0.4:
                        time.sleep(r.random() * 0.01)
                if r.random() < 0.6:
                    try:
                        s.recv(r.choice([1, 80, 4096]))
                    except OSError:
                        pass
            except OSError:
                pass
            finally:
                s.close()

    threads = [threading.Thread(target=worker, args=(s,)) for s in seeds]
    for t in threads:
        t.start()
    for t in threads:
        t.join()


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    binary = os.path.abspath(sys.argv[1])
    iterations = int(sys.argv[2]) if len(sys.argv) > 2 else 4000
    rng = random.Random(20260730)

    tmp = tempfile.mkdtemp(prefix="http-server-fuzz.")
    root = os.path.join(tmp, "root")
    os.makedirs(os.path.join(root, "sub"))
    with open(os.path.join(root, "index.html"), "w") as f:
        f.write("ROOT\n")
    with open(os.path.join(root, "sub", "index.html"), "w") as f:
        f.write("SUB\n")
    with open(os.path.join(root, "big.bin"), "wb") as f:
        f.write(b"X" * (2 * 1024 * 1024))
    with open(os.path.join(tmp, "secret.txt"), "wb") as f:
        f.write(CANARY + b"\n")

    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()

    # The server echoes request targets into its log, so it can contain
    # whatever bytes the fuzzer sent.
    log = open(os.path.join(tmp, "server.log"), "w+",
               encoding="utf-8", errors="replace")
    proc = subprocess.Popen([binary, "-a", "127.0.0.1", "-p", str(port), "-d", root],
                            stdout=log, stderr=subprocess.STDOUT, cwd=tmp)
    try:
        deadline = time.time() + 15
        ready = False
        while time.time() < deadline and proc.poll() is None:
            try:
                c = socket.socket()
                c.settimeout(0.5)
                c.connect(("127.0.0.1", port))
                c.close()
                ready = True
                break
            except OSError:
                time.sleep(0.1)
        if not ready:
            log.seek(0)
            print("server did not start:\n" + log.read())
            return 1

        baseline = fd_count(proc.pid)

        phase_targets(port, iterations, rng)
        if proc.poll() is not None:
            failures.append(f"server died during the target phase (exit {proc.returncode})")
        else:
            phase_connections(port, 40, rng)
            if proc.poll() is not None:
                failures.append(f"server died during the connection phase (exit {proc.returncode})")

        if proc.poll() is None:
            time.sleep(1)
            grown = fd_count(proc.pid)
            if baseline is not None and grown is not None and grown - baseline > 32:
                failures.append(f"descriptors grew from {baseline} to {grown}")
            if not one_shot(port, b"GET /index.html HTTP/1.1\r\nHost: x\r\n"
                                  b"Connection: close\r\n\r\n").endswith(b"ROOT\n"):
                failures.append("server no longer serves a known file")
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        log.seek(0)
        output = log.read()
        log.close()

    for marker in ("AddressSanitizer", "UndefinedBehaviorSanitizer", "runtime error:",
                   "LeakSanitizer", "SEGV"):
        if marker in output:
            failures.append(f"sanitizer reported {marker}")
            break

    if failures:
        print("\nFAILED:")
        for f in failures:
            print("  -", f)
        print("\n--- server output (tail) ---")
        print("\n".join(output.splitlines()[-60:]))
        return 1
    print("\nno crash, no leak of the canary, descriptors stable")
    return 0


if __name__ == "__main__":
    sys.exit(main())
