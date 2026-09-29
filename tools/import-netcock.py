#!/usr/bin/env python3
"""
NET-COCK (X68000) のホストデータを null-net-cock のデータベース (net-cock.db) に変換する

    python3 tools/import-netcock.py <NET-COCK のフォルダ> <出力先の data_dir> [--force]

NET-COCK のフォルダには SYS_DATA.DAT / USER.DAT / TITLE.DAT / DATA.DAT / LOG.DAT / PDS/ があること。
出力先に net-cock.db を新しく作る (既にあるときは --force で上書き)。元のファイルは読むだけで変更しない。

読み取るデータ (数値はすべてビッグエンディアン、文字列は Shift_JIS で NUL 終端の固定長):

  SYS_DATA.DAT  先頭 512 バイトがシステム情報、続いて 128 バイト × 72 がボード (0 番がメール)
                  +00 ネットワーク ID (4)  +04 使った ID の数 (w)  +14 会員の持ち時間 (b)
                  +18 ゲストの持ち時間 (b)  +1A 有効なタイトルの数 (w)  +1E 次の通し番号 (l)
                  +22 次の PDS ファイル番号 (w)  +116 SYSOP レベル / 仮 ID レベル / 一般レベル (b×3)
                ボード: +00 タイトル (60) +3C インデックス (8) +44 リードレベル +45 ライトレベル
                  +46 年令の下限 +4A 年令の上限 (0 は制限なし)
                  +4E フラグ (bit7 使用中 / bit6 メール / bit5 プログラム / bit3 性別制限 / bit2 男性のみ
                        / bit1 CUG / bit0 ESC 可)  +50 ボードオペ (w) +52 最低保存数 (w) +54 数 (w)
  USER.DAT      512 バイト × 1024 (ID 順)。項目の位置は NET-COCK 本体の UDLIST の表と同じ
                  +00 レベル +01 年令 +02 モード (bit6 ESC なし / bit5 BS で 1 文字戻る)
                  +03 bit0 女性  +04 メールレベル  +06 持ち時間 +07 本日の残り  +08 bit0 登録済 bit1 仮 ID
                  +0C ログネーム(20) +20 郵便番号(6) +26 住所(74) +70 氏名(16) +80 パスワード(8)
                  +88 職業(20) +9C 公開住所(40) +C4 機種(16) +D4 生年月日 +D8 申請日 +DC 登録日
                  +E0 最終接続日 +E4 時刻 +E8 ログイン数(w) +EA フリガナ(20) +FE 自己紹介(40)
                  +12A 総接続秒 +12E 今月 +132 前月 (l) +136 書込 B +13A M +13E P (w)
                  +146 電話番号(12) +168 ボードごとの既読位置 (w×64: 次に読むタイトルの番号)
                  +1E8 CUG リード権 / +1F0 CUG ライト権 (ボード番号のビット表 8 バイト)
                  日付は 年(w) 月 日、時刻は 0 時 分 秒
  TITLE.DAT     128 バイト × 8192。SYS_DATA の「有効なタイトルの数」までが使われている
                  +00 ボード +02 発信者 (w) +04 リード回数 (w) +06 日付 +0A 時刻 +0E 本文の長さ (l)
                  +12 本文の位置 (DATA.DAT 内, l) +16 リプライ先のタイトル番号 (w, FFFF は無し)
                  +18 bit0 削除 / bit1 ファイル付き +1A PDS のファイル番号 (w) +1C リプライ数 (w)
                  +22 通し番号 (l) +26 ファイルの大きさ (l) +2A ファイル名 (8+3 の 12) +36 ダウンロード数
                  +3E タイトル (50)  +70 メールの宛先 (w フラグ + w ID) × 4
                        フラグ: bit15 使用中 / bit2 既読 / bit1 削除
  DATA.DAT      本文 (Shift_JIS、改行は CR LF)
  PDS/NNNN      プログラムボードのファイル (NNNN はファイル番号の 10 進 4 桁)
  LOG.DAT       32 バイト × 512: +00 ID (w) +03 回線 +06 入った日付・時刻 +0E 出た日付・時刻
"""
import argparse
import datetime
import os
import shutil
import sqlite3
import sys
import tempfile

JST = datetime.timezone(datetime.timedelta(hours=9))


def be(b):
    return int.from_bytes(b, 'big')


def sjis(b):
    """NUL までを Shift_JIS として読んで UTF-8 の文字列にする"""
    b = b.split(b'\0', 1)[0]
    s = b.decode('cp932', errors='replace')
    return s.replace('\x1a', '')


def text(b):
    """本文: Shift_JIS → UTF-8、CR LF → LF"""
    s = b.decode('cp932', errors='replace')
    s = s.replace('\r\n', '\n').replace('\r', '\n').replace('\x00', '').replace('\x1a', '')
    return s


def mktime(date4, time4=b'\0\0\0\0'):
    y, m, d = be(date4[0:2]), date4[2], date4[3]
    if y < 1900 or not (1 <= m <= 12) or not (1 <= d <= 31):
        return 0
    hh, mm, ss = time4[1], time4[2], time4[3]
    try:
        return int(datetime.datetime(y, m, d, min(hh, 23), min(mm, 59), min(ss, 59), tzinfo=JST).timestamp())
    except ValueError:
        return 0


def birth(date4):
    y, m, d = be(date4[0:2]), date4[2], date4[3]
    if y <= 1900 or not (1 <= m <= 12) or not (1 <= d <= 31):
        return ''
    return '%02d-%02d-%02d' % (y % 100, m, d)


def fname83(b):
    b = b.split(b'\0', 1)[0].ljust(11)
    base = b[:8].decode('cp932', 'replace').rstrip()
    ext = b[8:11].decode('cp932', 'replace').rstrip()
    return base + ('.' + ext if ext else '')


# ------------------------------------------------------------ 表 (src/db.c の項目と同じ並び)

USER_COLS = ['id', 'flags', 'level', 'day_minutes', 'today_left', 'mail_level', 'logname', 'sex', 'kana', 'name',
             'tel', 'zip', 'addr_priv', 'addr_pub', 'job', 'machine', 'pass', 'birth', 'intro', 'applied', 'issued',
             'last_login', 'prev_login', 'wb', 'wm', 'wp', 'logins', 'logins_month', 'logins_prev', 'total_sec',
             'month_sec', 'prev_month_sec', 'today_sec', 'pass_miss', 'esc', 'bs_one_col', 'menu', 'chat_on_login',
             'mailbox_closed', 'secret', 'menu_always', 'chat_esc', 'chat_noecho', 'chat_self', 'chat_byid', 'lcall',
             'x_account']
USER_STR = {'logname', 'sex', 'kana', 'name', 'tel', 'zip', 'addr_priv', 'addr_pub', 'job', 'machine', 'pass',
            'birth', 'intro', 'x_account'}
BOARD_COLS = ['no', 'bop', 'rlevel', 'wlevel', 'group', 'age_min', 'age_max', 'keep', 'sex', 'type', 'esc', 'cug',
              'timelimit', 'index', 'title', 'next_seq']
BOARD_STR = {'index', 'title'}
TITLE_COLS = ['id', 'board', 'seq', 't', 'title', 'sender', 'reply_to', 'replies', 'len', 'reads', 'dls', 'deleted',
              'fname', 'fsize', 'fid', 'to0', 'to1', 'to2', 'to3', 'st0', 'st1', 'st2', 'st3']
TITLE_STR = {'title', 'fname'}
LOG_COLS = ['t_in', 't_out', 'line', 'id', 'logname', 'speed']
LOG_STR = {'logname', 'speed'}
APP_COLS = ['no', 't', 'name', 'kana', 'addr', 'zip', 'tel', 'pass', 'issued_id', 'logname', 'x_account']
APP_STR = {'name', 'kana', 'addr', 'zip', 'tel', 'pass', 'logname', 'x_account'}
SCHED_COLS = ['no', 'spot', 'start', 'end', 'kind', 'date', 'wday', 'hhmm', 'down', 'timer_wday', 'timer_hhmm', 'msg']
SCHED_STR = {'start', 'end', 'date', 'hhmm', 'timer_hhmm', 'msg'}


def create(db, table, cols, strs, extra=''):
    defs = ', '.join('"%s" %s' % (c, 'TEXT' if c in strs else 'INTEGER') for c in cols)
    db.execute('CREATE TABLE %s (%s%s)' % (table, defs, extra))


def insert(db, table, cols, row):
    db.execute('INSERT INTO %s (%s) VALUES (%s)' % (table, ','.join('"%s"' % c for c in cols), ','.join('?' * len(cols))),
               [row.get(c, '' if c in (USER_STR | BOARD_STR | TITLE_STR | LOG_STR) else 0) for c in cols])


# ------------------------------------------------------------ 読み込み

def main():
    ap = argparse.ArgumentParser(description='NET-COCK のデータを null-net-cock の net-cock.db に変換する')
    ap.add_argument('src', help='NET-COCK のフォルダ (SYS_DATA.DAT などがあるところ)')
    ap.add_argument('dst', help='出力先の data_dir (net-cock.db を作る)')
    ap.add_argument('--force', action='store_true', help='既にある net-cock.db を上書きする')
    ap.add_argument('--no-pds', action='store_true', help='PDS のファイル本体を取り込まない')
    a = ap.parse_args()

    def rd(name):
        with open(os.path.join(a.src, name), 'rb') as f:
            return f.read()

    sysd = rd('SYS_DATA.DAT')
    userd = rd('USER.DAT')
    titled = rd('TITLE.DAT')
    datad = rd('DATA.DAT')
    try:
        logd = rd('LOG.DAT')
    except OSError:
        logd = b''

    os.makedirs(a.dst, exist_ok=True)
    path = os.path.join(a.dst, 'net-cock.db')
    if os.path.exists(path) and not a.force:
        sys.exit('%s は既にあります (上書きするときは --force)' % path)
    # 共有フォルダなどでは SQLite のロックが使えないことがあるので、一時ディレクトリで作ってからコピーする
    tmpdir = tempfile.mkdtemp(prefix='netcock-import-')
    tmp = os.path.join(tmpdir, 'net-cock.db')
    db = sqlite3.connect(tmp)
    db.execute('PRAGMA journal_mode = MEMORY')
    create(db, 'users', USER_COLS, USER_STR, ', PRIMARY KEY (id)')
    create(db, 'boards', BOARD_COLS, BOARD_STR, ', PRIMARY KEY (no)')
    create(db, 'titles', TITLE_COLS, TITLE_STR)
    db.execute('CREATE TABLE bodies (id INTEGER PRIMARY KEY, body TEXT)')
    db.execute('CREATE TABLE files (id INTEGER PRIMARY KEY, data BLOB)')
    db.execute('CREATE TABLE pointers (user INTEGER, board INTEGER, ptr INTEGER, bset TEXT, cug TEXT, '
               'PRIMARY KEY (user, board))')
    create(db, 'log', LOG_COLS, LOG_STR)
    create(db, 'newmem', APP_COLS, APP_STR)
    db.execute('CREATE TABLE sys (key TEXT PRIMARY KEY, value TEXT)')
    create(db, 'sched', SCHED_COLS, SCHED_STR)

    # ---------------------------------------------------- システム
    net_id = sysd[0:4].split(b'\0')[0].decode('ascii', 'replace').strip()
    nusers = be(sysd[4:6])
    ntitles = min(be(sysd[0x1a:0x1c]), len(titled) // 128)
    user_minutes = sysd[0x14]
    guest_minutes = sysd[0x18]
    temp_level, user_level = sysd[0x117], sysd[0x118]

    # ---------------------------------------------------- ボード
    boards = {}
    for no in range(64):
        r = sysd[0x200 + no * 128:0x200 + (no + 1) * 128]
        if len(r) < 128 or not (r[0x4e] & 0x80):
            continue
        f = r[0x4e]
        boards[no] = dict(
            no=no, bop=be(r[0x50:0x52]), rlevel=r[0x44], wlevel=r[0x45], group=0,
            age_min=r[0x46], age_max=r[0x4a], keep=be(r[0x52:0x54]),
            sex=(1 if f & 0x04 else 2) if f & 0x08 else 0,
            type=ord('M') if f & 0x40 else ord('P') if f & 0x20 else ord('B'),
            esc=1 if f & 0x01 else 0, cug=1 if f & 0x02 else 0, timelimit=0,
            index=sjis(r[0x3c:0x44]), title=sjis(r[0:0x3c]), next_seq=1)
    if 0 not in boards:
        boards[0] = dict(no=0, type=ord('M'), index='MAIL', title='メール', next_seq=1)
    boards[0]['type'] = ord('M')

    # ---------------------------------------------------- タイトルと本文
    T = [titled[i * 128:(i + 1) * 128] for i in range(ntitles)]
    seq_of = {}       # タイトルの番号 → (ボード, 番号)
    count = {}
    for i, r in enumerate(T):
        b = r[0]
        if b not in boards:
            continue
        count[b] = count.get(b, 0) + 1
        seq_of[i] = (b, count[b])
    pds_dir = os.path.join(a.src, 'PDS')
    n_msgs = n_files = n_missing = pds_bytes = body_bytes = 0
    for i, r in enumerate(T):
        if i not in seq_of:
            continue
        b, seq = seq_of[i]
        off, ln = be(r[0x12:0x16]), be(r[0x0e:0x12])
        body = text(datad[off:off + ln]) if off + ln <= len(datad) else ''
        cur = db.execute('INSERT INTO bodies (body) VALUES (?)', (body,))
        bid = cur.lastrowid
        body_bytes += len(body.encode())
        rep = be(r[0x16:0x18])
        m = dict(id=bid, board=b, seq=seq, t=mktime(r[6:10], r[10:14]), title=sjis(r[0x3e:0x70]),
                 sender=be(r[2:4]), reply_to=seq_of[rep][1] if rep in seq_of and seq_of[rep][0] == b else 0,
                 replies=be(r[0x1c:0x1e]), len=len(body.encode()), reads=be(r[4:6]), dls=0,
                 deleted=1 if r[0x18] & 1 else 0, fname='', fsize=0, fid=0)
        if b == 0:
            for k in range(4):
                fl, uid = be(r[0x70 + 4 * k:0x72 + 4 * k]), be(r[0x72 + 4 * k:0x74 + 4 * k])
                if fl & 0x8000:
                    m['to%d' % k] = uid
                    m['st%d' % k] = ord('d') if fl & 2 else ord('r') if fl & 4 else ord('n')
        if r[0x18] & 2:
            m['fname'] = fname83(r[0x2a:0x36])
            m['dls'] = be(r[0x36:0x38])
            size = be(r[0x26:0x2a])
            p = os.path.join(pds_dir, '%04d' % be(r[0x1a:0x1c]))
            if not a.no_pds and os.path.exists(p):
                with open(p, 'rb') as f:
                    data = f.read()
                if 0 < size <= len(data):
                    data = data[:size]   # XMODEM の詰め物を落とす
                cur = db.execute('INSERT INTO files (data) VALUES (?)', (data,))
                m['fid'] = cur.lastrowid
                m['fsize'] = len(data)
                pds_bytes += len(data)
                n_files += 1
            else:
                m['fsize'] = size
                n_missing += 1
        insert(db, 'titles', TITLE_COLS, m)
        n_msgs += 1
    for b, bd in boards.items():
        bd['next_seq'] = count.get(b, 0) + 1
        insert(db, 'boards', BOARD_COLS, bd)

    # ---------------------------------------------------- 会員
    n_users = 0
    names = {}
    for uid in range(min(nusers or 1024, len(userd) // 512)):
        r = userd[uid * 512:(uid + 1) * 512]
        st = r[8]
        if uid > 1 and not (st & 3):
            continue            # 削除された ID
        u = dict(id=uid, level=r[0], day_minutes=r[6], today_left=r[7], mail_level=r[4],
                 logname=sjis(r[0x0c:0x20]), sex='F' if r[3] & 1 else 'M', kana=sjis(r[0xea:0xfe]),
                 name=sjis(r[0x70:0x80]), tel=sjis(r[0x146:0x152]), zip=sjis(r[0x20:0x26]),
                 addr_priv=sjis(r[0x26:0x70]), addr_pub=sjis(r[0x9c:0xc4]), job=sjis(r[0x88:0x9c]),
                 machine=sjis(r[0xc4:0xd4]), **{'pass': sjis(r[0x80:0x88])}, birth=birth(r[0xd4:0xd8]),
                 intro=sjis(r[0xfe:0x126]), applied=mktime(r[0xd8:0xdc]), issued=mktime(r[0xdc:0xe0]),
                 last_login=mktime(r[0xe0:0xe4], r[0xe4:0xe8]), wb=be(r[0x136:0x138]), wm=be(r[0x13a:0x13c]),
                 wp=be(r[0x13e:0x140]), logins=be(r[0xe8:0xea]), total_sec=be(r[0x12a:0x12e]),
                 month_sec=be(r[0x12e:0x132]), prev_month_sec=be(r[0x132:0x136]),
                 esc=0 if r[2] & 0x40 else 1, bs_one_col=0 if r[2] & 0x20 else 1, chat_on_login=1)
        u['prev_login'] = u['last_login']
        if uid == 0:
            u.update(flags=1, level=0, logname=u['logname'] or 'Guest', sex='')
        elif st & 2 and not st & 1:
            u['flags'] = 4      # 仮 ID
        if uid > 1 and not u['logname'] and not u.get('flags'):
            continue
        insert(db, 'users', USER_COLS, u)
        names[uid] = u['logname']
        n_users += 1
        # 既読位置 (次に読むタイトルの番号) → そのボードで読んだ最後の番号
        cugr, cugw = r[0x1e8:0x1f0], r[0x1f0:0x1f8]
        for b in boards:
            ptr = be(r[0x168 + 2 * b:0x16a + 2 * b]) if b < 64 else 0
            last = 0
            if b != 0:
                for i in range(min(ptr, ntitles)):
                    if i in seq_of and seq_of[i][0] == b:
                        last = seq_of[i][1]
            rr = bool(cugr[b >> 3] >> (b & 7) & 1)
            ww = bool(cugw[b >> 3] >> (b & 7) & 1)
            c = 'a' if rr and ww else 'r' if rr else 'w' if ww else 'n'
            if last or c != 'n':
                db.execute('INSERT INTO pointers VALUES (?,?,?,?,?)', (uid, b, last, 'Y', c))

    # ---------------------------------------------------- ログ
    logs = []
    for k in range(len(logd) // 32):
        r = logd[k * 32:(k + 1) * 32]
        if not r.strip(b'\0'):
            continue
        t_in = mktime(r[6:10], r[10:14])
        if not t_in:
            continue
        uid = be(r[0:2])
        logs.append(dict(t_in=t_in, t_out=mktime(r[14:18], r[18:22]), line=r[3] & 0x7f, id=uid,
                         logname=names.get(uid, ''), speed=''))
    logs.sort(key=lambda e: e['t_in'])
    for e in logs:
        insert(db, 'log', LOG_COLS, e)

    # ---------------------------------------------------- システム設定
    sysv = dict(net_id=net_id or 'TEST', user_level=user_level or 50, temp_level=temp_level or 30,
                min_minutes=guest_minutes or 15, user_minutes=user_minutes or 60, manager=1, signup=0,
                board_size=max(16 << 20, body_bytes * 2), pds_size=max(64 << 20, pds_bytes * 2),
                mail_size=8192)
    for k, v in sysv.items():
        db.execute('INSERT INTO sys VALUES (?,?)', (k, str(v)))

    db.commit()
    db.close()
    for ext in ('-wal', '-shm'):
        if os.path.exists(path + ext):
            os.remove(path + ext)   # 古いデータベースの WAL が残っていると混ざる
    shutil.copyfile(tmp, path)
    shutil.rmtree(tmpdir, ignore_errors=True)
    print('ネットワーク ID: %s' % net_id)
    print('会員: %d 人' % n_users)
    print('ボード: %d (メールを含む)' % len(boards))
    print('メッセージ: %d 件 (本文 %.1f MB)' % (n_msgs, body_bytes / 1e6))
    print('PDS のファイル: %d 個 (%.1f MB)%s' % (n_files, pds_bytes / 1e6,
                                               '、見つからない %d 個' % n_missing if n_missing else ''))
    print('ログ: %d 件' % len(logs))
    print('→ %s' % path)


if __name__ == '__main__':
    main()
