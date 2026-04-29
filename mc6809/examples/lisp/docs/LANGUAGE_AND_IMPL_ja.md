# Hha Lisp — 言語仕様と実装解説

本ドキュメントは `examples/lisp/lisp.asm` の**言語仕様**と**実装上の設計**を
まとめます。ユーザ向け使い方は [USER_GUIDE_ja.md](USER_GUIDE_ja.md) を
参照してください。

---

## 1. 実装規模

| 指標 | 値 |
|---|---|
| ソース行数 | **6,635 行** (単一ファイル `lisp.asm`) |
| 生バイナリ | **18,981 bytes** (≒ 18.5 KB) |
| SREC ファイル | 52,150 bytes (ASCII 形式、実体は生バイナリと同じ) |
| Primitive 数 | **60 個** (BI_* として予約済み) |
| Pre-declared symbol | **82 個** |
| Stdlib エントリ (ROM 埋込 Lisp) | **47 本** |
| Smoke test | **35 件** (約 128 秒、全件 passing) |

スケール感の比較:
- SectorLisp (512 B, 7 primitive) を大きく超える
- uLisp (ARM/AVR 向け ~30 KB, 200+ primitive) の約半分
- 古典 Tiny Lisp (2K LOC, 20 prim) の 2〜3 倍

---

## 2. 値タグとメモリマップ

### 2.1 値タグ方式 (16-bit word)

Lisp 値は常に 16-bit のタグ付き word。

| 値 | 範囲 | 種別 |
|---|---|---|
| `$0000` | NIL_VAL | NIL (偽 / 空リスト) |
| `$0002` | T_VAL | T (真) |
| `$0003..$2FFF` (**odd**) | fixnum | bit 0 = 1、`(x-1)/2` が符号付き整数 (-16384..16383) |
| `$4C00..$67FF` (**even**) | pair | cons セル、4 バイト単位 (car 2B + cdr 2B) |
| `$6800..$6FFF` | symbol | 可変長エントリ (next 2B + len 1B + name bytes) |
| `$7000..$7FFF` | builtin | primitive 関数の識別子 (BI_* tag) |
| `$8E00..$8FFF` | char | `CHAR_BASE + 2*code` (stride 2 で fixnum と衝突回避) |
| `$9000..$9FFF` | string | 偶数アドレス整列、`[len 1B][content]` |
| `$A200..$AFFF` | vector | 偶数アドレス、`[len 2B][elem 2B]*` |
| `$B000..$BFFF` | int32 box | 4 バイト単位、大きな整数を格納 |

**bit 0 = 1 は fixnum** の判定に使うため、他の値タグ (pair, string, vector
など) は常に **偶数アドレス**に配置されます。intern 時の奇数アドレス
衝突は `align_str_next` や symbol 整列ルーチンで回避されています。

### 2.2 メモリマップ (64 KB 中)

```
$0100..$4BFF  code + initialised data  (~19 KB)
$4C00..$67FF  pair pool      (7 KB = 1792 cells, GC 対象)
$6800..$6FFF  symbol pool    (2 KB, 永続)
$7000..$7FFF  builtin tag range (no RAM)
$8000..$8BFF  pair mark bitmap (3 KB)
$8C00..$8C7F  int32 mark     (128 B, 現状未使用)
$8C80..$8DFF  未使用
$8E00..$8FFF  char tag range (no RAM)
$9000..$9FFF  string pool    (4 KB, bump only)
$A000..$A1FF  TIB            (512 B, REPL 行バッファ)
$A200..$AFFF  vector pool    (3.5 KB, bump only)
$B000..$BFFF  int32 pool     (4 KB, bump only)
$C000..$FEFE  hardware stack (16 KB)
$FF00/$FF01   ACIA SR/DR
$FF02/$FF03   tick MMIO (CPU cycle low 16 bit, read-only)
$FFFE/$FFFF   reset vector → cold
```

---

## 3. 言語仕様

### 3.1 Reader

- S 式: `(...)` / `'x` (QUOTE) / `` `x `` (QUASIQUOTE) /
  `,x` (UNQUOTE) / `,@x` (UNQUOTE-SPLICING)
- 整数: 10 進、`-` 符号可、15-bit 範囲を超えたら **自動 box 化** (int32)
- 文字リテラル: `#\c`、stride-2 エンコード
- 文字列: `"..."`、エスケープ `\n` `\t` `\\` `\"`
- Dotted pair: `(a . b)`、ドット前後に whitespace 必須
- コメント: `;` から行末まで
- シンボル: whitespace / `()` / `$09` (TAB) / LF / CR で終端、**読込時 upcase**
- 数値 override: `-` 単独、`-foo` は symbol だが `-7` は negative number

### 3.2 評価規則 (`eval`)

```
eval(x) =
  if fixnum(x) / NIL / T / builtin / char / string / vector: self
  if symbol(x): lookup(x, current_env → global_env)
  if pair(x):
    if car(x) is a known special-form symbol: dispatch to handler
    else: apply(eval(car(x)), eval each cdr(x))
```

### 3.3 特殊形式 (17 個)

`QUOTE` `IF` `DEFVAR` `LAMBDA` `DEFUN` `COND` `LET` `LET*` `LETREC` `SETQ`
`PROGN` `AND` `OR` `DEFMACRO` `QUASIQUOTE` `CATCH` `THROW`

### 3.4 関数適用 (`ev_apply`)

- 評価された operator が
  - **builtin 値** (`$7000..$7FFF`): `BI_*` テーブルから dispatch
  - **closure** (pair with `car = sym_LAMBDA`): 3-pair chain で
    `(LAMBDA . (params . (body . env)))` を持ち、params に引数を bind して
    body を評価
  - **macro** (pair with `car = sym_MACRO`): 同じ 3-pair 構造だが、引数を
    **未評価のまま** bind し、body を評価して**展開**、展開結果を**もう一度**
    評価 (2-step)

### 3.5 Quasi-quote

- `` ` x ``: `(QUASIQUOTE x)` にラップされ、ev_quasiquote で展開
- `qq_depth` カウンタでネストを追跡、内側の UNQUOTE は depth > 1 では eval せず
  `(UNQUOTE x)` のまま再構築
- `qq_walk` が tail-pointer 方式でリストを再構築、`,@` は splice

---

## 4. 実装解説

### 4.1 Closure / Macro 構造

Closure / macro は 3 つの pair チェーン:

```
outer pair: (LAMBDA-or-MACRO . mid)
mid pair:   (params . inner)
inner pair: (body . captured_env)
```

- `build_closure` で alloc (3 pair 消費)
- `ev_apply` が `car(fn)` で LAMBDA / MACRO を区別

### 4.2 環境

- `current_env`: lambda / let が push する局所環境
- `global_env`: defvar / defun が prepend する大域環境
- 両者とも alist `((sym . val) ...)` の pair リスト
- Lookup は `ev_lookup` で current → global の順

### 4.3 Garbage Collector

**Mark-and-sweep**、pair pool のみを対象 (symbol, string, vector, int32 は
bump only)。

- **Mark bitmap**: `$8000..$8BFF` に 1 byte/pair
- **Free list**: 回収済み cell は car を通じてチェーン
- **Alloc 戦略**: free list 優先 → bump alloc → OOM
- **ルート**: `global_env`, `current_env`, `current_closure`, 全 `ev_*`
  スクラッチ変数
- **Vector pool スキャン**: `gc_mark_vec_pool` が allocated vector 内の
  pair ptr を mark (vector 要素の pair は vector 経由でしか reachable でないため)
- **Hybrid auto-GC** (`alloc_pair` 内):
  - 入口で `pshs y,d` して Y=car / D=cdr を stack に置く
  - pool 枯渇時に `gc_run_safe` を 1 回実行 (保守的 stack scan 付き)
  - 成功なら retry、失敗なら OOM
  - `alloc_gc_tried` flag で無限ループ回避
- **REPL トップでの auto-(gc)**: 毎行開始時に `gc_run` 実行、前ターンの
  garbage を確実に回収 (stack 完全に空の安全なタイミング)

### 4.4 末尾呼出最適化 (TCO)

`ev_ap_done_bind` で closure の body 最終式を tail 位置として扱う:

1. Body が `(PROGN e1 ... eN)` の場合、eN が tail 位置
2. tail 位置で IF / PROGN は**tail-transparent** (分岐先 / 末尾式が
   改めて tail 位置となり再帰的に dispatch)
3. tail 位置が関数呼出なら:
   - **Self-TCO** (current closure と同一): 
     - 引数を全て eval して `stco_vals[]` にバッファ
     - 既存 binding の cdr を **in-place mutate**
     - ev_ap_body_start へ lbra (新 frame 作らず)
     - **新規 pair 割当ゼロ** (garbage-free)
   - **General TCO** (別 closure): `ev_tail_mode` = 1、
     ev_ap_closure_proper へ lbra (outer の env save pshs を再利用)

### 4.5 Stdlib ブートストラップ

`load_stdlib` が cold boot で:

1. `stdlib_table` の各 source 文字列ポインタを順に取得
2. TIB_ADDR にコピー (TIB_SIZE=512 超えは切捨て)
3. `read_expr` → `eval` で順次評価
4. これで `not` / `mapcar` / `case` / `defstruct` / `format` 等を定義

47 エントリ、ほとんどが `(defun ...)` または `(defmacro ...)`、一部に
ヘルパ関数 (`case-expand`, `ds-acc`, `format-step` 等)。

### 4.6 Primitive の第一級化

Builtin 値は `$7000..$7FFF` の tag ID として `global_env` に bind される:

```
(CONS . $7000)  (CAR . $7002)  (CDR . $7004) ...
```

シンボル `CONS` を評価すると `$7000` (builtin 値) が返り、
`(cons 1 2)` と `((eval 'cons) 1 2)` と `((if t car cdr) '(1 . 2))` が
どれも動く。

この第一級化のおかげで、stdlib 側で Scheme 風の `?`-末尾エイリアスを
追加コストほぼゼロで提供できる。`(defvar null? null)` のように
同じ callable 値を別名で再束縛するだけ:

```
(defvar null? null)
(defvar atom? atom)
(defvar eq?    eq)
(defvar zero?  zerop)
```

これで `(filter zero? xs)` や `(any null? xs)` と CL 風の bare 名が
共存できる。意図と背景は §6 を参照。

### 4.7 エラー処理

- `catch_stack` (8 エントリ × 6 byte)、各 frame = (tag, saved_s, saved_env)
- `(catch tag body)` で frame push、saved_s に現在の S を記録
- `(throw tag value)` が catch_stack を walk、一致 tag で longjmp
  (saved_s に S を戻して value を X に)
- `(error msg)` は REPL 入口に unwind (`lds repl_init_s`)

### 4.8 ev_let の nested clobbering 対策

`ev_let` は `ev_lt_bindings` / `ev_lt_cur` / `ev_lt_newenv` / `ev_lt_body`
の 4 つのメモリ scratch を使う。nested LET (例: `(let ((i (inner-fn-with-let ...))) ...)`)
が内側で外側の scratch を上書きするため、arg 値 eval 前後で 4 変数を
`pshs d` / `puls d` で hardware stack に退避。

### 4.9 Cycle counter (MMIO)

- `PluginBus::tick_word: u16` を step_one で cycle 数加算
- `$FF02` read → high byte、`$FF03` read → low byte
- `(tick)` primitive は `ldd $FF02` で 16-bit 値を取得、14-bit に mask して
  fixnum で返す

---

## 5. 実装統計

### 5.1 主要ファイル構成

```
lisp.asm       6,635 行 (単一ファイル)
  ├ equ / 定数          1-120
  ├ cold-boot / intern  120-400
  ├ global_env bind     400-570
  ├ alloc_pair + auto-GC 1070-1170
  ├ intern + align      1170-1290
  ├ read_expr family    1290-1560
  ├ print_expr family   1560-1900
  ├ eval + lookup       1900-2140
  ├ primitive 分岐      2140-2780
  ├ ev_apply + TCO      2780-3270
  ├ special forms       3270-3900
  ├ string / char / vec primitive 3900-4400
  ├ logical / arith     4400-4700
  ├ xorshift32 + tick   4700-4880
  ├ conversions         4880-5080
  ├ stdlib text         5800-6100
  ├ REPL + gc           6100-6300
  └ RAM 変数宣言        6300-6635
```

### 5.2 Pair pool 消費実測

- Cold boot + stdlib load で ~400 cells 使用
- 典型的な `(fact 10)` が ~30 cells/call (自動 GC で回収)
- 相互再帰 2000 深 (tco stress test) も安定通過

### 5.3 開発過程で発見された em6809 CPU バグ 3 件

1. **SBC borrow-in 反転** — 32-bit 多バイト減算 (Tiny Lisp の int32 実装中に発見)
2. **PC-relative indexed の `,S` 誤解釈** — `addd ,s` が S ではなく PC を読む
3. **LEAS/LEAU 入替 / ABX / TST mem / INC mem 等の不足**

これらは `em6809` クレートに修正として反映されました。

---

## 6. 設計上の選択と妥協

| 選択 | 理由 |
|---|---|
| 固定サイズ pool (bump / free-list) | malloc 実装回避、単純で堅牢 |
| 単一ファイル asm | mc6809 では `.include` 依存が面倒、単一の方が読みやすい |
| UPPERCASE 正規化 | Common Lisp 互換、小文字入力も動く |
| Classic Lisp 風 (defun/setq) | Scheme より書きやすく教材向き |
| 第一級 primitive | `(mapcar car ...)` が動く Modern Lisp 感 |
| Scheme 風 `?`-末尾エイリアス (`null?` / `atom?` / `eq?` / `zero?`) | SICP / Racket / Clojure 出身者の onboarding コスト低減、実装コストは defvar 1 行ずつ |
| 3-pair closure | Lisp 1.5 風で実装単純、GC 対象 |
| 2-phase self-TCO | eval 順序と mutation のタイミング分離で semantic 正しい |
| ROM 埋込 stdlib | ライブラリロード機構不要、cold boot で確実に使える |

---

## 7. 今後の改善候補

| 項目 | 予想コスト | 優先度 |
|---|---|---|
| STR_POOL の copying GC | 8-10 h | ⭐⭐ |
| int32 pool の GC | 4-6 h | ⭐⭐ |
| 浮動小数点 (IEEE 754 単精度) | 大 | ⭐ |
| `call/cc` | 大 | ⭐ |
| BASE 切替 (16 進数入力) | 1-2 h | ⭐ |
| バイトコード化 | 大 | ⭐ |

---

## 8. ライセンス

MIT OR Apache-2.0 (デュアルライセンス)。詳細は `lisp.asm` の SPDX ヘッダを
参照してください。
