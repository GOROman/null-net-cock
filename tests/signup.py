#!/usr/bin/env python3
"""入会のテスト: 既定 (ハンドル名とパスワードだけ) と、signup_fields / profile_fields の設定、X のアカウント"""
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(__file__))
import e2e  # noqa: E402


def start(extra=""):
    tmp = tempfile.mkdtemp(prefix="nnc-j-")
    conf = os.path.join(tmp, "test.conf")
    with open(conf, "w") as f:
        f.write(f"listen = 127.0.0.1:{e2e.PORT}\ndata_dir = {tmp}/data\n{extra}")
    srv = subprocess.Popen([e2e.BIN, "-c", conf], stderr=None if os.environ.get("VERBOSE") else subprocess.DEVNULL)
    e2e.wait_port()
    return srv


def guest_signup(answers):
    g = e2e.Client()
    g.talk("ID:", "GUEST")
    g.talk("ID を取りますか", "y")
    for p, v in answers:
        g.talk(p, v)
    g.talk("この内容で ID を作りますか", "y")
    got = g.expect("があなたの ID です")
    g.talk("Enter を押してください。", "")
    g.talk(">", "OFF")
    g.talk("回線を切りますか", "y")
    return got


def main():
    # 既定: ハンドル名とパスワードだけ。初回の登録は無い
    srv = start()
    try:
        # 氏名などは聞かない。使えないハンドル名 (数字だけ・Sysop と同じ) は聞き直す
        g = e2e.Client()
        g.talk("ID:", "GUEST")
        g.talk("ID を取りますか", "y")
        got = g.expect("ログネーム")
        assert "氏名" not in got and "フリガナ" not in got and "住所" not in got, got
        g.send("1234")
        g.expect("そのログネームは使えません")
        g.talk("ログネーム", "sysop")
        g.expect("そのログネームは使えません")
        g.talk("ログネーム", "ねこ")
        g.talk("パスワード (8", "NEKO")
        g.talk("もう一度", "NEKO")
        g.talk("この内容で ID を作りますか", "y")
        got = g.expect("があなたの ID です")
        assert "TEST0002" in got, got
        g.talk("Enter を押してください。", "")
        # 仮 ID 発行後は g もすぐそのままの ID になっているので、
        # 別回線から同じ ID でログインする前に切っておく
        g.talk(">", "OFF")
        g.talk("回線を切りますか", "y")

        c = e2e.Client()
        c.talk("ID:", "ねこ")
        c.talk("Password:", "neko")
        got = c.expect(">")
        assert "公開する住所" not in got and "0002:ねこ" in got.replace("TEST", ""), got
        # UWRITE で X のアカウントを付ける (ほかは Enter で今のまま)。@ は付けても付けなくてもよい
        c.send("UWRITE")
        for _ in range(4):
            c.expect(">")
            c.send("")
        for p in ["男性ですか", "ESC", "見えますか"]:
            c.talk(p, "n" if p == "男性ですか" else "y")
        c.talk("ログネーム", "")
        c.talk("自己紹介", "")
        c.talk("X (旧 Twitter)", "bad name!")
        c.expect("英数字と _ の 15 文字まで")
        c.talk("X (旧 Twitter)", "@neko_x68k")
        c.expect(">")
        a = e2e.Client()
        a.talk("ID:", "1")
        a.talk("Password:", "ABC")
        a.talk(">", "UREAD ねこ")
        got = a.expect("ログイン")
        assert "X         : @neko_x68k" in got, got
    finally:
        srv.terminate()
        srv.wait(timeout=5)

    # 設定: 申し込みで X も聞き、初回ログインで機種と自己紹介を聞く
    srv = start("signup_fields = handle, x\nprofile_fields = machine, intro\n")
    try:
        got = guest_signup([("ログネーム", "いぬ"), ("X (旧 Twitter)", "inu"), ("パスワード (8", "INU"),
                            ("もう一度", "INU")])
        c = e2e.Client()
        c.talk("ID:", "いぬ")
        c.talk("Password:", "INU")
        got = c.expect("機種")
        assert "公開する住所" not in got, got
        c.expect(">")
        c.send("X68030")
        c.talk("自己紹介 (40 文字まで)>", "よろしく")
        c.expect(">")
        a = e2e.Client()
        a.talk("ID:", "1")
        a.talk("Password:", "ABC")
        a.talk(">", "UREAD いぬ")
        got = a.expect("ログイン")
        assert "@inu" in got and "X68030" in got and "よろしく" in got, got
        print("OK: 入会のテスト通過")
    finally:
        srv.terminate()
        srv.wait(timeout=5)


if __name__ == "__main__":
    main()
