# Hha Lisp — 言語仕様と実装解説

本ドキュメントは `examples/lisp/lisp.asm` の**言語仕様**と**実装上の設計**を
まとめます。ユーザ向け使い方は [USER_GUIDE_ja.md](USER_GUIDE_ja.md) を
参照してください。

---

## 0. 系譜と設計方針

Hha Lisp は単一の Lisp 系譜には収まりません。教育用としての分かりやすさ・
組込みターゲットの制約・複数文化からのユーザー受け入れコストの 3 軸の
バランスを取って、複数伝統から機能を意図的に組み合わせています。本章を
最初に読むと、後続の設計判断 (述語エイリアス、`set!`、printer の
case mode 等) を「個別の判断」として評価しやすくなります。

### 0.1 系譜マップ

| 観点 | 採用伝統 | 理由 |
|---|---|---|
| 表面の特殊形式 (`defun` / `setq` / `cond` / `progn` / `defmacro`) | Common Lisp | `(defun f (x) ...)` は `(define (f x) ...)` より初学者に説明しやすい |
| ブール / 空リスト | Common Lisp (`t` / `nil`、`nil` は空リストでもある) | tag 体系がシンプル、定数 1 個分の節約 |
| シンボル case | Common Lisp (reader が読み込み時に UPPERCASE 化) | 小文字入力も受け、表示は正規化される。printer 側の case mode は今後対応予定 |
| Quasiquote | Common Lisp + Scheme 共通 (`` ` `` / `,` / `,@`) | Lisp 共通方言、構文同一 |
| マクロ衛生性 | Common Lisp (`defmacro` 非衛生、`with-gensyms` で手動衛生化) | `syntax-rules` はコスト過大、手動 gensym は教材としても良い |
| 評価名前空間 | **Lisp-1** (Scheme / Arc / Clojure 系) | 第一級プリミティブで `(filter zero? xs)` が `#'` 不要で動く |
| ユーティリティ命名 (`string->symbol` / `vector-set!` / `char->integer`) | Scheme / R5RS | 矢印は変換、`!` は破壊的操作という規約が広く通じる |
| 末尾呼出最適化 | Scheme (必須) | Lisp-1 環境ではループは再帰で書くのが自然 |
| メモリモデル (固定 pool / mark-sweep GC / ROM 埋込 stdlib) | 組込み Lisp 伝統 (uLisp、Lispkit) | 予測可能、`malloc` 不要、起動時 I/O 不要 |

最も近い直系は **uLisp** (AVR / ARM 向け Common Lisp サブセット
組込み Lisp) ですが、Hha Lisp は意図的に **Lisp-1** 化されています。
プリミティブ数も uLisp より小さく (60 個 vs 200+)、コアもよりリーン
です。

### 0.2 設計原則

1. **Classic Lisp 表面を保つ** — `defun` / `setq` / `t` / `nil` は
   Scheme エイリアスの別名ではなく、それ自体が一級の名前。Lisp-1
   評価により `defun` は意味的に Scheme の `define` と等価だが、
   それでも名前を変えない。CL / Emacs Lisp 出身者を混乱させる
   見返りが無いから。
2. **複数文化からのエイリアスは追加のみ** — `null?` / `atom?` /
   `eq?` / `zero?` / `set!` は CL 流の既存名のエイリアスとして
   提供される。Scheme / Racket / Clojure / SICP 出身者が自然に
   読み書きできるように。古い名前は一切リネーム/リダイレクトされて
   いない。
3. **トレードオフは docs に明記する** — 第 6 章「設計上の選択と
   妥協」表を見れば、明示的な選択がすべて並んでいる。新規エイリアス
   や機能の追加判断は ROM コスト・dispatcher cycle・概念表面積との
   引き換えで毎回判定。
4. **組込み制約は一級市民** — どの機能も固定 pool に収まり、
   mark-sweep GC で正しく動き、lwasm でランタイムライブラリ不要で
   実装可能でなければならない。収まらない機能 (CL の `loop` マクロ
   フル実装、`call/cc`、IEEE 754) は cost/benefit が明白に勝つまで
   保留。
5. **Reader は変えない、printer は設定可能に** — 読み込み時の
   UPPERCASE 化は変えられない (シンボル identity が壊れる) が、
   printer 側の case (`T` / `NIL` / `FACT` の "叫んでる" 感覚) は
   セッション単位の toggle で切替可能。`(set-print-case! 1)` で
   lowercase 出力に、`(set-print-case! 0)` で upper に戻る。
   default は upper でトランスクリプト互換維持。

### 0.3 これが利用者に意味すること

| 出身 | そのまま動くこと |
|---|---|
| **Common Lisp / Emacs Lisp** | 全特殊形式 (`defun` / `setq` / `t` / `nil` / `defmacro` / quasiquote) が完全に期待通り。高階関数で `#'` が不要なのは罠ではなく嬉しい簡素化 |
| **Scheme / Racket / SICP / Clojure** | `null?` / `atom?` / `eq?` / `zero?` / `set!` を使える、`cons` / `car` / `cdr` が分かる、末尾再帰 idiom が動く、プリミティブを高階関数の引数に直接渡せる |
| **uLisp / 組込み Lisp 利用者** | 表面構文は Lisp-1 という違い以外そのまま、ROM 埋込 stdlib bootstrap も同系統 |
| **プログラミング初学者** | `(defun f (x) ...)` から素直に入れる、数値 / リスト / 文字列 / ベクタ / Q8.8 / hashtable / records が classic 命名で揃う |

足りないエイリアスや表面の選択で困ったら、追加なら気軽に対応する。
既存名のリネーム/削除こそが避けたいケース。

---

## 1. 実装規模

| 指標 | 値 |
|---|---|
| ソース行数 | **6,635 行** (単一ファイル `lisp.asm`) |
| 生バイナリ | **18,981 bytes** (≒ 18.5 KB) |
| SREC ファイル | 52,150 bytes (ASCII 形式、実体は生バイナリと同じ) |
| Primitive 数 | **62 個** (BI_* として予約済み) |
| Pre-declared symbol | **84 個** |
| Stdlib エントリ (ROM 埋込 Lisp) | **51 本** |
| Smoke test | **38 件** (約 135 秒、全件 passing) |

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
| `$4D80..$6FFF` (**even**) | pair | cons セル、4 バイト単位 (car 2B + cdr 2B) |
| `$7000..$7DFF` | symbol | 可変長エントリ (next 2B + len 1B + name bytes) |
| `$7E00..$7E7F` | builtin | primitive 関数の識別子 (BI_* tag) |
| `$8E00..$8FFF` | char | `CHAR_BASE + 2*code` (stride 2 で fixnum と衝突回避) |
| `$9000..$9FFF` | string | 偶数アドレス整列、`[len 1B][content]` |
| `$A200..$AFFF` | vector | 偶数アドレス、`[len 2B][elem 2B]*` |
| `$B000..$BFFF` | int32 box | 4 バイト単位、大きな整数を格納 |

**bit 0 = 1 は fixnum** の判定に使うため、他の値タグ (pair, string, vector
など) は常に **偶数アドレス**に配置されます。intern 時の奇数アドレス
衝突は `align_str_next` や symbol 整列ルーチンで回避されています。

### 2.2 メモリマップ (64 KB 中)

```
$0000..$00FF  reset stub / 未使用         (256 B)
$0100..$4D7F  コード + 初期化済データ    (~19.5 KB)
$4D80..$6FFF  pair pool                   (8.5 KB = 2208 cells、GC 対象)
$7000..$7DFF  symbol pool                 (3.5 KB、永続)
$7E00..$7E7F  builtin tag range           (RAM 不在 — 論理 ID 64 個)
$7E80..$7FFF  予約
$8000..$8BFF  pair mark bitmap            (3 KB — 1 cell につき 1 byte)
$8C00..$8C7F  int32 mark                  (128 B、現状未使用)
$8C80..$8DFF  予約
$8E00..$8FFF  char tag range              (RAM 不在 — 論理 ID 256 個、stride 2)
$9000..$9FFF  string pool                 (4 KB、bump only)
$A000..$A1FF  TIB                         (512 B — REPL 行バッファ)
$A200..$AFFF  vector pool                 (3.5 KB、bump only)
$B000..$BFFF  int32 box pool              (4 KB、bump only)
$C000..$FEFE  hardware stack              (~16 KB、$FEFE から下に伸びる)
$FF00/$FF01   ACIA SR / DR                (host I/O)
$FF02/$FF03   tick MMIO                   (CPU cycle 下位 16 bit、RO)
$FFFE/$FFFF   reset vector → cold
```

64 KB のアドレス空間は、**Lisp 値の bit パターンそのものが種別を
一意に識別する**ように仕切られている。ポインタが落ちる位置がそのまま
種別なので、別途のタグワードは不要。これにより `eval` のディス
パッチは数個の定数比較だけで済み、各値は 16 bit に収まる。

#### なぜこの配置か

- **`$0000..$00FF`** は 6809 の direct page (DP) 領域。本実装は DP
  最適化を使わないので空けてある — scratch 用の余裕。
- **`$0100..$4D7F`** はアセンブル済の interpreter 本体: opcode、
  ROM 埋込 stdlib (`sl_*` の文字列群)、事前確保された symbol
  scratch、GC ルート — 起動時に `lisp.s19` から読み込まれる全て。
  **最大の単一ブロック**で、interpreter ソースが ~6,800 行ある
  ためここを占有する。`PAIR_POOL` の開始位置はコードが伸びる
  ごとに上がる; 直近の修正 (PR #18 → #19 → #20) で `$4C80` から
  `$4D80` まで上がった。
- **`$4D80..$6FFF` (pair pool)** が稼働メモリの主役。あらゆる
  cons セル・closure・環境バインディングがここに住む。各セルは
  4 byte (car 2B + cdr 2B)、**4 byte 整列が必須** — これにより
  「bit 0 = 1 は fixnum」が確実な判定になる (pair pointer は常に
  偶数かつ `$4D80` 以上)。mark-sweep GC はこの pool だけが対象、
  *他は GC されない*。
- **`$7000..$7DFF` (symbol pool)** は **永続** — intern された
  symbol は解放されない。各エントリは `[next 2B][len 1B][name…]`、
  next リンクで `sym_list` を頭とした単方向リスト。sweep 時は
  完全にスキップ。
- **`$7E00..$7E7F` (builtin tag)** は **論理空間のみ** — RAM は
  マップされない。`BI_*` 定数 (例: `BI_CONS = $7E00`) は固有の
  16-bit パターンで、`eval` は範囲チェックで識別する。primitive
  を一級値として扱う (Lisp-1) ので `(filter zero? xs)` が `#'`
  なしで動く。
- **`$8000..$8BFF` (pair mark bitmap)** は pair cell 1 つにつき
  1 byte を持つ; `gc_mark` で立て、`gc_sweep` で消す。1 bit/cell
  ではなく 1 byte/cell にしたのは 3 KB のコスト引き換えに mark
  テストを `tst ,x` 1 命令で済ませるため (shift / mask 不要)。
- **`$8E00..$8FFF` (char tag)** も論理のみ — 文字コード `c` は
  `$8E00 + 2*c` で表現、stride 2 で偶数アドレスを保つ。256 文字
  × 2 byte = 512 個の論理 ID。
- **`$9000..$9FFF` (string pool)** / **`$A200..$AFFF` (vectors)**
  / **`$B000..$BFFF` (int32 boxes)** は全て **bump only**。一度
  確保された string / vector / int32 はリセットまで居座る。これは
  意図的な簡略化 — string と vector は典型的に定数 (リテラル
  リスト、`(string->vector ...)`) で、3 領域追加 sweep するコスト
  が大きすぎる。トレードオフは、長時間セッションで string churn
  が多いと最終的に bump 上限に当たることだが、実用上 pair pool
  枯渇のほうが先に来る。
- **`$A000..$A1FF` (TIB)** = Terminal Input Buffer。REPL は 1 行
  ずつ TIB に読み込み、`read_expr` がそこから parse。512 B あれば
  長い stdlib `defmacro` の本体や `>>` 継続入力で複数行になる
  REPL 式も収まる。
- **`$C000..$FEFE` (hardware stack)** は `$FEFE` から下へ伸びる。
  6809 の S レジスタはこの範囲を指す。関数のネスト呼出、`eval`
  の再帰中に scratch を保護する `pshs`/`puls` (これが in-flight
  pair pointer を conservative に root する経路 — §4.4 GC 参照)、
  `(error ...)` の longjmp 用 anchor `repl_init_s` などが全て
  この 16 KB を共有する。
- **`$FF00..$FF03`** は MMIO: `$FF00`/`$FF01` の ACIA (status /
  data、ホストとのシリアル) と `$FF02`/`$FF03` の tick カウンタ
  (CPU cycle 下位 16 bit) — ベンチマークや `(seed (tick))` 用。
- **`$FFFE/$FFFF`** は 6809 の reset vector。`cold:` を指す。

#### 数字で見る

| 領域 | バイト数 | セル数 / 最大要素数 | ライフタイム |
|---|---:|---:|---|
| Code + データ | ~19.5 KB | — | Static (`lisp.s19` から起動時にロード) |
| pair pool | 8.5 KB | **2208 cons セル** | GC 対象、mark-sweep |
| symbol pool | 3.5 KB | ~280 symbol (可変長) | 永続 |
| string pool | 4 KB | bump only | 永続 (リセットまで) |
| vector pool | 3.5 KB | bump only | 永続 (リセットまで) |
| int32 pool | 4 KB | 1024 box (4 B each) | overflow 時に free-list 経由で再利用 |
| TIB | 512 B | 1 入力行 | 各 REPL prompt |
| stack | ~16 KB | 関数呼出のフレーム | 各呼出 |

**おおよそ 64 KB の半分は interpreter 自身** (コード + 永続 symbol
+ bump pool 群)、残り半分が稼働中に動的に回転する状態 (pair pool
+ stack + TIB)。

#### 「2208 pairs に何が収まるか」の感覚

数字を実感するには: 8 クイーン (count) を `n = 8` で走らせると
探索木のノードを ≈ 1600 万回訪問する。各ノードで素朴な非 tail
再帰なら ~5 個の binding pair を確保 (≈ 8000 万回の確保)。
self-TCO は現フレームを mutate して再利用するので、この探索は
**pair pool 使用量を一定に抑えて**完走する。これが
[SHOWCASE_ja.md](SHOWCASE_ja.md) §2 「8 クイーン」で、Variant A
(末尾再帰の `try-rows`) は完走、Variant B (`+` の中の非末尾再帰)
は pool 枯渇、と分岐する根本理由。

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

`SET!` は `SETQ` の Scheme 流エイリアスとして eval dispatcher
(および TCO bail-out 判定) に登録されている。シンボルとしては
別個に intern されるが、どちらも同じ `ev_setq` ハンドラへ分岐するため
mutation の意味は完全に同じ。出身言語に合わせて好きな方を使えばよい。

### 3.4 関数適用 (`ev_apply`)

- 評価された operator が
  - **builtin 値** (`$7E00..$7E7F`): `BI_*` テーブルから dispatch
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

Builtin 値は `$7E00..$7E7F` の tag ID として `global_env` に bind される:

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

### 4.8a ev_cond の再入対策と tail-transparency

`ev_cond` は元々 `ev_cn_args` / `ev_cn_clause` のグローバル scratch を
ループ状態として使っていた。clause の test 式が再帰的に別の `cond` を
発火させた場合 (例: predicate 関数が cond で書かれている)、内側の
evaluation がそれらのグローバルを上書きし、test 戻り後に外側の
`ev_cond` が誤った clause を再ロードして、本来評価すべき body が
黙って跳ばされる挙動になっていた。さらに `cond` は
`ev_ap_tail_dispatch` の blacklist にあり、cond ボディ最終 form の
TCO が効かなかった。両方が重なって、典型的な multi-body 再帰イディオム
が pair pool を枯渇させたり、明らかに誤った数値を返す症状を引き起こし、
8 queens を素直な cond で書くと再現した (issue #14)。

修正は 2 段:

- `ev_cond` のループ状態を S スタックに退避するようリライト。
  `ev_progn` / `ev_let` と同じ流儀。test 中の nested cond による
  外側状態の破壊が起きなくなる。
- `ev_ap_tail_dispatch` で `cond` を tail-transparent として認識し、
  match した clause body を `ev_ap_tail_progn` 経由で dispatch。
  これで body 最終 form が完全 TCO され、それ以前の form は副作用
  として正しく評価される。`(cond ((test) body1 body2 ...))` が
  `(if test (progn body1 body2 ...))` と同じくループ向き挙動を取る。

回帰テスト: `lisp_cond_reentrant_and_tail_transparent` が cond 版
8 queens を n ∈ {4, 5, 6} で実行、各 2 / 10 / 4 を assert。

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
| `SET!` を dispatcher で `SETQ` のエイリアスとして認識 | 同じ層の利用者向け — Scheme では `(set! x v)` が反射的に手から出る。コストはシンボルスロット 1 個と 2 箇所の dispatch site への `cmpy/lbeq` 各 1 ペアのみ |
| Printer case mode の toggle (`(set-print-case! 0|1)`) | UPPERCASE の "叫んでる" 出力からの逃げ道を提供 (reader は不変)。コストは BI primitive 2 個、RAM 1 byte、`pr_symbol` と `puts_cased` 内の case-fold チェック |
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
