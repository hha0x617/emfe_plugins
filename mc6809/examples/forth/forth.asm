; SPDX-License-Identifier: MIT OR Apache-2.0
; Copyright (c) 2026 hha0x617
;
; ---------------------------------------------------------------------------
; Hha Forth for MC6809 — ITC (indirect-threaded) kernel running on the
; emfe_plugin_mc6809 environment (MC6850 ACIA at $FF00/$FF01, 64 KB RAM).
;
; Register conventions:
;   U = data stack pointer (grows downward; TOS at  0,U)
;   S = return stack pointer (grows downward; also holds BSR/JSR frames)
;   X = IP                  (points at next CFA word to execute)
;   Y = W                   (holds CFA during NEXT / DOCOL)
;   D = scratch / arithmetic accumulator
;
; Threading (ITC):
;   CFA cell  = pointer to native code (for primitives) or DOCOL (for colon
;               definitions).
;   NEXT does: Y = *IP++; JMP [,Y]   (indirect through Y)
;
; Memory map:
;   $0100..$1FFF   kernel code + built-in dictionary
;   $2000..$9FFF   user dictionary (HERE grows upward)
;   $A000..$A07F   TIB (terminal input buffer, 128 bytes)
;   $B000..$BFFE   data stack (U starts at $BFFE)
;   $C000..$FEFE   return stack (S starts at $FEFE)
;   $FF00/$FF01    ACIA SR/CR, RDR/TDR
;   $FFFE/$FFFF    reset vector → cold
; ---------------------------------------------------------------------------

; --- equates ---------------------------------------------------------------
ACIA_SR     equ     $FF00       ; read: status      write: control
ACIA_DATA   equ     $FF01       ; read: RDR         write: TDR

PSP_TOP     equ     $BFFE       ; data stack top (U)
RSP_TOP     equ     $FEFE       ; return stack top (S)
TIB_ADDR    equ     $A000       ; terminal input buffer
TIB_SIZE    equ     128
DICT_START  equ     $2000       ; user dictionary grows from here

F_IMMED     equ     $80         ; flags: immediate
F_HIDDEN    equ     $40         ; flags: hidden (smudge)
F_LENMASK   equ     $1F         ; low 5 bits = name length (max 31)

; Forth VM entry vector — used by INTERPRET to pass control to a word.
; EXECUTE primitive expects TOS = CFA.

; --- cold-start vector -----------------------------------------------------
            org     $0100
cold:
            lds     #RSP_TOP            ; return stack
            ldu     #PSP_TOP            ; data stack
            ; ACIA init: master reset then 8N1 / div-16 / no IRQ.
            lda     #$03
            sta     ACIA_SR
            lda     #$15
            sta     ACIA_SR

            ; Greeting so the user knows the REPL is alive.
            ldx     #msg_ready
            bsr     puts_native
            ; Seed HERE / LATEST / STATE.
            ldx     #DICT_START
            stx     var_HERE
            ldx     #last_builtin_link
            stx     var_LATEST
            ldx     #0
            stx     var_STATE
            ; Enter the Forth VM: IP → boot_code → cfa_QUIT → DOCOL → QUIT body.
            ldx     #boot_code
            jmp     NEXT

boot_code:  fdb     cfa_QUIT

msg_ready   fcc     "Hha Forth for MC6809 ready."
            fcb     $0D,$0A
            fcc     "(c) 2026 hha0x617 - MIT/Apache-2.0"
            fcb     $0D,$0A,0

; Native puts: X -> NUL-terminated string. Uses A, B; preserves X? no,
; destroys X. Used during cold boot only.
puts_native:
            lda     ,x+
            beq     puts_done
puts_wait:  ldb     ACIA_SR
            bitb    #$02
            beq     puts_wait
            sta     ACIA_DATA
            bra     puts_native
puts_done:
            rts

; ---------------------------------------------------------------------------
; Inner interpreter — NEXT / DOCOL / EXIT
; ---------------------------------------------------------------------------
NEXT:
            ldy     ,x++            ; W = *IP; IP += 2
            jmp     [,y]            ; jump through CFA → code field's code

; DOCOL: enter a colon definition. Called via NEXT when the CFA's contents
; point here. On entry Y = CFA; the body starts at Y+2.
DOCOL:
            pshs    x               ; push old IP onto return stack
            leax    2,y             ; IP = CFA + 2 (body address)
            jmp     NEXT

; DOVAR: code field for VARIABLE. Pushes PFA onto the data stack.
DOVAR:
            leay    2,y             ; PFA = CFA + 2
            sty     ,--u            ; push PFA as 16-bit cell
            jmp     NEXT

; DOCON: code field for CONSTANT. Pushes *PFA onto the data stack.
DOCON:
            ldd     2,y             ; D = value stored at PFA
            std     ,--u
            jmp     NEXT

; ---------------------------------------------------------------------------
; Built-in dictionary — linked list grows from `last_builtin_link`, each
; entry has:
;
;     +0  flags+length byte
;     +1  name bytes (len bytes)
;     +N  link field (2 bytes, big-endian — pointer to previous entry's
;         flags byte, i.e. NFA of previous word)
;     +N+2  CFA  (2 bytes — for primitives: address of code;
;                        for colon defs: address of DOCOL)
;     +N+4  PFA  (body — only for non-primitives)
;
; The `WORD` macro below lays out a primitive header. A secondary macro lays
; out a colon definition header.
; ---------------------------------------------------------------------------

; Each primitive's header starts with a label for the flags/length byte
; (the NFA), followed by name+link+CFA. We emit them with inline .fcb/.fdb.

; link anchor: NULL terminator at the bottom of the chain
prev_link   set     0

            ; -- primitive: EXIT ( -- )  restore IP from R stack ------------
nfa_EXIT    fcb     4
            fcc     "EXIT"
lnk_EXIT    fdb     prev_link
cfa_EXIT    fdb     code_EXIT
prev_link   set     nfa_EXIT

code_EXIT:  puls    x               ; IP = saved IP
            jmp     NEXT

            ; -- primitive: (LIT) — inline literal --------------------------
nfa_LIT     fcb     5
            fcc     "(LIT)"
lnk_LIT     fdb     prev_link
cfa_LIT     fdb     code_LIT
prev_link   set     nfa_LIT

code_LIT:   ldd     ,x++            ; D = *IP++; IP advanced past the cell
            std     ,--u
            jmp     NEXT

            ; -- primitive: (0BRANCH) — branch if TOS == 0 ------------------
nfa_0BR     fcb     9
            fcc     "(0BRANCH)"
lnk_0BR     fdb     prev_link
cfa_0BR     fdb     code_0BR
prev_link   set     nfa_0BR

code_0BR:   ldd     ,u++            ; pop condition
            cmpd    #0
            beq     take_0br
            leax    2,x             ; skip offset, don't branch
            jmp     NEXT
take_0br:   ldd     ,x              ; D = branch offset (signed, cell-relative)
            leax    d,x             ; IP += offset
            jmp     NEXT

            ; -- primitive: (BRANCH) — unconditional branch -----------------
nfa_BR      fcb     8
            fcc     "(BRANCH)"
lnk_BR      fdb     prev_link
cfa_BR      fdb     code_BR
prev_link   set     nfa_BR

code_BR:    ldd     ,x
            leax    d,x
            jmp     NEXT

            ; -- primitive: EXECUTE ( xt -- ) -------------------------------
nfa_EXEC    fcb     7
            fcc     "EXECUTE"
lnk_EXEC    fdb     prev_link
cfa_EXEC    fdb     code_EXEC
prev_link   set     nfa_EXEC

code_EXEC:  ldy     ,u++            ; Y = CFA popped from data stack
            jmp     [,y]

; ---------------------------------------------------------------------------
; Stack primitives
; ---------------------------------------------------------------------------
            ; DUP ( a -- a a )
nfa_DUP     fcb     3
            fcc     "DUP"
lnk_DUP     fdb     prev_link
cfa_DUP     fdb     code_DUP
prev_link   set     nfa_DUP
code_DUP:   ldd     ,u
            std     ,--u
            jmp     NEXT

            ; DROP ( a -- )
nfa_DROP    fcb     4
            fcc     "DROP"
lnk_DROP    fdb     prev_link
cfa_DROP    fdb     code_DROP
prev_link   set     nfa_DROP
code_DROP:  leau    2,u
            jmp     NEXT

            ; SWAP ( a b -- b a )
nfa_SWAP    fcb     4
            fcc     "SWAP"
lnk_SWAP    fdb     prev_link
cfa_SWAP    fdb     code_SWAP
prev_link   set     nfa_SWAP
code_SWAP:  ldd     ,u
            ldy     2,u
            sty     ,u
            std     2,u
            jmp     NEXT

            ; OVER ( a b -- a b a )
nfa_OVER    fcb     4
            fcc     "OVER"
lnk_OVER    fdb     prev_link
cfa_OVER    fdb     code_OVER
prev_link   set     nfa_OVER
code_OVER:  ldd     2,u
            std     ,--u
            jmp     NEXT

            ; ROT ( a b c -- b c a )
nfa_ROT     fcb     3
            fcc     "ROT"
lnk_ROT     fdb     prev_link
cfa_ROT     fdb     code_ROT
prev_link   set     nfa_ROT
code_ROT:   ldd     4,u             ; a
            ldy     2,u             ; b
            sty     4,u
            ldy     ,u              ; c
            sty     2,u
            std     ,u
            jmp     NEXT

            ; >R ( n -- ) ( R: -- n )
nfa_TOR     fcb     2
            fcc     ">R"
lnk_TOR     fdb     prev_link
cfa_TOR     fdb     code_TOR
prev_link   set     nfa_TOR
code_TOR:   ldd     ,u++
            pshs    d
            jmp     NEXT

            ; R> ( -- n ) ( R: n -- )
nfa_RFROM   fcb     2
            fcc     "R>"
lnk_RFROM   fdb     prev_link
cfa_RFROM   fdb     code_RFROM
prev_link   set     nfa_RFROM
code_RFROM: puls    d
            std     ,--u
            jmp     NEXT

            ; R@ ( -- n ) ( R: n -- n )
nfa_RFETCH  fcb     2
            fcc     "R@"
lnk_RFETCH  fdb     prev_link
cfa_RFETCH  fdb     code_RFETCH
prev_link   set     nfa_RFETCH
code_RFETCH:
            ldd     ,s
            std     ,--u
            jmp     NEXT

; ---------------------------------------------------------------------------
; Arithmetic / logical primitives — 16-bit signed
; ---------------------------------------------------------------------------
            ; + ( a b -- a+b )
nfa_ADD     fcb     1
            fcc     "+"
lnk_ADD     fdb     prev_link
cfa_ADD     fdb     code_ADD
prev_link   set     nfa_ADD
code_ADD:   ldd     ,u++
            addd    ,u
            std     ,u
            jmp     NEXT

            ; - ( a b -- a-b )
nfa_SUB     fcb     1
            fcc     "-"
lnk_SUB     fdb     prev_link
cfa_SUB     fdb     code_SUB
prev_link   set     nfa_SUB
code_SUB:   ldd     2,u             ; D = a
            subd    ,u              ; D = a - b
            leau    2,u
            std     ,u
            jmp     NEXT

            ; * ( a b -- a*b )  16-bit multiply, low-16 result (sign-agnostic)
            ; a = (ah:al) at 2,u, b = (bh:bl) at 0,u
            ;   a*b = ah*bh*65536 + (ah*bl + al*bh)*256 + al*bl
            ; For the low 16 bits we discard ah*bh, then add in the low bytes
            ; of (ah*bl) and (al*bh) shifted up 8. Three MULs total.
nfa_MUL     fcb     1
            fcc     "*"
lnk_MUL     fdb     prev_link
cfa_MUL     fdb     code_MUL
prev_link   set     nfa_MUL
code_MUL:   lda     3,u             ; A = al
            ldb     1,u             ; B = bl
            mul                     ; D = al*bl
            pshs    d               ; save partial: ,s=hi 1,s=lo
            lda     2,u             ; A = ah
            ldb     1,u             ; B = bl
            mul                     ; D = ah*bl (we only want B, the low 8 bits)
            addb    ,s              ; add into hi byte of partial
            stb     ,s
            lda     3,u             ; A = al
            ldb     ,u              ; B = bh
            mul                     ; D = al*bh
            addb    ,s
            stb     ,s
            puls    d               ; D = final 16-bit product
            leau    2,u
            std     ,u
            jmp     NEXT

            ; /MOD ( a b -- rem quot )  16-bit signed division, truncate to zero.
            ; Shared implementation used by /, MOD, and /MOD.
            ; On divide-by-zero we leave the stack with rem=a, quot=0.
nfa_SLMOD   fcb     4
            fcc     "/MOD"
lnk_SLMOD   fdb     prev_link
cfa_SLMOD   fdb     code_SLMOD
prev_link   set     nfa_SLMOD
code_SLMOD: lbsr    divmod_core
            jmp     NEXT

            ; / ( a b -- a/b ) signed quotient (truncating toward zero).
nfa_DIV     fcb     1
            fcc     "/"
lnk_DIV     fdb     prev_link
cfa_DIV     fdb     code_DIV
prev_link   set     nfa_DIV
code_DIV:   lbsr    divmod_core     ; leaves ( rem quot ) on U stack
            ldd     ,u              ; D = quot
            leau    2,u             ; drop remainder slot
            std     ,u              ; overwrite with quot only
            jmp     NEXT

            ; MOD ( a b -- a mod b ) signed remainder (sign of dividend).
nfa_MOD     fcb     3
            fcc     "MOD"
lnk_MOD     fdb     prev_link
cfa_MOD     fdb     code_MOD
prev_link   set     nfa_MOD
code_MOD:   lbsr    divmod_core     ; leaves ( rem quot ) on U stack
            ldd     2,u             ; D = rem (below quot)
            leau    2,u             ; drop quot
            std     ,u              ; overwrite quot slot with rem
            jmp     NEXT

            ; AND ( a b -- a&b )
nfa_AND     fcb     3
            fcc     "AND"
lnk_AND     fdb     prev_link
cfa_AND     fdb     code_AND
prev_link   set     nfa_AND
code_AND:   ldd     ,u++            ; D = b
            anda    ,u              ; A &= NOS hi
            andb    1,u             ; B &= NOS lo
            std     ,u
            jmp     NEXT

            ; OR ( a b -- a|b )
nfa_OR      fcb     2
            fcc     "OR"
lnk_OR      fdb     prev_link
cfa_OR      fdb     code_OR
prev_link   set     nfa_OR
code_OR:    ldd     ,u++
            ora     ,u
            orb     1,u
            std     ,u
            jmp     NEXT

            ; XOR ( a b -- a^b )
nfa_XOR     fcb     3
            fcc     "XOR"
lnk_XOR     fdb     prev_link
cfa_XOR     fdb     code_XOR
prev_link   set     nfa_XOR
code_XOR:   ldd     ,u++
            eora    ,u
            eorb    1,u
            std     ,u
            jmp     NEXT

            ; INVERT ( a -- ~a )
nfa_INV     fcb     6
            fcc     "INVERT"
lnk_INV     fdb     prev_link
cfa_INV     fdb     code_INV
prev_link   set     nfa_INV
code_INV:   ldd     ,u
            coma
            comb
            std     ,u
            jmp     NEXT

            ; NEGATE ( a -- -a )
nfa_NEG     fcb     6
            fcc     "NEGATE"
lnk_NEG     fdb     prev_link
cfa_NEG     fdb     code_NEG
prev_link   set     nfa_NEG
code_NEG:   ldd     ,u
            coma
            comb
            addd    #1
            std     ,u
            jmp     NEXT

            ; ABS ( n -- |n| )
nfa_ABS     fcb     3
            fcc     "ABS"
lnk_ABS     fdb     prev_link
cfa_ABS     fdb     code_ABS
prev_link   set     nfa_ABS
code_ABS:   ldd     ,u
            bpl     abs_done
            coma
            comb
            addd    #1
            std     ,u
abs_done:   jmp     NEXT

            ; MIN ( a b -- min )  signed
nfa_MIN     fcb     3
            fcc     "MIN"
lnk_MIN     fdb     prev_link
cfa_MIN     fdb     code_MIN
prev_link   set     nfa_MIN
code_MIN:   ldd     2,u             ; D = a
            cmpd    ,u              ; D - b
            blt     min_keep_a      ; a < b: keep a
            ldd     ,u              ; else overwrite slot with b
            std     2,u
min_keep_a: leau    2,u
            jmp     NEXT

            ; MAX ( a b -- max )  signed
nfa_MAX     fcb     3
            fcc     "MAX"
lnk_MAX     fdb     prev_link
cfa_MAX     fdb     code_MAX
prev_link   set     nfa_MAX
code_MAX:   ldd     2,u             ; D = a
            cmpd    ,u              ; D - b
            bgt     max_keep_a      ; a > b: keep a
            ldd     ,u              ; else use b
            std     2,u
max_keep_a: leau    2,u
            jmp     NEXT

            ; 1+ ( n -- n+1 )
nfa_1PLUS   fcb     2
            fcc     "1+"
lnk_1PLUS   fdb     prev_link
cfa_1PLUS   fdb     code_1PLUS
prev_link   set     nfa_1PLUS
code_1PLUS: ldd     ,u
            addd    #1
            std     ,u
            jmp     NEXT

            ; 1- ( n -- n-1 )
nfa_1MINUS  fcb     2
            fcc     "1-"
lnk_1MINUS  fdb     prev_link
cfa_1MINUS  fdb     code_1MINUS
prev_link   set     nfa_1MINUS
code_1MINUS: ldd    ,u
            subd    #1
            std     ,u
            jmp     NEXT

            ; 2+ ( n -- n+2 )
nfa_2PLUS   fcb     2
            fcc     "2+"
lnk_2PLUS   fdb     prev_link
cfa_2PLUS   fdb     code_2PLUS
prev_link   set     nfa_2PLUS
code_2PLUS: ldd     ,u
            addd    #2
            std     ,u
            jmp     NEXT

            ; 2- ( n -- n-2 )
nfa_2MINUS  fcb     2
            fcc     "2-"
lnk_2MINUS  fdb     prev_link
cfa_2MINUS  fdb     code_2MINUS
prev_link   set     nfa_2MINUS
code_2MINUS: ldd    ,u
            subd    #2
            std     ,u
            jmp     NEXT

            ; 2* ( n -- n*2 )  arithmetic shift left (same bit pattern for
            ; signed and unsigned — high bit replaced by bit 14).
nfa_2MUL    fcb     2
            fcc     "2*"
lnk_2MUL    fdb     prev_link
cfa_2MUL    fdb     code_2MUL
prev_link   set     nfa_2MUL
code_2MUL:  asl     1,u             ; low byte: bit 7 → C
            rol     ,u              ; high byte: C → bit 0, bit 7 discarded
            jmp     NEXT

            ; 2/ ( n -- n/2 )  FORTH-83 arithmetic (signed) shift right.
nfa_2DIV    fcb     2
            fcc     "2/"
lnk_2DIV    fdb     prev_link
cfa_2DIV    fdb     code_2DIV
prev_link   set     nfa_2DIV
code_2DIV:  asr     ,u              ; high byte: sign bit preserved, bit 0 → C
            ror     1,u             ; low byte: C → bit 7, bit 0 discarded
            jmp     NEXT

            ; 0= ( a -- flag )   flag = -1 if a==0, else 0
nfa_ZEQ     fcb     2
            fcc     "0="
lnk_ZEQ     fdb     prev_link
cfa_ZEQ     fdb     code_ZEQ
prev_link   set     nfa_ZEQ
code_ZEQ:   ldd     ,u
            cmpd    #0
            beq     zeq_true
            ldd     #0
            std     ,u
            jmp     NEXT
zeq_true:   ldd     #-1
            std     ,u
            jmp     NEXT

            ; 0< ( a -- flag )
nfa_ZLT     fcb     2
            fcc     "0<"
lnk_ZLT     fdb     prev_link
cfa_ZLT     fdb     code_ZLT
prev_link   set     nfa_ZLT
code_ZLT:   ldd     ,u
            bmi     zlt_true
            ldd     #0
            std     ,u
            jmp     NEXT
zlt_true:   ldd     #-1
            std     ,u
            jmp     NEXT

            ; NOT ( flag -- !flag )  FORTH-83 logical inversion (same as 0=).
            ; Bitwise complement is INVERT.
nfa_NOT     fcb     3
            fcc     "NOT"
lnk_NOT     fdb     prev_link
cfa_NOT     fdb     code_NOT
prev_link   set     nfa_NOT
code_NOT    equ     code_ZEQ        ; alias — identical semantics

            ; = ( a b -- flag )
nfa_EQ      fcb     1
            fcc     "="
lnk_EQ      fdb     prev_link
cfa_EQ      fdb     code_EQ
prev_link   set     nfa_EQ
code_EQ:    ldd     ,u++
            cmpd    ,u
            beq     eq_true
            ldd     #0
            std     ,u
            jmp     NEXT
eq_true:    ldd     #-1
            std     ,u
            jmp     NEXT

            ; < ( a b -- flag )  signed
nfa_LT      fcb     1
            fcc     "<"
lnk_LT      fdb     prev_link
cfa_LT      fdb     code_LT
prev_link   set     nfa_LT
code_LT:    ldd     2,u             ; D = a
            cmpd    ,u              ; compare a to b
            blt     lt_true
            leau    2,u
            ldd     #0
            std     ,u
            jmp     NEXT
lt_true:    leau    2,u
            ldd     #-1
            std     ,u
            jmp     NEXT

            ; > ( a b -- flag )  signed
nfa_GT      fcb     1
            fcc     ">"
lnk_GT      fdb     prev_link
cfa_GT      fdb     code_GT
prev_link   set     nfa_GT
code_GT:    ldd     2,u             ; D = a
            cmpd    ,u              ; compare a to b
            bgt     gt_true
            leau    2,u
            ldd     #0
            std     ,u
            jmp     NEXT
gt_true:    leau    2,u
            ldd     #-1
            std     ,u
            jmp     NEXT

            ; <> ( a b -- flag )  not-equal
nfa_NE      fcb     2
            fcc     "<>"
lnk_NE      fdb     prev_link
cfa_NE      fdb     code_NE
prev_link   set     nfa_NE
code_NE:    ldd     ,u++            ; D = b; advance U (drops b)
            cmpd    ,u              ; compare b to a (equality is symmetric)
            bne     ne_true
            ldd     #0
            std     ,u
            jmp     NEXT
ne_true:    ldd     #-1
            std     ,u
            jmp     NEXT

; ---------------------------------------------------------------------------
; Memory access primitives
; ---------------------------------------------------------------------------
            ; @ ( addr -- value )
nfa_FETCH   fcb     1
            fcc     "@"
lnk_FETCH   fdb     prev_link
cfa_FETCH   fdb     code_FETCH
prev_link   set     nfa_FETCH
code_FETCH: ldy     ,u
            ldd     ,y
            std     ,u
            jmp     NEXT

            ; ! ( value addr -- )
nfa_STORE   fcb     1
            fcc     "!"
lnk_STORE   fdb     prev_link
cfa_STORE   fdb     code_STORE
prev_link   set     nfa_STORE
code_STORE: ldy     ,u++            ; addr
            ldd     ,u++            ; value
            std     ,y
            jmp     NEXT

            ; C@ ( addr -- byte )
nfa_CFETCH  fcb     2
            fcc     "C@"
lnk_CFETCH  fdb     prev_link
cfa_CFETCH  fdb     code_CFETCH
prev_link   set     nfa_CFETCH
code_CFETCH:
            ldy     ,u
            clra
            ldb     ,y
            std     ,u
            jmp     NEXT

            ; C! ( byte addr -- )
nfa_CSTORE  fcb     2
            fcc     "C!"
lnk_CSTORE  fdb     prev_link
cfa_CSTORE  fdb     code_CSTORE
prev_link   set     nfa_CSTORE
code_CSTORE:
            ldy     ,u++            ; addr
            ldd     ,u++            ; value (hi byte ignored)
            stb     ,y
            jmp     NEXT

; ---------------------------------------------------------------------------
; I/O primitives — EMIT / KEY / ?KEY
; ---------------------------------------------------------------------------
            ; EMIT ( char -- )
nfa_EMIT    fcb     4
            fcc     "EMIT"
lnk_EMIT    fdb     prev_link
cfa_EMIT    fdb     code_EMIT
prev_link   set     nfa_EMIT
code_EMIT:  ldd     ,u++            ; D = char (byte is in B)
            pshs    b               ; preserve char across ACIA poll
emit_wait:  ldb     ACIA_SR
            bitb    #$02
            beq     emit_wait
            puls    b               ; restore char
            stb     ACIA_DATA
            jmp     NEXT

            ; KEY ( -- char )  blocking read from ACIA
nfa_KEY     fcb     3
            fcc     "KEY"
lnk_KEY     fdb     prev_link
cfa_KEY     fdb     code_KEY
prev_link   set     nfa_KEY
code_KEY:
key_wait:   ldb     ACIA_SR
            bitb    #$01
            beq     key_wait
            clra
            ldb     ACIA_DATA
            std     ,--u
            jmp     NEXT

; ---------------------------------------------------------------------------
; HERE / , / ALLOT  (dictionary growth)
; ---------------------------------------------------------------------------
            ; HERE ( -- addr )
nfa_HERE    fcb     4
            fcc     "HERE"
lnk_HERE    fdb     prev_link
cfa_HERE    fdb     code_HERE
prev_link   set     nfa_HERE
code_HERE:  ldd     var_HERE
            std     ,--u
            jmp     NEXT

            ; , ( n -- )  — compile one cell at HERE, HERE += 2
nfa_COMMA   fcb     1
            fcc     ","
lnk_COMMA   fdb     prev_link
cfa_COMMA   fdb     code_COMMA
prev_link   set     nfa_COMMA
code_COMMA: ldy     var_HERE
            ldd     ,u++
            std     ,y++
            sty     var_HERE
            jmp     NEXT

            ; C, ( b -- )  — compile one byte at HERE, HERE += 1
nfa_CCOMMA  fcb     2
            fcc     "C,"
lnk_CCOMMA  fdb     prev_link
cfa_CCOMMA  fdb     code_CCOMMA
prev_link   set     nfa_CCOMMA
code_CCOMMA:
            ldy     var_HERE
            ldd     ,u++
            stb     ,y+
            sty     var_HERE
            jmp     NEXT

            ; ALLOT ( n -- )  — advance HERE by n bytes
nfa_ALLOT   fcb     5
            fcc     "ALLOT"
lnk_ALLOT   fdb     prev_link
cfa_ALLOT   fdb     code_ALLOT
prev_link   set     nfa_ALLOT
code_ALLOT: ldd     ,u++
            addd    var_HERE
            std     var_HERE
            jmp     NEXT

; ---------------------------------------------------------------------------
; STATE / LATEST access (as variables)
; ---------------------------------------------------------------------------
            ; STATE ( -- addr )
nfa_STATE   fcb     5
            fcc     "STATE"
lnk_STATE   fdb     prev_link
cfa_STATE   fdb     DOVAR
pfa_STATE   fdb     0                       ; storage; 0=interpret, 1=compile
prev_link   set     nfa_STATE

            ; LATEST ( -- addr )
nfa_LATEST  fcb     6
            fcc     "LATEST"
lnk_LATEST  fdb     prev_link
cfa_LATEST  fdb     DOVAR
pfa_LATEST  fdb     0
prev_link   set     nfa_LATEST

; var_HERE, var_STATE, var_LATEST — convenience aliases for kernel code
var_HERE    equ     here_var
var_STATE   equ     pfa_STATE
var_LATEST  equ     pfa_LATEST

here_var    fdb     DICT_START              ; separate 16-bit RAM cell

; ---------------------------------------------------------------------------
; Additional I/O primitives
; ---------------------------------------------------------------------------
            ; CR ( -- )  emit CR LF
nfa_CR      fcb     2
            fcc     "CR"
lnk_CR      fdb     prev_link
cfa_CR      fdb     code_CR
prev_link   set     nfa_CR
code_CR:    lda     #$0D
            lbsr    emit_a
            lda     #$0A
            lbsr    emit_a
            jmp     NEXT

            ; SPACE ( -- )
nfa_SPACE   fcb     5
            fcc     "SPACE"
lnk_SPACE   fdb     prev_link
cfa_SPACE   fdb     code_SPACE
prev_link   set     nfa_SPACE
code_SPACE: lda     #' '
            lbsr    emit_a
            jmp     NEXT

; Native helper: emit A as one character (preserves A, uses B).
emit_a:     pshs    a
ea_wait:    ldb     ACIA_SR
            bitb    #$02
            beq     ea_wait
            puls    a
            sta     ACIA_DATA
            rts


            ; TYPE ( addr len -- )
nfa_TYPE    fcb     4
            fcc     "TYPE"
lnk_TYPE    fdb     prev_link
cfa_TYPE    fdb     code_TYPE
prev_link   set     nfa_TYPE
code_TYPE:  ldd     ,u++            ; len
            ldx     ,u++            ; addr
            tstb                    ; zero-length?
            beq     type_done
            ; D = len (only low byte used for now)
            pshs    b
type_loop:  lda     ,x+
            lbsr    emit_a
            dec     ,s
            bne     type_loop
            leas    1,s
type_done:  jmp     NEXT

            ; COUNT ( caddr -- addr len )  counted string → pair
nfa_COUNT   fcb     5
            fcc     "COUNT"
lnk_COUNT   fdb     prev_link
cfa_COUNT   fdb     code_COUNT
prev_link   set     nfa_COUNT
code_COUNT: ldx     ,u              ; caddr
            clra
            ldb     ,x+             ; len byte, X = addr+1
            stx     ,u              ; replace with addr (past length)
            std     ,--u            ; push length
            jmp     NEXT

            ; . ( n -- )  print signed decimal
nfa_DOT     fcb     1
            fcc     "."
lnk_DOT     fdb     prev_link
cfa_DOT     fdb     code_DOT
prev_link   set     nfa_DOT
code_DOT:   ldd     ,u++
            bsr     print_dec
            lda     #' '
            lbsr    emit_a
            jmp     NEXT

; Native helper: print D as signed decimal (no trailing space).
; Strategy: repeated div-by-10, push each digit into a small buffer, then
; emit most-significant-digit first.  Handles -32768..32767 and zero.
print_dec:
            cmpd    #0
            bge     pd_nonneg
            pshs    d
            lda     #'-'
            lbsr    emit_a
            ldd     ,s++
            coma
            comb
            addd    #1              ; D = |value| (unsigned)
pd_nonneg:
            ldy     #pd_buf_end     ; Y = write pointer, grows downward
pd_div_loop:
            pshs    x               ; preserve X (callers may rely on it)
            ldx     #0              ; X = quotient accumulator
pd_sub10:   cmpd    #10
            blo     pd_sub10_done
            subd    #10
            leax    1,x
            bra     pd_sub10
pd_sub10_done:
            ; D < 10, so B holds the remainder digit (A is guaranteed zero
            ; once the dividend has been reduced below 256 on some pass;
            ; for smaller dividends A was already zero on entry).
            addb    #'0'
            leay    -1,y
            stb     ,y              ; store digit at front of buffer
            tfr     x,d             ; D = quotient
            puls    x
            cmpd    #0
            bne     pd_div_loop     ; continue while quotient nonzero
            ; Emit digits from Y up to pd_buf_end-1 (MSB first).
pd_emit:    cmpy    #pd_buf_end
            bhs     pd_emit_done
            lda     ,y+
            lbsr    emit_a
            bra     pd_emit
pd_emit_done:
            rts

pd_buf      fcb     0,0,0,0,0,0     ; up to "-32768" fits in 6 bytes (sign emitted separately)
pd_buf_end

; ---------------------------------------------------------------------------
; divmod_core — signed 16/16 → quotient+remainder, used by /, MOD, /MOD.
; Entry:  2,U = dividend (a), 0,U = divisor (b)
; Exit:   U stack is rewritten as ( rem quot ):   2,U=rem, 0,U=quot
;         (a.k.a. -2-slot net push — same cell count as the ( a b ) input)
;         U itself is NOT adjusted; callers either return as-is (/MOD) or
;         pop one extra cell and overwrite (/, MOD).
; Sign rule: truncation toward zero. Quotient sign = sign(a) XOR sign(b).
;            Remainder sign follows the dividend (C99 / ANS Forth SM/REM).
; Divide-by-zero: leaves rem=a, quot=0 on stack (no trap).
; ---------------------------------------------------------------------------
            ; Scratch. Not DP-based — standard 16-bit addressing.
dm_quot     fdb     0               ; quotient accumulator (also shifts dividend in)
dm_dvsr     fdb     0               ; absolute divisor
dm_signq    fcb     0               ; bit7 = quotient sign  (1 = negate at end)
dm_signr    fcb     0               ; bit7 = remainder sign (1 = negate at end)

divmod_core:
            ldd     ,u              ; D = b
            beq     dm_div_by_zero

            ; --- take absolute values of a and b, track signs --------------
            clr     dm_signq
            clr     dm_signr
            ldd     2,u             ; D = a
            bpl     dm_a_pos
            coma
            comb
            addd    #1
            std     2,u             ; replace a with |a|
            com     dm_signq        ; quotient sign: flip
            com     dm_signr        ; remainder sign: follows dividend
dm_a_pos:
            ldd     ,u              ; D = b
            bpl     dm_b_pos
            coma
            comb
            addd    #1              ; D = |b|
            com     dm_signq        ; quotient sign: flip again (XOR)
dm_b_pos:
            std     dm_dvsr

            ; --- unsigned 16/16 divide via restoring shift-subtract --------
            ; [rem:quot] starts as [0:|a|]. 16 iterations: shift left, if the
            ; 16-bit rem is >= dvsr, subtract and set bit 0 of quot.
            ldd     2,u             ; |a|
            std     dm_quot         ; quot slot starts as dividend
            ldd     #0              ; D = remainder
            ldx     #16             ; loop counter
dm_loop:
            ; Shift quot left 1, carry-out into LSB of remainder.
            ; We do it on the in-memory quot so we can use ROL directly.
            lsl     dm_quot+1
            rol     dm_quot
            rolb                    ; remainder <<= 1, bit shifted in from quot
            rola
            ; Trial subtract: if rem >= dvsr, commit subtract and set quot bit 0.
            cmpd    dm_dvsr
            blo     dm_no_sub
            subd    dm_dvsr
            inc     dm_quot+1       ; set new quotient LSB (previous LSL left 0)
dm_no_sub:
            leax    -1,x
            bne     dm_loop

            ; --- apply signs -----------------------------------------------
            tst     dm_signr
            bpl     dm_rem_ok
            coma
            comb
            addd    #1              ; negate remainder
dm_rem_ok:
            std     2,u             ; remainder -> slot a  (so NOS = rem)

            ldd     dm_quot
            tst     dm_signq
            bpl     dm_quot_ok
            coma
            comb
            addd    #1              ; negate quotient
dm_quot_ok:
            std     ,u              ; quotient -> slot b  (so TOS = quot)
            rts

dm_div_by_zero:
            ; Leave dividend as remainder, 0 as quotient. Caller picks.
            ldd     2,u             ; D = a (unchanged)
            std     2,u             ; rem = a  (NOS)
            clra
            clrb
            std     ,u              ; quot = 0 (TOS)
            rts

; ---------------------------------------------------------------------------
; Outer-interpreter state + kernel helpers.
; ---------------------------------------------------------------------------
var_TOIN    fdb     0               ; >IN — offset into TIB of next unread byte
var_TIBLEN  fdb     0               ; #TIB — bytes currently valid in TIB

; --------- parse_name_kernel -----------------------------------------------
; Skip leading blanks (chars <= $20) in TIB, then scan to the next blank or
; end of buffer.  Updates var_TOIN.
; Exit:  X = token addr (inside TIB)
;        D = token length (0 when TIB is exhausted; A is always 0)
parse_name_kernel:
            ldd     var_TIBLEN
            addd    #TIB_ADDR
            std     pn_end          ; pn_end = TIB + TIBLEN
            ldx     #TIB_ADDR
            ldd     var_TOIN
            leax    d,x             ; X = TIB + >IN
pn_skip:    cmpx    pn_end
            bhs     pn_empty
            lda     ,x
            cmpa    #' '
            bhi     pn_start
            leax    1,x
            bra     pn_skip
pn_empty:   tfr     x,d
            subd    #TIB_ADDR
            std     var_TOIN
            ldx     #TIB_ADDR
            clra
            clrb
            rts
pn_start:   stx     pn_start_addr   ; save token start
pn_scan:    cmpx    pn_end
            bhs     pn_scan_done
            lda     ,x
            cmpa    #' '
            bls     pn_scan_done
            leax    1,x
            bra     pn_scan
pn_scan_done:
            tfr     x,d
            subd    #TIB_ADDR
            std     var_TOIN        ; >IN advanced past the token
            tfr     x,d
            subd    pn_start_addr   ; D = end - start
            ldx     pn_start_addr   ; X = token start
            rts

pn_end        fdb   0
pn_start_addr fdb   0

; --------- sfind_kernel -----------------------------------------------------
; Search the dictionary for (X = name addr, B = name length, A irrelevant).
; Exit:  CC_Z=1 → not found (X=0, B=0).
;        CC_Z=0 → X = CFA address, B = 1 (normal) or 2 (IMMEDIATE).
sfind_kernel:
            stx     sf_target
            stb     sf_tlen
            ldx     var_LATEST
sf_iter:    cmpx    #0
            beq     sf_fail
            lda     ,x              ; A = flag/len byte
            bita    #F_HIDDEN
            bne     sf_advance      ; hidden — skip entry
            sta     sf_flags        ; stash flag for IMMED test below
            anda    #F_LENMASK
            cmpa    sf_tlen
            bne     sf_advance      ; length mismatch
            tsta
            beq     sf_hit          ; zero-length match
            stx     sf_cur
            leay    1,x             ; Y = entry name
            ldx     sf_target       ; X = target addr
            ldb     sf_tlen
sf_cmpbytes:
            lda     ,y+
            cmpa    ,x+
            bne     sf_miss
            decb
            bne     sf_cmpbytes
            ldx     sf_cur          ; restore NFA pointer
sf_hit:     ldb     ,x
            andb    #F_LENMASK
            addb    #3              ; CFA offset = flag(1) + name(len) + link(2)
            abx                     ; X = CFA address
            lda     sf_flags
            bita    #F_IMMED
            beq     sf_normal
            ldb     #2              ; B=2 → CC.Z=0 since B is nonzero
            rts
sf_normal:  ldb     #1              ; B=1 → CC.Z=0
            rts
sf_miss:    ldx     sf_cur
sf_advance: lda     ,x
            anda    #F_LENMASK
            tfr     a,b
            addb    #1              ; link offset = flag(1) + name(len)
            abx                     ; X = link field address
            ldx     ,x              ; X = next NFA (or 0)
            bra     sf_iter
sf_fail:    ldx     #0
            clrb                    ; B=0 → CC.Z=1 (and N=0)
            rts

sf_target   fdb     0
sf_cur      fdb     0
sf_tlen     fcb     0
sf_flags    fcb     0

; --------- number_kernel ----------------------------------------------------
; Parse (X = addr, D = len, only B used) as a signed decimal.  Accepts one
; optional leading '-'.
; Exit:  CC_Z=1 → parse failure.  CC_Z=0 → Y = value.
number_kernel:
            tstb
            beq     num_fail
            clr     num_sign
            ldy     #0
            lda     ,x
            cmpa    #'-'
            bne     num_digit
            inc     num_sign
            leax    1,x
            decb
            beq     num_fail
num_digit:  lda     ,x+
            suba    #'0'
            bmi     num_fail
            cmpa    #9
            bhi     num_fail
            pshs    b
            pshs    a
            lbsr    mul10_y
            puls    a
            leay    a,y
            puls    b
            decb
            bne     num_digit
            tst     num_sign
            beq     num_ok_pos
            tfr     y,d
            coma
            comb
            addd    #1
            tfr     d,y
num_ok_pos: andcc   #$FB            ; clear Z
            rts
num_fail:   clra
            clrb
            orcc    #$04            ; set Z
            rts

num_sign    fcb     0

; Y = Y * 10, clobbers D.
mul10_y:    tfr     y,d
            aslb
            rola                    ; D = Y*2
            pshs    d
            aslb
            rola
            aslb
            rola                    ; D = Y*8
            addd    ,s++            ; D = Y*8 + Y*2
            tfr     d,y
            rts

; ---------------------------------------------------------------------------
; Outer-interpreter primitives
; ---------------------------------------------------------------------------
            ; ACCEPT ( c-addr +n1 -- +n2 )  blocking line input, echoes chars
nfa_ACCEPT  fcb     6
            fcc     "ACCEPT"
lnk_ACCEPT  fdb     prev_link
cfa_ACCEPT  fdb     code_ACCEPT
prev_link   set     nfa_ACCEPT
code_ACCEPT:
            pshs    x               ; save IP — X is repurposed as buf ptr below
            ldd     ,u++            ; D = maxlen (low byte used)
            pshs    d
            ldx     ,u              ; X = buffer addr
            ldy     #0              ; Y = count
acc_rx:     ldb     ACIA_SR
            bitb    #$01
            beq     acc_rx
            ldb     ACIA_DATA
            cmpb    #$0D
            beq     acc_done
            cmpb    #$0A
            beq     acc_done
            cmpb    #$08
            beq     acc_bs
            cmpb    #$7F
            beq     acc_bs
            cmpy    ,s
            bhs     acc_rx          ; buffer full → drop char
            stb     ,x+
            leay    1,y
            tfr     b,a
            lbsr    emit_a
            bra     acc_rx
acc_bs:     cmpy    #0
            beq     acc_rx
            leax    -1,x
            leay    -1,y
            lda     #$08
            lbsr    emit_a
            lda     #' '
            lbsr    emit_a
            lda     #$08
            lbsr    emit_a
            bra     acc_rx
acc_done:   lda     #$0D
            lbsr    emit_a
            lda     #$0A
            lbsr    emit_a
            puls    d               ; discard saved maxlen (D clobbered next anyway)
            tfr     y,d
            std     ,u              ; replace buffer addr with count
            puls    x               ; restore IP
            jmp     NEXT

            ; PARSE-NAME ( -- c-addr u )
nfa_PARSEN  fcb     10
            fcc     "PARSE-NAME"
lnk_PARSEN  fdb     prev_link
cfa_PARSEN  fdb     code_PARSEN
prev_link   set     nfa_PARSEN
code_PARSEN:
            pshs    x               ; save IP
            lbsr    parse_name_kernel
            pshs    d               ; save len
            tfr     x,d
            std     ,--u            ; push c-addr
            puls    d
            std     ,--u            ; push u
            puls    x               ; restore IP
            jmp     NEXT

            ; SFIND ( c-addr u -- xt flag )   flag 0=notfound, 1=normal, 2=immediate
nfa_SFIND   fcb     5
            fcc     "SFIND"
lnk_SFIND   fdb     prev_link
cfa_SFIND   fdb     code_SFIND
prev_link   set     nfa_SFIND
code_SFIND:
            pshs    x               ; save IP
            ldd     ,u              ; D = u
            ldx     2,u             ; X = c-addr
            lbsr    sfind_kernel
            beq     sfind_miss
            stx     2,u             ; xt in place of c-addr
            clra
            std     ,u              ; flag = B (1 or 2)
            puls    x               ; restore IP
            jmp     NEXT
sfind_miss: ldx     #0
            stx     2,u
            stx     ,u
            puls    x               ; restore IP
            jmp     NEXT

            ; NUMBER? ( c-addr u -- value flag )   flag: 0=bad, -1=ok
nfa_NUMQ    fcb     7
            fcc     "NUMBER?"
lnk_NUMQ    fdb     prev_link
cfa_NUMQ    fdb     code_NUMQ
prev_link   set     nfa_NUMQ
code_NUMQ:  pshs    x               ; save IP
            ldd     ,u
            ldx     2,u
            lbsr    number_kernel
            beq     num_bad
            sty     2,u
            ldd     #-1
            std     ,u
            puls    x               ; restore IP
            jmp     NEXT
num_bad:    ldd     #0
            std     2,u
            std     ,u
            puls    x               ; restore IP
            jmp     NEXT

; ---------------------------------------------------------------------------
; Forth-visible variables: >IN, #TIB — each word pushes the address of its
; kernel storage cell so the standard `@`/`!` primitives work on them.
; ---------------------------------------------------------------------------
nfa_TOIN    fcb     3
            fcc     ">IN"
lnk_TOIN    fdb     prev_link
cfa_TOIN    fdb     code_TOIN
prev_link   set     nfa_TOIN
code_TOIN:  ldd     #var_TOIN
            std     ,--u
            jmp     NEXT

nfa_TIBL    fcb     4
            fcc     "#TIB"
lnk_TIBL    fdb     prev_link
cfa_TIBL    fdb     code_TIBL
prev_link   set     nfa_TIBL
code_TIBL:  ldd     #var_TIBLEN
            std     ,--u
            jmp     NEXT

; ---------------------------------------------------------------------------
; INTERPRET — native.  Parses tokens from TIB until exhausted.  On unknown
; words prints "<word>? CR/LF" and continues.  Respects STATE:
;   STATE@==0 → execute xt (or push number).
;   STATE@!=0 → compile xt (or compile LIT+number); IMMEDIATE always executes.
; ---------------------------------------------------------------------------
nfa_INTERP  fcb     9
            fcc     "INTERPRET"
lnk_INTERP  fdb     prev_link
cfa_INTERP  fdb     code_INTERPRET
prev_link   set     nfa_INTERP
code_INTERPRET:
            stx     int_saved_ip    ; preserve caller's IP
int_top:    lbsr    parse_name_kernel
            cmpd    #0
            lbeq    int_done
            stx     int_tok_addr
            std     int_tok_len
            lbsr    sfind_kernel
            beq     int_try_number
            cmpb    #2
            beq     int_exec
            ldd     var_STATE
            cmpd    #0
            beq     int_exec
            ; Compile mode: write xt at HERE, HERE += 2
            ldy     var_HERE
            stx     ,y++
            sty     var_HERE
            lbra    int_top
int_exec:   stx     int_frame
            ldx     #int_frame
            jmp     NEXT
int_try_number:
            ldx     int_tok_addr
            ldd     int_tok_len
            lbsr    number_kernel
            beq     int_undefined
            ; Y = parsed value
            ldd     var_STATE
            cmpd    #0
            beq     int_push_val
            ; Compile mode: emit LIT <value>
            ldx     var_HERE
            ldd     #cfa_LIT
            std     ,x++
            sty     ,x++
            stx     var_HERE
            lbra    int_top
int_push_val:
            sty     ,--u
            lbra    int_top
int_undefined:
            ; Print "<tok> ?" then CR LF, then continue.
            ldx     int_tok_addr
            ldb     int_tok_len+1
int_undef_lp:
            tstb
            beq     int_undef_q
            lda     ,x+
            pshs    b
            lbsr    emit_a
            puls    b
            decb
            bra     int_undef_lp
int_undef_q:
            lda     #'?'
            lbsr    emit_a
            lda     #$0D
            lbsr    emit_a
            lda     #$0A
            lbsr    emit_a
            lbra    int_top
int_done:   ldx     int_saved_ip
            jmp     NEXT

; Trampoline: after EXECUTE finishes, NEXT lands on cfa_INTRET and its code
; jumps back into int_top to parse the next token.
cfa_INTRET  fdb     code_INTRET
code_INTRET:
            lbra    int_top

int_frame   fdb     0               ; slot 0: xt to execute
            fdb     cfa_INTRET      ; slot 1: post-exec hook (fixed)
int_saved_ip fdb    0
int_tok_addr fdb    0
int_tok_len  fdb    0

; ---------------------------------------------------------------------------
; Colon compiler: `:` reads a name, creates a header, enters compile mode.
; `;` is IMMEDIATE — compiles EXIT and leaves compile mode.
; ---------------------------------------------------------------------------
            ; `:` ( -- )  parses next name and builds a new colon-def header
nfa_COLON   fcb     1
            fcc     ":"
lnk_COLON   fdb     prev_link
cfa_COLON   fdb     code_COLON
prev_link   set     nfa_COLON
code_COLON:
            pshs    x               ; save IP
            lbsr    parse_name_kernel
            cmpd    #0
            lbeq    col_done        ; no name given → no-op
            stx     col_name_addr
            stb     col_name_len
            ldy     var_HERE
            sty     col_new_nfa
            stb     ,y+             ; flag/len byte (no IMMED/HIDDEN)
            ldx     col_name_addr
col_copy:   lda     ,x+
            sta     ,y+
            decb
            bne     col_copy
            ldx     var_LATEST      ; link = old LATEST
            stx     ,y++
            ldx     #DOCOL          ; CFA = DOCOL
            stx     ,y++
            sty     var_HERE
            ldx     col_new_nfa
            stx     var_LATEST
            ldd     #1
            std     var_STATE
col_done:   puls    x
            jmp     NEXT

col_name_addr fdb 0
col_name_len  fcb 0
col_new_nfa   fdb 0

            ; `;` IMMEDIATE ( -- )  compiles EXIT, leaves compile mode
nfa_SEMI    fcb     F_IMMED|1
            fcc     ";"
lnk_SEMI    fdb     prev_link
cfa_SEMI    fdb     code_SEMI
prev_link   set     nfa_SEMI
code_SEMI:
            pshs    x
            ldy     var_HERE
            ldx     #cfa_EXIT
            stx     ,y++
            sty     var_HERE
            ldx     #0
            stx     var_STATE
            puls    x
            jmp     NEXT

; ---------------------------------------------------------------------------
; Control structures — all IMMEDIATE.  Use placeholder offsets that get
; patched when the matching THEN / UNTIL / AGAIN is seen.
; ---------------------------------------------------------------------------
            ; IF ( -- addr )  IMMEDIATE
nfa_IF      fcb     F_IMMED|2
            fcc     "IF"
lnk_IF      fdb     prev_link
cfa_IF      fdb     code_IF
prev_link   set     nfa_IF
code_IF:
            pshs    x
            ldy     var_HERE
            ldx     #cfa_0BR
            stx     ,y++            ; compile (0BRANCH)
            sty     ctrl_tmp        ; save offset-cell address
            ldx     #0
            stx     ,y++            ; placeholder offset
            sty     var_HERE
            ldx     ctrl_tmp
            stx     ,--u            ; push offset-cell addr
            puls    x
            jmp     NEXT

            ; THEN ( addr -- )  IMMEDIATE
nfa_THEN    fcb     F_IMMED|4
            fcc     "THEN"
lnk_THEN    fdb     prev_link
cfa_THEN    fdb     code_THEN
prev_link   set     nfa_THEN
code_THEN:
            pshs    x
            ldx     ,u++            ; X = placeholder addr
            ldy     var_HERE
            ; Offset = HERE - X
            tfr     y,d
            pshs    x               ; save X
            subd    ,s++            ; D = Y - X
            std     ,x              ; patch placeholder
            puls    x
            jmp     NEXT

            ; ELSE ( addr1 -- addr2 )  IMMEDIATE
nfa_ELSE    fcb     F_IMMED|4
            fcc     "ELSE"
lnk_ELSE    fdb     prev_link
cfa_ELSE    fdb     code_ELSE
prev_link   set     nfa_ELSE
code_ELSE:
            pshs    x
            ldx     ,u++            ; X = addr1 (IF's placeholder)
            stx     ctrl_tmp        ; save addr1
            ldy     var_HERE
            ldx     #cfa_BR
            stx     ,y++            ; compile (BRANCH)
            sty     ctrl_tmp2       ; save addr2 (our placeholder)
            ldx     #0
            stx     ,y++            ; placeholder
            sty     var_HERE
            ; Patch addr1: offset = HERE - addr1
            ldx     ctrl_tmp
            tfr     y,d
            pshs    x
            subd    ,s++
            std     ,x
            ; Push addr2
            ldx     ctrl_tmp2
            stx     ,--u
            puls    x
            jmp     NEXT

            ; BEGIN ( -- addr )  IMMEDIATE
nfa_BEGIN   fcb     F_IMMED|5
            fcc     "BEGIN"
lnk_BEGIN   fdb     prev_link
cfa_BEGIN   fdb     code_BEGIN
prev_link   set     nfa_BEGIN
code_BEGIN:
            pshs    x
            ldd     var_HERE
            std     ,--u
            puls    x
            jmp     NEXT

            ; UNTIL ( addr -- )  IMMEDIATE
nfa_UNTIL   fcb     F_IMMED|5
            fcc     "UNTIL"
lnk_UNTIL   fdb     prev_link
cfa_UNTIL   fdb     code_UNTIL
prev_link   set     nfa_UNTIL
code_UNTIL:
            pshs    x
            ldd     ,u++            ; B0 (BEGIN addr)
            std     ctrl_tmp
            ldy     var_HERE
            ldx     #cfa_0BR
            stx     ,y++            ; compile (0BRANCH); Y = offset-cell addr
            ; Offset = B0 - Y
            tfr     y,d
            pshs    d               ; [S] = Y
            ldd     ctrl_tmp        ; D = B0
            subd    ,s++            ; D = B0 - Y
            std     ,y++            ; write offset
            sty     var_HERE
            puls    x
            jmp     NEXT

            ; AGAIN ( addr -- )  IMMEDIATE
nfa_AGAIN   fcb     F_IMMED|5
            fcc     "AGAIN"
lnk_AGAIN   fdb     prev_link
cfa_AGAIN   fdb     code_AGAIN
prev_link   set     nfa_AGAIN
code_AGAIN:
            pshs    x
            ldd     ,u++
            std     ctrl_tmp
            ldy     var_HERE
            ldx     #cfa_BR
            stx     ,y++
            tfr     y,d
            pshs    d
            ldd     ctrl_tmp
            subd    ,s++
            std     ,y++
            sty     var_HERE
            puls    x
            jmp     NEXT

ctrl_tmp    fdb     0
ctrl_tmp2   fdb     0

; ---------------------------------------------------------------------------
; VARIABLE / CONSTANT / ."  / (  — defining words and string/comment helpers
; ---------------------------------------------------------------------------
            ; VARIABLE name ( -- )
nfa_VAR     fcb     8
            fcc     "VARIABLE"
lnk_VAR     fdb     prev_link
cfa_VAR     fdb     code_VAR
prev_link   set     nfa_VAR
code_VAR:
            pshs    x
            lbsr    parse_name_kernel
            cmpd    #0
            lbeq    var_done
            stx     col_name_addr
            stb     col_name_len
            ldy     var_HERE
            sty     col_new_nfa
            stb     ,y+
            ldx     col_name_addr
var_copy:   lda     ,x+
            sta     ,y+
            decb
            bne     var_copy
            ldx     var_LATEST
            stx     ,y++
            ldx     #DOVAR
            stx     ,y++
            ldx     #0
            stx     ,y++            ; initial storage = 0
            sty     var_HERE
            ldx     col_new_nfa
            stx     var_LATEST
var_done:   puls    x
            jmp     NEXT

            ; CONSTANT name ( x -- )
nfa_CONST   fcb     8
            fcc     "CONSTANT"
lnk_CONST   fdb     prev_link
cfa_CONST   fdb     code_CONST
prev_link   set     nfa_CONST
code_CONST:
            pshs    x
            lbsr    parse_name_kernel
            cmpd    #0
            lbeq    const_done
            stx     col_name_addr
            stb     col_name_len
            ldy     var_HERE
            sty     col_new_nfa
            stb     ,y+
            ldx     col_name_addr
const_copy: lda     ,x+
            sta     ,y+
            decb
            bne     const_copy
            ldx     var_LATEST
            stx     ,y++
            ldx     #DOCON
            stx     ,y++
            ldd     ,u++            ; pop value
            std     ,y++
            sty     var_HERE
            ldx     col_new_nfa
            stx     var_LATEST
const_done: puls    x
            jmp     NEXT

            ; (LITSTR)  — inline counted-string literal primitive
            ; When the thread reaches this CFA, X (IP) points to the length
            ; byte; the chars follow.  Emit the chars, then advance X past
            ; the string so NEXT resumes with the following cell.
nfa_LITSTR  fcb     8
            fcc     "(LITSTR)"
lnk_LITSTR  fdb     prev_link
cfa_LITSTR  fdb     code_LITSTR
prev_link   set     nfa_LITSTR
code_LITSTR:
            ldb     ,x+             ; len, X advances to string start
            tstb
            beq     ls_done
ls_loop:    lda     ,x+
            pshs    b
            lbsr    emit_a
            puls    b
            decb
            bne     ls_loop
ls_done:    jmp     NEXT

            ; ."  IMMEDIATE — parse until '"', compile (LITSTR)+len+chars
nfa_DOTQ    fcb     F_IMMED|2
            fcb     $2E,$22         ; ." (period + quote)
lnk_DOTQ    fdb     prev_link
cfa_DOTQ    fdb     code_DOTQ
prev_link   set     nfa_DOTQ
code_DOTQ:
            pshs    x
            ldx     #TIB_ADDR
            ldd     var_TOIN
            leax    d,x             ; X = TIB + >IN
            lda     ,x
            cmpa    #' '
            bne     dq_have_start
            leax    1,x             ; skip single leading space
dq_have_start:
            stx     dq_start_addr
            ldy     #TIB_ADDR
            ldd     var_TIBLEN
            leay    d,y
            sty     dq_end
dq_scan:    cmpx    dq_end
            bhs     dq_no_quote
            lda     ,x
            cmpa    #'"'
            beq     dq_got_quote
            leax    1,x
            bra     dq_scan
dq_got_quote:
            ; X is at '"'.  Length = X - start.  Advance X past the '"'.
            tfr     x,d
            subd    dq_start_addr
            std     dq_len
            leax    1,x
            bra     dq_update_toin
dq_no_quote:
            ; No closing quote — length is X - start, leave X as-is.
            tfr     x,d
            subd    dq_start_addr
            std     dq_len
dq_update_toin:
            tfr     x,d
            subd    #TIB_ADDR
            std     var_TOIN
            ; Compile (LITSTR)
            ldy     var_HERE
            ldx     #cfa_LITSTR
            stx     ,y++
            ldb     dq_len+1
            stb     ,y+             ; len byte
            tstb
            beq     dq_compile_done
            ldx     dq_start_addr
dq_copy:    lda     ,x+
            sta     ,y+
            decb
            bne     dq_copy
dq_compile_done:
            sty     var_HERE
            puls    x
            jmp     NEXT

dq_start_addr fdb   0
dq_end        fdb   0
dq_len        fdb   0

            ; (  IMMEDIATE — skip TIB chars until ')'
nfa_PAREN   fcb     F_IMMED|1
            fcc     "("
lnk_PAREN   fdb     prev_link
cfa_PAREN   fdb     code_PAREN
prev_link   set     nfa_PAREN
code_PAREN:
            pshs    x
            ldx     #TIB_ADDR
            ldd     var_TOIN
            leax    d,x
            ldy     #TIB_ADDR
            ldd     var_TIBLEN
            leay    d,y
            sty     ctrl_tmp
par_scan:   cmpx    ctrl_tmp
            bhs     par_done
            lda     ,x+
            cmpa    #')'
            bne     par_scan
par_done:   tfr     x,d
            subd    #TIB_ADDR
            std     var_TOIN
            puls    x
            jmp     NEXT

; ---------------------------------------------------------------------------
; QUIT — REPL loop: ACCEPT into TIB, INTERPRET, print " ok", CR, repeat.
; ---------------------------------------------------------------------------
nfa_QUIT    fcb     4
            fcc     "QUIT"
lnk_QUIT    fdb     prev_link
cfa_QUIT    fdb     DOCOL
pfa_QUIT:
            fdb     cfa_LIT
            fdb     TIB_ADDR
            fdb     cfa_LIT
            fdb     TIB_SIZE
            fdb     cfa_ACCEPT                  ; ( -- len )
            fdb     cfa_LIT
            fdb     var_TIBLEN
            fdb     cfa_STORE                   ; len → #TIB
            fdb     cfa_LIT
            fdb     0
            fdb     cfa_LIT
            fdb     var_TOIN
            fdb     cfa_STORE                   ; 0   → >IN
            fdb     cfa_INTERP
            fdb     cfa_LIT
            fdb     $20                         ; ' '
            fdb     cfa_EMIT
            fdb     cfa_LIT
            fdb     'o'
            fdb     cfa_EMIT
            fdb     cfa_LIT
            fdb     'k'
            fdb     cfa_EMIT
            fdb     cfa_CR
            fdb     cfa_BR
quit_br_tgt:
            fdb     (pfa_QUIT)-(quit_br_tgt)
prev_link   set     nfa_QUIT

last_builtin_link equ prev_link


; ---------------------------------------------------------------------------
; Reset vector
; ---------------------------------------------------------------------------
            org     $FFFE
            fdb     cold

            end     cold
