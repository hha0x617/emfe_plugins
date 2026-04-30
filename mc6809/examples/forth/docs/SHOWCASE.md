# Hha Forth — Algorithm Showcase

Five algorithms running on Hha Forth inside the MC6809 emulator.
Three classics (Tower of Hanoi, 8-Queens, Quicksort) plus two
Forth-specific themes (`CREATE`...`DOES>` and `BASE` switching) that
have no clean translation in most other languages.

Every block below is **paste-ready** — the transcripts shown were
captured from the actual emulator REPL. For language reference see
[USER_GUIDE.md](USER_GUIDE.md); for ITC threading and dictionary
internals see [LANGUAGE_AND_IMPL.md](LANGUAGE_AND_IMPL.md).

> **Multi-line `:` definitions are supported** even though there is
> no `>>` continuation prompt.  Each line gets ` ok` while compile
> mode persists across `ACCEPT` cycles, and `;` ends the definition.
> Use multi-line whenever it improves readability — the 512-byte TIB
> is large enough that line-length is no longer a constraint.

---

## 1. Tower of Hanoi

The simplest demonstration of recursion. Move *n* disks from peg
`A` to `C` via `B` without ever placing a larger disk on a smaller
one.

Forth has no named locals, so juggling four arguments
(`n / from / to / via`) on the data stack across recursive calls
is unpleasant.  The idiomatic workaround is to keep the three pegs
in `VARIABLE`s and swap pairs of them in place — then the recursion
itself only carries `n`.

### Code

```forth
VARIABLE FROM-PEG  VARIABLE DEST-PEG  VARIABLE VIA-PEG

: SHOW-MOVE  ( n -- )  ." Move disk " . ." from " FROM-PEG @ EMIT SPACE ." to " DEST-PEG @ EMIT CR ;
: SWAP-DV  DEST-PEG @ VIA-PEG @ DEST-PEG ! VIA-PEG ! ;
: SWAP-FV  FROM-PEG @ VIA-PEG @ FROM-PEG ! VIA-PEG ! ;

: HANOI  ( n -- )  DUP 0> IF  SWAP-DV  DUP 1- RECURSE  SWAP-DV  DUP SHOW-MOVE  SWAP-FV  DUP 1- RECURSE  SWAP-FV  THEN  DROP ;

CHAR A FROM-PEG !  CHAR C DEST-PEG !  CHAR B VIA-PEG !
```

### REPL transcript

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

### Points

- **`RECURSE`** compiles a self-call; standard FORTH-83 idiom.
- **`SWAP-DV` / `SWAP-FV`** demonstrate the classic
  `var1 @ var2 @ var1 ! var2 !` swap-by-value pattern (no `DUP` —
  the values consume themselves cleanly through the two `!`s).
- **`CHAR A`** at the REPL pushes the ASCII code 65; the bracketed
  `[CHAR]` form is for inside `:` definitions only.
- **`."  to "`** would print as `to ` (one trailing space) — Forth's
  `."` parses past a single delimiter character. Use `SPACE`
  explicitly when you want a leading space.

---

## 2. Eight Queens — solution count

Backtracking search. Place a queen on each column `1..n` such that
no two queens share a row or a diagonal; count all valid placements.

In Lisp this is a list of placed rows threaded through recursion.
Forth wants an **array** instead — `CREATE QROW 16 CELLS ALLOT`
gives us 16 cells of mutable state, indexed by column.

### Code

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

### REPL transcript

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

The dip at `n = 6` (4 solutions, *fewer* than `n = 5`'s 10) is
canonical — see [OEIS A000170](https://oeis.org/A000170). Hha Forth
hits the same numbers as any other implementation.

### Points

- **`I` works only inside the immediately enclosing `DO`/`LOOP`**.
  When `PLACE-COL`'s body calls a colon-defined word like `SAFE?`,
  `SAFE?`'s own `DO` loop pushes a fresh frame on the return stack;
  any reference to `I` inside `SAFE?` reads `SAFE?`'s loop index,
  not `PLACE-COL`'s. We get around this by reading `I` directly
  in `PLACE-COL`'s body and storing into `ROW-V`.
- **No `?DO`** in FORTH-83. `0 DO` with start `==` limit would
  iterate 65536 times instead of zero, so `SAFE?` short-circuits
  with `COL-V @ 0= IF TRUE EXIT THEN` before entering the loop.
- **`+!` (`PSTORE`)** in-place increment for `QCNT` is one of the
  many short Forth idioms that other languages need a temporary
  for.
- **Stack juggling** is the dominant cost: this is a 14-line
  algorithm in Lisp; here it took five small helper words plus
  three `VARIABLE`s for state that Lisp could have kept in
  closure-local bindings.

---

## 3. Quicksort

In-place array quicksort with Lomuto partition. Operates directly
on `ARR[lo..hi]` using `@` and `!` — no list allocation, no GC,
fully imperative.

### Code

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

### REPL transcript

```
> LOAD-TEST  8 SHOW-N CR
5 3 8 1 9 4 2 7
 ok
> LOAD-TEST  0 7 QSORT-R  8 SHOW-N CR
1 2 3 4 5 7 8 9
 ok
```

### Points

- **`CELLS`** and **`+`** index a typed cell array. `: ARR@ CELLS
  ARR + @ ;` is the canonical accessor — `CELLS` multiplies the
  index by the cell size (2 bytes on the 6809), `+` adds the base
  address, `@` fetches.
- **Tail-recursion elimination by hand** — the body uses
  `BEGIN ... WHILE ... REPEAT` and recurses **only** on the left
  partition; the right side becomes the next iteration of the
  outer loop. This keeps return-stack depth at `O(log n)` rather
  than `2 × O(log n)`. A Lisp implementation can rely on the
  `let` / `dolist` infrastructure to absorb temporaries; in Forth
  we trade that for explicit stack management.
- **`>R SWAP R@ 1- RECURSE`** stashes the pivot index on the
  return stack across the recursive call, then `R> 1+ SWAP`
  reconstructs `( p+1 hi )` for the next iteration. This kind of
  "save outside, restore inside" pattern is everywhere in Forth.
- **`PARTITION` consumes both arguments via `QS-HI ! QS-LO !`**
  with no `DUP` / `SWAP`. A first attempt that wrote
  `DUP QS-HI ! SWAP QS-LO !` looked plausible but **leaked one
  cell on the data stack** because `DUP` adds a copy that the two
  `!`s don't fully consume. The shorter form is correct because
  each `!` cleanly removes its arg.

---

## 4. Forth-distinctive — `CREATE` / `DOES>`

This is where Forth has no peer. **A defining word that defines
words** — three lines of source create a new family of named
mutable arrays:

```forth
: MYARRAY  CREATE CELLS ALLOT  DOES> SWAP CELLS + ;
```

`MYARRAY` itself is now a defining word. Calling it parses a name,
allocates space, and ties the name's runtime behaviour to the code
after `DOES>`.

### REPL transcript

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

### Points

- **`CREATE`** allocates a header in the dictionary and links it
  to the current code position. The runtime behaviour pushes the
  body's address onto the data stack.
- **`CELLS ALLOT`** at definition time reserves `5 × 2 = 10` bytes
  of cell storage immediately after the header.
- **`DOES>`** rewrites the runtime behaviour: instead of "push
  body address," now run "swap, multiply by cells, add to body
  address." So `2 GRID` computes `&GRID + 2*CELLS` — the address
  of `GRID[2]` — and `@` fetches from there.
- **No equivalent in C, Python, or even Lisp**: C macros can fake
  textual expansion but not runtime computation; Lisp macros can
  rewrite syntax but not redirect a name's *call* behaviour.
  Common Lisp's `defstruct` does something similar but only for
  one specific shape.
- This same pattern builds **records, lookup tables, state
  machines, dispatch tables, custom number-printing words** — any
  data structure whose access pattern is repeated. Three lines.

---

## 5. Forth-distinctive — `BASE` and global state

Forth's input/output number base is a single global variable. The
words `HEX`, `DECIMAL`, and the verbose form `2 BASE !` (set base
to 2) all toggle the same cell.

### REPL transcript

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

### Points

- The `255` in `255 HEX .` is parsed in **decimal** (current base
  before `HEX`), pushed as 255, then `HEX` flips the printer base.
  `.` prints 255 decimal as `FF` hex.
- The `42 2 BASE ! .` line goes the other way: push 42 in current
  base (decimal), then set base to 2, then `.` prints 42 in
  binary as `101010`.
- **The same global controls input parsing and output formatting.**
  Forgetting `DECIMAL` after a `HEX` session leaks confusion: the
  next typed `255` will be parsed as 597 decimal (`0x255`).
- **Why is global state acceptable in Forth?** Because Forth assumes
  one programmer at one terminal at one time. The implicit-state
  cost of a function call is zero (one cell read, one branch).
  In a multi-threaded language with hundreds of contributors this
  approach would be unworkable, but for the kind of embedded /
  REPL / single-user setting Forth lives in, it's a feature: short
  notation, no plumbing.

---

## Why these five together

| # | Algorithm | Forth's strength on display | Forth's friction on display |
|---|---|---|---|
| 1 | Hanoi | recursion | 4-arg stack juggling forced into VARIABLEs |
| 2 | Queens | array + DO/LOOP | no LOCALS, `I` shadows across calls, no `?DO` |
| 3 | Quicksort | direct memory + tail-iter optimisation | manual save/restore via `>R`/`R>`/`R@` |
| 4 | `CREATE`/`DOES>` | **defining words — Forth's signature** | (none — this is where Forth shines) |
| 5 | `BASE` | terse global mode-switching | global state hygiene is the user's problem |

If you're coming from Lisp, chapters 1–3 will show you why Forth
feels lower-level and more error-prone for algorithm work. Chapters
4–5 will show you why people still pick Forth: **defining-words
and direct memory control are unmatched** for the kinds of
embedded / DSL / bring-up tasks Forth was designed for.

For the actual layout of the 64 KB address space — what is at
each address, why the dictionary grows upward while stacks grow
downward, how `HERE` / `ALLOT` advance — see
[LANGUAGE_AND_IMPL.md §3](LANGUAGE_AND_IMPL.md#3-memory-map).
