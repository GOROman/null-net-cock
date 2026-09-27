#!/usr/bin/env python3
"""会員管理のテスト: JOIN → MAKEID (o/s) / GULV / ULEVEL / UDLIST / UDEDIT / IDCOPY / IDKILL / UPASS / MCHANGE"""
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(__file__))
import e2e  # noqa: E402


def join(name, kana):
    g = e2e.Client()
    g.talk("ID:", "GUEST")
    g.expect(">")
    g.send("JOIN")
    for p, v in [("氏名", name), ("フリガナ", kana), ("住所", "大阪"), ("郵便番号", "530-91"), ("電話番号", "06-000-0000"),
                 ("パスワード (8", "PW"), ("もう一度", "PW")]:
        g.talk(p, v)
    g.talk("この内容で申し込みますか", "y")
    g.expect("受け付けました")
    g.talk(">", "OFF")
    g.talk("回線を切りますか", "y")


def main():
    tmp = tempfile.mkdtemp(prefix="nnc-u-")
    conf = os.path.join(tmp, "test.conf")
    with open(conf, "w") as f:
        f.write(f"listen = 127.0.0.1:{e2e.PORT}\ndata_dir = {tmp}/data\nsignup_fields = netcock\nprofile_fields = netcock\n")
    srv = subprocess.Popen([e2e.BIN, "-c", conf], stderr=None if os.environ.get("VERBOSE") else subprocess.DEVNULL)
    try:
        e2e.wait_port()
        a = e2e.Client()
        a.talk("ID:", "1")
        a.talk("Password:", "ABC")
        # 手作業の発行にして、2 人申し込む
        a.talk(">", "SIGNUP")
        a.talk("オンラインで入会を受け付けますか", "y")
        a.talk("手作業にしますか", "y")
        a.expect("手作業にしました。")
        join("山田太郎", "ﾔﾏﾀﾞ ﾀﾛｳ")
        join("鈴木花子", "ｽｽﾞｷ ﾊﾅｺ")

        a.talk(">", "MAKEID")
        a.talk("(y/n/o/s/q):", "y")
        got = a.expect("ID を発行しました。")
        assert "0002" in got, got
        a.talk("(y/n/o/s/q):", "s")
        a.talk("探すフリガナ＞", "ﾔﾏﾀﾞ")
        got = a.expect("(y/n/o/s/q):")
        assert "0002,,ﾔﾏﾀﾞ ﾀﾛｳ,山田太郎" in got, got
        a.send("y")
        a.expect("ID を発行しました。")
        a.expect("申し込みはもうありません")

        # UDLIST (b の組み合わせ) と UDEDIT (条件で探して書き換える)
        a.talk(">", "UDLIST")
        a.talk("(a/b/c/u)", "u")
        a.talk("カンマで区切る", "0,7,26")
        a.talk("何番から", "2")
        a.talk("続けて表示しますか", "y")
        got = a.expect(">")
        assert "0002,50,山田太郎,2" in got and "0003,50,鈴木花子,3" in got, got
        a.send("UDEDIT")
        a.talk("(a/b/c/u)", "u")
        a.talk("カンマで区切る", "7,1")
        a.talk("条件の項目の番号", "7")
        a.talk("値", "鈴木花子")
        a.talk("条件 (0:一致", "0")
        a.talk("q:やめる", "s")
        got = a.expect("q:終わる")
        assert "0003,鈴木花子,60" in got, got
        a.send("e")
        a.talk("項目の番号", "1")
        a.talk("新しい値", "90")
        got = a.expect("q:終わる")
        assert "0003,鈴木花子,90" in got, got
        a.send("q")

        # ULEVEL は今の L と T を出す
        a.talk(">", "ULEVEL")
        a.talk("設定を変えますか＞", "3")
        got = a.expect("レベル＞")
        assert "L:50 T:90" in got, got
        a.send("40")
        a.expect("設定を変えました。")

        # 3 番の人が書き込み、IDCOPY で 9 番に写すと書き込みも 9 番のものになる
        b = e2e.Client()
        b.talk("ID:", "3")
        b.talk("Password:", "PW")
        for p, v in [("公開する住所", "東京"), ("職業", "会社員"), ("機種", "X68000"), ("生年月日", "71-02-03"),
                     ("男性ですか", "n"), ("ESC", "n"), ("見えますか", "y"), ("ログネーム", "はなこ"), ("自己紹介", "")]:
            b.talk(p, v)
        b.talk(">", "MW")
        b.talk("宛先 (1/4)", "1")
        b.talk("さんでいいですか", "y")
        b.talk("宛先 (2/4)", "")
        b.talk("タイトル", "テスト")
        b.expect(">\t")
        b.send("本文")
        b.send(".")
        b.talk("書き込みますか", "Y")
        b.expect("書き込みました")
        b.talk(">", "OFF")
        b.talk("回線を切りますか", "y")

        a.talk(">", "IDCOPY")
        a.talk("誰を複写しますか", "3")
        a.talk("どの ID に複写しますか", "9")
        a.talk("複写しませんね", "n")
        a.expect("複写しました。")
        a.talk(">", "MREAD")
        got = a.expect("Command")
        assert "0009:はなこ" in got, got
        a.send("Q")

        # IDKILL: 9 番を消すと書き込みも消える
        a.talk(">", "IDKILL")
        a.talk("どの会員を削除しますか", "9")
        a.talk("削除しませんね", "n")
        a.expect("削除しました。")
        a.talk(">", "MREAD")
        a.expect("メッセージはありません")

        # UPASS: 2 番のログネームとパスワードを変える
        a.talk(">", "UPASS")
        a.talk("パスワードを表示しますか", "2")
        a.talk("変えませんね", "n")
        a.talk("ログネーム", "やまちゃん")
        a.talk("パスワード (8", "NEW")
        a.expect("パスワードを変えました。")
        c = e2e.Client()
        c.talk("ID:", "やまちゃん")
        c.talk("Password:", "NEW")
        c.expect("公開する住所")

        # MCHANGE: レベル 100 未満には移せない
        a.talk(">", "MCHANGE")
        a.talk("会員管理者にしますか", "2")
        a.expect("レベル 100 以上")
        print("OK: 会員管理のテスト通過")
    finally:
        srv.terminate()
        srv.wait(timeout=5)


if __name__ == "__main__":
    main()
