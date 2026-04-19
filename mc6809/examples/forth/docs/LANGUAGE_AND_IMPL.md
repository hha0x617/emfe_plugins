# Hha Forth — Language Spec & Implementation Notes

This document describes the **language spec** (Forth-family
characteristics) and the **implementation design** (threading model,
dictionary structure, inner interpreter) of
`examples/forth/forth.asm`.  For everyday use, see
[../README.md](../README.md).

---

## 1. Code-size metrics

| Metric | Value |
|---|---|
| Assembly source | **1,535 lines** (single `forth.asm`) |
| Raw binary | **2,605 bytes** (~2.5 KB) |
| SREC file | 7,262 bytes (ASCII S-record) |
| CFAs (primitive + colon definitions) | **63** |
| Smoke tests | **6**, all passing |

**About 1/7 the size of Hha Lisp** (18.5 KB, 60 primitives).  In the
figForth / jonesforth family of minimal-but-usable implementations.

---

## 2. Forth-family characteristics

### 2.1 Execution model

- **Indirect-threaded code (ITC)**: each dictionary entry's CFA slot
  holds either a pointer to native code (primitives) or to DOCOL
  (colon definitions).  NEXT performs a two-level indirect jump.
- **Separate data / return stacks**: U is the data stack, S is the
  return stack.  Both grow downward; TOS is at the lower address.
- **16-bit cell**: every cell is a signed 16-bit integer.  Numeric
  input is decimal only.

### 2.2 REPL (outer interpreter)

- `ACCEPT` reads one line (up to 128 bytes) into TIB
- `INTERPRET` calls `PARSE-NAME` then `SFIND` / `NUMBER?`
- At end of line, prints `" ok"` + CRLF and loops
- Unknown words print `<word>?`; the REPL survives but the stack is
  not unwound

### 2.3 Compilation mode

- `:` sets `STATE = 1`; `;` (IMMEDIATE) sets `STATE = 0`
- During compilation:
  - Non-IMMEDIATE words → their CFA is written at HERE
  - Numbers → `(LIT)` + value (two cells)
  - IMMEDIATE words → executed immediately

### 2.4 ANS Forth compatibility

**Partial:**
- ✅ `:` `;` `IF` `THEN` `ELSE` `BEGIN` `UNTIL` `AGAIN` `."` `(`
- ✅ `VARIABLE` `CONSTANT` `ALLOT` `,` `C,` `HERE` `LATEST`
- ✅ `>R` `R>` `R@` control-flow words
- ❌ `DO` `LOOP` `+LOOP` (not implemented)
- ❌ `BASE` switching (decimal only)
- ❌ `WORD` `FIND` are replaced by `PARSE-NAME` + `SFIND`
- ❌ `FORGET` / `MARKER` / `VOCABULARY` (single namespace)

---

## 3. Memory map

```
$0100..$1FFF  kernel code + built-in dictionary  (~8 KB)
$2000..$9FFF  user dictionary (HERE grows upward, ~32 KB)
$A000..$A07F  TIB (terminal input buffer, 128 bytes)
$A080..$AFFF  unused
$B000..$BFFE  data stack (U starts at $BFFE, grows down)
$BFFF..$BFFF  guard
$C000..$FEFE  return stack (S starts at $FEFE)
$FF00/$FF01   ACIA SR/CR, RDR/TDR
$FFFE/$FFFF   reset vector → cold
```

- **Cell size**: 16-bit, big-endian
- **Max token length**: 31 characters (`F_LENMASK = $1F`)
- **Dictionary**: latest-first linked list; each entry holds the link
  to the previous one, terminated by NIL

---

## 4. Dictionary entry layout

Standard figForth-style layout:

```
+---------+-----------+----------+-----------+-----------+------------+
| LINK 2B | FLAGS 1B  | NAME N B | (padding) | CFA 2B    | body ...   |
+---------+-----------+----------+-----------+-----------+------------+
   ^                                          ^
   LATEST points here                         xt (execution token)
```

- **LINK**: address of the previous LINK field (NIL terminates)
- **FLAGS**: `F_IMMED` (bit 7), `F_HIDDEN` (bit 6), **name length** in
  bits 0–4 (max 31)
- **NAME**: raw bytes (no null terminator; length comes from FLAGS)
- **CFA**: native code address for primitives, or DOCOL for colon
  definitions
- **body**:
  - Primitive: native code
  - Colon definition: a sequence of CFAs ending in `EXIT`
  - VARIABLE: DOVAR followed by a 2-byte cell
  - CONSTANT: DOCON followed by the 2-byte value

### 4.1 xt (execution token)

The CFA serves as the xt.  `EXECUTE` treats TOS as a CFA and performs
the equivalent of `jmp [,x]`.

---

## 5. Inner interpreter

### 5.1 NEXT / DOCOL / EXIT

```asm
NEXT:       ldy ,x++         ; W = *IP; IP += 2
            jmp [,y]         ; indirect jump through CFA → code

DOCOL:      pshs x           ; push old IP onto the return stack
            leax 2,y         ; IP = CFA + 2 (body)
            jmp NEXT

EXIT:       puls x           ; pop IP
            jmp NEXT
```

- **IP (X)** — position within the current thread
- **W (Y)** — the CFA currently being resolved
- Colon definition bodies are sequences of CFA pointers
  (`fdb cfa_word_1, cfa_word_2, …`)
- DOCOL saves IP, EXIT restores it

### 5.2 DOVAR / DOCON

- **DOVAR**: push PFA (CFA + 2) onto the data stack — used by VARIABLE
- **DOCON**: push `*PFA` — used by CONSTANT

### 5.3 NEXT cost

Per word:
- `ldy ,x++`: 8 cycles (postbyte indirect + autoincrement)
- `jmp [,y]`: 7 cycles (indirect JMP)
- Primitive body: ~10–30 cycles
- Total: ~30 cycles per word

---

## 6. Primitive categories

- **Stack ops**: `DUP` `DROP` `SWAP` `OVER` `ROT` `>R` `R>` `R@`
- **Arithmetic / logic**: `+` `-` `AND` `OR` `XOR` `INVERT` `NEGATE`
  `0=` `0<` `=` `<`
- **Memory**: `@` `!` `C@` `C!`
- **I/O**: `EMIT` `KEY` `CR` `SPACE` `TYPE` `COUNT` `.`
- **Dict / variables**: `HERE` `,` `C,` `ALLOT` `STATE` `LATEST`
  `>IN` `#TIB`
- **Outer interp**: `ACCEPT` `PARSE-NAME` `SFIND` `NUMBER?`
  `INTERPRET` `EXECUTE`
- **Compile-time helpers**: `(LIT)` `(BRANCH)` `(0BRANCH)` `(LITSTR)`
  `EXIT`
- **Defining / control**: `:` `;` `VARIABLE` `CONSTANT` `IF` `ELSE`
  `THEN` `BEGIN` `UNTIL` `AGAIN` `."` `(`
- **REPL**: `QUIT`

---

## 7. Implementation notes

### 7.1 `SFIND` — dictionary search

```
SFIND ( c-addr u -- xt flag )
  flag: 0 = not found
        1 = regular word
        2 = IMMEDIATE word
```

Walks the LATEST → LINK chain.  Skips entries whose `F_HIDDEN` flag is
set (the smudge bit used to hide a word during its own compilation).

### 7.2 `NUMBER?` — number parser

- Decimal only, signed 16-bit
- Leading `-` negates
- Any non-digit fails (`flag = 0`)
- On success, `flag = -1` and the value is on TOS

### 7.3 `:` implementation

```
:   ( "name" -- )
    create header with CFA = DOCOL
    set F_HIDDEN  ( so the word can't call itself recursively )
    STATE = 1
;   ( IMMEDIATE )
    compile EXIT CFA
    clear F_HIDDEN
    STATE = 0
```

Inside compilation, numbers and non-IMMEDIATE words are committed to
the dictionary; IMMEDIATE words run immediately to patch control-flow
placeholders.

### 7.4 Control structures

- `IF`: compile `(0BRANCH) + placeholder`, push placeholder address
- `THEN`: pop the placeholder, patch it with `HERE − placeholder`
- `ELSE`: compile `(BRANCH) + newplaceholder`, patch IF's placeholder,
  push the new one
- `BEGIN`: push HERE
- `UNTIL`: compile `(0BRANCH)` + a negative offset back to BEGIN
- `AGAIN`: compile `(BRANCH)` + a negative offset

### 7.5 `."` string literal

- Compile time: emit `(LITSTR)` + length byte + bytes
- Runtime: `(LITSTR)` reads the length, TYPEs the string, advances IP
  past the string

### 7.6 Comment `(`

- IMMEDIATE; uses PARSE to skip through to `)`.
- Works at both the REPL and inside colon definitions.

### 7.7 `ACCEPT` details

- Characters are echoed as typed
- `BS` (0x08) / `DEL` (0x7F): delete one character, send `BS SPACE BS`
  to erase the console glyph
- `CR` / `LF`: finalise the line, output CRLF, update `#TIB`

---

## 8. Stack limits

| Region | Size | Capacity |
|---|---|---|
| Data stack | $BFFE → $B000 = 4 KB | 2048 cells |
| Return stack | $FEFE → $C000 = 16 KB | ~8192 cells (shared with BSR frames) |
| TIB | 128 bytes | one line |
| User dictionary | $2000 → $9FFF = 32 KB | generous |

Both stacks grow downward with no overflow detection — keep depth
reasonable.

---

## 9. em6809 requirements

The kernel relies on these MC6809 features:

- All baseline instructions except ABX
- 16-bit: LDD/STD/ADDD/SUBD/CMPD, LDX/LDY/LDU, STX/STY/STU
- Indexed modes: `,X`, `,X++`, `,--X`, `d,X`
- Indirect: `[,Y]` (used by DOCOL)
- Branches: BSR/LBSR, BRA/LBRA, Bcc/LBcc
- Stack: PSHS/PULS with a register set
- `LEAX` `LEAY` `LEAS` `LEAU`

**Bugs found in the em6809 crate during development** (all fixed
upstream):

- LEAS and LEAU had swapped effects
- `ABX` was not implemented → Forth avoids ABX
- Certain `TST <mem>` / `INC <mem>` forms were missing → same

---

## 10. File layout

```
forth.asm  1,535 lines
  ├ equates / constants        1–45
  ├ cold / banner / puts      45–90
  ├ NEXT / DOCOL / EXIT       90–120
  ├ DOVAR / DOCON             100–120
  ├ stack primitives         120–260
  ├ arithmetic               260–400
  ├ memory access            400–470
  ├ I/O (EMIT / KEY / ...)   470–580
  ├ dict primitives          580–720
  ├ ACCEPT / PARSE-NAME      720–870
  ├ SFIND                    870–1000
  ├ NUMBER?                 1000–1100
  ├ INTERPRET               1100–1220
  ├ compile primitives      1220–1340
  ├ control structures      1340–1450
  └ built-in dictionary     1450–1535 (LATEST chain)
```

At cold-boot completion: HERE = `$2000` (empty user dictionary), built-in
dictionary fits in ~1.5 KB of the 2.6 KB binary.

---

## 11. Possible improvements

| Item | Cost estimate | Notes |
|---|---|---|
| `DO` / `LOOP` / `+LOOP` | 4-6 h | counted loops |
| `BASE` switching (hex / binary) | 2-3 h | extend NUMBER? and `.` |
| Case-insensitive search | 1-2 h | tweak SFIND comparison |
| `WORDS` (dictionary dump) | 1 h | walk LATEST |
| `FORGET` / `MARKER` | 3-4 h | snapshot LATEST + HERE |
| Floating point (Q8.8 or IEEE) | large | depends on use case |
| Metacompiler / target compiler | large | for self-hosting |

---

## 12. License

MIT OR Apache-2.0 (dual-licensed) — see the SPDX header in
`forth.asm`.
