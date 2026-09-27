#!/usr/bin/env python3
"""プログラムボードの転送テスト: lrzsz (sz / rz) と XMODEM / YMODEM でやり取りする"""
import os
import select
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(__file__))
import e2e  # noqa: E402

IAC = 0xFF


def relay(cli, argv, cwd):
    """sz / rz を起動し、telnet の IAC を処理しながらソケットとつなぐ"""
    p = subprocess.Popen(argv, cwd=cwd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    sock = cli.s
    sock.setblocking(False)
    pending = bytearray(cli.buf)  # すでに受け取っていた分
    cli.buf = b""
    state = 0
    deadline = time.time() + 60
    try:
        while p.poll() is None and time.time() < deadline:
            if pending:
                data, pending = bytes(pending), bytearray()
            else:
                r, _, _ = select.select([sock, p.stdout], [], [], 0.2)
                data = b""
                if p.stdout in r:
                    out = os.read(p.stdout.fileno(), 65536)
                    if out:
                        sock.setblocking(True)
                        sock.sendall(out.replace(b"\xff", b"\xff\xff"))
                        sock.setblocking(False)
                if sock in r:
                    data = sock.recv(65536)
                    if not data:
                        break
            if data:
                clean = bytearray()
                for b in data:
                    if state == 0:
                        if b == IAC:
                            state = 1
                        else:
                            clean.append(b)
                    elif state == 1:
                        if b == IAC:
                            clean.append(b)
                            state = 0
                        elif 251 <= b <= 254:
                            state = 2
                        else:
                            state = 0
                    else:
                        state = 0
                try:
                    p.stdin.write(bytes(clean))
                    p.stdin.flush()
                except BrokenPipeError:
                    break
        p.wait(timeout=5)
        # 終わる直前に書いたもの (最後の ACK など) も届ける
        rest = p.stdout.read()
        if rest:
            sock.setblocking(True)
            sock.sendall(rest.replace(b"\xff", b"\xff\xff"))
    finally:
        sock.setblocking(True)
        sock.settimeout(10)
        if p.poll() is None:
            p.kill()
    return p.returncode


def main():
    tmp = tempfile.mkdtemp(prefix="nnc-x-")
    conf = os.path.join(tmp, "test.conf")
    with open(conf, "w") as f:
        f.write(f"listen = 127.0.0.1:{e2e.PORT}\ndata_dir = {tmp}/data\n")
    src = os.path.join(tmp, "SAMPLE.BIN")
    payload = bytes(range(256)) * 37 + b"\xff\xff\r\n\r\x00end"  # IAC と CR を含む
    with open(src, "wb") as f:
        f.write(payload)
    srv = subprocess.Popen([e2e.BIN, "-c", conf], stderr=None if os.environ.get("VERBOSE") else subprocess.DEVNULL)
    try:
        e2e.wait_port()
        a = e2e.Client()
        a.talk("ID:", "1")
        a.talk("Password:", "ABC")
        a.talk(">", "BMAKE")
        a.talk("一覧を表示しますか", "n")
        a.talk("新しく作るボードの番号＞", "5")
        for _ in range(8):
            a.expect("＞")
            a.send("")
        a.talk("(board/mail/program):", "p")
        a.talk("(y/n):", "n")
        a.talk("(y/n):", "y")
        a.talk("ボードインデックス＞", "PDS")
        a.talk("ボードのタイトル＞", "プログラム")
        a.expect("設定を変えました。")

        # YMODEM で上げる
        a.talk(">", "BW PDS")
        a.talk("タイトル", "テスト用のファイル")
        a.expect(">\t")
        a.send("バイナリです。")
        a.send(".")
        a.talk("書き込みますか", "Y")
        a.talk("YMODEM で送りますか", "y")
        a.talk("YMODEM-g にしますか", "n")
        a.expect("中止)。")
        rc = relay(a, ["sz", "--ymodem", "-q", src], tmp)
        assert rc == 0, f"sz {rc}"
        got = a.expect("書き込みました")
        assert "SAMPLE.BIN" in got and str(len(payload)) in got, got

        # XMODEM-CRC でも上げる
        a.talk(">", "BW PDS")
        a.talk("タイトル", "XMODEM で上げたもの")
        a.expect(">\t")
        a.send(".")
        a.talk("書き込みますか", "Y")
        a.talk("YMODEM で送りますか", "n")
        a.talk("CRC にしますか", "y")
        a.talk("ファイル名＞", "x.bin")
        a.talk("いいですか", "y")
        a.expect("中止)。")
        rc = relay(a, ["sz", "-X", "-q", src], tmp)
        assert rc == 0, f"sz -X {rc}"
        a.expect("書き込みました")

        # YMODEM で下ろす
        out = os.path.join(tmp, "down")
        os.mkdir(out)
        a.talk(">", "BREAD PDS")
        got = a.expect("Command")
        assert "[SAMPLE  .BIN]" in got, got
        a.send("Y")
        a.expect("中止)。")
        rc = relay(a, ["rz", "--ymodem", "-q", "-y", "-u"], out)
        assert rc == 0, f"rz {rc}"
        a.expect("転送が終わりました。")
        with open(os.path.join(out, "SAMPLE.BIN"), "rb") as f:
            assert f.read() == payload, "YMODEM で受け取った内容が違います"

        # XMODEM-1K で下ろす (末尾の 0x1A は XMODEM の仕様で残る)
        a.talk("Command", "X")
        a.talk("1024 バイトにしますか", "y")
        a.expect("中止)。")
        rc = relay(a, ["rz", "-X", "-q", "-y", "x.bin"], out)
        assert rc == 0, f"rz -X {rc}"
        a.expect("転送が終わりました。")
        with open(os.path.join(out, "x.bin"), "rb") as f:
            assert f.read().rstrip(b"\x1a") == payload.rstrip(b"\x1a"), "XMODEM で受け取った内容が違います"

        # バッチ: 2 つ登録して YMODEM でまとめて
        a.talk("Command", "S")
        a.expect("バッチリストに加えました。")
        a.talk("Command", "N")
        a.talk("Command", "S")
        a.expect("バッチリストに加えました。")
        a.talk("Command", "Q")
        a.talk(">", "BATCH")
        a.talk("バッチ転送 (Y/O/N/D/L/C/?):", "Y")
        a.expect("中止)。")
        out2 = os.path.join(tmp, "batch")
        os.mkdir(out2)
        rc = relay(a, ["rz", "--ymodem", "-q", "-y"], out2)
        assert rc == 0, f"rz batch {rc}"
        a.expect("転送が終わりました。")
        names = sorted(n.upper() for n in os.listdir(out2))
        assert names == ["SAMPLE.BIN", "X.BIN"], names
        for n in os.listdir(out2):
            with open(os.path.join(out2, n), "rb") as f:
                assert f.read() == payload, n
        print("OK: 転送テスト通過")
    finally:
        srv.terminate()
        srv.wait(timeout=5)
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
