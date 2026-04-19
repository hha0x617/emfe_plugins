# Hha Lisp — Language Spec & Implementation Notes

This document describes the **language specification** and **implementation
design** of `examples/lisp/lisp.asm`.  For everyday usage, see
[USER_GUIDE.md](USER_GUIDE.md).

---

## 1. Code-size metrics

| Metric | Value |
|---|---|
| Assembly source | **6,635 lines** (single `lisp.asm`) |
| Raw binary | **18,981 bytes** (~18.5 KB) |
| SREC file | 52,150 bytes (ASCII encoding of the raw image) |
| Primitives (`BI_*`) | **60** |
| Pre-declared symbols | **82** |
| Stdlib entries (ROM-embedded Lisp) | **47** |
| Smoke tests | **35** (~128 s, all passing) |

For context:
- Far larger than SectorLisp (512 B, 7 primitives)
- Roughly half of uLisp (~30 KB, 200+ primitives) for ARM/AVR
- 2-3× larger than typical "Tiny Lisp" teaching implementations (~2K LOC)

---

## 2. Value tags & memory map

### 2.1 Tagged-word representation (16-bit)

Every Lisp value is a 16-bit tagged word.

| Value | Range | Kind |
|---|---|---|
| `$0000` | NIL_VAL | NIL (false / empty list) |
| `$0002` | T_VAL | T (true) |
| `$0003..$2FFF` **odd** | fixnum | bit 0 = 1; `(x-1)/2` is a signed 15-bit integer (-16384..16383) |
| `$4C00..$67FF` **even** | pair | 4-byte cons cell (car 2B + cdr 2B) |
| `$6800..$6FFF` | symbol | variable-length entry (next 2B + len 1B + name bytes) |
| `$7000..$7FFF` | builtin | primitive-function tag ID |
| `$8E00..$8FFF` | char | `CHAR_BASE + 2*code` (stride 2 avoids colliding with fixnum tag) |
| `$9000..$9FFF` | string | even-aligned, `[len 1B][content]` |
| `$A200..$AFFF` | vector | even-aligned, `[len 2B][elem 2B]*` |
| `$B000..$BFFF` | int32 box | 4-byte cell for large integers |

**bit 0 = 1 marks a fixnum.**  All other tagged values are placed at
**even addresses** so they can't collide with the fixnum tag.  The
`align_str_next` helper and the symbol-entry stride rule enforce this
during allocation.

### 2.2 64 KB memory layout

```
$0100..$4BFF  code + initialised data  (~19 KB)
$4C00..$67FF  pair pool      (7 KB = 1792 cells, GC-managed)
$6800..$6FFF  symbol pool    (2 KB, permanent)
$7000..$7FFF  builtin tag range (no RAM)
$8000..$8BFF  pair mark bitmap (3 KB)
$8C00..$8C7F  int32 mark     (128 B, currently unused)
$8C80..$8DFF  reserved
$8E00..$8FFF  char tag range (no RAM)
$9000..$9FFF  string pool    (4 KB, bump only)
$A000..$A1FF  TIB            (512 B, REPL line buffer)
$A200..$AFFF  vector pool    (3.5 KB, bump only)
$B000..$BFFF  int32 pool     (4 KB, bump only)
$C000..$FEFE  hardware stack (16 KB)
$FF00/$FF01   ACIA SR/DR
$FF02/$FF03   tick MMIO (CPU cycle low 16 bits, read-only)
$FFFE/$FFFF   reset vector → cold
```

---

## 3. Language specification

### 3.1 Reader

- S-expressions: `(...)` / `'x` (QUOTE) / `` `x `` (QUASIQUOTE) /
  `,x` (UNQUOTE) / `,@x` (UNQUOTE-SPLICING)
- Integers: decimal, optional leading `-`, **auto-promotes to int32 box**
  when the value exceeds the 15-bit fixnum range
- Character literals: `#\c` (stride-2 encoding)
- Strings: `"..."` with `\n`, `\t`, `\\`, `\"` escapes
- Dotted pairs: `(a . b)` (dot must be whitespace-delimited)
- Comments: `;` to end of line
- Symbols: terminated by whitespace / `()` / TAB / LF / CR; **up-cased
  on read** (Common Lisp convention)

### 3.2 Evaluation rule (`eval`)

```
eval(x) =
  if x is fixnum / NIL / T / builtin / char / string / vector: self
  if x is a symbol: lookup(x, current_env → global_env)
  if x is a pair:
    if car(x) is a known special-form symbol: dispatch to its handler
    else: apply(eval(car(x)), [eval(c) for c in cdr(x)])
```

### 3.3 Special forms (17)

`QUOTE` `IF` `DEFVAR` `LAMBDA` `DEFUN` `COND` `LET` `LET*` `LETREC` `SETQ`
`PROGN` `AND` `OR` `DEFMACRO` `QUASIQUOTE` `CATCH` `THROW`

### 3.4 Function application (`ev_apply`)

After evaluating the operator:

- **Builtin value** (`$7000..$7FFF`): dispatch via the `BI_*` table
- **Closure** (pair whose car is `sym_LAMBDA`): a 3-pair chain
  `(LAMBDA . (params . (body . env)))`; bind params to arg values in a
  fresh env and evaluate body
- **Macro** (pair whose car is `sym_MACRO`): same 3-pair structure, but
  bind params to **unevaluated** arg forms, evaluate body to produce an
  **expansion**, then evaluate the expansion (2-step)

### 3.5 Quasi-quote

- `` ` x `` reads as `(QUASIQUOTE x)`, dispatched to `ev_quasiquote`
- `qq_depth` tracks nesting; an UNQUOTE at depth > 1 is rebuilt as
  `(UNQUOTE x)` rather than evaluated
- `qq_walk` uses the tail-pointer builder pattern; `,@` splices

---

## 4. Implementation notes

### 4.1 Closure / macro structure

Closures and macros share a 3-pair chain:

```
outer pair: (LAMBDA-or-MACRO . mid)
mid pair:   (params . inner)
inner pair: (body . captured_env)
```

- `build_closure` allocates the three pairs
- `ev_apply` branches on `car(fn)` to tell LAMBDA from MACRO

### 4.2 Environment

- `current_env`: the local chain, pushed by lambda / let calls
- `global_env`: the top-level chain, extended by defvar / defun
- Both are alists of `(sym . val)` pairs
- `ev_lookup` walks current then global

### 4.3 Garbage collector

**Mark-and-sweep on the pair pool only.**  Symbol, string, vector, and
int32 pools are bump-only.

- **Mark bitmap**: `$8000..$8BFF`, 1 byte per pair cell
- **Free list**: reclaimed cells chain through `car`
- **Allocation strategy**: prefer free list → bump → OOM
- **Roots**: `global_env`, `current_env`, `current_closure`, and every
  `ev_*` scratch variable
- **Vector-pool scan**: `gc_mark_vec_pool` walks each 16-bit word in
  `[VEC_POOL, vec_next)`; any word in the pair range is marked (vector
  elements are the only path to some pairs)
- **Hybrid auto-GC** (inside `alloc_pair`):
  - entry: `pshs y,d` puts Y = car and D = cdr on the stack where the
    conservative scan can see them
  - if the pool is exhausted, run `gc_run_safe` once (with a stack
    scan between current S and `repl_init_s`) and retry
  - `alloc_gc_tried` prevents an infinite retry loop
- **REPL-top auto-(gc)**: `gc_run` is invoked at the top of every REPL
  turn — the stack holds only the REPL's own frame, so the trivial
  root set is complete and safe

### 4.4 Tail-call optimisation

`ev_ap_done_bind` treats the closure body's last expression as a tail
position.

1. A body of `(PROGN e1 … eN)` makes eN the tail
2. IF and PROGN are **tail-transparent**: their chosen branch (IF) or
   last form (PROGN) becomes the new tail, dispatched recursively
3. When the tail is a function call:
   - **Self-TCO** (same closure as current):
     - Evaluate all new args into `stco_vals[]`
     - **Mutate each binding's cdr in place**
     - `lbra ev_ap_body_start` — reuse the frame
     - **Zero pair allocation**, garbage-free
   - **General TCO** (different closure): set `ev_tail_mode = 1`,
     `lbra ev_ap_closure_proper` — reuse the outer env-save pshs

### 4.5 Stdlib bootstrap

`load_stdlib` at cold boot:

1. Iterates `stdlib_table`, a null-terminated array of source pointers
2. Copies each source into TIB (truncated at 512 bytes)
3. `read_expr` then `eval` on each entry
4. Defines 47 functions / macros including `mapcar`, `case`,
   `defstruct`, `format`, `trace`, `make-ht`, `q-from`, etc.

### 4.6 First-class primitives

Builtins live as tag IDs in `$7000..$7FFF` and are bound in `global_env`:

```
(CONS . $7000)  (CAR . $7002)  (CDR . $7004)  …
```

Evaluating `CONS` returns `$7000` (a builtin value).  `(cons 1 2)`,
`((eval 'cons) 1 2)`, and `((if t car cdr) '(1 . 2))` all work.

### 4.7 Error handling

- `catch_stack` holds up to 8 frames of `(tag, saved_s, saved_env)`
- `(catch tag body)` pushes a frame with the current S register
- `(throw tag value)` walks the stack for a matching tag and `tfr d,s`
  unwinds to the saved S, delivering the value in X
- `(error msg)` unwinds to REPL entry via `lds repl_init_s`

### 4.8 ev_let nested-clobbering fix

`ev_let` uses four memory scratches (`ev_lt_bindings`, `ev_lt_cur`,
`ev_lt_newenv`, `ev_lt_body`).  A nested LET inside the arg evaluation
would overwrite all four.  The fix is pshs / puls around the inner
`lbsr eval`.

### 4.9 Cycle-counter MMIO

- `PluginBus::tick_word: u16` is updated in `step_one` by adding each
  instruction's cycle cost
- `$FF02` read → high byte, `$FF03` read → low byte
- The `(tick)` primitive reads `$FF02` as a 16-bit word and returns
  the low 14 bits as a fixnum

---

## 5. Implementation statistics

### 5.1 File layout

```
lisp.asm       6,635 lines (single file)
  ├ equates / constants           lines 1–120
  ├ cold boot / intern calls            120–400
  ├ global_env bind                     400–570
  ├ alloc_pair + auto-GC               1070–1170
  ├ intern + alignment                 1170–1290
  ├ read_expr family                   1290–1560
  ├ print_expr family                  1560–1900
  ├ eval + lookup                      1900–2140
  ├ primitive dispatch                 2140–2780
  ├ ev_apply + TCO                     2780–3270
  ├ special-form handlers              3270–3900
  ├ string/char/vector primitives      3900–4400
  ├ logical / arithmetic               4400–4700
  ├ xorshift32 + tick                  4700–4880
  ├ conversions                        4880–5080
  ├ stdlib source strings              5800–6100
  ├ REPL + gc_run                      6100–6300
  └ RAM variable declarations          6300–6635
```

### 5.2 Pair-pool consumption

- Cold boot + stdlib load: ~400 cells
- A typical `(fact 10)` allocates ~30 cells per call (auto-GC reclaims
  between turns)
- The mutual-recursion stress test (2000-deep `(e? / o?)`) passes

### 5.3 em6809 CPU bugs found during development

1. **SBC borrow-in inversion** — caught while implementing int32 math
2. **PC-relative indexed misparse of `,S`** — `addd ,s` read PC, not S
3. **Missing opcodes** — LEAS/LEAU mix-up, ABX, TST mem, INC mem, etc.

All were fixed upstream in the `em6809` crate.

---

## 6. Design choices and trade-offs

| Choice | Why |
|---|---|
| Fixed-size pools (bump / free list) | No malloc, simple, predictable |
| Single asm file | `.include` is awkward in lwasm; one readable file works |
| UPPERCASE normalisation | Classic Lisp convention; lowercase input still works |
| Classic-Lisp surface (defun/setq) | Easier for teaching than Scheme |
| First-class primitives | `(mapcar car …)` just works |
| 3-pair closure layout | Simple and GC-friendly |
| Two-phase self-TCO | Preserves left-to-right arg eval semantics when mutating bindings |
| ROM-embedded stdlib | No load-time I/O needed; everything is ready after cold boot |

---

## 7. Possible future improvements

| Item | Cost estimate | Priority |
|---|---|---|
| Copying GC for STR_POOL | 8-10 h | ⭐⭐ |
| GC for int32 pool | 4-6 h | ⭐⭐ |
| IEEE 754 single-precision floats | large | ⭐ |
| `call/cc` continuations | large | ⭐ |
| BASE switching (hex input) | 1-2 h | ⭐ |
| Bytecode compilation | large | ⭐ |

---

## 8. License

MIT OR Apache-2.0 (dual-licensed) — see the SPDX header in `lisp.asm`.
