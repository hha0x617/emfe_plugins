# Hha Lisp for MC6809

MC6809 上で動作する小規模 Lisp 処理系です。
`em6809` + `emfe_plugin_mc6809` 環境 (MC6850 ACIA at `$FF00/$FF01`、64 KB RAM)
で動きます。

- **単一アセンブリソース**: `lisp.asm` (約 6,600 行)
- **ROM イメージ**: 約 19 KB (code + 初期化済みデータ)
- **Primitive 数**: 60 個、**stdlib エントリ**: 47 本
- **テスト**: 35 smoke test すべて passing

古典 Lisp 風 (`defun` / `T` / `NIL` / `'x`) の構文、衛生的マクロ、mark-sweep GC、
末尾呼出最適化、文字列・文字・ベクタ・32-bit 自動昇格整数まで備えた
実用レベルの小型処理系です。

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
