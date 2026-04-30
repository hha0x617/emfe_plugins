# Hha Lisp — Algorithm Showcase

Three classic algorithms running on Hha Lisp inside the MC6809
emulator. Every sample below is **paste-ready**: copy the code into
the REPL and the result matches the verbatim transcript shown.

For language reference see [USER_GUIDE.md](USER_GUIDE.md); for
implementation internals see
[LANGUAGE_AND_IMPL.md](LANGUAGE_AND_IMPL.md).

---

## 1. Tower of Hanoi

The simplest demonstration of recursion: move *n* disks from peg `A`
to peg `C`, using `B` as scratch, never placing a larger disk on a
smaller one.

Two equally valid formulations, both producing the same 7-line trace
for `n = 3`. They differ in *style* — useful for contrasting Common
Lisp idioms.

### Variant A — explicit `n = 1` base case + display chain

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

### Variant B — `when` guard + `format`

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

### Style comparison

| Aspect | Variant A | Variant B |
|---|---|---|
| **Base case** | Explicit `(= n 1)` — names the smallest move | `(> n 0)` — `n = 0` bottoms out as a no-op via `when` |
| **Conditional** | `if` with two branches | `when` (one-armed; no else) |
| **Output** | `display` chain + `newline` | `format` placeholders + `newline` |
| **Length** | ~9 lines | ~5 lines |

A few notes worth flagging:

- **Whether to special-case `n = 1`** is the key axis. Variant A
  makes the smallest move explicit (`"Move disk 1 from ..."`),
  which reads well when teaching the algorithm. Variant B unifies
  every move under one `format` call and lets the recursion
  bottom out at `n = 0` with no output — mathematically cleaner
  and shorter, but the smallest move is no longer a distinct
  literal in the source.
- **`when` vs `if`** — `when` is the idiomatic way to express
  "do this sequence only if the predicate holds, otherwise return
  NIL." It saves the explicit `else` branch when there's nothing
  to do.
- **`format` in Hha Lisp is intentionally minimal** — every
  `~<char>` substitutes the next argument regardless of `<char>`.
  So `~d` and `~a` are functionally interchangeable; using
  `~d` for numbers and `~a` for symbols documents intent the way
  Common Lisp programmers expect, even though the runtime doesn't
  enforce the distinction.
- Both variants start the recursion exactly the same way — only
  the parameter names differ (`from`/`via`/`to` vs
  `source`/`helper`/`dest`). That's why `(hanoi 3 'A 'B 'C)` for
  variant A and `(hanoi 3 'A 'C 'B)` for variant B produce the
  same move sequence.

---

## 2. Eight Queens — solution count

Backtracking search: place a queen on each column `1..n` such that
no two queens share a row or a diagonal. Count all valid placements.

### Code

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

### REPL transcript

```
> (queens 4)
2
> (queens 5)
10
> (queens 8)
92
```

### Points

- **`cond` with multi-body clauses** — `safe?` walks back through
  the placed columns, checking same-row and diagonal collisions in
  one tail-recursive sweep.
- **Mutual recursion** — `place-col` and `try-rows` call each
  other; `try-rows` is also self-tail-recursive.
- **Global counter** via `defvar` + `setq`. Easy to reason about,
  and the recursion preserves it across all branches.

---

## 3. Eight Queens — visualize every solution

Same search, but each successful placement prints the resulting
8×8 board (`Q` for queen, `.` for empty), and the count is threaded
back through the recursion as a return value rather than a global.

### Code

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

### REPL transcript (n = 4)

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

`(show-queens 8)` likewise prints all 92 solutions and returns 92.

### Points

- **`dolist` macro** iterates the row list and invokes `print-row`
  per row.
- **Counter threaded through return values** — there is no global
  state. Each `try-rows-show` / `place-col-show` returns the
  running count, building it up across the entire backtracking
  tree.
- Re-uses `safe?` from §2 unchanged.
- The `dolist`/`let`/recursion combination here exercises the
  interpreter's GC roots and the `ev_append`/self-TCO scratch
  protection — historically the spot where bugs around
  long-running computation surfaced.

---

## 4. Quicksort

Classic divide-and-conquer using `let` for multiple bindings,
`dolist` + `setq` accumulators for the partition step, and `append`
to join the two recursive sorts.

### Code

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

### REPL transcript

```
> (qsort (list 5 3 8 1 9 4 2 7))
(1 2 3 4 5 7 8 9)
> (qsort (list 42 17 23 4 99 1 67 38 12 55))
(1 4 12 17 23 38 42 55 67 99)
```

### Points

- **`let` with multiple parallel bindings** — all four are
  evaluated against the outer environment, then bound at once.
- **Dolist + setq accumulation** — the partition step uses two
  list accumulators (`less`, `greater`) updated in-place by `setq`.
- **Two recursive calls flanking `append`** — the canonical
  divide-and-conquer shape. This is exactly the pattern that
  previously hit interpreter bugs around builtin reentrancy and
  GC root coverage; it now runs cleanly.

---

## Running these samples

Build and start the emulator (see [USER_GUIDE §1](USER_GUIDE.md#1-build--run)),
then paste each block into the REPL. Multi-line input is supported via
the `>>` continuation prompt; you can paste a whole `defun` directly.
