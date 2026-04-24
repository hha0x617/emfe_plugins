# Hha Forth — User Guide

`forth.asm` is a compact ITC (indirect-threaded code) Forth that runs under
em6809 / emfe_plugin_mc6809. It communicates interactively through a single
MC6850 ACIA and supports colon definitions with `IF`/`ELSE`/`THEN`,
`BEGIN`/`UNTIL`/`AGAIN`/`WHILE`/`REPEAT`, `DO`/`LOOP`/`+LOOP`,
`VARIABLE` / `CONSTANT`, runtime-base switching (`HEX` / `DECIMAL`),
mixed-precision and double-word arithmetic (`UM*`, `M*`, `UM/MOD`,
`*/`, `*/MOD`, `D+`, `D-`, `D.`, …), string literals `."` / `S"`,
block comments `(` and line comments `\`.

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
| `?DUP` | `( a -- a a \| 0 )` | DUP only when TOS is non-zero |
| `DROP` | `( a -- )` | Discard TOS |
| `SWAP` | `( a b -- b a )` | Swap the top two cells |
| `OVER` | `( a b -- a b a )` | Copy NOS to TOS |
| `NIP` | `( a b -- b )` | Drop NOS |
| `TUCK` | `( a b -- b a b )` | Copy TOS below NOS |
| `ROT` | `( a b c -- b c a )` | Rotate third cell to the top |
| `PICK` | `( xn … x0 n -- xn … x0 xn )` | `0 PICK` ≡ `DUP`, `1 PICK` ≡ `OVER`, … |
| `2DUP` | `( a b -- a b a b )` | Duplicate top two cells |
| `2DROP` | `( a b -- )` | Drop top two cells |
| `2SWAP` | `( a b c d -- c d a b )` | Swap two cell pairs |
| `2OVER` | `( a b c d -- a b c d a b )` | Copy NOS pair over |
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

Divide by zero is non-trapping: `/`, `MOD`, `/MOD`, `UM/MOD`, and the
mixed-precision operators with `b=0` leave the remainder equal to the
original dividend (or `0` on the unsigned path) and the quotient at `0`,
matching the kernel's general fail-soft behaviour on bad input.

### 4.3 Constants

| Word | Effect |
|----|------|
| `TRUE` | `( -- -1 )` Canonical true flag |
| `FALSE` | `( -- 0 )` Canonical false flag |
| `BL` | `( -- 32 )` ASCII space |

### 4.4 Mixed-precision / double arithmetic

Doubles on the stack are two cells: low word as NOS, high word as TOS
(`d` means `( d-low d-high )`). Memory layout for `2@`/`2!` is
low at `addr`, high at `addr+2`.

| Word | Stack effect | Notes |
|----|------|------|
| `2@` | `( addr -- d )` | Fetch a double |
| `2!` | `( d addr -- )` | Store a double |
| `D+` | `( d1 d2 -- d )` | Double add |
| `D-` | `( d1 d2 -- d )` | Double subtract |
| `DNEGATE` | `( d -- -d )` | Two's complement negation of a double |
| `DABS` | `( d -- \|d\| )` | Absolute value |
| `D.` | `( d -- )` | Print a signed double in the current `BASE` |
| `UM*` | `( u1 u2 -- ud )` | Unsigned 16×16 → 32, low-then-high on stack |
| `M*` | `( n1 n2 -- d )` | Signed 16×16 → 32 |
| `UM/MOD` | `( ud u -- urem uquot )` | Unsigned 32/16 division |
| `*/` | `( n1 n2 n3 -- n )` | `n1*n2/n3` with 32-bit intermediate; signed |
| `*/MOD` | `( n1 n2 n3 -- rem quot )` | Same as `*/` but also leaves the remainder |

### 4.5 Memory
| Word | Stack effect | Notes |
|----|------|------|
| `@` | `( addr -- w )` | 16-bit fetch |
| `!` | `( w addr -- )` | 16-bit store |
| `+!` | `( n addr -- )` | Add `n` into the cell at `addr` |
| `C@` | `( addr -- b )` | 8-bit fetch |
| `C!` | `( b addr -- )` | 8-bit store |
| `CELL+` | `( addr -- addr+2 )` | Advance by one cell |
| `CELLS` | `( n -- n*2 )` | Convert a cell count to a byte count |
| `CMOVE` | `( src dst u -- )` | Copy `u` bytes low-to-high |
| `FILL` | `( addr u byte -- )` | Write `u` copies of `byte` starting at `addr` |

### 4.6 I/O and number formatting
| Word | Effect |
|----|------|
| `EMIT` | `( c -- )` Emit one character |
| `KEY` | `( -- c )` Blocking single-character read |
| `CR` | Emit `CRLF` |
| `SPACE` | Emit a single space |
| `SPACES` | `( n -- )` Emit `n` spaces (non-positive: no-op) |
| `TYPE` | `( addr u -- )` Print a string |
| `COUNT` | `( c-addr -- addr u )` Unpack a counted string |
| `.` | `( n -- )` Print a signed integer in the current `BASE`, followed by a space |
| `U.` | `( u -- )` Unsigned variant |
| `.R` | `( n w -- )` Print signed `n` right-justified in `w` chars (no trailing space) |
| `U.R` | `( u w -- )` Unsigned right-justified |
| `DUMP` | `( addr u -- )` Hex dump of `u` bytes starting at `addr`, 16 per line |

### 4.7 Base (radix) control
| Word | Effect |
|----|------|
| `BASE` | `( -- addr )` Current I/O radix (cell; default 10) |
| `HEX` | Set `BASE` to 16 |
| `DECIMAL` | Set `BASE` to 10 |

`NUMBER?` accepts upper- or lower-case `A`–`Z` digits when `BASE > 10`.
All output words (`.`, `U.`, `.R`, `U.R`, `D.`, `DUMP`) respect `BASE`.

### 4.8 Dictionary / state variables
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

### 4.9 Outer-interpreter building blocks
| Word | Effect |
|----|------|
| `ACCEPT` | `( c-addr +n1 -- +n2 )` Read one line with echo |
| `PARSE-NAME` | `( -- c-addr u )` Fetch the next whitespace-delimited token |
| `SFIND` | `( c-addr u -- xt flag )` Dictionary search; flag: 0=not found, 1=normal, 2=IMMEDIATE |
| `NUMBER?` | `( c-addr u -- value flag )` `BASE`-aware parser; flag=-1 on success |
| `INTERPRET` | Walk through TIB executing or compiling each token |
| `EXECUTE` | `( xt -- )` Run the word whose CFA is xt |
| `'` | `( "name" -- xt )` Look up a word's execution token (0 on fail) |

### 4.10 Internal compilation primitives
| Word | Effect |
|----|------|
| `(LIT)` | Push the following cell onto the data stack |
| `(BRANCH)` | Add the following signed cell to IP |
| `(0BRANCH)` | Same as `(BRANCH)` when TOS is zero; skip the offset otherwise |
| `(LITSTR)` | Print the inline counted string and advance IP past it (for `."`) |
| `(SLITERAL)` | Push the inline counted string as `( addr u )` (for `S"`) |
| `(DO)` `(LOOP)` `(+LOOP)` | Runtime partners for `DO` / `LOOP` / `+LOOP` |
| `EXIT` | Return from a colon definition (what `;` compiles) |

### 4.11 Defining words and control structures
| Word | Effect |
|----|------|
| `:` | `( "name" -- )` Start a new colon definition, STATE=1 |
| `;` IMMEDIATE | Compile EXIT, STATE=0 |
| `VARIABLE` | `( "name" -- )` Create a 16-bit variable (DOVAR) |
| `CONSTANT` | `( x "name" -- )` Create a constant (DOCON) |
| `IMMEDIATE` | Mark the most-recently-defined word IMMEDIATE |
| `LITERAL` IMMEDIATE | `( x -- )` at compile time → compiles `(LIT) x` |
| `RECURSE` IMMEDIATE | Compile a call to the colon definition currently being defined |
| `IF` `ELSE` `THEN` IMMEDIATE | Forward branches via `(0BRANCH)` / `(BRANCH)` + HERE patching |
| `BEGIN` `UNTIL` `AGAIN` IMMEDIATE | Backward loops |
| `BEGIN` `WHILE` `REPEAT` IMMEDIATE | Conditional-exit loop |
| `DO` `LOOP` `+LOOP` IMMEDIATE | Counted loops |
| `I` `J` | Access the inner / next-outer loop index |
| `LEAVE` | Force-exit the current loop (sets index := limit; exit occurs at next `LOOP`/`+LOOP`) |
| `."` IMMEDIATE | Parse up to the closing `"` and compile as a string literal (prints at runtime) |
| `S"` IMMEDIATE | Parse up to the closing `"` and compile as a string literal (leaves `( addr u )` at runtime) |
| `(` IMMEDIATE | Skip TIB characters until the closing `)` |
| `\` IMMEDIATE | Rest-of-line comment |

### 4.12 Debugging
| Word | Effect |
|----|------|
| `.S` | `( -- )` Non-destructive stack dump: `<depth> a b c …` |
| `WORDS` | `( -- )` Print every dictionary entry, newest first |

### 4.13 REPL
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

( this is a block comment — the REPL skips it )
\ this is a line comment — everything through end-of-line is ignored
```

### 5.7 DO / LOOP and doubles

```forth
: SQUARES  10 0 DO I I * . LOOP ;
SQUARES     → 0 1 4 9 16 25 36 49 64 81  ok

( 32-bit product: 40000 * 3 = 120000, prints as a double )
40000 3 M* D.  → 120000  ok
```

### 5.8 HEX / DECIMAL

```forth
HEX  255 .     → FF  ok
     FF .      → FF  ok
DECIMAL
     FF .      → FF?             \ treated as a word, not a number
     255 .     → 255  ok
```

### 5.9 BEGIN / WHILE / REPEAT

```forth
: COUNTDOWN  10 BEGIN DUP 0> WHILE DUP . 1- REPEAT DROP ;
COUNTDOWN   → 10 9 8 7 6 5 4 3 2 1  ok
```

### 5.10 Stack-manipulation helpers

```forth
( ?DUP keeps a zero check cheap — duplicate only when non-zero )
: NON-ZERO?  ?DUP IF ." yes" DROP ELSE ." no" THEN ;
42 NON-ZERO?   → yes  ok
0  NON-ZERO?   → no   ok

( PICK generalises DUP / OVER )
11 22 33 44  0 PICK .   → 44  ok     \ same as DUP
11 22 33 44  2 PICK .   → 22  ok     \ third from the top

( NIP drops the second item, TUCK copies the top below it )
1 2 3 NIP .S    → <2> 1 3     ok
1 2   TUCK .S   → <3> 2 1 2   ok

( 2DUP is handy when a value has to survive its own comparison )
: CLAMP-LOW  ( n lo -- n' )   2DUP < IF SWAP THEN DROP ;
  5 0 CLAMP-LOW .    → 5  ok
 -3 0 CLAMP-LOW .    → 0  ok
```

### 5.11 Memory helpers

```forth
VARIABLE COUNTER   0 COUNTER !
1 COUNTER +!       COUNTER @ .   → 1  ok
5 COUNTER +!       COUNTER @ .   → 6  ok

( VARIABLE gives 2 bytes; use ALLOT to extend it into a buffer )
VARIABLE BUF   14 ALLOT         \ BUF now spans 16 bytes
BUF 16 0 FILL                   \ zero the buffer
BUF 16 DUMP                     \ hex dump (addresses honour BASE)

( S" leaves ( addr u ); CMOVE copies bytes )
: PUT-HELLO   S" HELLO" BUF SWAP CMOVE ;
PUT-HELLO
BUF 5 TYPE                      → HELLO  ok
```

### 5.12 Double-precision values and `2@` / `2!`

```forth
VARIABLE BIG  4 ALLOT            \ reserve one extra cell so BIG holds a double

( store 0x0001_2345 at BIG )
HEX  2345 1 BIG 2!  DECIMAL
BIG 2@ D.                         → 74565  ok     \ 0x00012345

( double add: 65000 + 100 = 65100, no 16-bit overflow )
65000 0  100 0  D+  D.            → 65100  ok

( DABS turns a negative double positive )
-1 -1  DABS  D.                   → 1  ok         \ 0xFFFFFFFF → 1
```

### 5.13 Mixed-precision arithmetic (`M*`, `UM*`, `UM/MOD`, `*/`)

```forth
( keep precision across a multiply: 40000 * 7 = 280000 )
40000 7 M* D.                     → 280000  ok

( unsigned 16×16 product, low-then-high )
1000 1000 UM*  SWAP .  .          → 16960 15  ok
 \ 1000 * 1000 = 1_000_000 = 15 * 65536 + 16960

( unsigned long division — split a single double by a single cell )
0 1  100 UM/MOD  SWAP . .         → 655 36  ok
 \ (1 * 65536) / 100 = 655 rem 36

( */ avoids 16-bit overflow in percent / ratio calculations )
12345 37 100 */ .                 → 4567  ok       \ 12345 * 37 / 100
12345 37 100 */MOD . .            → 65 4567  ok    \ rem=65 quot=4567
```

### 5.14 DO / LOOP / +LOOP / I / J / LEAVE

```forth
( countdown using +LOOP with a negative step )
: BLASTOFF  0 10 DO I . -1 +LOOP ." LIFTOFF" CR ;
BLASTOFF    → 10 9 8 7 6 5 4 3 2 1 LIFTOFF  ok

( nested loop: print a multiplication table — J is the outer index )
: TABLE  5 1 DO 5 1 DO J I * 4 .R LOOP CR LOOP ;
TABLE
   →    1   2   3   4
        2   4   6   8
        3   6   9  12
        4   8  12  16   ok

( LEAVE: force early exit. LEAVE doesn't exit immediately — it sets
  index := limit, and the actual exit happens at the next LOOP. )
: FIRST-FIVE   10 0 DO I . I 4 = IF LEAVE THEN LOOP ;
FIRST-FIVE  → 0 1 2 3 4  ok
```

### 5.15 Number formatting and base

```forth
( right-justify a table of values in decimal )
: HIST   10 0 DO I DUP * 6 .R LOOP CR ;
HIST    →      0     1     4     9    16    25    36    49    64    81  ok

( U.R in hex for an address table )
HEX
: MAP  3 0 DO I 1000 * 5 U.R LOOP CR ;
MAP     →     0  1000  2000  ok
DECIMAL

( U. shows values interpreted as unsigned )
-1 .      → -1  ok
-1 U.     → 65535  ok
```

### 5.16 REPL debug helpers

```forth
1 2 3 .S               → <3> 1 2 3  ok
DROP DROP DROP

WORDS                  \ lists every built-in + user-defined word, newest first

HEX
C000 40 DUMP           \ hex dump of the first 64 bytes of the return-stack region
DECIMAL
```

### 5.17 Compile-time tricks

```forth
( ' ("tick") gets the xt of a word — run it later with EXECUTE )
' + .                → <some address>  ok
3 4 ' + EXECUTE .    → 7  ok

( RECURSE lets a colon definition call itself — needed because
  the name is still F_HIDDEN while the definition is being compiled )
: FACT  DUP 1 > IF DUP 1- RECURSE * THEN ;
6 FACT .             → 720  ok

( S" leaves ( addr u ) — good for TYPE or CMOVE )
: SHOUT  S" HELLO!" TYPE CR ;
SHOUT                → HELLO!  ok

( LITERAL is IMMEDIATE — it compiles TOS as an inline constant.
  Typically called from other IMMEDIATE words to bake a computed
  value into the caller's definition. )
```

---

## 6. Limitations / caveats

- **Case-sensitive**: `dup` and `DUP` are different names; only the latter
  is defined.
- **`."` / `S"` strings** are capped at 255 bytes (the length is a single byte).
- `LEAVE` sets the current loop index equal to the limit — the actual loop
  exit happens on the next `LOOP` / `+LOOP`, not immediately.
- On errors the REPL prints `<word>?` and keeps running, but the data stack
  is **not** rewound — if it looks corrupted, push `0`s to balance or reset.
- **Bounds are not checked** on TIB and dictionary growth — use sensible
  input sizes.
- No pictured-numeric-output words (`<# # #> HOLD SIGN`) — formatting is
  done by `.`, `U.`, `.R`, `U.R`, and `D.`.
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
