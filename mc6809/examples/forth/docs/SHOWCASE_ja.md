# Hha Forth — アルゴリズムショーケース

MC6809 エミュレータ上の Hha Forth で動かすアルゴリズムを 5 本紹介。
古典 3 本 (ハノイの塔・8 クイーン・クイックソート) に加え、Forth
固有テーマ 2 本 (`CREATE`...`DOES>` と `BASE` の基数切替) を並べ、
他言語にきれいに対訳できない Forth の長所も見せます。

各ブロックは **paste-ready**: 下のトランスクリプトは実機 REPL から
そのまま採取しました。言語仕様の参照は [USER_GUIDE_ja.md](USER_GUIDE_ja.md)、
ITC スレッディングや辞書構造は [LANGUAGE_AND_IMPL_ja.md](LANGUAGE_AND_IMPL_ja.md)
を参照。

> **複数行 `:` 定義に対応**しています (`>>` のような継続プロンプトは
> 無いが、各行末に ` ok` が出ても compile state は ACCEPT サイクルを
> 跨いで保たれ、`;` で確定する)。複数行は **可読性のため** に使う
> もので、TIB が 512 byte あるので 1 行の長さは事実上の制約には
> なりません。

---

## 1. ハノイの塔

最も素直な再帰のデモ。*n* 枚の円盤を `A` から `C` へ、`B` を中継地
点として、常に大きい円盤を小さい円盤の上に置かないように移動する。

Forth には名前付きローカル変数が無いため、4 つの引数 (`n / from /
to / via`) を再帰の度にデータスタックで manage するのは非常に骨が
折れる。**3 本のペグを `VARIABLE` に置いてその場で値を入れ替える**
のが Forth 流の素直な解決で、再帰自体が運ぶのは `n` だけになる。

### コード

```forth
VARIABLE FROM-PEG  VARIABLE DEST-PEG  VARIABLE VIA-PEG

: SHOW-MOVE  ( n -- )  ." Move disk " . ." from " FROM-PEG @ EMIT SPACE ." to " DEST-PEG @ EMIT CR ;
: SWAP-DV  DEST-PEG @ VIA-PEG @ DEST-PEG ! VIA-PEG ! ;
: SWAP-FV  FROM-PEG @ VIA-PEG @ FROM-PEG ! VIA-PEG ! ;

: HANOI  ( n -- )  DUP 0> IF  SWAP-DV  DUP 1- RECURSE  SWAP-DV  DUP SHOW-MOVE  SWAP-FV  DUP 1- RECURSE  SWAP-FV  THEN  DROP ;

CHAR A FROM-PEG !  CHAR C DEST-PEG !  CHAR B VIA-PEG !
```

### REPL トランスクリプト

```
> 3 HANOI
Move disk 1 from A to C
Move disk 2 from A to B
Move disk 1 from C to B
Move disk 3 from A to C
Move disk 1 from B to A
Move disk 2 from B to C
Move disk 1 from A to C
 ok
```

### 解説ポイント

- **`RECURSE`** は自己呼出をコンパイル。FORTH-83 標準のイディオム。
- **`SWAP-DV` / `SWAP-FV`** は古典的な `var1 @ var2 @ var1 ! var2 !`
  パターン (DUP 不要 — 2 つの `!` がきれいに値を消費する)。
- **`CHAR A`** は REPL で 65 を push する。`[CHAR]` は `:` 定義の
  内部用。
- **`."  to "`** は実は `"to "` (先頭 space は delimiter として消費)
  と展開される。先頭 space が必要なときは `SPACE` を明示する。

---

## 2. 8 クイーン — 解の総数

バックトラッキング探索。`1..n` の各列に互いに脅かし合わない位置へ
クイーンを置き、その解の数を数える。

Lisp ではリスト `placed` を再帰で持ち回したが、Forth では **配列**
を使う方が自然。`CREATE QROW 16 CELLS ALLOT` で 16 セルの mutable
領域を確保し、column を index にして row を格納する。

### コード

```forth
VARIABLE NQ  VARIABLE QCNT
CREATE QROW 16 CELLS ALLOT
VARIABLE COL-V  VARIABLE ROW-V  VARIABLE CFL

: ABS_  DUP 0< IF NEGATE THEN ;
: GET-ROW  CELLS QROW + @ ;
: BAD-ROW?  GET-ROW ROW-V @ = ;
: BAD-DIAG?  DUP GET-ROW ROW-V @ - ABS_  SWAP COL-V @ - ABS_  = ;
: AT-CONFL?  DUP BAD-ROW? IF DROP TRUE EXIT THEN BAD-DIAG? ;

: SAFE?  ( -- flag )
  COL-V @ 0= IF TRUE EXIT THEN  0 CFL !  COL-V @ 0 DO I AT-CONFL? IF TRUE CFL ! LEAVE THEN LOOP  CFL @ 0= ;

: PLACE-COL  ( c -- )
  DUP NQ @ = IF DROP 1 QCNT +! EXIT THEN  NQ @ 0 DO  DUP COL-V !  I ROW-V !  SAFE? IF DUP CELLS QROW + I SWAP !  DUP 1+ RECURSE THEN  LOOP  DROP ;

: QUEENS  ( n -- count )  NQ !  0 QCNT !  0 PLACE-COL  QCNT @ ;
```

### REPL トランスクリプト

```
> 4 QUEENS .
2  ok
> 5 QUEENS .
10  ok
> 6 QUEENS .
4  ok
> 7 QUEENS .
40  ok
> 8 QUEENS .
92  ok
```

`n = 6` で 4 という値は canonical な「凹み」 (`n = 5` の 10 より
*少ない*) で、[OEIS A000170](https://oeis.org/A000170) 通り。Hha
Forth は他のどの実装とも同じ数値を返す。

### 解説ポイント

- **`I` は直接の `DO`/`LOOP` の中でのみ動く**。`PLACE-COL` の本体が
  colon-defined な `SAFE?` を呼び出すと、`SAFE?` 自身の `DO` ループ
  が return stack に独自の枠を push する。`SAFE?` の中で `I` を読む
  と `SAFE?` のループ index が読まれ、`PLACE-COL` のものではない。
  これを避けるため、`I` は `PLACE-COL` 直下で読み `ROW-V` に格納
  する形にしている。
- **`?DO` は FORTH-83 に無い**。`0 DO` で start `==` limit だと
  65536 回ループしてしまう。よって `SAFE?` 冒頭で
  `COL-V @ 0= IF TRUE EXIT THEN` のショートカット必須。
- **`+!` (`PSTORE`)** で QCNT のインプレース加算。Forth ならではの
  短い idiom で、他言語なら一時変数が要る場面。
- **スタック juggling が支配的なコスト**。Lisp なら 14 行のアルゴ
  リズムが、ここでは小さなヘルパ語 5 個 + 状態保持 `VARIABLE` 3 個
  に分解された。

---

## 3. クイックソート

Lomuto partition による in-place 配列クイックソート。Forth で書ける
QSort の **一つの選択肢** であって、後述するように他にも道はある。
それでも in-place を採るのは、**Hha Forth に GC が無い** から
(Forth-79 から Forth 2012 までいずれの仕様も GC を規定していない —
ANS Forth / Forth-94 は代わりに明示的 `ALLOCATE` / `FREE` を導入
したが、自動回収は応用側に委ねる立場をとった)。untyped cell +
linear data space という Forth の core モデルが GC を構造的に
組み込みにくくしている。

### 代替案 — なぜ in-place が勝つか

| 方式 | Pros | Cons |
|---|---|---|
| **配列 in-place (採用)** | メモリ最小、追加確保不要、`CELLS @ !` の標準イディオム、GC 不在環境でも安全 | Lomuto は安定でない、スタック juggling、index 演算が in-bounds に収まる前提 |
| **cons セル連結リスト** (手作りの `HERE 2! 4 ALLOT`) | Lisp に近い書き味、再帰 partition が短く綺麗 | GC が無いため、再帰 1 回ごとに dictionary が永久に伸びる。2 回目の呼出で領域が枯渇する可能性 |
| **auxiliary buffer + マージソート** | 安定ソートにできる、partition logic が単純 | メモリ 2 倍、`CREATE` で事前に固定サイズを取る必要 |

3 つのうち、free-list を手作りせずに **繰返し** 走らせられるのは最初の
ものだけなので、本章は in-place 方式で進める。

### コード

```forth
16 CONSTANT NN
CREATE ARR NN CELLS ALLOT

: ARR@  CELLS ARR + @ ;
: ARR!  CELLS ARR + ! ;

VARIABLE QS-TMP  VARIABLE QS-PIV  VARIABLE QS-PI  VARIABLE QS-LO  VARIABLE QS-HI

: SWAP-CELLS  ( i j -- )  OVER ARR@ QS-TMP !  DUP ARR@ ROT ARR!  QS-TMP @ SWAP ARR! ;

: PARTITION  ( lo hi -- p )
  QS-HI !  QS-LO !  QS-HI @ ARR@ QS-PIV !  QS-LO @ 1- QS-PI !
  QS-HI @ QS-LO @ DO I ARR@ QS-PIV @ > 0= IF QS-PI @ 1+ QS-PI !  QS-PI @ I SWAP-CELLS THEN LOOP
  QS-PI @ 1+ DUP QS-HI @ SWAP-CELLS ;

: QSORT-R  ( lo hi -- )  BEGIN 2DUP < WHILE 2DUP PARTITION >R SWAP R@ 1- RECURSE R> 1+ SWAP REPEAT 2DROP ;

: LOAD-TEST  5 0 ARR! 3 1 ARR! 8 2 ARR! 1 3 ARR!  9 4 ARR! 4 5 ARR! 2 6 ARR! 7 7 ARR! ;
: SHOW-N  ( n -- ) 0 DO I ARR@ . LOOP ;
```

### REPL トランスクリプト

```
> LOAD-TEST  8 SHOW-N CR
5 3 8 1 9 4 2 7
 ok
> LOAD-TEST  0 7 QSORT-R  8 SHOW-N CR
1 2 3 4 5 7 8 9
 ok
```

### 解説ポイント

- **`CELLS`** と **`+`** で型付きセル配列を index する。
  `: ARR@ CELLS ARR + @ ;` が canonical な accessor。`CELLS` で
  index にセルサイズ (6809 では 2 byte) を掛け、`+` で base address
  に加え、`@` で読み出す。
- **手動の末尾再帰除去** — 本体は `BEGIN ... WHILE ... REPEAT` で
  右半分を **iterate**、左半分のみ `RECURSE` する。これにより return
  stack 深度を `O(log n)` に抑える (素朴な双方再帰だと `2 × O(log n)`)。
  Lisp なら `let` / `dolist` のインフラが一時変数を吸収するが、Forth
  ではその代わりに明示的なスタック管理を引き受ける。
- **`>R SWAP R@ 1- RECURSE`** は pivot index を return stack に
  退避してから再帰呼出、`R> 1+ SWAP` で次の iteration 用に
  `( p+1 hi )` を組み立て直す。「外で save、内で restore」のパターンは
  Forth の至る所に出てくる。
- **`PARTITION` は `QS-HI ! QS-LO !` だけで両引数を消費する**
  (DUP / SWAP 不要)。最初の試作で `DUP QS-HI ! SWAP QS-LO !` と
  書いたが、`DUP` が増やした分を 2 つの `!` が消費しきれず **1 セル
  分のデータスタックリーク**になった。短い形のほうが正しい。

---

## 4. Forth 固有 — `CREATE` / `DOES>`

Forth が他言語に無い領域に踏み込む真骨頂。**「定義語を定義する語」**
が 3 行で書ける:

```forth
: MYARRAY  CREATE CELLS ALLOT  DOES> SWAP CELLS + ;
```

`MYARRAY` 自身が定義語になり、これを呼べば名前を parse し、領域を
確保し、その名前のランタイム挙動を `DOES>` 以降のコードに結びつける。

### REPL トランスクリプト

```
> 5 MYARRAY GRID
 ok
> 42 0 GRID !
 ok
> 99 1 GRID !
 ok
> 0 GRID @ .
42  ok
> 1 GRID @ .
99  ok
```

### 解説ポイント

- **`CREATE`** は dictionary に header を確保し、現在のコード位置に
  リンクする。ランタイム挙動は body アドレスをデータスタックに
  push する。
- **`CELLS ALLOT`** が定義時に `5 × 2 = 10` byte のセル領域を
  header の直後に予約する。
- **`DOES>`** がランタイム挙動を上書きする。「body アドレスを push」
  ではなく「swap, セルサイズ倍, body アドレスに加算」が走るように
  なる。つまり `2 GRID` は `&GRID + 2 × CELLS` (= `GRID[2]` の
  アドレス) を計算し、`@` でそこから読み出す。
- **C にも Python にも Lisp にも対応物が無い**: C の macro はテキスト
  展開しかできず、ランタイムの計算は不可。Lisp の macro は構文を
  書き換えられるが「名前の *呼出時*挙動を redirect する」ことは
  できない。Common Lisp の `defstruct` がこれに近いことをするが、
  特定の構造に固定されている。
- 同じパターンで **records、ルックアップテーブル、状態機械、
  ディスパッチテーブル、独自数値出力語** など、アクセスパターンが
  繰り返される任意のデータ構造を定義できる。3 行で。

---

## 5. Forth 固有 — `BASE` とグローバル状態

Forth の入出力数値基数は **グローバル変数 1 個**。`HEX`、`DECIMAL`、
冗長形 `2 BASE !` (基数を 2 に) はすべて同じセルを書き換える。

### REPL トランスクリプト

```
> DECIMAL 255 .
255  ok
> 255 HEX .
FF  ok
> 42 2 BASE !  .
101010  ok
> DECIMAL
 ok
```

### 解説ポイント

- `255 HEX .` の `255` は **decimal で parse** される (現基数は
  decimal)。`HEX` で printer の基数だけを切り替え、`.` が 255 を
  hex `FF` で出力する。
- `42 2 BASE ! .` は逆方向: 42 を decimal で push、basis を 2 に
  set、`.` で 42 を binary `101010` で出力。
- **同じグローバルが入力 parse と出力 format の両方を制御する**。
  `HEX` のセッションで `DECIMAL` を戻し忘れると、次に typed する
  `255` が hex parse されて 597 (decimal) になる、という事故が
  起きる。
- **なぜ Forth でグローバル状態が許容されるのか** — Forth は「1
  人のプログラマが 1 つの端末で 1 つの仕事をする」前提だから。
  暗黙の状態の関数呼出コストは 0 (1 セル read + 1 分岐)。マルチ
  スレッドで数百人のコントリビュータがいる言語なら破綻するが、
  Forth が住む組込み / REPL / シングルユーザの環境では「短い
  記法、配管不要」というメリットが勝つ。

---

## 5 章を並べる狙い

| # | アルゴリズム | Forth が見せる強み | Forth が見せる摩擦 |
|---|---|---|---|
| 1 | ハノイ | 再帰 | 4 引数のスタック juggling → VARIABLE に逃げる |
| 2 | 8 クイーン | 配列 + DO/LOOP | LOCALS 無し、`I` の colon 跨ぎ shadow、`?DO` 無し |
| 3 | クイックソート | 直接メモリ + tail-iter 最適化 | `>R`/`R>`/`R@` での手動 save/restore |
| 4 | `CREATE`/`DOES>` | **定義語 — Forth の signature** | (無し — ここは Forth が圧勝する領域) |
| 5 | `BASE` | 簡潔なグローバルモード切替 | グローバル状態の衛生はユーザの責任 |

Lisp から来た人にとって、Chapter 1〜3 は Forth がアルゴリズム作業
で低レベル感・誤りやすさを伴う理由を見せ、Chapter 4〜5 は **それ
でも Forth が選ばれる理由** — defining word と直接メモリ制御の
比類なさ — を見せる。これらは組込み / DSL / bring-up といった
Forth 設計の本来の用途で本領を発揮する。

64 KB アドレス空間の各領域 — 何がどこにあるか、なぜ dictionary は
上向きに、stack は下向きに伸びるのか、`HERE` / `ALLOT` がどう
進むか — については [LANGUAGE_AND_IMPL_ja.md §3](LANGUAGE_AND_IMPL_ja.md#3-メモリマップ)
を参照。
