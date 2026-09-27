#!/usr/bin/env python3
"""SYSOP の機能とチャットのテスト: LINESET / スケジュール / SDTIME / CTIME / チャットのサブコマンド / CMODE / LCSET"""
import os
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(__file__))
import e2e  # noqa: E402


def login(user, pw):
    c = e2e.Client()
    c.talk("ID:", user)
    c.talk("Password:", pw)
    return c


def main():
    tmp = tempfile.mkdtemp(prefix="nnc-s-")
    conf = os.path.join(tmp, "test.conf")
    with open(conf, "w") as f:
        f.write(f"listen = 127.0.0.1:{e2e.PORT}\ndata_dir = {tmp}/data\nsignup_fields = netcock\nprofile_fields = netcock\n")
    srv = subprocess.Popen([e2e.BIN, "-c", conf], stderr=None if os.environ.get("VERBOSE") else subprocess.DEVNULL)
    try:
        e2e.wait_port()
        a = login("1", "ABC")
        a.expect(">")

        # ゲストに ID を取らせて会員を 1 人作る (回線 2)
        g = e2e.Client()
        g.talk("ID:", "GUEST")
        g.talk("ID を取りますか", "y")
        for p, v in [("氏名", "山田太郎"), ("フリガナ", "ﾔﾏﾀﾞ ﾀﾛｳ"), ("住所", "大阪"), ("郵便番号", "530-91"),
                     ("電話番号", "06-000-0000"), ("パスワード (8", "PW"), ("もう一度", "PW")]:
            g.talk(p, v)
        g.talk("この内容で ID を作りますか", "y")
        g.talk("Enter を押してください。", "")
        g.talk(">", "OFF")
        g.talk("回線を切りますか", "y")

        b = login("2", "PW")
        for p, v in [("公開する住所", "大阪"), ("職業", "学生"), ("機種", "X68000"), ("生年月日", "70-01-01"),
                     ("男性ですか", "y"), ("ESC", "n"), ("見えますか", "y"), ("ログネーム", "やまだ"), ("自己紹介", "")]:
            b.talk(p, v)
        b.expect(">")

        # チャット: 全員あて、.P で 1 回線だけ、.C でシスオペを呼ぶ
        a.send("CHAT")
        a.talk("chat:", "みなさんこんにちは")
        b.expect("(Sysop)みなさんこんにちは")
        b.send("CHAT")
        b.talk("chat:", ".C")
        b.expect("シスオペを呼び出しています")
        a.expect("がシスオペを呼んでいます")
        b.talk("chat:", ".P1 ないしょ話")
        a.expect("(やまだ)ないしょ話")
        # .O1 で回線 1 との送受信をやめると届かない
        b.talk("chat:", ".O1")
        b.talk("chat:", "聞こえない")
        a.talk("chat:", "聞こえますか")
        b.talk("chat:", ".S")
        b.talk("chat:", "また聞こえる")
        got = a.expect("(やまだ)また聞こえる")
        assert "聞こえない" not in got, got
        b.talk("chat:", ".Q")
        a.talk("chat:", ".Q")

        # CMODE O (OFF) ならシスオペを呼べない
        a.talk(">", "CMODE")
        a.talk("(N/L/M/H/S/O):", "O")
        a.expect("OFF")
        b.talk(">", "CHAT")
        b.talk("chat:", ".C")
        b.expect("シスオペはチャットの呼び出しに応じられません")
        b.talk("chat:", ".Q")

        # LINESET: やまだの回線を常時タイムカウントにすると LLIST の時間に * が付く
        a.talk(">", "LLIST")
        got = a.expect(">")
        bline = int([l for l in got.split("\r\n") if "やまだ" in l][0][:2])
        a.send("LINESET")
        for i in range(1, bline + 1):
            a.talk("レベル＞", "")
            a.talk("時間のモード", "A" if i == bline else "N")
        a.talk("レベル＞", "")
        a.talk("時間のモード", "Q")
        time.sleep(1.5)
        a.talk(">", "LLIST")
        got = a.expect(">")
        line = [l for l in got.split("\r\n") if "やまだ" in l]
        assert line and "*\t" in line[0], got

        # スケジュール: 期間型を登録するとログイン時に出る。SCLIST で消す
        a.send("SCSET")
        a.talk("スポット型ですか", "n")
        a.talk("開始日", "2000-01-01")
        a.talk("終了日", "2099-12-31")
        a.talk("＞", "本日はメンテナンスの日です")
        c = login("SYSOP", "ABC")
        c.expect("その ID は別の回線で利用中です。")
        b.talk(">", "OFF")
        b.talk("回線を切りますか", "y")
        c = login("2", "PW")
        c.expect("本日はメンテナンスの日です")
        c.expect(">")
        b = c
        a.talk(">", "SCLIST")
        a.talk("削除しませんね", "n")
        a.expect("削除しました。")

        # SDTIME を掛けて外す
        a.talk(">", "SDTIME")
        a.talk("(0,1,3,5)", "1")
        a.expect("設定しました")
        a.talk(">", "SDTIME")
        a.talk("(0,1,3,5)", "0")
        a.expect("解除しました")

        # CTIME: 今の曜日・時間を禁止にし、やまだの回線を対象にするとチャットできない
        a.talk(">", "LLIST")
        got = a.expect(">")
        bline2 = int([l for l in got.split("\r\n") if "やまだ" in l][0][:2])
        now = time.localtime()
        wd = (now.tm_wday + 1) % 7  # Python は月曜 0、NET-COCK は日曜 0
        a.send("CTIME")
        a.talk("終わる＞", f"L {bline2}")
        a.talk("終わる＞", f"{wd} {now.tm_hour}-{now.tm_hour} X")
        a.talk("終わる＞", "")
        b.send("CHAT")
        b.expect("この回線ではチャットできません")
        print("OK: SYSOP・チャットのテスト通過")
    finally:
        srv.terminate()
        srv.wait(timeout=5)


if __name__ == "__main__":
    main()
