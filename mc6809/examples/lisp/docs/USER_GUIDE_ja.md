# Hha Lisp — ユーザーガイド

`lisp.asm` は em6809 / emfe_plugin_mc6809 環境で動く Lisp 処理系です。
ACIA 1 本の上で対話的に動作し、REPL で式を入力して評価できます。

---

## 0. この処理系について (系譜の概要)

Hha Lisp は次の 3 つを意図的に組み合わせています:

- **Common Lisp 風の表面構文** — `defun` / `setq` / `t` / `nil` /
  `cond` / `progn` / `defmacro` / quasiquote。Scheme よりも教材向き。
- **Lisp-1 評価** — 関数と変数の名前空間が単一。プリミティブは第一級の
  値なので、`(filter zero? xs)` や `(mapcar car '((1) (2) (3)))` が
  そのまま動く。`#'` 不要。
- **Scheme 流のユーティリティ命名** — `string->symbol` / `vector-set!`
  / `char->integer`。変換は矢印、破壊的操作は `!`。

最も近い直系は **uLisp** (AVR / ARM 向けの Common Lisp サブセット
組込み Lisp) ですが、Hha Lisp は意図的に Lisp-1 化されています。

**複数文化からのエイリアス**: 出身言語に応じて自然に書けるよう、
両流派の名前が共存します。

| こう書ける | 同じ意味 | 流派 |
|---|---|---|
| `(null? xs)` | `(null xs)` | Scheme / CL |
| `(atom? x)` | `(atom x)` | Scheme / CL |
| `(eq? a b)` | `(eq a b)` | Scheme / CL |
| `(zero? n)` | `(zerop n)` | Scheme / CL |
| `(set! x v)` | `(setq x v)` | Scheme / CL |

エイリアスは **追加のみで、既存名のリネーム/削除は一切ない**ため、
出身言語に合わせて好きな方を使ってよく、混ぜても構いません。

系譜マップと設計原則の詳細 (`defun` / `setq` / `t` / `nil` を
あえて Scheme のエイリアスにせず first-class な名前として残している
理由など) は [LANGUAGE_AND_IMPL_ja.md §0](LANGUAGE_AND_IMPL_ja.md) を
参照。

---

## 1. ビルドと起動

### クイックスタート (ビルド済 emfe フロントエンド利用)

何もビルドせずに **すぐ Hha Lisp を試したい** 場合の最短経路:

1. GUI ホストの最新リリースから `emfe.exe` を入手
   ([emfe_WinUI3Cpp](https://github.com/hha0x617/emfe_WinUI3Cpp/releases)
   または [emfe_CsWPF](https://github.com/hha0x617/emfe_CsWPF/releases))。
   合わせて [emfe_plugins リリース](https://github.com/hha0x617/emfe_plugins/releases)
   から `emfe_plugin_mc6809.dll` を入手し、`emfe.exe` 隣の `plugins\`
   フォルダに配置する。
2. `emfe.exe` を起動し、**File → Switch Plugin…** で **MC6809** を選択。
3. **File → Open S-Record…** (Ctrl+S) で同梱の
   `examples/lisp/lisp.s19` を開く (リリースにも同梱されている)。
4. **F5** (Run) を押す。Console ウィンドウに boot banner と `> `
   プロンプトが表示されたら入力開始。

`lisp.s19` は本リポジトリにコミット済みかつリリース毎にも同梱される
ので、上記手順は `lwasm` のインストールなしで動きます。以下の
「ビルド」「ホストからのロード」セクションは、`lisp.asm` を **改変**
したい場合や Hha Lisp を自前のコードから埋め込みたい場合のみ必要です。

### ビルド (`lisp.asm` を改変するときのみ)

[lwasm](http://www.lwtools.ca/) (lwtools 同梱) が PATH にあることを前提:

```sh
lwasm -9 -f srec -o lisp.s19 lisp.asm
```

- `-9` は MC6809 モード、`-f srec` は Motorola S-record 出力。
- PATH に無い場合はフルパスで指定 (例: `C:\path\to\lwasm.exe` /
  `/usr/local/bin/lwasm`)。

### ホストからのロード (Rust 例)

```rust
let mut h: EmfeInstance = ptr::null_mut();
emfe_create(&mut h);
emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut());
let path = CString::new("examples/lisp/lisp.s19").unwrap();
emfe_load_srec(h, path.as_ptr());
emfe_run(h);
emfe_send_char(h, b'(' as c_char); // 以下、入力を送る
```

起動すると以下が表示されます:

```
Hha Lisp for MC6809
(c) 2026 hha0x617 - MIT/Apache-2.0
> 
```

---

## 2. REPL の動作

1. `> ` プロンプトを出し、1 行を入力待ち (最大 512 バイトの TIB)。
2. 入力行の括弧が閉じていなければ `>> ` プロンプトで継続入力を受け付け。
3. 式を順に読み込み、評価、結果を印字。
4. 行終了後、`(gc)` を自動実行して前ターンの一時的な garbage を回収。

- `'x` / `` `x `` / `,x` / `,@x` は QUOTE / QUASIQUOTE / UNQUOTE / UNQUOTE-SPLICING。
- `#\c` は文字リテラル、`"..."` は文字列 (`\n` `\t` `\\` `\"` エスケープ対応)。
- `;` 以降は行末までコメント。
- シンボルは**読み込み時に大文字化** (Common Lisp 慣例)。
- `(a . b)` で dotted pair。

---

## 3. 組込みの値型

| 型 | リテラル例 | 備考 |
|---|---|---|
| fixnum | `42` `-17` `0` | 15-bit 符号付き (-16384..16383) |
| int32 box | `100000` | fixnum 範囲超過時に自動昇格 |
| NIL / T | `nil` `t` | (大文字小文字不問) |
| シンボル | `foo` `make-vector` | 読み込み時に upcase |
| pair | `(1 2 3)` `(a . b)` | GC 対象 |
| 文字 | `#\a` `#\Z` `#\~` | |
| 文字列 | `"hello"` | |
| ベクタ | `#(1 2 3)` | プリンタ出力、構築は `(make-vector)` 等で |
| closure | `#<CLOSURE>` | `(lambda ...)` / `(defun ...)` の値 |
| macro | `#<MACRO>` | `(defmacro ...)` の値 |
| builtin | `#<BUILTIN>` | primitive 関数値 (第一級) |

---

## 4. 主要な特殊形式

| 形式 | 用法 |
|---|---|
| `(quote x)` / `'x` | 評価せずそのまま返す |
| `(if cond then else)` | else 省略可 |
| `(cond (test1 body1...) (test2 body2...) (t default...))` | |
| `(let ((v1 e1) (v2 e2)) body...)` | 並列束縛 |
| `(let* ((v1 e1) (v2 e2)) body...)` | 逐次 (後ろは前の値を見られる) |
| `(letrec ((f (lambda ...)) ...) body...)` | 相互再帰可 |
| `(defvar sym value)` | global 束縛 |
| `(defun name (params) body...)` | 関数定義 (暗黙 PROGN body) |
| `(lambda (params) body...)` | 匿名関数 |
| `(setq sym value)` / `(set! sym value)` | 既存束縛を mutate (`set!` は Scheme 流のエイリアス、意味は完全に同じ) |
| `(progn e1 e2 ... eN)` | 順次実行、最終値を返す |
| `(and e1 e2 ...)` / `(or e1 e2 ...)` | 短絡評価 |
| `(defmacro name (params) body...)` | マクロ定義 |
| `` `(...) `` + `,x` + `,@xs` | 構文糖 (quasi-quote) |
| `(catch tag body...)` / `(throw tag value)` | 非局所脱出 |
| `(case key (keys1 body...) (keys2 body...) (t default...))` | stdlib マクロ |

### dotted 引数 (可変長)

```lisp
(defun head-tail (x . rest) (list x rest))
(head-tail 1 2 3 4)  ; → (1 (2 3 4))

(defun my-list args args)
(my-list 10 20 30)   ; → (10 20 30)
```

---

## 5. Primitive (抜粋)

### リスト
`cons` `car` `cdr` `atom` `eq` `null` `list` `length` `append` `cadr` `caddr`
`cddr` `assoc` `apply`

### 算術・比較
`+` `-` `*` `/` `mod` `<` `=` `ash` `logand` `logior` `logxor` `lognot`

### 文字列
`string-length` `string=` `string-append` `string-ref` `string->list`
`list->string`

### 文字
`char->integer` `integer->char` `char?`

### ベクタ
`make-vector` `vector-length` `vector-ref` `vector-set!` `vector->list`
`list->vector` `vector?`

### 変換
`number->string` `string->number` `symbol->string` `string->symbol`

### メタ
`eval` `read-string` `load-memory`

### 入出力
`print` `display` `newline` `putchar`

### PRNG / 時間
- `(seed n)` — PRNG シード設定
- `(rand)` — 0..16383 の乱数
- `(tick)` — CPU サイクル由来の 14-bit カウンタ

### GC / メタ
`gc` `gensym` `error` `catch` `throw`

### Printer case mode
- `(print-case)` — シンボル名の case mode を fixnum で返す
  (0 = upper、1 = lower)
- `(set-print-case! n)` — case mode を設定。`n` は 0 または 1
  (それ以外は黙って 0 に正規化)。default は 0 (upper) で既存
  トランスクリプト互換のため。途中で lowercase 出力に切り替えるには:

  ```
  > (defun foo () 42)
  FOO
  > (set-print-case! 1)
  1
  > (defun bar () 99)
  bar
  > t
  t
  ```

  対象はシンボル名内の ASCII 英字と `T` / `NIL` / `#<BUILTIN>` /
  `#<MACRO>` / `#<CLOSURE>` の表示のみ。数字・記号・文字列内容・
  文字リテラル・reader (常に upcase) は影響を受けない。背景は
  [LANGUAGE_AND_IMPL_ja.md §0.2 原則 5](LANGUAGE_AND_IMPL_ja.md) を参照。

---

## 6. Stdlib (ROM 埋込 Lisp 関数)

### Tier 1
`not` `zerop` `inc` `dec` `>` `abs` `max` `min`

### Tier 2
`reverse` `nth` `last` `member` `mapcar` `filter` `reduce` `any` `all`
`equal` `<=` `>=`

### マクロ
`when` `unless` `swap` `with-gensyms` `while` `dolist` `funcall` `case`
`format`

### Records
`defstruct` — `(defstruct point x y)` で `make-point / point? / point-x /
point-y / set-point-x / set-point-y` を自動生成

### Hashtable
`make-ht` `ht-hash` `ht-get` `ht-put`

### 固定小数点 Q8.8
`q-from` `q-to` `q*` `q/`

### デバッグ
`trace` `untrace` — 関数呼出の入出力を自動ログ

### Scheme スタイルのエイリアス
`null?` `atom?` `eq?` `zero?` — それぞれ `null` / `atom` / `eq` / `zerop`
と同じ callable 値を指す束縛。第一級プリミティブのため、
`(filter zero? xs)` や `(any null? xs)` のように高階関数の引数としても
そのまま使える。SICP / Scheme / Clojure 出身の利用者は `?` 付きの
記法を、Common Lisp / Emacs Lisp 出身の利用者は元の名前をそのまま
使ってよい。

---

## 7. 使用例

### 基本

```lisp
> (+ 1 2)
3
> (cons 'a (cons 'b nil))
(A B)
> (mapcar (lambda (x) (* x x)) '(1 2 3 4))
(1 4 9 16)
```

### マクロ

```lisp
> (case 'red ((red green) 1) ((blue) 2) (t 0))
1
> (dolist (i '(10 20 30)) (print i))
10
20
30
NIL
```

### defstruct

```lisp
> (defstruct point x y)
SET-POINT-Y
> (defvar p (make-point 3 4))
P
> (point-x p)
3
> (set-point-x p 99)
99
> (point-x p)
99
> (point? p)
T
```

### Hashtable

```lisp
> (defvar H (make-ht))
H
> (ht-put H 'alice 1)
1
> (ht-put H 'bob 2)
2
> (ht-get H 'alice)
1
> (ht-get H 'missing)
NIL
```

### trace

```lisp
> (defun fact (n) (if (< n 2) 1 (* n (fact (- n 1)))))
FACT
> (trace 'fact)
FACT
> (fact 3)
ENTER FACT (3)
ENTER FACT (2)
ENTER FACT (1)
EXIT  FACT -> 1
EXIT  FACT -> 2
EXIT  FACT -> 6
6
```

### 乱数

```lisp
> (seed (tick))
17234
> (rand)
12033
> (rand)
8841
```

### メタ循環

```lisp
> (eval (read-string "(+ 10 20)"))
30
```

### load-memory

テストハーネスから `emfe_poke_byte` でメモリ上に Lisp ソースを注入:

```rust
// Rust 側: $3E00 番地から NUL 終端で書き込む
let script = b"(defvar gx 42)\n(defvar gy (+ gx 1))\n";
for (i, &b) in script.iter().enumerate() {
    emfe_poke_byte(h, 0x3E00 + i as u64, b);
}
emfe_poke_byte(h, 0x3E00 + script.len() as u64, 0);
```

```lisp
> (load-memory 15872)    ; 15872 = $3E00
GY
> gx
42
> gy
43
```

### アルゴリズムショーケース

ハノイの塔、8 クイーン (解の数 + 盤面可視化)、クイックソートの
paste-ready トランスクリプトを [SHOWCASE_ja.md](SHOWCASE_ja.md) に
まとめました。

---

## 8. 複数行入力

括弧が閉じていない行では `>> ` プロンプトで継続入力を受け付けます:

```lisp
> (defun fact (n)
>>   (if (< n 2)
>>       1
>>     (* n (fact (- n 1)))))
FACT
> 
```

`;` コメントや `"..."` 内の `)` は適切に扱われます。

---

## 9. 制限事項

- **整数範囲**: fixnum (-16384..16383) と int32 (-2^31..2^31-1) のみ。浮動小数点は未対応 (Q8.8 固定小数点で代用可)
- **文字列・ベクタ・int32 pool は bump alloc**。GC されない (長時間セッションで枯渇)
- **TCO**: self-tail は in-place mutation で garbage-free、相互 TCO は各 call で 2 pair alloc (auto-GC で回収)
- **TIB は 512 バイト**。1 行中のソースがこれを超えると切り詰め
- **シンボルは永続**。一度 intern したら解放されない (2 KB の sym pool)
- `call/cc` / 浮動小数 / `defstruct` 以外のレコード型などは未実装

---

## 10. テスト

mc6809 プラグイン crate のルート (`Cargo.toml` がある場所) で:

```sh
cargo test --release lisp_
```

35 smoke test が実行され、全件 passing すれば環境が正常動作しています。
