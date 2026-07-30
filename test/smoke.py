#!/usr/bin/env python3
"""Smoke test for http-server.

Usage: test/smoke.py ./http-server

Starts the given binary on a free port with a temporary document root and
checks that it serves files, rejects targets it should reject, and is still
alive at the end.
"""
import os
import socket
import subprocess
import sys
import tempfile
import time

CANARY = "SECRET-CANARY-DO-NOT-SERVE"

failures = []


def check(name, got, want):
    if got == want:
        print(f"  ok    {name}")
    else:
        print(f"  FAIL  {name}: got {got!r}, want {want!r}")
        failures.append(name)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def request(port, target, connection=b"close", read_all=True):
    """Send one request, return the whole response (headers and body)."""
    req = (b"GET " + target + b" HTTP/1.1\r\nHost: localhost\r\n"
           b"Connection: " + connection + b"\r\n\r\n")
    s = socket.socket()
    s.settimeout(5)
    try:
        s.connect(("127.0.0.1", port))
        s.sendall(req)
        data = b""
        while read_all:
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
        return data
    finally:
        s.close()


def status(port, target):
    data = request(port, target)
    if not data:
        return "<empty>"
    return data.split(b"\r\n", 1)[0].decode("latin-1")


def body(port, target):
    data = request(port, target)
    return data.split(b"\r\n\r\n", 1)[1] if b"\r\n\r\n" in data else b""


def wait_until_listening(proc, port, timeout=15.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc.poll() is not None:
            return False
        try:
            s = socket.socket()
            s.settimeout(0.5)
            s.connect(("127.0.0.1", port))
            s.close()
            return True
        except OSError:
            time.sleep(0.1)
    return False


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    binary = os.path.abspath(sys.argv[1])

    tmp = tempfile.mkdtemp(prefix="http-server-smoke.")
    root = os.path.join(tmp, "root")
    os.makedirs(os.path.join(root, "sub"))
    with open(os.path.join(root, "index.html"), "w") as f:
        f.write("ROOT-INDEX\n")
    with open(os.path.join(root, "sub", "index.html"), "w") as f:
        f.write("SUB-INDEX\n")
    with open(os.path.join(root, "sub", "f.txt"), "w") as f:
        f.write("SUBFILE\n")
    # Outside the document root: must never be served.
    with open(os.path.join(tmp, "secret.txt"), "w") as f:
        f.write(CANARY + "\n")

    port = free_port()
    log = open(os.path.join(tmp, "server.log"), "w+")
    proc = subprocess.Popen(
        [binary, "-a", "127.0.0.1", "-p", str(port), "-d", root],
        stdout=log, stderr=subprocess.STDOUT, cwd=tmp)

    try:
        if not wait_until_listening(proc, port):
            log.seek(0)
            print("server did not start:\n" + log.read())
            return 1

        print("serving files")
        check("GET /", body(port, b"/"), b"ROOT-INDEX\n")
        check("GET /index.html", body(port, b"/index.html"), b"ROOT-INDEX\n")
        check("GET /sub/", body(port, b"/sub/"), b"SUB-INDEX\n")
        check("GET /sub/f.txt", body(port, b"/sub/f.txt"), b"SUBFILE\n")
        check("GET /sub//f.txt", body(port, b"/sub//f.txt"), b"SUBFILE\n")
        check("GET /./index.html", body(port, b"/./index.html"), b"ROOT-INDEX\n")
        check("GET /sub/../index.html", body(port, b"/sub/../index.html"), b"ROOT-INDEX\n")
        check("missing file is 404",
              status(port, b"/nope").startswith("HTTP/1.0 404"), True)

        print("rejecting bad targets")
        # An over-long target must be refused, not copied into a fixed buffer.
        check("over-long target is 414",
              status(port, b"/" + b"A" * 5000).startswith("HTTP/1.0 414"), True)
        check("target without a leading slash is 400",
              status(port, b"foo").startswith("HTTP/1.0 400"), True)

        print("staying inside the document root")
        for target in (b"/../secret.txt",
                       b"/./../secret.txt",
                       b"/sub/../../secret.txt",
                       b"//../secret.txt",
                       b"/sub/./../../secret.txt",
                       b"/a/b/../../../secret.txt"):
            leaked = CANARY.encode() in request(port, target)
            check(f"{target.decode()} does not escape", leaked, False)

        print("keep-alive")
        s = socket.socket()
        s.settimeout(5)
        s.connect(("127.0.0.1", port))
        got = []
        for target, marker in ((b"/index.html", b"ROOT-INDEX"), (b"/sub/f.txt", b"SUBFILE")):
            s.sendall(b"GET " + target + b" HTTP/1.1\r\nHost: localhost\r\n"
                      b"Connection: keep-alive\r\n\r\n")
            data = b""
            for _ in range(8):
                try:
                    chunk = s.recv(65536)
                except socket.timeout:
                    break
                if not chunk:
                    break
                data += chunk
                if marker in data:
                    break
            got.append(marker in data)
        s.close()
        check("two requests on one connection", got, [True, True])

        print("still alive")
        check("server survived", proc.poll(), None)
        check("serves after all of the above", body(port, b"/index.html"), b"ROOT-INDEX\n")
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

    for marker in ("AddressSanitizer", "runtime error:", "LeakSanitizer"):
        if marker in output:
            print(f"  FAIL  sanitizer reported {marker}")
            failures.append("sanitizer")
            break

    if output.strip():
        print("--- server output ---")
        print(output.strip())

    if failures:
        print(f"\n{len(failures)} check(s) failed: {', '.join(failures)}")
        return 1
    print("\nall checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
