#!/usr/bin/env python3
"""Smoke tests for peeryeet-signal. Usage: python3 test_signal.py [path-to-binary]

Starts the server on a spare port and speaks raw WebSocket to it (stdlib only).
"""
import base64
import hashlib
import os
import re
import socket
import struct
import subprocess
import sys
import time

BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "peeryeet-signal")
GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


class WS:
    def __init__(self, port, xff=None):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=3)
        key = base64.b64encode(os.urandom(16)).decode()
        extra = f"X-Forwarded-For: {xff}\r\n" if xff else ""
        self.s.sendall(
            (f"GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
             f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n{extra}\r\n").encode())
        resp = b""
        while b"\r\n\r\n" not in resp:
            resp += self.s.recv(4096)
        head, self.buf = resp.split(b"\r\n\r\n", 1)
        assert head.startswith(b"HTTP/1.1 101"), head
        want = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
        assert f"Sec-WebSocket-Accept: {want}".encode() in head, head

    def frame(self, payload, opcode=1):
        if isinstance(payload, str):
            payload = payload.encode()
        mask = os.urandom(4)
        n = len(payload)
        if n < 126:
            hdr = struct.pack("!BB", 0x80 | opcode, 0x80 | n)
        elif n < 65536:
            hdr = struct.pack("!BBH", 0x80 | opcode, 0x80 | 126, n)
        else:
            hdr = struct.pack("!BBQ", 0x80 | opcode, 0x80 | 127, n)
        return hdr + mask + bytes(b ^ mask[i & 3] for i, b in enumerate(payload))

    def send(self, payload, opcode=1):
        self.s.sendall(self.frame(payload, opcode))

    def _need(self, n):
        while len(self.buf) < n:
            chunk = self.s.recv(65536)
            if not chunk:
                raise EOFError
            self.buf += chunk

    def recv(self):
        """Returns (opcode, payload bytes)."""
        self._need(2)
        op, n = self.buf[0] & 0x0F, self.buf[1] & 0x7F
        assert not self.buf[1] & 0x80, "server frames must be unmasked"
        hl = 2
        if n == 126:
            self._need(4)
            n, hl = struct.unpack("!H", self.buf[2:4])[0], 4
        elif n == 127:
            self._need(10)
            n, hl = struct.unpack("!Q", self.buf[2:10])[0], 10
        self._need(hl + n)
        payload, self.buf = self.buf[hl:hl + n], self.buf[hl + n:]
        return op, payload

    def msg(self):
        op, p = self.recv()
        assert op == 1, (op, p)
        return p.decode()

    def closed(self):
        try:
            while True:
                op, _ = self.recv()
                if op == 8:
                    return True
        except (EOFError, ConnectionResetError):
            return True
        except socket.timeout:
            return False


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def main():
    port = free_port()
    proc = subprocess.Popen([BIN, "-p", str(port)], stderr=subprocess.PIPE)
    try:
        for _ in range(50):
            try:
                socket.create_connection(("127.0.0.1", port), timeout=0.1).close()
                break
            except OSError:
                time.sleep(0.05)
        run(port)
        print("all tests passed")
    finally:
        proc.terminate()
        proc.wait()


def run(port):
    # create
    a = WS(port, xff="198.51.100.1")
    a.send("C")
    m = a.msg()
    assert m[0] == "C" and re.fullmatch(r"[A-Z]+-[A-Z]+-\d{4}", m[1:]), m
    code = m[1:]
    print("created", code)

    # wrong code, then right code
    b = WS(port, xff="198.51.100.2")
    b.send("JNOPE-NOPE-0000")
    assert b.msg().startswith("ENo transfer")
    b.send("J" + code)
    assert b.msg() == "J"
    assert a.msg() == "P"

    # third party can't join a paired session
    c = WS(port, xff="198.51.100.3")
    c.send("J" + code)
    assert "already in use" in c.msg()

    # opaque forwarding both ways, including a 16-bit-length frame
    a.send('S{"sdp":"offer"}')
    assert b.msg() == 'S{"sdp":"offer"}'
    big = "S" + "x" * 60000
    b.send(big)
    assert a.msg() == big
    # a frame trickled in one byte at a time
    for byte in a.frame('S{"c":1}'):
        a.s.send(bytes([byte]))
        time.sleep(0.001)
    assert b.msg() == 'S{"c":1}'
    print("forwarding ok")

    # ping -> pong
    a.send(b"hi", opcode=9)
    assert a.recv() == (10, b"hi")

    # peer leaves -> X, and the code is gone
    a.s.close()
    assert b.msg() == "X"
    c.send("J" + code)
    assert c.msg().startswith("ENo transfer")
    print("teardown ok")

    # rate limit: 10 failed joins allowed, then cut off
    d = WS(port, xff="203.0.113.9")
    for i in range(10):
        d.send(f"JBAD-CODE-{i:04d}")
        assert d.msg().startswith("E")
    d.send("JBAD-CODE-9999")
    assert d.msg().startswith("EToo many")
    assert d.closed()
    # a fresh connection from the same IP is still blocked
    e = WS(port, xff="203.0.113.9")
    e.send("JBAD-CODE-0000")
    assert e.msg().startswith("EToo many")
    assert e.closed()
    # but another IP is fine
    f = WS(port, xff="203.0.113.10")
    f.send("JBAD-CODE-0000")
    assert f.msg().startswith("ENo transfer")
    print("rate limit ok")

    # every generated code is well-formed (catches holes in the word list)
    for _ in range(500):
        w = WS(port)
        w.send("C")
        m = w.msg()
        assert re.fullmatch(r"C[A-Z]+-[A-Z]+-\d{4}", m), m
        w.s.close()
    print("code format ok")

    # oversized message closes the connection
    g = WS(port)
    g.s.sendall(g.frame("S" + "y" * 70000))
    assert g.closed()

    # non-websocket request gets a 400
    h = socket.create_connection(("127.0.0.1", port), timeout=3)
    h.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
    assert h.recv(100).startswith(b"HTTP/1.1 400")
    print("protocol errors ok")


if __name__ == "__main__":
    main()
