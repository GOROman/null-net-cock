#!/usr/bin/env python3
"""null-net-cock の結合テスト: 一時ディレクトリで起動し、Shift_JIS の TCP クライアントで操作する"""
import os
import socket
import subprocess
import sys
import tempfile
import time

BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "./null-net-cock")
PORT = 16868


def wait_port(timeout=10):
    """サーバーが待ち受けを始めるまで待つ"""
    end = time.time() + timeout
    while time.time() < end:
        try:
            socket.create_connection(("127.0.0.1", PORT), timeout=1).close()
            time.sleep(0.3)  # 確かめるための接続が回線を返すのを待つ
            return
        except OSError:
            time.sleep(0.1)
    raise RuntimeError("サーバーが起動しません")


class Client:
    def __init__(self):
        self.s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
        self.buf = b""

    def _recv(self):
        try:
            data = self.s.recv(4096)
        except socket.timeout:
            self._dead = True
            return
        if not data:
            raise EOFError("切断されました")
        # telnet のネゴシエーション (IAC xx yy) を捨てる
        out = bytearray()
        i = 0
        while i < len(data):
            if data[i] == 0xFF and i + 2 < len(data) + 1:
                i += 3
                continue
            out.append(data[i])
            i += 1
        self.buf += bytes(out)

    def expect(self, text, timeout=5):
        want = text.encode("cp932")
        end = time.time() + timeout
        while want not in self.buf:
            if time.time() > end or getattr(self, "_dead", False):
                raise AssertionError(f"{text!r} が来ません。受信: {self.buf.decode('cp932', 'replace')[-400:]!r}")
            self._recv()
        idx = self.buf.index(want) + len(want)
        got, self.buf = self.buf[:idx], self.buf[idx:]
        return got.decode("cp932", "replace")

    def send(self, line):
        self.s.sendall(line.encode("cp932") + b"\r\n")

    def talk(self, prompt, line):
        self.expect(prompt)
        self.send(line)


def main():
    tmp = tempfile.mkdtemp(prefix="nnc-")
    conf = os.path.join(tmp, "test.conf")
    with open(conf, "w") as f:
        f.write(f'bbs_name = "TEST-NET"\nlisten = 127.0.0.1:{PORT}\ndata_dir = {tmp}/data\nsignup_fields = netcock\nprofile_fields = netcock\n')
    srv = subprocess.Popen([BIN, "-c", conf], stderr=subprocess.PIPE)
    try:
        wait_port()
        # SYSOP でログインしてボードを作り、書き込む
        a = Client()
        a.talk("ID:", "1")
        a.talk("Password:", "abc")
        a.talk(">", "BMAKE")
        a.talk("一覧を表示しますか", "n")
        a.talk("新しく作るボードの番号＞", "1")
        for _ in range(8):  # ボードオペ〜残すタイトル数は既定値
            a.expect("＞")
            a.send("")
        a.talk("(board/mail/program):", "b")
        a.talk("(y/n):", "n")
        a.talk("(y/n):", "y")  # CUG にしない
        a.talk("ボードインデックス＞", "FREE")
        a.talk("ボードのタイトル＞", "フリーボード")
        a.expect("設定を変えました。")
        # BCHANGE: 空 Enter は今の値のまま、タイトルだけ変える
        a.talk(">", "BCHANGE")
        a.talk("一覧を表示しますか", "n")
        a.talk("どのボードを変えますか", "FREE")
        for _ in range(8):
            a.expect("＞")
            a.send("")
        for p in ["(board/mail/program):", "(y/n):", "(y/n):", "ボードインデックス＞"]:
            a.talk(p, "")
        a.talk("ボードのタイトル＞", "フリーボード２")
        a.expect("設定を変えました。")
        a.talk(">", "BREAD")
        got = a.expect("どのボードを読みますか")
        assert "\\FREE" in got and "フリーボード２" in got, got
        a.send("")
        a.talk(">", "BW FREE")
        a.talk("タイトル (50 文字まで):", "テスト書き込み")
        a.expect(">")  # ルーラー
        a.send("こんにちは、表示のテストです。")
        a.send("２行目")
        a.send(".")
        a.talk("書き込みますか", "Y")
        a.expect("==== 書き込みました ====")

        # ゲストで入って ID を取り、仮会員として読む
        b = Client()
        b.talk("ID:", "GUEST")
        b.talk("ID を取りますか (y/n):", "y")
        for p, v in [("氏名", "山田太郎"), ("フリガナ", "ヤマダタロウ"), ("住所", "大阪市北区"), ("郵便番号", "530-91"),
                     ("電話番号", "06-373-9961"), ("パスワード (8", "PASS"), ("もう一度", "PASS")]:
            b.talk(p, v)
        b.talk("この内容で ID を作りますか", "y")
        got = b.expect("があなたの ID です")
        assert "TEST0002" in got or "NULL0002" in got or "0002" in got, got
        b.talk("Enter を押してください。", "")
        a.expect("Guest")  # ゲストのログイン通知 (割り込み表示)
        b.talk(">", "OFF")
        b.talk("回線を切りますか", "y")
        b.expect("00:")

        c = Client()
        c.talk("ID:", "2")
        c.talk("Password:", "pass")
        # 初回の登録
        for p, v in [("公開する住所", "大阪"), ("職業", "学生"), ("機種", "X68000"), ("生年月日", "70-01-01"),
                     ("男性ですか", "y"), ("ESC", "y"), ("見えますか", "y"), ("ログネーム", "やまだ"), ("自己紹介", "よろしく")]:
            c.talk(p, v)
        c.talk(">", "RALL")
        got = c.expect("Command")  # 未読で一番古いもののヘッダ
        assert "FREE(1/1)" in got and "テスト書き込み" in got, got
        c.send("")  # R: 本文を読む
        got = c.expect("==== 最後まで読みました ====")
        assert "こんにちは、表示のテストです。" in got, got
        c.expect("==== 指定したボードを全部読みました ====")
        c.talk(">", "MW")
        c.talk("宛先 (1/4)＞", "1")
        c.talk("さんでいいですか", "y")
        c.talk("宛先 (2/4)＞", "")
        c.talk("タイトル", "ごあいさつ")
        c.expect(">")
        c.send("入会しました。")
        c.send(".")
        c.talk("書き込みますか", "Y")
        c.expect("書き込みました")
        c.talk(">", "LLIST")
        got = c.expect(">")
        assert "やまだ" in got and "Sysop" in got, got

        a.talk(">", "MREAD")
        got = a.expect("Command")
        assert "ごあいさつ" in got and "mail" in got, got
        a.send("")
        got = a.expect("削除しますか")
        assert "入会しました。" in got, got
        a.send("n")
        a.talk("Command", "Q")
        c.send("MCHECK")
        got = c.expect("次のメールに進みますか")
        assert "0001:Sysop(r)" in got, got
        c.send("q")
        c.expect("メールの確認を終わります。")
        a.talk(">", "LOG")
        got = a.expect(">")
        assert "Guest" in got, got
        a.talk("", "CHAT")
        a.talk("chat:", "こんにちは")
        c.expect("こんにちは")
        a.talk("chat:", "q")
        # 再起動しても残っていること (SQLite)。古い形式 (fid 列が無い) からの移行も、2 回の再起動で確かめる
        srv.terminate()
        srv.communicate(timeout=5)
        import sqlite3
        db = sqlite3.connect(os.path.join(tmp, "data", "net-cock.db"))
        db.execute("ALTER TABLE titles DROP COLUMN fid")
        db.commit()
        db.close()
        for _ in range(2):
            srv = subprocess.Popen([BIN, "-c", conf], stderr=subprocess.PIPE)
            wait_port()
            srv.terminate()
            srv.communicate(timeout=5)
        srv = subprocess.Popen([BIN, "-c", conf], stderr=subprocess.PIPE)
        wait_port()
        d = Client()
        d.talk("ID:", "やまだ")
        d.talk("Password:", "PASS")
        d.talk(">", "BREAD FREE")
        d.talk("Command", "T")
        d.talk("Command", "R")
        got = d.expect("Command")
        assert "２行目" in got, got
        d.send("Q")
        d.talk(">", "UREAD 1")  # UREAD はレベル 40 から。仮会員 (30) は使えない
        d.expect("そのコマンドはありません。")
        # メニュー方式
        d.talk(">", "MODE")
        for p, v in [("ESC", "n"), ("見えますか", "y"), ("メニュー方式にしますか", "y"), ("毎回メニュー", "y"),
                     ("チャットを受け付けますか", "y")]:
            d.talk(p, v)
        got = d.expect("番号＞")
        assert "《メインメニュー》" in got and "2. ボードを読む・書く" in got and "9. 回線を切る" in got, got
        d.send("2")
        got = d.expect("番号＞")
        assert "《ボード》" in got and "1. BREAD" in got and "0. メインメニューに戻る" in got, got
        d.send("0")
        d.talk("番号＞", "9")
        d.talk("回線を切りますか", "y")
        print("OK: e2e 通過")
    finally:
        srv.terminate()
        err = srv.communicate(timeout=5)[1].decode("utf-8", "replace")
        if os.environ.get("VERBOSE"):
            print(err)


if __name__ == "__main__":
    main()
