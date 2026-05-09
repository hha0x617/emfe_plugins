# Hha Forth for MC6809

## 由来

`mc6809` プラグインの `examples/` 配下にエントリーしている Hha Forth
および姉妹の [Hha Lisp](../lisp/) は、**`mc6809` プラグインの ISA 実装に
潜むバグを炙り出す** ために、ある程度の規模感を持ち多様な命令を使う
題材を考えている中から生まれました。期待通り多くの問題を炙り出すこと
ができ、ISA を成熟させることに大きく貢献しました。

今後 ISA を触る変更でも、引き続きこの 2 つを end-to-end のリグレッション
ターゲットとして活用できるよう、`examples/` 配下に同梱しています。

## 概要

MC6809 上で動作する小規模 Forth 処理系です。
`em6809` + `emfe_plugin_mc6809` 環境 (MC6850 ACIA at `$FF00/$FF01`、64 KB RAM)
で動きます。

- **単一アセンブリソース**: `forth.asm` (約 4,900 行)
- **ROM イメージ**: 約 8.4 KB
- **CFA 数**: 190 (primitive + colon 定義合計)
- **FORTH-83 Required Word Set カバー率**: 約 100% (ブロック記憶系は
  ハードウェア非対応のため除外)
- **テスト**: 7 smoke test すべて passing

ITC (indirect-threaded code) 方式のコンパクトな Forth で、
`:` によるコロン定義、`IF`/`ELSE`/`THEN`、
`BEGIN`/`UNTIL`/`AGAIN`/`WHILE`/`REPEAT`、`DO`/`LOOP`/`+LOOP`、
`VARIABLE` / `CONSTANT` / `CREATE` / `DOES>`、`FORGET` / `MARKER`、
実行時基数切替 (`HEX` / `DECIMAL`)、
混合精度・倍精度演算 (`UM*`, `M*`, `UM/MOD`, `SM/REM`, `FM/MOD`,
`*/`, `*/MOD`, `M+`, `D+`, `D-`, `D.` 等)、
絵的数値出力 (`<# # #S #> HOLD SIGN`)、
文字列操作 (`COMPARE`, `/STRING`, `-TRAILING`, `CMOVE`, `MOVE`,
`FILL`, `ERASE`, `BLANK`)、文字列リテラル `."` / `S"` / `ABORT"`、
ブロックコメント `(` と行コメント `\` まで備えた FORTH-83 準拠の
figForth / jonesforth 系処理系です。

```
Hha Forth for MC6809 ready.
3 4 + .           → 7  ok
: DOUBLE DUP + ;
5 DOUBLE DOUBLE . → 20  ok
```

## ドキュメント

| ドキュメント | 内容 |
|---|---|
| **[docs/USER_GUIDE_ja.md](docs/USER_GUIDE_ja.md)** | 起動方法、REPL 操作、全組込語 (primitives + コンパイラ)、使用例 |
| **[docs/LANGUAGE_AND_IMPL_ja.md](docs/LANGUAGE_AND_IMPL_ja.md)** | 言語仕様 (ITC スレッディング、辞書エントリ構造、内側インタプリタ) と実装解説、実装規模 |
| **[docs/SHOWCASE_ja.md](docs/SHOWCASE_ja.md)** | 5 つのアルゴリズム — ハノイの塔、8 クイーン、クイックソート、Forth 固有の `CREATE`/`DOES>`、`BASE` 切替 — を paste-ready REPL トランスクリプトとイディオム解説で紹介 |

## English

- [README.md](README.md)
- [docs/USER_GUIDE.md](docs/USER_GUIDE.md)
- [docs/LANGUAGE_AND_IMPL.md](docs/LANGUAGE_AND_IMPL.md)

## ライセンス

MIT OR Apache-2.0 (`forth.asm` の SPDX ヘッダに記載)。
