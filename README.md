# null-net-cock

X68000 用のパソコン通信ホストプログラム **NET-COCK** と互換の動きをするホストを、C 言語で新しく書いたものです。
macOS / Linux で動き、TCP (telnet) とモデム (シリアル) で接続できます。

- NET-COCK のコマンド体系をそのまま使えます (BREAD / BWRITE / RALL / MREAD / MWRITE / CHAT / LLIST / LOG …)。コマンドは省略でき、前方一致で最初に当たるものが選ばれます
- メッセージ ID の割り当ては NET-COCK の `MES.TXT` と同じです。お手持ちの `MES.TXT` / `SYS_MES.DAT` を設定で指定すると、元と同じ文言で表示されます
- 初期データも NET-COCK と同じで、ゲスト (ID 0) と SYSOP (ID 1、パスワード `ABC`) だけ、ボードはありません
- プログラムボード (PDS) に対応。XMODEM (SUM / CRC / 1K)・YMODEM・YMODEM-g でファイルを上げ下ろしでき、BATCH で YMODEM バッチ転送もできます
- データは SQLite (`data/net-cock.db`) に保存します。終了のシグナルを受けたときも保存してから終わります
- 1 回線 1 スレッド、最大 128 回線
- 端末の文字コードは Shift_JIS (既定) か UTF-8。全角文字の BS も正しく消えます
- チャット・電報・ログイン/ログアウトの通知は、入力中でも割り込んで表示します

## NET-COCK との関係

NET-COCK のプログラムとソースは使っていません。公開されている説明書から動作を調べて、互換品として新しく書いたものです (クリーンルーム実装)。
NET-COCK の配布ファイル (文言ファイルを含む) はこのリポジトリには入っていません。元の文言で動かしたいときは、お手持ちの配布物のファイルを指定してください。
指定しないときは、このプログラム独自の文言で表示します。

## ビルド

SQLite と iconv が必要です (macOS は標準で入っています)。

```sh
make
make test     # 結合テスト (Python 3。転送のテストには lrzsz の sz / rz が必要)
```

## 起動

```sh
cp null-net-cock.conf.example null-net-cock.conf
./null-net-cock                  # -c で設定ファイルを指定できる
```

別のターミナルから接続します。

```sh
telnet localhost 6868
```

`ID:` に `1`、`Password:` に `ABC` と入れると SYSOP でログインします。ゲストは `GUEST` です。
`?` でコマンドの一覧が出ます。まず `BMAKE` でボードを作ってください。

使い方の詳細は [docs/usage.md](docs/usage.md) にあります。

## ライセンス

MIT
