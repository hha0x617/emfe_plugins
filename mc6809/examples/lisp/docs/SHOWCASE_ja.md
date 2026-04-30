# Hha Lisp — アルゴリズムショーケース

MC6809 エミュレータ上の Hha Lisp で動かす古典アルゴリズムを 3 本
紹介します。下記サンプルはどれも **paste-ready**: REPL に貼り付け
れば、トランスクリプト通りの出力が得られます。

言語仕様の参照は [USER_GUIDE_ja.md](USER_GUIDE_ja.md)、
処理系の内部構造は
[LANGUAGE_AND_IMPL_ja.md](LANGUAGE_AND_IMPL_ja.md) を参照。

---

## 1. ハノイの塔

最も素直な再帰のデモンストレーション。*n* 枚の円盤を `A` から `C`
へ、`B` を中継地点として、常に大きい円盤を小さい円盤の上に置かない
ように移動する。

書き方の異なる 2 通り。`n = 3` での出力 (7 行) はどちらも同一だが、
**スタイル**が違う — Common Lisp 流イディオムの対比として参考になる。

### バリアント A — `n = 1` の明示的なベースケース + display 連鎖

```lisp
(defun hanoi (n from via to)
  (if (= n 1)
      (progn (display "Move disk 1 from ") (display from)
             (display " to ") (display to) (newline))
      (progn (hanoi (- n 1) from to via)
             (display "Move disk ") (display n)
             (display " from ") (display from)
             (display " to ") (display to) (newline)
             (hanoi (- n 1) via from to))))
```

```
> (hanoi 3 'A 'B 'C)
Move disk 1 from A to C
Move disk 2 from A to B
Move disk 1 from C to B
Move disk 3 from A to C
Move disk 1 from B to A
Move disk 2 from B to C
Move disk 1 from A to C
NIL
```

### バリアント B — `when` ガード + `format`

```lisp
(defun hanoi (n source dest helper)
  (when (> n 0)
    (hanoi (- n 1) source helper dest)
    (format "Move disk ~d from ~a to ~a" n source dest)
    (newline)
    (hanoi (- n 1) helper dest source)))
```

```
> (hanoi 3 'A 'C 'B)
Move disk 1 from A to C
Move disk 2 from A to B
Move disk 1 from C to B
Move disk 3 from A to C
Move disk 1 from B to A
Move disk 2 from B to C
Move disk 1 from A to C
NIL
```

### スタイル比較

| 観点 | バリアント A | バリアント B |
|---|---|---|
| **ベースケース** | `(= n 1)` で最小の移動を明示 | `(> n 0)`、`n = 0` は `when` で何もせず止まる |
| **条件分岐** | `if` で 2 分岐 | `when` の片腕分岐 (else 不要) |
| **出力** | `display` 連鎖 + `newline` | `format` のプレースホルダ + `newline` |
| **行数** | 約 9 行 | 約 5 行 |

ポイント:

- **`n = 1` を特別扱いするか否か** が一番の判断軸。バリアント A
  は最小の移動 (`"Move disk 1 from ..."`) を明示するので、アル
  ゴリズムを教えるときに読みやすい。バリアント B は全ての移動を
  `format` の 1 呼出に統一し、`n = 0` で再帰がきれいに底打ちする
  ように書く — 数学的には簡潔で短いが、最小の移動はソース上の
  リテラルとして見えなくなる。
- **`when` と `if`** — 「述語が真のときだけ一連の処理を走らせ、
  そうでなければ NIL を返す」という意図には `when` が慣用的。
  else 節を書かずに済む。
- **Hha Lisp の `format` は意図的に minimal** — どんな文字でも
  `~<char>` の形で次の引数に置き換えられる。つまり `~d` と `~a`
  は実行時には等価。だが Common Lisp 流に「数値は `~d`、シンボル
  は `~a`」と書き分けると、処理系が強制しなくとも意図がコードに
  残る。
- 2 つのバリアントは再帰の起動の仕方が同一で、引数名が違うだけ
  (`from`/`via`/`to` vs `source`/`helper`/`dest`)。だから
  `(hanoi 3 'A 'B 'C)` (バリアント A) と
  `(hanoi 3 'A 'C 'B)` (バリアント B) で同じ移動列が出る。

---

## 2. 8 クイーン — 解の総数

バックトラッキング探索で、`1..n` の各列に互いに脅かし合わない位置
へクイーンを置き、その解の数を数える。

このアルゴリズムは、**再帰呼出の置き場所が処理系の TCO とどう絡む
か**を非常に分かりやすく見せてくれる例。同じ答えを大域カウンタの
変更でも、純関数的な戻り値の連鎖でも書けるが、この小規模 pool の
Lisp で `n = 8` まで耐えられるのは片方だけ。

### バリアント A — `setq` による大域カウンタ

```lisp
(defvar qc 0)

(defun safe? (row placed dist)
  (cond ((null? placed) t)
        ((= (car placed) row) nil)
        ((= (abs (- (car placed) row)) dist) nil)
        (t (safe? row (cdr placed) (+ dist 1)))))

(defun place-col (n placed col)
  (if (> col n)
      (setq qc (+ qc 1))
      (try-rows n placed col 1)))

(defun try-rows (n placed col row)
  (if (<= row n)
      (progn
        (if (safe? row placed 1)
            (place-col n (cons row placed) (+ col 1)))
        (try-rows n placed col (+ row 1)))))

(defun queens (n) (setq qc 0) (place-col n nil 1) qc)
```

```
> (queens 4)
2
> (queens 5)
10
> (queens 8)
92
```

> 📊 **canonical な解の数列** ([OEIS A000170](https://oeis.org/A000170)).
> n-queens の解の総数は `n = 1, 2, 3, ...` に対して
> `1, 0, 0, 2, 10, 4, 40, 92, 352, 724, ...` と続く。`n = 6` で
> 4 という値 (`n = 5` の 10 より *少ない*) になるのは間違いではなく、
> **本質的に異なる配置 (回転・反射で同一視) はちょうど 1 つだけ**、
> それを 4 方向に回転した分が総数になっている。下のバリアント B の
> `(queensF 6)` で `4` が返ってきても異常ではないので慌てないこと。

### バリアント B — 純関数的、`+` でカウントを持ち回す

大域変更なし — 各再帰呼出がカウントを返し、呼出元が合算する。

```lisp
;; safe? はバリアント A のまま再利用

(defun count-rows (n placed col row)
  (if (> row n) 0
      (+ (if (safe? row placed 1)
             (count-cols n (cons row placed) (+ col 1))
             0)
         (count-rows n placed col (+ row 1)))))

(defun count-cols (n placed col)
  (if (> col n) 1
      (count-rows n placed col 1)))

(defun queensF (n) (count-cols n nil 1))
```

```
> (queensF 4)
2
> (queensF 5)
10
> (queensF 6)
4
```

> ⚠ **次の行は意図的に pair pool を枯渇させて失敗の挙動を
> 観察させるためのもの。** `alloc_pair` が枯渇すると現在の
> 実装は `ALLOC: pool exhausted` を出した後、`bra ap_hang` で
> **無限ループ**に入り、REPL に longjmp で戻る経路は無い。
> **続きを読むにはエミュレータを reset する必要がある**。
> ハングを避けたければこの paste はスキップして構わない —
> 下の解説はそれ単体で読める。

```
> (queensF 8)
ALLOC: pool exhausted
   (REPL は無限ループ状態。続けるには emulator を reset)
```

### バリアント B が `n = 8` で吹っ飛ぶ理由

`count-rows` の再帰呼出がどこに座っているか見てみる:

```lisp
(+ (if ... (count-cols ...) 0)         ; 再帰 #1
   (count-rows n placed col (+ row 1)))  ; 再帰 #2
```

両方の再帰呼出が `+` の **内側** にある。どちらも tail position
ではない — 結果が戻ってきて `+` で合算される必要があるため。
処理系の self-TCO は呼出を現フレームの mutate に潰せず、毎回の
呼出で binding pair を pair pool に新規確保する。

バリアント A の `try-rows` と対比してみる:

```lisp
(progn
  (if (safe? row placed 1)
      (place-col n (cons row placed) (+ col 1)))
  (try-rows n placed col (+ row 1)))   ; ← progn の最後の form、tail
```

再帰呼出 `(try-rows ...)` は `progn` 本体の **最後の form**。
tail position に座っているので self-TCO が利き、現フレームを
書き換えて再利用するため、行を走査するループは何も確保しない。
`place-col` 経由のバックトラッキング自体は確保するが、ともあれ
*行スイープ* は確保しない。

小さな `n` ではこの差は見えない — 余分な確保が pool に収まるから。
`n = 8` では探索木が爆発的に広がり (成功は 92 だが訪問ノードは
1600 万近く)、バリアント B の非 tail 再帰は 2208 セル pool を
枯らす。

### スタイル比較

| 観点 | バリアント A | バリアント B |
|---|---|---|
| **状態管理** | 大域 `qc` を `setq` で書換 | カウントを持ち回し、大域なし |
| **再帰の形** | `try-rows` の最後の form は tail call → self-TCO 可 | 再帰が `+` の中 → TCO 不可 |
| **`n = 8` のメモリ** | 一定 — TCO がフレーム再利用 | 探索木深さに比例 — pool 枯渇 |
| **読み筋** | 「命令的ループ + カウンタ」 | 「数学的再帰式」 |

教訓 — 固定 pool の Lisp では「再帰呼出が exactly どこに座って
いるか」がスケールするか否かを左右する。

> pool の数字 (なぜ 2208 cell なのか / 64 KB に他に何が同居して
> いるか / interpreter が code / pair / symbol / string / vector
> / int32 / stack をどう 16-bit アドレス空間に詰めているか) は
> [LANGUAGE_AND_IMPL_ja.md §2.2](LANGUAGE_AND_IMPL_ja.md#22-メモリマップ-64-kb-中)
> を参照。

---

## 3. 8 クイーン — 全解を盤面表示

同じ探索だが、解が見つかるたびに 8×8 の盤面 (`Q` がクイーン、`.` が
空) を出力する。カウンタは大域変数ではなく戻り値として再帰を貫通
させて持ち回る。

### コード

```lisp
(defun print-row (queen-col n c)
  (cond ((> c n) (newline))
        ((= c queen-col) (display "Q") (print-row queen-col n (+ c 1)))
        (t              (display ".") (print-row queen-col n (+ c 1)))))

(defun print-board (rows n)
  (dolist (r rows) (print-row r n 1))
  (newline))

(defun try-rows-show (n placed col row count)
  (if (> row n)
      count
      (try-rows-show n placed col (+ row 1)
                     (if (safe? row placed 1)
                         (place-col-show n (cons row placed) (+ col 1) count)
                         count))))

(defun place-col-show (n placed col count)
  (cond ((> col n)
         (print-board (reverse placed) n)
         (+ count 1))
        (t (try-rows-show n placed col 1 count))))

(defun show-queens (n) (place-col-show n nil 1 0))
```

### REPL トランスクリプト (n = 4)

```
> (show-queens 4)
.Q..
...Q
Q...
..Q.

..Q.
Q...
...Q
.Q..

2
```

`(show-queens 8)` も同様に 92 個の盤面をすべて出力したのち 92 を返す。

### 解説ポイント

- **`dolist` マクロ** が行リストを反復して `print-row` を呼び出す。
- **カウンタは戻り値で持ち回る** — 大域状態は無し。
  `try-rows-show` / `place-col-show` がそれぞれ現在のカウントを
  返却し、バックトラッキング木の全体で積み上げる。
- §2 の `safe?` をそのまま流用。
- `dolist` + `let` + 再帰のこの組合せは処理系の GC ルートと
  `ev_append` / self-TCO の scratch 保護を踏みつける所であり、
  歴史的に長時間計算で起きていたバグの震源地でもあった。

---

## 4. クイックソート

古典的 divide-and-conquer。queens と同じ軸 — 命令的な分割 vs.
関数的な分割。

### バリアント A — `dolist` + `setq` アキュムレータ

分割相は残りリストを 1 回走査し、`setq` で各要素を `less` か
`greater` に振り分ける。

```lisp
(defun qsort (lst)
  (if (null? lst) nil
    (let ((pivot (car lst))
          (rest (cdr lst))
          (less nil)
          (greater nil))
      (dolist (x rest)
        (if (< x pivot)
            (setq less (cons x less))
            (setq greater (cons x greater))))
      (append (qsort less) (cons pivot (qsort greater))))))
```

```
> (qsort (list 5 3 8 1 9 4 2 7))
(1 2 3 4 5 7 8 9)
> (qsort (list 42 17 23 4 99 1 67 38 12 55))
(1 4 12 17 23 38 42 55 67 99)
```

### バリアント B — `filter` + `lambda`

`pivot` をクロージャで束縛した 2 本の述語ラムダで `filter`。
mutation 無し。但し各 `filter` 呼出は `rest` を独立に走査する。

```lisp
(defun qsortF (lst)
  (if (null? lst) nil
    (let ((pivot (car lst))
          (rest (cdr lst)))
      (append (qsortF (filter (lambda (x) (< x pivot)) rest))
              (cons pivot
                    (qsortF (filter (lambda (x) (>= x pivot)) rest)))))))
```

```
> (qsortF (list 5 3 8 1 9 4 2 7))
(1 2 3 4 5 7 8 9)
> (qsortF (list 42 17 23 4 99 1 67 38 12 55))
(1 4 12 17 23 38 42 55 67 99)
```

### スタイル比較

| 観点 | バリアント A | バリアント B |
|---|---|---|
| **分割の仕方** | `dolist` + `setq` で 1 パス | `filter` 2 本 (片側ずつ) |
| **状態管理** | 局所的 mutation (`setq`) | 純関数 |
| **クロージャ** | 無し | 呼出毎にラムダ 2 本 (pivot を捕捉) |
| **読み筋** | 「ループ + バケツ」 | 「述語による定義」 |

### 両方が動く理由 — そして罠の所在

両者とも REPL で人手で流すサイズのリストならクリーンに動く。
`(append (qsort ...) (cons pivot (qsort ...)))` も実は queens
バリアント B と同じ非 tail 構造である — 落ちてもおかしくない。

それでも生き残る理由:

- 良い pivot なら分割は半分・半分に近く、再帰深度は **O(log n)**。
  queens の探索木深度のような爆発はしない。
- 各 `qsort` 呼出が返すリストはすぐ `append` / `cons` に消費されて
  ガベージになり、再帰の戻りで pool が回収される。
- queens で問題が出たのは 1600 万ノードの探索があるから。30
  要素のソートではほぼ pool を擦りもしない。

つまり「非 tail 再帰のリスク」という構造的な脆弱性は同じく
存在しているが、現実的な入力では発火しない。最悪ケース (既に
ソート済 / 逆順、つまり毎回 1 要素しか剥がせず再帰深度が線形)
では、バリアント B の方が早く劣化する — 各レベルでクロージャ
2 つ余分に確保し、`rest` を 2 回ずつ走査するから。

このペアは処理系の 2 つの重要バグの canary でもあった:

- `ev_append` の non-reentrancy: `(append (qsort less) (cons pivot
  (qsort greater)))` の正準形が第 1 引数を黙って捨てていた問題
  ([PR #20] 以前)。
- `let` / `dolist` / `append` / `apply` の scratch グローバルが
  GC ルートに漏れており、マクロ大量展開下で expansion が壊れて
  いた問題 ([PR #19] 以前)。

両方修正済。このセクションは小さなカナリアとして機能している。

---

## サンプル実行

エミュレータをビルドして起動 ([USER_GUIDE_ja.md §1](USER_GUIDE_ja.md#1-ビルドと実行)
参照)、各ブロックを REPL に貼り付ける。複数行入力は `>>`
継続プロンプトで対応するので、`defun` 全体をそのまま貼って構わない。
