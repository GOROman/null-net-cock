#!/usr/bin/env python3
"""WebSocket 回線のテスト: 最小限の WebSocket クライアントでつなぎ、ログインして LLIST と LOG を見る"""
import base64
import os
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(__file__))
import e2e  # noqa: E402

WS_PORT = e2e.PORT + 1


class WsClient:
    def __init__(self):
        self.s = socket.create_connection(("127.0.0.1", WS_PORT), timeout=5)
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall((f"GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                        f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n"
                        f"CF-Connecting-IP: 203.0.113.9\r\n\r\n").encode())
        resp = b""
        while b"\r\n\r\n" not in resp:
            resp += self.s.recv(1024)
        head, self.raw = resp.split(b"\r\n\r\n", 1)
        assert b"101" in head.split(b"\r\n")[0], head
        import hashlib
        acc = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest())
        assert acc in head, head
        self.buf = b""

    def _frames(self):
        while True:
            if len(self.raw) < 2:
                return
            n = self.raw[1] & 0x7F
            hl = 2
            if n == 126:
                n = int.from_bytes(self.raw[2:4], "big")
                hl = 4
            if len(self.raw) < hl + n:
                return
            op = self.raw[0] & 0x0F
            if op == 2 or op == 1:
                self.buf += self.raw[hl:hl + n]
            self.raw = self.raw[hl + n:]

    def expect(self, text, timeout=5):
        want = text.encode()
        end = time.time() + timeout
        while want not in self.buf:
            if time.time() > end:
                raise AssertionError(f"{text!r} が来ません: {self.buf[-300:].decode('utf-8', 'replace')!r}")
            self.s.settimeout(max(0.1, end - time.time()))
            self.raw += self.s.recv(4096)
            self._frames()
        i = self.buf.index(want) + len(want)
        got, self.buf = self.buf[:i], self.buf[i:]
        return got.decode("utf-8", "replace")

    def send(self, text):
        data = (text + "\r").encode()
        mask = os.urandom(4)
        hdr = bytes([0x82, 0x80 | len(data)]) + mask
        self.s.sendall(hdr + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

    def talk(self, prompt, line):
        self.expect(prompt)
        self.send(line)


def main():
    tmp = tempfile.mkdtemp(prefix="nnc-w-")
    conf = os.path.join(tmp, "test.conf")
    with open(conf, "w") as f:
        f.write(f"listen = 127.0.0.1:{e2e.PORT}\nws_listen = 127.0.0.1:{WS_PORT}\ndata_dir = {tmp}/data\n")
    srv = subprocess.Popen([e2e.BIN, "-c", conf], stderr=None if os.environ.get("VERBOSE") else subprocess.DEVNULL)
    try:
        e2e.wait_port()
        w = WsClient()
        w.talk("ID:", "1")
        w.talk("Password:", "ABC")
        w.talk(">", "LLIST")
        got = w.expect(">")
        assert "0001:Sysop" in got, got
        w.send("OFF")
        w.talk("回線を切りますか", "y")
        w.expect("00:")
        time.sleep(0.5)
        # LOG に速度 WS、接続元は中継が付けたアドレス
        a = e2e.Client()
        a.talk("ID:", "1")
        a.talk("Password:", "ABC")
        a.talk(">", "LOG")
        got = a.expect(">")
        assert "-WS" in got, got
        print("OK: WebSocket 回線のテスト通過")
    finally:
        srv.terminate()
        srv.wait(timeout=5)


if __name__ == "__main__":
    main()
