#!/usr/bin/env python3
"""HELP.TXT のテスト: 15 ブロックのテンプレート (Shift_JIS) を読み込み、マクロと条件の展開を確かめる"""
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(__file__))
import e2e  # noqa: E402

# ブロック 0〜14 (ブロックの間はコメント行)
BLOCKS = [
    "<<BOARD %] %\"  %->>",
    "<<RE %. %$ by /+>>",
    "<<HEAD /-%\\//%[ %# /Xreply!%%%Xroot!%%/Zprog%%%Znotprog%%>>%/",
    "<<GUEST /1>>",
    "<<OPEN %?/xSYSOP%%%xNOTSYSOP%% L=%K///%>>",
    "<<BYE %?>>",
    "<<UREAD me=%? /mthem=%? id=/S>>",
    "<<USTAT line=%M logins=%N>>",
    "<<ACCESS today=%f* total=%h*>>",
    "<<CHATHELP>>",
    "<<WRITEHELP>>",
    "<<READHELP>>",
    "<<BATCHHELP>>",
    "<<AUTO>>",
    "<<MANUAL>>",
]


def main():
    tmp = tempfile.mkdtemp(prefix="nnc-p-")
    help_path = os.path.join(tmp, "HELP.TXT")
    with open(help_path, "wb") as f:
        for i, b in enumerate(BLOCKS):
            f.write(f"*\r\n* ブロック {i}\r\n*\r\n{b}\r\n".encode("cp932"))
        f.write(b"!!\r\n")
    conf = os.path.join(tmp, "test.conf")
    with open(conf, "w") as f:
        f.write(f"listen = 127.0.0.1:{e2e.PORT}\ndata_dir = {tmp}/data\nhelp_file = {help_path}\n")
    srv = subprocess.Popen([e2e.BIN, "-c", conf], stderr=None if os.environ.get("VERBOSE") else subprocess.DEVNULL)
    try:
        e2e.wait_port()
        a = e2e.Client()
        a.talk("ID:", "1")
        a.talk("Password:", "ABC")
        a.expect("<<OPEN SysopSYSOP L=Sysop./%>>")
        # ボードを作って 2 件 (2 件目はリプライ)
        a.send("BMAKE")
        a.talk("一覧を表示しますか", "n")
        a.talk("番号＞", "1")
        for _ in range(8):
            a.expect("＞")
            a.send("")
        for p, v in [("(board/mail/program):", "b"), ("(y/n):", "n"), ("(y/n):", "y"), ("インデックス＞", "FREE"),
                     ("タイトル＞", "自由")]:
            a.talk(p, v)
        a.talk(">", "BW FREE")
        a.talk("タイトル", "最初")
        a.expect(">\t")
        a.send("本文")
        a.send(".")
        a.talk("書き込みますか", "Y")
        a.talk(">", "BREAD FREE")
        a.expect("<<BOARD 1 自由  free>>")
        a.expect("<<HEAD FREE1/1 最初 root!notprog>>")
        a.send("P")
        a.talk("タイトル", "")
        a.expect(">\t")
        a.send("返事")
        a.send(".")
        a.talk("書き込みますか", "Y")
        a.talk("Command", "N")
        a.expect("<<HEAD FREE2/2 最初 reply!notprog>>")
        a.expect("<<RE 1 最初 by 0001>>")
        a.send("?")
        a.expect("<<READHELP>>")
        a.talk("Command", "Q")
        a.talk(">", "UREAD 0")
        a.expect("<<UREAD me=Sysop them=Guest id=TEST0000>>")
        a.send("USTAT")
        a.expect("<<USTAT line=1 logins=1>>")
        a.send("ACCESS")
        a.expect("<<ACCESS today=0 total=0>>")
        a.send("OFF")
        a.talk("回線を切りますか", "y")
        a.expect("<<BYE Sysop>>")
        print("OK: HELP.TXT のテスト通過")
    finally:
        srv.terminate()
        srv.wait(timeout=5)


if __name__ == "__main__":
    main()
