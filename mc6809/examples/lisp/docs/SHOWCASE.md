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

This algorithm gives a particularly clear demonstration of how the
**shape of the recursion** interacts with the interpreter's TCO
support — the same answer can be computed with either a global
counter or a pure functional return chain, but only one of those
scales to `n = 8` in this small-pool Lisp.

### Variant A — global counter via `setq`

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

### Variant B — pure functional, count threaded through `+`

No global mutation — each recursive call returns a count and the
caller sums them.

```lisp
;; reuse safe? from variant A

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

> ⚠ **The next line deliberately exhausts the pair pool to
> demonstrate the failure mode.** When `alloc_pair` runs out,
> the current implementation emits `ALLOC: pool exhausted` and
> then enters an infinite loop (`bra ap_hang`) — there is no
> longjmp back to the REPL. **You will have to reset the
> emulator to continue reading.** Skip this paste if you'd
> rather not hang the VM; the explanation below stands on its
> own.

```
> (queensF 8)
ALLOC: pool exhausted
   (REPL is now hung; reset the emulator to recover)
```

### Why does Variant B blow up at `n = 8`?

Look at where the recursive call sits in `count-rows`:

```lisp
(+ (if ... (count-cols ...) 0)         ; recurse #1
   (count-rows n placed col (+ row 1)))  ; recurse #2
```

Both recursive calls are **inside** `+`. Neither is in tail
position — the result has to come back so `+` can sum it. The
interpreter's self-TCO can therefore not collapse the call into a
mutation of the current frame, so every call allocates fresh
binding pairs in the pair pool.

Now contrast with Variant A's `try-rows`:

```lisp
(progn
  (if (safe? row placed 1)
      (place-col n (cons row placed) (+ col 1)))
  (try-rows n placed col (+ row 1)))   ; ← LAST form in body, tail
```

The recursive `(try-rows ...)` call is the **last form** in the
`progn` body. It is in tail position; self-TCO mutates the frame
in place and the loop allocates nothing per iteration. The
backtracking through `place-col` does allocate (mutual recursion,
non-tail), but the *sweep across rows* doesn't.

For small `n` this difference is invisible — the extra allocations
fit. At `n = 8` the search tree explodes (≈ 16 million nodes
visited overall, even though only 92 succeed) and Variant B's
non-tail recursion exhausts the 2208-cell pair pool.

### Style comparison

| Aspect | Variant A | Variant B |
|---|---|---|
| **State** | Global `qc` mutated via `setq` | Threaded count, no globals |
| **Recursion shape** | `try-rows` last form is tail call → self-TCO applies | Both recursions sit inside `+` → no TCO |
| **Memory at `n = 8`** | Bounded — TCO reuses frame | Linear in search-tree depth — exhausts pool |
| **Reads as** | "Imperative loop + counter" | "Mathematical recurrence" |

The lesson — in a fixed-pool Lisp like this, "where exactly does
the recursive call sit?" decides whether the code scales.

> For the actual numbers behind the pool — why "2208 cells",
> what else lives in the 64 KB, and how the interpreter packs
> code / pair / symbol / string / vector / int32 / stack into a
> single 16-bit address space — see
> [LANGUAGE_AND_IMPL.md §2.2](LANGUAGE_AND_IMPL.md#22-64-kb-memory-layout).

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

Classic divide-and-conquer. Same axis as queens above — imperative
partition vs. functional partition.

### Variant A — `dolist` + `setq` accumulators

The partition step walks the rest of the list once, pushing each
element onto either `less` or `greater` via `setq`.

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

### Variant B — `filter` + `lambda`

Two filters with predicate lambdas that close over `pivot`. No
mutation, but each `filter` call traverses `rest` independently.

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

### Style comparison

| Aspect | Variant A | Variant B |
|---|---|---|
| **Partition** | One pass via `dolist` + `setq` | Two `filter` passes (one per side) |
| **State** | Local mutation (`setq`) | Pure functional |
| **Closures** | None | Two `lambda`s per call (capture `pivot`) |
| **Reads as** | "Loop and bucket" | "Definition by predicate" |

### Why both work — and where the trap lurks

Both variants run cleanly on the lists you'd realistically sort by
hand at the REPL. They look like they should hit the same
non-TCO trap that queens Variant B did — `(append (qsort ...)
(cons pivot (qsort ...)))` is *not* in tail position either!

So why do they survive? Because:

- The partition splits the input in half at each level (assuming
  reasonable pivots), so the recursion depth is **O(log n)**, not
  the full search-tree depth that queens has.
- Each `qsort` call returns a list that is immediately consumed by
  `append` / `cons` and becomes garbage. The pool gets reclaimed
  between recursive returns.
- The dramatic case for queens was a 16-million-node search; even
  a 30-element list here barely scratches the pool.

In other words, the same "non-tail recursion" structural risk is
present here — it just doesn't get triggered by realistic input.
On a worst-case sorted-or-reversed list (where every call peels
off only one element, making the recursion depth linear in the
list length), Variant B would degrade much faster than Variant A
because each level allocates two extra closures and walks `rest`
twice.

This pair is also where the interpreter previously had two
serious bugs:

- `ev_append` non-reentrancy: the canonical
  `(append (qsort less) (cons pivot (qsort greater)))` form
  silently dropped the first argument before [PR #20].
- Several scratch globals used by `let` / `dolist` / `append` /
  `apply` were not in the GC root set, leading to corrupted
  expansions under heavy macro load before [PR #19].

Both are fixed; this section runs as a small canary.

---

## Running these samples

Build and start the emulator (see [USER_GUIDE §1](USER_GUIDE.md#1-build--run)),
then paste each block into the REPL. Multi-line input is supported via
the `>>` continuation prompt; you can paste a whole `defun` directly.
