#!/usr/bin/env python3
"""ホストまわりのテスト: HFCONT (受信・一覧・名前の変更・複写・削除・送信) / REPORT / DEBUG / 全角パスワードの伏せ字"""
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(__file__))
import e2e  # noqa: E402
from xfer import relay  # noqa: E402


def main():
    tmp = tempfile.mkdtemp(prefix="nnc-h-")
    conf = os.path.join(tmp, "test.conf")
    with open(conf, "w") as f:
        f.write(f"listen = 127.0.0.1:{e2e.PORT}\ndata_dir = {tmp}/data\n")
    src = os.path.join(tmp, "UP.BIN")
    payload = os.urandom(5000)
    with open(src, "wb") as f:
        f.write(payload)
    srv = subprocess.Popen([e2e.BIN, "-c", conf], stderr=None if os.environ.get("VERBOSE") else subprocess.DEVNULL)
    try:
        e2e.wait_port()
        a = e2e.Client()
        a.talk("ID:", "1")
        a.talk("Password:", "ABC")

        # 全角のパスワードは全角の＊で伏せる (PASS の新しいパスワード)
        a.talk(">", "PASS")
        a.talk("パスワードを変えますか", "y")
        a.expect("パスワード (8")
        a.send("あいう")
        got = a.expect("もう一度")
        assert "＊＊＊" in got and "あいう" not in got, got
        a.send("あいう")
        a.expect("パスワードを変えました。")

        a.talk(">", "HFCONT")
        a.talk("(?:説明)>", "K sub")
        a.talk("(?:説明)>", "Y")
        a.expect("中止)。")
        assert relay(a, ["sz", "--ymodem", "-q", src], tmp) == 0
        a.expect("UP.BIN 5000 bytes")
        a.talk("(?:説明)>", "R UP.BIN DATA.BIN")
        a.talk("(?:説明)>", "P DATA.BIN sub")
        a.talk("(?:説明)>", "C sub")
        got = a.expect("(?:説明)>")
        assert "DATA.BIN" in got and "5000" in got, got
        a.send("S DATA.BIN")
        a.talk("(?:説明)>", "C ..")
        a.talk("(?:説明)>", "C ../..")  # 外へは出られない
        got = a.expect("(?:説明)>")
        assert "そのディレクトリはありません" in got, got
        a.send("G")
        a.expect("中止)。")
        out = os.path.join(tmp, "down")
        os.mkdir(out)
        assert relay(a, ["rz", "--ymodem", "-q", "-y"], out) == 0
        a.expect("転送が終わりました。")
        with open(os.path.join(out, os.listdir(out)[0]), "rb") as f:
            assert f.read() == payload
        a.talk("(?:説明)>", "D DATA.BIN")
        a.talk("削除しますか", "y")
        a.expect("削除しました。")
        a.talk("(?:説明)>", "Q")
        assert not os.path.exists(os.path.join(tmp, "data", "files", "DATA.BIN"))
        assert os.path.exists(os.path.join(tmp, "data", "files", "sub", "DATA.BIN"))

        a.talk(">", "REPORT")
        got = a.expect(">")
        assert "総ボード" not in got and "PDS の容量" in got and "入会の方式" in got, got
        a.send("DEBUG")
        a.talk("debug:", "lines")
        got = a.expect("debug:")
        assert "=1 uid=1" in got, got
        a.send("q")
        print("OK: ホストまわりのテスト通過")
    finally:
        srv.terminate()
        srv.wait(timeout=5)
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
