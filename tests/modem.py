#!/usr/bin/env python3
"""モデム回線のテスト: pty の向こうに AT コマンドに答える偽モデムを置き、
初期化 → RING → ATA → CONNECT → ログイン → NO CARRIER → 切断 → 再初期化 を確かめる"""
import os
import select
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(__file__))
import e2e  # noqa: E402


class FakeModem:
    def __init__(self):
        self.master, self.slave = os.openpty()
        self.path = os.ttyname(self.slave)
        self.buf = b""

    def read(self, timeout):
        r, _, _ = select.select([self.master], [], [], timeout)
        if r:
            self.buf += os.read(self.master, 4096)

    def expect(self, text, timeout=10):
        want = text.encode("cp932")
        end = time.time() + timeout
        while want not in self.buf:
            if time.time() > end:
                raise AssertionError(f"{text!r} が来ません: {self.buf[-300:]!r}")
            self.read(0.2)
        i = self.buf.index(want) + len(want)
        got, self.buf = self.buf[:i], self.buf[i:]
        return got.decode("cp932", "replace")

    def send(self, s):
        os.write(self.master, s.encode("cp932"))

    def answer_init(self, count):
        """AT コマンドに OK を返す (count 個)"""
        for _ in range(count):
            got = self.expect("\r")
            while "AT" not in got.upper():
                got = self.expect("\r")
            self.send("\r\nOK\r\n")


def main():
    tmp = tempfile.mkdtemp(prefix="nnc-m-")
    fm = FakeModem()
    conf = os.path.join(tmp, "test.conf")
    with open(conf, "w") as f:
        f.write(f"listen = 127.0.0.1:{e2e.PORT}\ndata_dir = {tmp}/data\ndefault_code = utf8\n"
                f"modem1.path = {fm.path}\nmodem1.baud = 2400\nmodem1.rings = 2\nmodem1.carrier = text\n"
                "modem1.code = sjis\nmodem1.init = ATZ; ATE0V1Q0X4&C1&D2S0=0\n")
    srv = subprocess.Popen([e2e.BIN, "-c", conf], stderr=None if os.environ.get("VERBOSE") else subprocess.DEVNULL)
    try:
        fm.answer_init(2)
        time.sleep(0.5)
        # 2 回目の RING で ATA
        fm.send("\r\nRING\r\n")
        time.sleep(0.5)
        fm.send("\r\nRING\r\n")
        fm.expect("ATA\r")
        fm.send("\r\nCONNECT 2400/V42BIS\r\n")
        fm.expect("ID:")
        fm.send("GUEST\r")
        fm.expect("ID を取りますか")
        fm.send("n\r")
        fm.expect(">")
        fm.send("LLIST\r")
        got = fm.expect(">")
        assert " 1." in got and "Guest" in got, got
        # キャリア断 → 切断して再初期化
        fm.send("\r\nNO CARRIER\r\n")
        fm.expect("ATZ")
        fm.buf = b"ATZ\r" + fm.buf
        fm.answer_init(2)

        # TCP から SYSOP で入り、LOG に速度が残っているか見る (TCP は 1 番を使わない)
        e2e.wait_port()
        a = e2e.Client()
        a.buf = b""
        a.talk("ID:", "1")
        a.talk("Password:", "ABC")
        a.talk(">", "LOG")
        got = a.expect(">")
        assert "01-2400/V42BIS" in got, got
        a.send("LLIST")
        got = a.expect(">")
        assert " 2." in got and "Sysop" in got, got
        print("OK: モデム回線テスト通過")
    finally:
        srv.terminate()
        srv.wait(timeout=5)


if __name__ == "__main__":
    main()
