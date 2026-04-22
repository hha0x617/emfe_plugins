# Hha Forth for MC6809

MC6809 上で動作する小規模 Forth 処理系です。
`em6809` + `emfe_plugin_mc6809` 環境 (MC6850 ACIA at `$FF00/$FF01`、64 KB RAM)
で動きます。

- **単一アセンブリソース**: `forth.asm` (約 1,700 行)
- **ROM イメージ**: 約 3 KB
- **CFA 数**: 79 (primitive + colon 定義合計)
- **テスト**: 6 smoke test すべて passing

ITC (indirect-threaded code) 方式の小さな Forth で、`:` によるユーザ定義、
`IF`/`THEN`/`BEGIN`/`UNTIL` 等の制御構造、`VARIABLE`/`CONSTANT`、
文字列リテラル `."` とコメント `(` まで備えた「最小で実用」な処理系です。

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

## English

- [README.md](README.md)
- [docs/USER_GUIDE.md](docs/USER_GUIDE.md)
- [docs/LANGUAGE_AND_IMPL.md](docs/LANGUAGE_AND_IMPL.md)

## ライセンス

MIT OR Apache-2.0 (`forth.asm` の SPDX ヘッダに記載)。
