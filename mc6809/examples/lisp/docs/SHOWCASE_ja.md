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

### コード

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

### REPL トランスクリプト

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

### 解説ポイント

- **直接再帰のみ**、ループ無し。
- 出力は `display` (引用符なし) と `(newline)` の組合せ。`format`
  の `~%` は無いので明示的に改行を出す。
- 戻り値は `NIL` — `display` は副作用専用。

---

## 2. 8 クイーン — 解の総数

バックトラッキング探索で、`1..n` の各列に互いに脅かし合わない位置
へクイーンを置き、その解の数を数える。

### コード

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

### REPL トランスクリプト

```
> (queens 4)
2
> (queens 5)
10
> (queens 8)
92
```

### 解説ポイント

- **`cond` の multi-body 節** — `safe?` は配置済の列を逆順に辿り、
  同一行・斜めの衝突を一つの末尾再帰スイープで判定。
- **相互再帰** — `place-col` と `try-rows` がお互いを呼び合う。
  `try-rows` 自体も自己末尾再帰。
- **大域カウンタ** を `defvar` + `setq` で持つ。シンプルで分かり
  やすく、再帰の全分岐で値が保たれる。

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

古典的 divide-and-conquer。`let` で複数バインディング、`dolist` +
`setq` で分割相のアキュムレータ、`append` で 2 つの再帰結果を連結。

### コード

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

### REPL トランスクリプト

```
> (qsort (list 5 3 8 1 9 4 2 7))
(1 2 3 4 5 7 8 9)
> (qsort (list 42 17 23 4 99 1 67 38 12 55))
(1 4 12 17 23 38 42 55 67 99)
```

### 解説ポイント

- **`let` の並列バインディング** — 4 つの初期化式は全て外側の環境
  で評価され、まとめて束縛される。
- **dolist + setq による蓄積** — 分割相は 2 本のリストアキュムレ
  ータ (`less` / `greater`) を `setq` でその場更新。
- **append を挟んだ 2 つの再帰呼出** — divide-and-conquer の正準形。
  この形は以前 builtin reentrancy と GC ルート不足のバグを踏んで
  いたが、今は健全に動作する。

---

## サンプル実行

エミュレータをビルドして起動 ([USER_GUIDE_ja.md §1](USER_GUIDE_ja.md#1-ビルドと実行)
参照)、各ブロックを REPL に貼り付ける。複数行入力は `>>`
継続プロンプトで対応するので、`defun` 全体をそのまま貼って構わない。
