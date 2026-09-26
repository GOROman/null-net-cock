#!/usr/bin/env python3
"""ホストコンソールのテスト: 標準入力を pty にして、ホストからのログイン・監視・代わりの操作・切断・終了を確かめる"""
import os
import select
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(__file__))
import e2e  # noqa: E402


class Console:
    def __init__(self, master):
        self.m = master
        self.buf = b""

    def expect(self, text, timeout=10):
        want = text.encode()
        end = time.time() + timeout
        while want not in self.buf:
            if time.time() > end:
                raise AssertionError(f"{text!r} が来ません: {self.buf[-400:].decode('utf-8', 'replace')!r}")
            r, _, _ = select.select([self.m], [], [], 0.2)
            if r:
                self.buf += os.read(self.m, 4096)
        i = self.buf.index(want) + len(want)
        got, self.buf = self.buf[:i], self.buf[i:]
        return got.decode("utf-8", "replace")

    def send(self, s, end="\n"):
        os.write(self.m, (s + end).encode())


def main():
    tmp = tempfile.mkdtemp(prefix="nnc-c-")
    conf = os.path.join(tmp, "test.conf")
    with open(conf, "w") as f:
        f.write(f"listen = 127.0.0.1:{e2e.PORT}\ndata_dir = {tmp}/data\n")
    master, slave = os.openpty()
    srv = subprocess.Popen([e2e.BIN, "-c", conf], stdin=slave, stdout=slave, stderr=subprocess.DEVNULL)
    con = Console(master)
    try:
        con.expect("host> ")
        # ホストからログイン (1 文字ずつ読むモード。改行は CR)
        con.send("cock")
        con.expect("ID:")
        con.send("1", "\r")
        con.expect("Password:")
        con.send("ABC", "\r")
        con.expect(">")
        con.send("LLIST", "\r")
        got = con.expect(">")
        assert " 0*" in got and "0001:Sysop" in got, got
        con.send("OFF", "\r")
        con.send("y", "\r")
        con.expect("host> ")

        # TCP のゲストの画面を監視し、代わりに操作する
        e2e.wait_port()
        g = e2e.Client()
        g.talk("ID:", "GUEST")
        g.talk("ID を取りますか", "n")
        g.expect(">")
        con.send("lines")
        got = con.expect("host> ")
        line = [l for l in got.splitlines() if "Guest" in l]
        assert line, got
        no = int(line[0][:2])
        con.send(f"op {no}")
        con.expect("~. で終わり")
        con.send("VERSION")
        got = g.expect("互換ホスト")  # ゲストの画面にも出る
        con.expect("互換ホスト")    # 監視しているホストにも出る
        con.send("~.")
        con.expect("host> ")

        # quit はログインしている人がいると断る。kick で切る
        con.send("quit")
        con.expect("終了できません")
        con.send(f"kick {no}")
        g.expect("ホストが回線を切りました")
        time.sleep(0.5)
        con.send("quit")
        srv.wait(timeout=5)
        assert srv.returncode == 0
        print("OK: ホストコンソールのテスト通過")
    finally:
        if srv.poll() is None:
            srv.terminate()
            srv.wait(timeout=5)


if __name__ == "__main__":
    main()
