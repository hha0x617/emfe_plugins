# Hha Forth — User Guide

`forth.asm` is a minimal ITC (indirect-threaded code) Forth that runs under
em6809 / emfe_plugin_mc6809. It communicates interactively through a single
MC6850 ACIA and supports colon definitions, control structures, variables,
constants, string literals, and comments.

For the implementation design (ITC threading, dictionary layout, inner
interpreter, code-size metrics) see
**[LANGUAGE_AND_IMPL.md](LANGUAGE_AND_IMPL.md)**.

日本語版: [USER_GUIDE_ja.md](USER_GUIDE_ja.md)

---

## 1. Building and Running

### Build

Assuming [`lwasm`](http://www.lwtools.ca/) (from lwtools) is on your
PATH:

```sh
lwasm -9 -f srec -o forth.s19 forth.asm
```

- `-9` selects MC6809 mode; `-f srec` produces Motorola S-record output.
- The resulting `forth.s19` is what the plugin loads.
- If `lwasm` isn't on PATH, invoke with the full path instead
  (e.g. `C:\path\to\lwasm.exe` / `/usr/local/bin/lwasm`).

### Loading from the host (Rust example)

```rust
emfe_create(&mut handle);
emfe_set_console_char_callback(handle, Some(tx_cb), std::ptr::null_mut());
let path = std::ffi::CString::new("examples/forth/forth.s19").unwrap();
emfe_load_srec(handle, path.as_ptr());
emfe_run(handle);

// Feed keystrokes
emfe_send_char(handle, b'4' as c_char);
// ...
```

On startup you should see this banner, after which `ACCEPT` is waiting for
a line of input:

```
Hha Forth for MC6809 ready.
```

---

## 2. Memory Map

| Range | Purpose |
|------|------|
| `$0100..$1FFF` | Kernel code + built-in dictionary |
| `$2000..$9FFF` | User dictionary (`HERE` grows upward into this region) |
| `$A000..$A07F` | TIB (terminal input buffer, 128 bytes) |
| `$B000..$BFFE` | Data stack (U, grows downward; TOS at the lower address) |
| `$C000..$FEFE` | Return stack (S) |
| `$FF00/$FF01` | ACIA SR/CR, RDR/TDR |
| `$FFFE/$FFFF` | Reset vector → `cold` |

- **Names are case-sensitive.** Dictionary lookup compares bytes exactly.
- Cells are 16-bit big-endian.
- Maximum token length is 31 characters (low 5 bits of the flags/length byte).

---

## 3. How the REPL Works

1. `ACCEPT` reads one line into the TIB (up to 128 bytes).
   - Each character is echoed as it arrives.
   - `BS` (0x08) and `DEL` (0x7F) delete the previous character and emit
     `BS SPACE BS`.
   - `CR` or `LF` terminates the line; `CRLF` is echoed.
2. `#TIB` is set to the line length and `>IN` is cleared to 0.
3. `INTERPRET` walks the buffer, alternating `PARSE-NAME` with `SFIND` /
   `NUMBER?`, and either executes or compiles each token.
4. At end-of-line it prints " ok" + `CRLF` and returns to step 1.
5. An unknown word prints `<word>?` and the REPL keeps running (errors do
   not kill the interpreter).

In compile mode (`STATE @` non-zero — you enter with `:` and leave with `;`),
non-IMMEDIATE words are appended to the current definition as `xt` cells,
and numeric literals compile as `(LIT) value`.

---

## 4. Built-in Vocabulary

### 4.1 Stack manipulation
| Word | Stack effect | Notes |
|----|------|------|
| `DUP` | `( a -- a a )` | Duplicate TOS |
| `DROP` | `( a -- )` | Discard TOS |
| `SWAP` | `( a b -- b a )` | Swap the top two cells |
| `OVER` | `( a b -- a b a )` | Copy NOS to TOS |
| `ROT` | `( a b c -- b c a )` | Rotate third cell to the top |
| `>R` | `( n -- ) (R: -- n)` | Move TOS to return stack |
| `R>` | `( -- n ) (R: n -- )` | Move return-stack top to data stack |
| `R@` | `( -- n ) (R: n -- n)` | Copy return-stack top to data stack |

### 4.2 Arithmetic / logic (16-bit signed)
| Word | Effect |
|----|------|
| `+` `-` | Addition / subtraction |
| `*` | `( a b -- a*b )` 16-bit multiply, low-16 result (sign-agnostic) |
| `/` | `( a b -- a/b )` Signed division, truncation toward zero |
| `MOD` | `( a b -- a mod b )` Signed remainder; sign follows the dividend |
| `/MOD` | `( a b -- rem quot )` Combined division; `/` and `MOD` each drop one of these |
| `1+` `1-` | `( n -- n±1 )` Increment / decrement by one |
| `2+` `2-` | `( n -- n±2 )` Increment / decrement by two |
| `2*` | `( n -- n*2 )` Arithmetic shift left 1 |
| `2/` | `( n -- n/2 )` FORTH-83 arithmetic (signed) shift right 1 |
| `NEGATE` | Two's complement |
| `ABS` | `( n -- \|n\| )` Absolute value |
| `MIN` `MAX` | `( a b -- m )` Signed minimum / maximum |
| `AND` `OR` `XOR` | Bitwise |
| `INVERT` | One's complement (bitwise NOT) |
| `NOT` | `( flag -- !flag )` FORTH-83 logical inversion (alias of `0=`) |
| `0=` | `( n -- flag )` `-1` if zero, `0` otherwise |
| `0<` | `( n -- flag )` `-1` if negative |
| `=` `<>` | Equality / inequality, returning `-1` / `0` |
| `<` `>` | Signed ordering, returning `-1` / `0` |

Divide by zero is non-trapping: `/`, `MOD`, and `/MOD` with `b=0` leave
the remainder equal to the original dividend and the quotient at `0`,
matching the kernel's general fail-soft behaviour on bad input.

### 4.3 Memory
| Word | Stack effect | Notes |
|----|------|------|
| `@` | `( addr -- w )` | 16-bit fetch |
| `!` | `( w addr -- )` | 16-bit store |
| `C@` | `( addr -- b )` | 8-bit fetch |
| `C!` | `( b addr -- )` | 8-bit store |

### 4.4 I/O
| Word | Effect |
|----|------|
| `EMIT` | `( c -- )` Emit one character |
| `KEY` | `( -- c )` Blocking single-character read |
| `CR` | Emit `CRLF` |
| `SPACE` | Emit a single space |
| `TYPE` | `( addr u -- )` Print a string |
| `COUNT` | `( c-addr -- addr u )` Unpack a counted string |
| `.` | `( n -- )` Print a signed decimal (followed by a space) |

### 4.5 Dictionary / state variables
| Word | Effect |
|----|------|
| `HERE` | `( -- addr )` Next free address in the dictionary |
| `,` | `( w -- )` Write a cell at HERE and advance HERE by 2 |
| `C,` | `( b -- )` Write one byte at HERE and advance HERE by 1 |
| `ALLOT` | `( n -- )` Advance HERE by n bytes |
| `STATE` | `( -- addr )` Address of the compile-mode flag |
| `LATEST` | `( -- addr )` Address of the most-recent header pointer |
| `>IN` | `( -- addr )` Address of the next-read offset into TIB |
| `#TIB` | `( -- addr )` Address of the TIB valid-byte count |

### 4.6 Outer-interpreter building blocks
| Word | Effect |
|----|------|
| `ACCEPT` | `( c-addr +n1 -- +n2 )` Read one line with echo |
| `PARSE-NAME` | `( -- c-addr u )` Fetch the next whitespace-delimited token |
| `SFIND` | `( c-addr u -- xt flag )` Dictionary search; flag: 0=not found, 1=normal, 2=IMMEDIATE |
| `NUMBER?` | `( c-addr u -- value flag )` Decimal parser; flag=-1 on success |
| `INTERPRET` | Walk through TIB executing or compiling each token |
| `EXECUTE` | `( xt -- )` Run the word whose CFA is xt |

### 4.7 Internal compilation primitives
| Word | Effect |
|----|------|
| `(LIT)` | Push the following cell onto the data stack (compiled via `,`) |
| `(BRANCH)` | Add the following signed cell to IP |
| `(0BRANCH)` | Same as `(BRANCH)` when TOS is zero; skip the offset otherwise |
| `(LITSTR)` | Print the inline counted string and advance IP past it |
| `EXIT` | Return from a colon definition (what `;` compiles) |

### 4.8 Defining words and control structures
| Word | Effect |
|----|------|
| `:` | `( "name" -- )` Start a new colon definition, STATE=1 |
| `;` IMMEDIATE | Compile EXIT, STATE=0 |
| `VARIABLE` | `( "name" -- )` Create a 16-bit variable (DOVAR) |
| `CONSTANT` | `( x "name" -- )` Create a constant (DOCON) |
| `IF` `ELSE` `THEN` IMMEDIATE | Forward branches via `(0BRANCH)` / `(BRANCH)` + HERE patching |
| `BEGIN` `UNTIL` `AGAIN` IMMEDIATE | Backward loops |
| `."` IMMEDIATE | Parse up to the closing `"` and compile it as a string literal |
| `(` IMMEDIATE | Skip TIB characters until the closing `)` |

### 4.9 REPL
| Word | Effect |
|----|------|
| `QUIT` | The REPL itself (running from boot) |

---

## 5. Examples

### 5.1 Arithmetic and `.`

```forth
3 4 + .           → 7  ok
10 3 - .          → 7  ok
-5 NEGATE .       → 5  ok
```

### 5.2 New definitions

```forth
: DOUBLE DUP + ;
3 DOUBLE .        → 6  ok
5 DOUBLE DOUBLE . → 20  ok
```

### 5.3 Conditionals

```forth
: ABS DUP 0< IF NEGATE THEN ;
-17 ABS .         → 17  ok
42 ABS .          → 42  ok

: SIGN DUP 0< IF DROP -1 ELSE 0= IF 0 ELSE 1 THEN THEN ;
-5 SIGN .         → -1 ok
0 SIGN .          → 0 ok
7 SIGN .          → 1 ok
```

### 5.4 BEGIN / UNTIL

```forth
( emit '*' while counting down to 0 )
: STARS BEGIN 42 EMIT 1 - DUP 0= UNTIL DROP ;
( Note: calling with a 0 or negative start value will loop forever. )
```

### 5.5 Variables and constants

```forth
VARIABLE CNT
0 CNT !
CNT @ . → 0  ok
42 CNT !
CNT @ . → 42  ok

100 CONSTANT MAX
MAX .  → 100  ok
```

### 5.6 Strings and comments

```forth
: GREET ." Hello, Forth!" CR ;
GREET
→ Hello, Forth!
  ok

( this is a comment — the REPL skips it )
```

---

## 6. Limitations / caveats

- **Case-sensitive**: `dup` and `DUP` are different names; only the latter
  is defined.
- **Decimal input only**; no `BASE` switch yet.
- **No DO/LOOP**; use `BEGIN ... UNTIL` for counted loops.
- **`."` strings** are capped at 255 bytes (the length is a single byte).
- On errors the REPL prints `<word>?` and keeps running, but the data stack
  is **not** rewound — if it looks corrupted, push `0`s to balance or reset.
- **Bounds are not checked** on TIB and dictionary growth — use sensible
  input sizes.
- `FORGET` / `MARKER` / `VOCABULARY` and other vocabulary management are
  not implemented.

---

## 7. Notes on em6809

A few MC6809 opcodes are not implemented by the `em6809` crate (e.g.
`ABX`, `TST <mem>`, `INC <mem>`).  This kernel avoids those directly,
so users do not need to care.  If you extend the kernel, keep that
short list in mind — equivalents using other instructions are
straightforward.

---

## 8. Tests

The smoke tests live in `tests/smoke.rs` of the mc6809 plugin crate:

- `forth_kernel_banner` — boot banner sanity check
- `forth_repl_dot` — `42 .` echo + execution
- `forth_colon_define_and_call` — `: DOUBLE DUP + ;`
- `forth_if_then_and_begin_until` — `ABS` and `ONCE` (minimal BEGIN/UNTIL)
- `forth_variable_constant_string` — VARIABLE / CONSTANT / `."` / `(`

From the crate root (where `Cargo.toml` lives):

```sh
cargo test --release forth_
```
