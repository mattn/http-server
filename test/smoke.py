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


def content_type(port, target):
    for line in request(port, target).split(b"\r\n"):
        if line.lower().startswith(b"content-type:"):
            return line.split(b":", 1)[1].strip().decode("latin-1")
    return "<none>"


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
    with open(os.path.join(root, "a b.txt"), "w") as f:
        f.write("SPACE\n")
    with open(os.path.join(root, "c+d.txt"), "w") as f:
        f.write("PLUS\n")
    with open(os.path.join(root, "日本語.txt"), "w") as f:
        f.write("UTF8\n")
    with open(os.path.join(root, "a.png"), "w") as f:
        f.write("PNG\n")
    # Bigger than WRITE_BUF_SIZE, so serving it spans several loop iterations.
    with open(os.path.join(root, "big.bin"), "wb") as f:
        f.write(b"X" * (2 * 1024 * 1024))
    with open(os.path.join(root, "unknown.bin"), "w") as f:
        f.write("BIN\n")
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

        print("percent escapes")
        check("%20 becomes a space", body(port, b"/a%20b.txt"), b"SPACE\n")
        check("lowercase hex", body(port, b"/a%2ab.txt".replace(b"%2a", b"%20")), b"SPACE\n")
        check("%2B becomes a plus", body(port, b"/c%2Bd.txt"), b"PLUS\n")
        # '+' is form-encoding, not path-encoding: it stays a literal plus.
        check("plus stays a plus", body(port, b"/c+d.txt"), b"PLUS\n")
        check("utf-8 name", body(port, b"/%E6%97%A5%E6%9C%AC%E8%AA%9E.txt"), b"UTF8\n")
        check("escaped dot in a name", body(port, b"/a%20b%2Etxt"), b"SPACE\n")

        print("rejecting bad escapes")
        for target, why in ((b"/abc%", "truncated escape"),
                            (b"/abc%2", "one hex digit"),
                            (b"/abc%zz", "non-hex digits"),
                            (b"/a%00b", "encoded NUL"),
                            (b"/%2fetc/passwd", "encoded separator"),
                            (b"/%2Fetc/passwd", "encoded separator, upper case")):
            check(f"{why} is 400", status(port, target).startswith("HTTP/1.0 400"), True)

        print("content types")
        check("known extension", content_type(port, b"/a.png"), "image/png")
        # An unknown extension must not inherit the type of an earlier response.
        check("unknown extension after a known one",
              content_type(port, b"/unknown.bin"), "application/octet-stream")
        check("known extension again", content_type(port, b"/index.html"), "text/html")
        check("unknown extension again",
              content_type(port, b"/unknown.bin"), "application/octet-stream")

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
                       b"/a/b/../../../secret.txt",
                       # Escaped forms must resolve to the same thing, not slip past.
                       b"/%2e%2e/secret.txt",
                       b"/%2E%2E/secret.txt",
                       b"/sub/%2e%2e/%2e%2e/secret.txt",
                       b"/%2e%2e%2fsecret.txt",
                       b"/..%2fsecret.txt",
                       b"/%252e%252e/secret.txt"):
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

        print("surviving overlapping requests on one connection")
        # A second request, or garbage, arriving while a response is streaming
        # used to give the connection a second owner and get it closed twice.
        for name, payloads in (
                ("garbage during a transfer",
                 [b"GET /big.bin HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n",
                  b"GARBAGE\r\n\r\n"]),
                ("second request during a transfer",
                 [b"GET /big.bin HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n",
                  b"GET /index.html HTTP/1.1\r\nHost: x\r\n\r\n"]),
                ("two misses back to back",
                 [b"GET /nope1 HTTP/1.1\r\nHost: x\r\n\r\n",
                  b"GET /nope2 HTTP/1.1\r\nHost: x\r\n\r\n"]),
                ("pipelined in one write",
                 [b"GET /nope1 HTTP/1.1\r\nHost: x\r\n\r\n"
                  b"GET /nope2 HTTP/1.1\r\nHost: x\r\n\r\n"]),
        ):
            for _ in range(5):
                try:
                    s = socket.socket()
                    s.settimeout(2)
                    s.connect(("127.0.0.1", port))
                    for payload in payloads:
                        s.sendall(payload)
                    time.sleep(0.02)
                    s.close()
                except OSError:
                    pass
            time.sleep(0.3)
            check(f"alive after {name}", proc.poll(), None)

        print("not leaking connections")
        # A client that connects and goes away leaves no request in flight, so
        # nothing but on_read() can close the handle.
        fds = os.path.join("/proc", str(proc.pid), "fd")
        if os.path.isdir(fds):
            before = len(os.listdir(fds))
            for _ in range(40):
                s = socket.socket()
                s.connect(("127.0.0.1", port))
                s.close()
            time.sleep(0.5)
            for _ in range(20):
                body(port, b"/index.html")
            time.sleep(0.5)
            grew = len(os.listdir(fds)) - before
            check("descriptors reclaimed after clients disconnect", grew < 10, True)
        else:
            print("  skip  descriptor check (no /proc)")

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
