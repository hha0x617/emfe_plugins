# Hha Lisp for MC6809

## 由来

`mc6809` プラグインの `examples/` 配下にエントリーしている Hha Lisp
および姉妹の [Hha Forth](../forth/) は、**`mc6809` プラグインの ISA 実装に
潜むバグを炙り出す** ために、ある程度の規模感を持ち多様な命令を使う
題材を考えている中から生まれました。期待通り多くの問題を炙り出すこと
ができ、ISA を成熟させることに大きく貢献しました。

今後 ISA を触る変更でも、引き続きこの 2 つを end-to-end のリグレッション
ターゲットとして活用できるよう、`examples/` 配下に同梱しています。

## 概要

MC6809 上で動作する小規模 Lisp 処理系です。
`em6809` + `emfe_plugin_mc6809` 環境 (MC6850 ACIA at `$FF00/$FF01`、64 KB RAM)
で動きます。

- **単一アセンブリソース**: `lisp.asm` (約 6,600 行)
- **ROM イメージ**: 約 19 KB (code + 初期化済みデータ)
- **Primitive 数**: 62 個、**stdlib エントリ**: 51 本
- **テスト**: 38 smoke test すべて passing

古典 Lisp 風 (`defun` / `T` / `NIL` / `'x`) の構文、`defmacro` +
quasiquote (`with-gensyms` による手動衛生化)、mark-sweep GC、
末尾呼出最適化、文字列・文字・ベクタ・32-bit 自動昇格整数まで備えた
実用レベルの小型処理系です。

**系譜の概要**: Common Lisp 風の表面構文 (`defun` / `setq` / `t` / `nil`) +
**Lisp-1 評価** (Scheme / Arc / Clojure 系) + Scheme 流のユーティリティ名
(`string->symbol` / `vector-set!`) のハイブリッド。最も近い直系は uLisp
ですが、Lisp-1 化されています。複数文化からの利用者向けのエイリアス
(述語の `null?` / `atom?` / `eq?` / `zero?`、mutate の `set!`) は
**追加のみで置換ではない**ため、SICP / CL / Emacs Lisp 出身者が
同じコードを摩擦なく読めます。系譜マップと設計原則の詳細は
[docs/LANGUAGE_AND_IMPL_ja.md §0](docs/LANGUAGE_AND_IMPL_ja.md) を参照。

```
> (defun fact (n) (if (< n 2) 1 (* n (fact (- n 1)))))
FACT
> (fact 10)
3628800
> (format "2+3=~D, hello ~A!" 5 'world)
2+3=5, hello WORLD!
NIL
```

## ドキュメント

| ドキュメント | 内容 |
|---|---|
| **[docs/USER_GUIDE_ja.md](docs/USER_GUIDE_ja.md)** | 起動方法、REPL 操作、主要な primitive と stdlib、使用例 |
| **[docs/LANGUAGE_AND_IMPL_ja.md](docs/LANGUAGE_AND_IMPL_ja.md)** | 言語仕様 (値タグ、特殊形式、メモリモデル) と実装解説 (GC、TCO、stdlib ブートストラップ等)、実装規模 |

## English

- [README.md](README.md)
- [docs/USER_GUIDE.md](docs/USER_GUIDE.md)
- [docs/LANGUAGE_AND_IMPL.md](docs/LANGUAGE_AND_IMPL.md)

## ライセンス

MIT OR Apache-2.0 (lisp.asm の SPDX ヘッダに記載)。
