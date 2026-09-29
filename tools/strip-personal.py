#!/usr/bin/env python3
"""会員の個人情報 (氏名・フリガナ・電話番号・郵便番号・住所) を消した DB の複製を作る。

  使い方: tools/strip-personal.py <元の data_dir> <出力 data_dir> [--keep-pub-addr] [--force]

- 元の DB は読むだけ (SQLite のバックアップ API で写すので、ホストが動いたままでもよい)
- 消すもの: users の name / kana / tel / zip / addr_priv / addr_pub、入会申請 (newmem) の全件
  --keep-pub-addr を付けると公開住所 (addr_pub) は残す
- 最後に VACUUM して、消した値がファイルの空き領域に残らないようにする。WAL も使わない形で書き出す
- 本文 (メール・掲示板) に会員が登録していた電話番号・住所と同じ文字列があれば「＊＊＊」に置き換える
  (登録と同じ書き方のものだけ。全角で書いたものも拾う。--keep-bodies で置き換えない)
- さらにメール (ボード 0) の本文は、電話番号らしい文字列を書き方によらず全部「＊＊＊」にする
  (0 で始まる 市外局番-市内局番-番号、(011)123-4567、0 始まりの 10〜11 桁。全角も。--keep-mail-phones で置き換えない)
- ハンドル名・パスワード・生年月日・職業・機種・自己紹介はそのまま
"""
import argparse, os, re, sqlite3, sys

FIELDS = ['name', 'kana', 'tel', 'zip', 'addr_priv', 'addr_pub']

a = argparse.ArgumentParser()
a.add_argument('src'); a.add_argument('dst')
a.add_argument('--keep-pub-addr', action='store_true')
a.add_argument('--force', action='store_true')
a.add_argument('--keep-bodies', action='store_true')
a.add_argument('--keep-mail-phones', action='store_true')
a = a.parse_args()

src = os.path.join(a.src, 'net-cock.db')
dst = os.path.join(a.dst, 'net-cock.db')
if os.path.abspath(src) == os.path.abspath(dst):
    sys.exit('元と出力が同じです')
os.makedirs(a.dst, exist_ok=True)
for ext in ('', '-wal', '-shm', '-journal'):
    if os.path.exists(dst + ext):
        if not a.force:
            sys.exit(f'{dst + ext} があります (--force で上書き)')
        os.remove(dst + ext)

s = sqlite3.connect(f'file:{src}?mode=ro', uri=True)
d = sqlite3.connect(dst)
s.backup(d)
s.close()

def zen(v):  # 半角英数記号 → 全角
    return ''.join(chr(ord(ch) + 0xfee0) if '!' <= ch <= '~' else ch for ch in v)

secrets = set()
for tel, addr in d.execute('SELECT tel, addr_priv FROM users'):
    for v in (tel, addr):
        v = (v or '').strip()
        if len(v) >= 8:
            secrets.update({v, zen(v)})
masked = 0
if not a.keep_bodies:
    for bid, body in d.execute('SELECT id, body FROM bodies').fetchall():
        if not body:
            continue
        nb = body
        for v in sorted(secrets, key=len, reverse=True):
            if v in nb:
                nb = nb.replace(v, '＊＊＊')
        if nb != body:
            d.execute('UPDATE bodies SET body = ? WHERE id = ?', (nb, bid))
            masked += 1

D = '[0-9０-９]'
S = '[-‐−ー－―‑ 　]'
PHONE = re.compile(
    rf'(?<![0-9０-９])(?:'
    rf'[0０]{D}{{0,4}}{S}?[(（]{D}{{1,4}}[)）]{S}?{D}{{3,4}}'        # 011(123)4567
    rf'|[(（][0０]{D}{{0,4}}[)）]{S}?{D}{{1,4}}{S}?{D}{{3,4}}'         # (011)123-4567
    rf'|[0０]{D}{{0,4}}{S}{D}{{1,4}}{S}{D}{{3,4}}'                     # 011-123-4567
    rf'|[0０]{D}{{9,10}}'                                              # 0111234567
    rf')(?![0-9０-９])')
mail_masked = 0
if not a.keep_mail_phones:
    for bid, body in d.execute('SELECT b.id, b.body FROM bodies b JOIN titles t ON t.id = b.id WHERE t.board = 0').fetchall():
        if not body:
            continue
        nb = PHONE.sub('＊＊＊', body)
        if nb != body:
            d.execute('UPDATE bodies SET body = ? WHERE id = ?', (nb, bid))
            mail_masked += 1

fields = [f for f in FIELDS if not (a.keep_pub_addr and f == 'addr_pub')]
n = d.execute('UPDATE users SET ' + ', '.join(f"{f} = ''" for f in fields)).rowcount
m = d.execute('DELETE FROM newmem').rowcount
d.commit()
d.execute('PRAGMA journal_mode = DELETE')
d.execute('PRAGMA secure_delete = ON')
d.execute('VACUUM')
d.close()

# 確認: 値が残っていないこと、元の値の断片がファイルに残っていないこと
d = sqlite3.connect(dst)
left = d.execute('SELECT count(*) FROM users WHERE ' + ' OR '.join(f"coalesce({f},'') <> ''" for f in fields)).fetchone()[0]
d.close()
s = sqlite3.connect(f'file:{src}?mode=ro', uri=True)
samples = [v for (v,) in s.execute("SELECT tel FROM users WHERE length(tel) >= 8 UNION SELECT addr_priv FROM users WHERE length(addr_priv) >= 8")]
if a.keep_bodies:
    samples = []
s.close()
raw = open(dst, 'rb').read()
hits = sum(1 for v in samples if v.encode() in raw)
print(f'メール {mail_masked} 件の中の電話番号らしい文字列を伏せました')
print(f'本文 {masked} 件の中の電話番号・住所を伏せました')
print(f'会員 {n} 人の {", ".join(fields)} を消しました。入会申請 {m} 件を消しました')
print(f'確認: 値が残っている会員 {left} 人 / ファイル内に残った電話番号・住所 {hits} 件 (どちらも 0 なら OK)')
if left or hits:
    sys.exit(1)
