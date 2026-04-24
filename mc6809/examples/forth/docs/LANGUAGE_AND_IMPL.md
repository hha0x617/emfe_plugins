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
| Assembly source | **4,561 lines** (single `forth.asm`) |
| Raw binary | **7,955 bytes** (~7.8 KB) |
| SREC file | 22,034 bytes (ASCII S-record) |
| CFAs (primitive + colon definitions) | **175** |
| FORTH-83 required word-set coverage | **~95%** |
| Smoke tests | **7**, all passing |

Still under half the size of Hha Lisp (18.5 KB) while covering the
bulk of the FORTH-83 Required Word Set: colon definitions,
`IF`/`ELSE`/`THEN`, `BEGIN`/`UNTIL`/`AGAIN`/`WHILE`/`REPEAT`,
`DO`/`LOOP`/`+LOOP`, `CREATE`/`DOES>`, runtime-base switching,
mixed-precision / double arithmetic, pictured numeric output,
string ops, `FORGET` / `MARKER`, and `ABORT"`.  In the figForth /
jonesforth family of minimal-but-usable implementations.

---

## 2. Forth-family characteristics

### 2.1 Execution model

- **Indirect-threaded code (ITC)**: each dictionary entry's CFA slot
  holds either a pointer to native code (primitives) or to DOCOL
  (colon definitions).  NEXT performs a two-level indirect jump.
- **Separate data / return stacks**: U is the data stack, S is the
  return stack.  Both grow downward; TOS is at the lower address.
- **16-bit cell**: every cell is a signed 16-bit integer.
- **Double cell**: 32-bit, stored as low-at-`addr` / high-at-`addr+2`,
  and on the stack as `( low high )` with the high word as TOS.
- **Runtime radix**: `BASE` (default 10) is honoured by both
  `NUMBER?` and every output word.

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

### 2.4 FORTH-83 / ANS Forth compatibility

**Covered (~95% of the FORTH-83 Required Word Set):**
- ✅ Control flow: `:` `;` `IF` `THEN` `ELSE` `BEGIN` `UNTIL` `AGAIN`
      `WHILE` `REPEAT` `DO` `LOOP` `+LOOP` `I` `J` `LEAVE` `UNLOOP`
      `."` `S"` `ABORT"` `(` `\`
- ✅ Defining: `VARIABLE` `CONSTANT` `CREATE` `DOES>` `ALLOT` `,` `C,`
      `HERE` `LATEST` `IMMEDIATE` `LITERAL` `RECURSE` `POSTPONE`
      `FORGET` `MARKER` `'` `[']` `CHAR` `[CHAR]`
- ✅ Stack: `DUP` `?DUP` `DROP` `SWAP` `OVER` `NIP` `TUCK` `ROT`
      `-ROT` `PICK` `ROLL` `DEPTH`
      `2DUP` `2DROP` `2SWAP` `2OVER` `>R` `R>` `R@`
- ✅ Memory: `@` `!` `+!` `C@` `C!` `2@` `2!` `CELL+` `CELLS`
      `ALIGN` `ALIGNED` `CMOVE` `CMOVE>` `MOVE` `FILL` `ERASE` `BLANK`
- ✅ Strings: `COUNT` `COMPARE` `/STRING` `-TRAILING`
- ✅ Arithmetic: `+` `-` `*` `/` `MOD` `/MOD` `1+` `1-` `2+` `2-`
      `2*` `2/` `LSHIFT` `RSHIFT` `NEGATE` `ABS` `MIN` `MAX`
- ✅ Logic: `AND` `OR` `XOR` `INVERT` `NOT`
      `0=` `0<` `0>` `=` `<>` `<` `>` `U<` `U>`
- ✅ Constants / vars: `TRUE` `FALSE` `BL` `BASE` `HEX` `DECIMAL`
- ✅ Number I/O: `.` `U.` `.R` `U.R` `D.` `D.R` `SPACES`
      `<#` `#` `#S` `#>` `HOLD` `SIGN`
- ✅ Mixed / double: `M+` `UM*` `M*` `UM/MOD` `SM/REM` `FM/MOD`
      `*/` `*/MOD` `D+` `D-` `DNEGATE` `DABS`
- ✅ Error handling: `ABORT` `ABORT"`
- ✅ Debug: `.S` `WORDS` `DUMP`

**Intentionally excluded (low value or security concern):**
- ❌ `SP@` / `SP!` / `RP@` / `RP!` — stack-pointer introspection
- ❌ `EXPECT` / `QUERY` — redundant with `ACCEPT`
- ❌ `VOCABULARY` / `DEFINITIONS` / `ONLY` — vocabulary system (single
      namespace here)
- ❌ `WORD` + `FIND` — `WORD` is provided; `FIND` replaced by
      `PARSE-NAME` + `SFIND`
- ❌ Mass-storage words (`BLOCK` / `BUFFER` / `UPDATE` / `SAVE-BUFFERS`) —
      not applicable on this hardware target

---

## 3. Memory map

```
$0100..$27FF  kernel code + built-in dictionary  (~10 KB)
$2800..$9FFF  user dictionary (HERE grows upward, ~30 KB)
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

- **Stack ops**: `DUP` `?DUP` `DROP` `SWAP` `OVER` `NIP` `TUCK` `ROT`
  `-ROT` `PICK` `ROLL` `DEPTH` `2DUP` `2DROP` `2SWAP` `2OVER`
  `>R` `R>` `R@`
- **Arithmetic / logic (16-bit)**: `+` `-` `*` `/` `MOD` `/MOD`
  `1+` `1-` `2+` `2-` `2*` `2/` `LSHIFT` `RSHIFT`
  `NEGATE` `ABS` `MIN` `MAX`
  `AND` `OR` `XOR` `INVERT` `NOT`
  `0=` `0<` `0>` `=` `<>` `<` `>` `U<` `U>`
- **Mixed / double**: `2@` `2!` `D+` `D-` `DNEGATE` `DABS` `D.` `D.R`
  `M+` `UM*` `M*` `UM/MOD` `SM/REM` `FM/MOD` `*/` `*/MOD`
- **Constants**: `TRUE` `FALSE` `BL`
- **Memory**: `@` `!` `+!` `C@` `C!` `CELL+` `CELLS`
  `ALIGN` `ALIGNED` `CMOVE` `CMOVE>` `MOVE` `FILL` `ERASE` `BLANK`
- **Strings**: `COMPARE` `/STRING` `-TRAILING`
- **I/O and formatting**: `EMIT` `KEY` `CR` `SPACE` `SPACES` `TYPE`
  `COUNT` `.` `U.` `.R` `U.R` `DUMP`
  `<#` `#` `#S` `#>` `HOLD` `SIGN`
- **Radix**: `BASE` `HEX` `DECIMAL`
- **Dict / variables**: `HERE` `,` `C,` `ALLOT` `STATE` `LATEST`
  `>IN` `#TIB`
- **Outer interp**: `ACCEPT` `PARSE-NAME` `WORD` `SFIND` `NUMBER?`
  `INTERPRET` `EXECUTE` `'` `CHAR` `[CHAR]`
- **Compile-time helpers**: `(LIT)` `(BRANCH)` `(0BRANCH)` `(LITSTR)`
  `(SLITERAL)` `(DO)` `(LOOP)` `(+LOOP)` `(;DOES)` `(ABORT")` `EXIT`
- **Defining / control**: `:` `;` `VARIABLE` `CONSTANT` `CREATE` `DOES>`
  `IMMEDIATE` `LITERAL` `RECURSE` `POSTPONE` `[']`
  `IF` `ELSE` `THEN` `BEGIN` `UNTIL` `AGAIN` `WHILE` `REPEAT`
  `DO` `LOOP` `+LOOP` `I` `J` `LEAVE` `UNLOOP`
  `FORGET` `MARKER`
  `."` `S"` `ABORT"` `(` `\`
- **Error handling**: `ABORT` `ABORT"`
- **Debug**: `.S` `WORDS`
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

- Signed 16-bit; base taken from the `BASE` variable at call time
- Accepts digits 0–9 and A–Z (upper or lower case), each with digit
  value `< BASE`
- Leading `-` negates
- Any out-of-range digit fails (`flag = 0`)
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
- `WHILE`: compile `(0BRANCH) + placeholder`, remember that address
- `REPEAT`: compile `(BRANCH)` + offset back to BEGIN, then patch the
  `WHILE` placeholder so a false branch jumps past `REPEAT`

### 7.5 `DO` / `LOOP` / `+LOOP`

- `DO`: compile `(DO)`, push HERE for later `LOOP` back-patching
- `(DO)` (runtime): pop limit and start off the data stack and push
  them onto the **return stack** as (limit, index) pairs — index
  ends up at the top so `I` can read it with `ldd 0,s`
- `LOOP`: compile `(LOOP)` + negative offset back to the HERE saved
  by `DO`
- `(LOOP)`: increment index; if it equals limit, discard both and
  fall through; otherwise take the branch
- `+LOOP`: like `LOOP` but adds the TOS increment (signed) — exits
  when the loop crosses the limit in the appropriate direction
- `I` / `J`: read the innermost / next-outer index from the return
  stack (indices are at `0,s` and `4,s` respectively)
- `LEAVE`: sets `index := limit` so the next `LOOP` / `+LOOP` exits
  immediately. This is **not** an unconditional exit — code between
  `LEAVE` and `LOOP` still runs.

### 7.6 `."` / `S"` string literals

- Compile time: emit the runtime (`(LITSTR)` for `."`,
  `(SLITERAL)` for `S"`) + length byte + bytes
- `(LITSTR)`: read the length, `TYPE` the string, advance IP past it
- `(SLITERAL)`: read the length, push `( addr u )`, advance IP past
  the string (caller can then `TYPE`, store, etc.)

### 7.7 Comments

- `(` is IMMEDIATE; walks TIB until it sees `)`.  Works at the REPL
  and inside colon definitions.
- `\` is IMMEDIATE; advances `>IN` to the end of the current line.

### 7.8 `ACCEPT` details

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
forth.asm  3,331 lines  (section boundaries are approximate)
  ├ equates / constants
  ├ cold / banner / puts
  ├ NEXT / DOCOL / EXIT / DOVAR / DOCON
  ├ stack primitives (inc. ?DUP / NIP / TUCK / PICK / 2DUP / …)
  ├ arithmetic (16-bit + divmod / shift)
  ├ mixed-precision / double   (UM* / M* / UM/MOD / */ / */MOD /
  │                              D+ / D- / DNEGATE / DABS / D.)
  ├ memory access (@ / ! / +! / CMOVE / FILL / 2@ / 2!)
  ├ I/O and formatting (EMIT / ...  / . / U. / .R / U.R / DUMP / SPACES)
  ├ BASE / HEX / DECIMAL (and BASE-aware NUMBER? + fmt_sd / fmt_ud)
  ├ dict primitives and state variables
  ├ ACCEPT / PARSE-NAME
  ├ SFIND / sfind_kernel (shared with ')
  ├ NUMBER?
  ├ INTERPRET
  ├ compile primitives ((LIT) / (BRANCH) / (0BRANCH) / (LITSTR) /
  │                      (SLITERAL) / (DO) / (LOOP) / (+LOOP))
  ├ control structures (: / ; / IMMEDIATE / LITERAL / RECURSE /
  │                      IF / ELSE / THEN /
  │                      BEGIN / UNTIL / AGAIN / WHILE / REPEAT /
  │                      DO / LOOP / +LOOP / I / J / LEAVE /
  │                      ." / S" / ( / \)
  ├ debug (.S / WORDS)
  └ built-in dictionary (LATEST chain, 137 CFAs)
```

At cold-boot completion: HERE = `$2800` (empty user dictionary), built-in
code + dictionary fits inside the ~8 KB binary.

---

## 11. Possible improvements

| Item | Cost estimate | Notes |
|---|---|---|
| Pictured numeric output (`<# # #S #> HOLD SIGN`) | 2-3 h | complements `D.` / `.R` |
| Case-insensitive dictionary search | 1-2 h | tweak SFIND comparison |
| `FORGET` / `MARKER` | 3-4 h | snapshot LATEST + HERE |
| String helpers: `COMPARE` / `/STRING` / `-TRAILING` | 2-3 h | extend memory ops |
| Block storage | medium | for file-less persistence |
| Floating point (Q8.8 or IEEE) | large | depends on use case |
| Metacompiler / target compiler | large | for self-hosting |

---

## 12. License

MIT OR Apache-2.0 (dual-licensed) — see the SPDX header in
`forth.asm`.
