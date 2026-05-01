# Hha Lisp — User Guide

`lisp.asm` is a Lisp interpreter that runs under em6809 /
emfe_plugin_mc6809 and talks to the outside world via a single ACIA.

---

## 0. About this Lisp (lineage at a glance)

Hha Lisp combines:

- **Common Lisp surface syntax** — `defun` / `setq` / `t` / `nil` /
  `cond` / `progn` / `defmacro` / quasiquote.  Easier to teach than
  Scheme.
- **Lisp-1 evaluation** — single namespace; primitives are first-class
  values, so `(filter zero? xs)` and `(mapcar car '((1) (2) (3)))`
  Just Work.  No `#'` needed.
- **Scheme-style utility naming** — `string->symbol`, `vector-set!`,
  `char->integer`.  Arrows for conversion, `!` for mutation.

The closest single named relative is **uLisp** (a Common Lisp subset
for AVR / ARM microcontrollers), with the deliberate twist that Hha
Lisp is Lisp-1 rather than Lisp-2.

**Cross-tradition aliases** are provided so users from different Lisp
backgrounds can write naturally:

| You can write… | … or equivalently | Style |
|---|---|---|
| `(null? xs)` | `(null xs)` | Scheme / CL |
| `(atom? x)` | `(atom x)` | Scheme / CL |
| `(eq? a b)` | `(eq a b)` | Scheme / CL |
| `(zero? n)` | `(zerop n)` | Scheme / CL |
| `(set! x v)` | `(setq x v)` | Scheme / CL |

These are **additive**: nothing has been renamed or removed.  Pick the
names that match your background, or mix freely.

For the full lineage map and design principles (including why `defun`
/ `setq` / `t` / `nil` stay first-class names instead of becoming
aliases of Scheme equivalents), see
[LANGUAGE_AND_IMPL.md §0](LANGUAGE_AND_IMPL.md).

---

## 1. Build & Run

### Quick start (with a prebuilt emfe frontend)

If you just want to **try Hha Lisp** without building anything from source:

1. Install one of the GUI host frontends
   ([emfe_WinUI3Cpp](https://github.com/hha0x617/emfe_WinUI3Cpp/releases)
   or [emfe_CsWPF](https://github.com/hha0x617/emfe_CsWPF/releases)).
2. From the
   [emfe_plugins releases page](https://github.com/hha0x617/emfe_plugins/releases),
   install the plugin DLLs — the release notes there walk through both
   the installer (`emfe_plugins-*-setup.exe`, recommended) and the
   manual `.zip` extraction. Both routes deploy the DLLs to the
   `%LOCALAPPDATA%\emfe_*\plugins\` directory the host scans.
3. Launch `emfe.exe`, then **File → Switch Plugin…** and pick **MC6809**.
4. **File → Open S-Record…** (Ctrl+S) and select the shipped
   `examples/lisp/lisp.s19` (also bundled with each release).
5. Press **F5** (Run). The Console window shows the boot banner and the
   `> ` prompt — start typing.

`lisp.s19` is committed to this repository and re-published with every
release, so the steps above work without installing `lwasm`. The build /
C-ABI sections below are only needed if you want to **modify** `lisp.asm`
or embed Hha Lisp from your own code.

### Build (only when modifying `lisp.asm`)

Assuming [`lwasm`](http://www.lwtools.ca/) (from lwtools) is on your
PATH:

```sh
lwasm -9 -f srec -o lisp.s19 lisp.asm
```

If it isn't on PATH, invoke it with the full path instead
(e.g. `C:\path\to\lwasm.exe` / `/usr/local/bin/lwasm`).

### Loading from a host program (Rust example)

```rust
let mut h: EmfeInstance = ptr::null_mut();
emfe_create(&mut h);
emfe_set_console_char_callback(h, Some(tx_cb), ptr::null_mut());
let path = CString::new("examples/lisp/lisp.s19").unwrap();
emfe_load_srec(h, path.as_ptr());
emfe_run(h);
emfe_send_char(h, b'(' as c_char);  // feed input…
```

Boot banner:

```
Hha Lisp for MC6809
(c) 2026 hha0x617 - MIT/Apache-2.0
> 
```

---

## 2. REPL behaviour

1. `> ` prompt, reads one line (up to 512 bytes of TIB).
2. If the line has unbalanced `(`, continues with `>> ` until balanced.
3. Reads forms, evaluates each, prints each result.
4. At the top of every turn, `(gc)` runs automatically to reclaim the
   previous line's macro-expansion garbage.

Reader: `'x`, `` `x ``, `,x`, `,@x`, `#\c` char literals, `"..."` strings
(with `\n`/`\t`/`\\`/`\"` escapes), `;` line comments.  Symbols are
up-cased at read time (Common Lisp convention).  `(a . b)` for dotted
pairs.

---

## 3. Value types

| Type | Literals | Notes |
|---|---|---|
| fixnum | `42` `-17` | 15-bit signed (-16384..16383) |
| int32 box | `100000` | auto-promoted on overflow |
| NIL / T | `nil` `t` | case-insensitive |
| symbol | `foo` `make-vector` | up-cased on read |
| pair | `(1 2 3)` `(a . b)` | GC-managed |
| char | `#\a` `#\Z` | |
| string | `"hello"` | |
| vector | `#(1 2 3)` | print form; build via `make-vector`, etc. |
| closure | `#<CLOSURE>` | `(lambda …)` / `(defun …)` value |
| macro | `#<MACRO>` | `(defmacro …)` value |
| builtin | `#<BUILTIN>` | primitive-function value (first class) |

---

## 4. Special forms

`quote` / `'` / `if` / `cond` / `let` / `let*` / `letrec` / `defvar` /
`defun` / `lambda` / `setq` (alias `set!`) / `progn` / `and` / `or` /
`defmacro` / `` ` `` (quasi-quote) + `,` + `,@` / `catch` / `throw` /
`case` (stdlib macro).

`set!` is a Scheme-style alias for `setq` — both forms mutate the same
binding identically.  Pick whichever name matches your background.

Dotted-rest parameters for variadic functions:

```lisp
(defun head-tail (x . rest) (list x rest))
(head-tail 1 2 3 4)  ; → (1 (2 3 4))
```

---

## 5. Primitives (selected)

- **List**: `cons` `car` `cdr` `atom` `eq` `null` `list` `length`
  `append` `cadr` `caddr` `cddr` `assoc` `apply`
- **Arithmetic / comparison**: `+` `-` `*` `/` `mod` `<` `=` `ash`
  `logand` `logior` `logxor` `lognot`
- **String**: `string-length` `string=` `string-append` `string-ref`
  `string->list` `list->string`
- **Char**: `char->integer` `integer->char` `char?`
- **Vector**: `make-vector` `vector-length` `vector-ref` `vector-set!`
  `vector->list` `list->vector` `vector?`
- **Conversion**: `number->string` `string->number` `symbol->string`
  `string->symbol`
- **Meta**: `eval` `read-string` `load-memory`
- **I/O**: `print` `display` `newline` `putchar`
- **PRNG / clock**: `(seed n)`, `(rand)` — 0..16383, `(tick)` — 14-bit
  cycle-derived counter
- **GC / misc**: `gc` `gensym` `error` `catch` `throw`
- **Printer case mode**: `(print-case)` returns the current symbol-name
  case mode as a fixnum (0 = upper, 1 = lower).
  `(set-print-case! n)` sets it; `n` must be 0 or 1 (any other value
  silently coerces to 0).  Default is 0 (upper) for transcript
  compatibility — flip to lowercase output mid-session with
  `(set-print-case! 1)`:

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

  Only ASCII letters in symbol names and the literal `T` / `NIL` /
  `#<BUILTIN>` / `#<MACRO>` / `#<CLOSURE>` displays are affected;
  digits, punctuation, string contents, character literals, and the
  reader (which always upcases input) are untouched.  See
  [LANGUAGE_AND_IMPL.md §0.2 principle 5](LANGUAGE_AND_IMPL.md) for
  the rationale on why the reader is fixed and only the printer is
  configurable.

---

## 6. Stdlib (ROM-embedded Lisp)

- **Tier 1**: `not` `zerop` `inc` `dec` `>` `abs` `max` `min`
- **Tier 2**: `reverse` `nth` `last` `member` `mapcar` `filter` `reduce`
  `any` `all` `equal` `<=` `>=`
- **Macros**: `when` `unless` `swap` `with-gensyms` `while` `dolist`
  `funcall` `case` `format`
- **Records**: `defstruct name field...` — auto-generates
  `make-name / name? / name-field / set-name-field`
- **Hashtable**: `make-ht` `ht-hash` `ht-get` `ht-put`
- **Fixed-point Q8.8**: `q-from` `q-to` `q*` `q/`
- **Debug**: `trace` `untrace`
- **Scheme-style aliases**: `null?` `atom?` `eq?` `zero?` — bound to
  the same callable values as `null` / `atom` / `eq` / `zerop`, so
  `(filter zero? xs)` and `(any null? xs)` work alongside the CL
  bare names.  Adopt whichever style matches your background.

---

## 7. Examples

### Basics

```lisp
> (+ 1 2)
3
> (mapcar (lambda (x) (* x x)) '(1 2 3 4))
(1 4 9 16)
```

### Macros & records

```lisp
> (case 'red ((red green) 1) ((blue) 2) (t 0))
1
> (defstruct point x y)
SET-POINT-Y
> (defvar p (make-point 3 4))
P
> (point-x p)
3
```

### Trace

```lisp
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

### Meta-circularity

```lisp
> (eval (read-string "(+ 10 20)"))
30
```

### load-memory

Inject a Lisp source into RAM via the plugin ABI, then evaluate it:

```rust
let script = b"(defvar gx 42)\n(defvar gy (+ gx 1))\n";
for (i, &b) in script.iter().enumerate() {
    emfe_poke_byte(h, 0x3E00 + i as u64, b);
}
emfe_poke_byte(h, 0x3E00 + script.len() as u64, 0);
```

```lisp
> (load-memory 15872)
GY
> gy
43
```

### Algorithm showcase

Tower of Hanoi, eight queens (count + visualize), and quicksort —
paste-ready transcripts in [SHOWCASE.md](SHOWCASE.md).

---

## 8. Multi-line input

`>> ` continuation prompt for unbalanced lines:

```lisp
> (defun fact (n)
>>   (if (< n 2)
>>       1
>>     (* n (fact (- n 1)))))
FACT
```

`;` comments and `)` inside `"..."` are handled correctly.

---

## 9. Limitations

- **Integers**: fixnum (-16384..16383) and int32 boxes only.  No
  floating point (use Q8.8 fixed-point stdlib if needed).
- **String / vector / int32 pools are bump-allocated**, no GC.  Long
  sessions with heavy string work may eventually OOM those pools.
- **TCO**: self-tail calls reuse bindings in place (garbage-free);
  mutual tail calls still allocate 2 pairs/call but auto-GC reclaims.
- **TIB is 512 bytes**.  Lines longer than that are truncated.
- **Symbols are permanent** (2 KB sym pool) — `intern` never shrinks.
- `call/cc`, floats, and record types beyond `defstruct` are not
  implemented.

---

## 10. Tests

From the mc6809 plugin crate root (where `Cargo.toml` lives):

```sh
cargo test --release lisp_
```

35 smoke tests all passing confirms a healthy environment.
