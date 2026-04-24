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
DICT_START  equ     $2800       ; user dictionary grows from here
                                ; (kernel code now exceeds $2000; headroom up to ~$9FFF)

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
            ; Seed HERE / LATEST / STATE and sync FORTH's vocab cell.
            ldx     #DICT_START
            stx     var_HERE
            ldx     #last_builtin_link
            stx     var_LATEST           ; live cache
            stx     pfa_FORTH_LATEST     ; FORTH vocab's persistent cell
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

; DOCREATE: code field for CREATE-d words.  Body layout:
;   CFA+0  → DOCREATE (here)
;   CFA+2  → DOES_ADDR (0 if no DOES>, else address of runtime code)
;   CFA+4  → PFA (user data from ALLOT)
; If DOES_ADDR == 0, push PFA and continue.
; Otherwise push PFA, save caller's IP, and enter the runtime code.
DOCREATE:
            ldd     2,y             ; D = DOES_ADDR
            beq     docr_no_does
            ; Has DOES> runtime.  Save IP, replace with DOES_ADDR, push PFA.
            pshs    x               ; save caller's IP on the return stack
            tfr     d,x             ; X = DOES_ADDR (new IP)
            leay    4,y             ; Y = PFA
            sty     ,--u
            jmp     NEXT
docr_no_does:
            leay    4,y             ; Y = PFA
            sty     ,--u
            jmp     NEXT

; DOMARKER: code field for MARKER-created words.  PFA layout:
;   PFA+0 : saved LATEST (2 bytes)
;   PFA+2 : saved HERE   (2 bytes)
; Executing the marker restores both, effectively forgetting everything
; defined after (and including) the marker itself.
DOMARKER:
            leay    2,y             ; Y = PFA
            ldd     ,y              ; saved LATEST
            std     var_LATEST
            ldd     2,y             ; saved HERE
            std     var_HERE
            jmp     NEXT

; DOVOC: code field for VOCABULARY-created words.  PFA layout:
;   PFA+0 : latest NFA in this vocabulary (0 = empty)
;   PFA+2 : parent-vocab's PFA+0 address (0 = root)
; Executing a vocab switches the active search/define namespace to it.
; Before switching, var_LATEST (the cache) is flushed back to the OLD
; vocab's PFA+0; then the NEW vocab's PFA+0 is loaded into var_LATEST.
DOVOC:
            leay    2,y                  ; Y = new vocab's PFA+0
            ldx     var_LATEST_PTR       ; X = old vocab's PFA+0
            ldd     var_LATEST
            std     ,x                   ; flush cache to old vocab
            sty     var_LATEST_PTR
            ldd     ,y                   ; load new vocab's latest
            std     var_LATEST
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

            ; SP@ ( -- addr )  push the current data stack pointer.
            ; Standard-compliant: addr points to the current TOS cell,
            ; BEFORE SP@ pushes its own result.
nfa_SPFETCH fcb     3
            fcc     "SP@"
lnk_SPFETCH fdb     prev_link
cfa_SPFETCH fdb     code_SPFETCH
prev_link   set     nfa_SPFETCH
code_SPFETCH: tfr   u,d
            std     ,--u
            jmp     NEXT

            ; SP! ( addr -- )  set the data stack pointer.
            ; The caller is responsible for ensuring the new U is valid
            ; (coherent stack frame).
nfa_SPSTORE fcb     3
            fcc     "SP!"
lnk_SPSTORE fdb     prev_link
cfa_SPSTORE fdb     code_SPSTORE
prev_link   set     nfa_SPSTORE
code_SPSTORE: ldu   ,u
            jmp     NEXT

            ; RP@ ( -- addr )  push the current return stack pointer.
            ; addr points at the current TOP of the return stack.
nfa_RPFETCH fcb     3
            fcc     "RP@"
lnk_RPFETCH fdb     prev_link
cfa_RPFETCH fdb     code_RPFETCH
prev_link   set     nfa_RPFETCH
code_RPFETCH: tfr   s,d
            std     ,--u
            jmp     NEXT

            ; RP! ( addr -- )  set the return stack pointer.
            ; WARNING: changes S mid-flight.  Current code is designed to
            ; NOT need S until the next EXIT, which will then pull from the
            ; new location.  The caller must have arranged the new R-stack
            ; contents accordingly.
nfa_RPSTORE fcb     3
            fcc     "RP!"
lnk_RPSTORE fdb     prev_link
cfa_RPSTORE fdb     code_RPSTORE
prev_link   set     nfa_RPSTORE
code_RPSTORE: ldd   ,u++
            tfr     d,s
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

            ; ?DUP ( x -- x x | 0 )  DUP only if TOS non-zero
nfa_QDUP    fcb     4
            fcc     "?DUP"
lnk_QDUP    fdb     prev_link
cfa_QDUP    fdb     code_QDUP
prev_link   set     nfa_QDUP
code_QDUP:  ldd     ,u
            beq     qdup_zero
            std     ,--u
qdup_zero:  jmp     NEXT

            ; NIP ( a b -- b )  drop NOS
nfa_NIP     fcb     3
            fcc     "NIP"
lnk_NIP     fdb     prev_link
cfa_NIP     fdb     code_NIP
prev_link   set     nfa_NIP
code_NIP:   ldd     ,u              ; D = b (TOS)
            std     2,u             ; overwrite NOS slot
            leau    2,u             ; drop one cell
            jmp     NEXT

            ; TUCK ( a b -- b a b )
nfa_TUCK    fcb     4
            fcc     "TUCK"
lnk_TUCK    fdb     prev_link
cfa_TUCK    fdb     code_TUCK
prev_link   set     nfa_TUCK
code_TUCK:  ldd     ,u              ; D = b
            ldy     2,u             ; Y = a
            std     2,u             ; NOS := b
            pshs    d               ; save b on R so we can rebuild
            tfr     y,d             ; D = a
            std     ,u              ; TOS slot := a
            puls    d               ; D = b
            std     ,--u            ; push b
            jmp     NEXT

            ; PICK ( xn ... x1 x0 n -- xn ... x1 x0 xn )
            ; n=0 → DUP, n=1 → OVER, etc. Assumes 0 ≤ n < 128.
nfa_PICK    fcb     4
            fcc     "PICK"
lnk_PICK    fdb     prev_link
cfa_PICK    fdb     code_PICK
prev_link   set     nfa_PICK
code_PICK:  ldd     ,u              ; D = n
            aslb
            rola                    ; D = n*2
            addd    #2              ; skip the n cell itself: offset = 2 + 2n
            leay    d,u             ; Y = addr of xn
            ldd     ,y
            std     ,u              ; replace n with xn
            jmp     NEXT

            ; -ROT ( a b c -- c a b )  inverse of ROT
nfa_MROT    fcb     4
            fcc     "-ROT"
lnk_MROT    fdb     prev_link
cfa_MROT    fdb     code_MROT
prev_link   set     nfa_MROT
code_MROT:  ldd     ,u              ; c
            ldy     2,u             ; b
            sty     ,u
            ldy     4,u             ; a
            sty     2,u
            std     4,u
            jmp     NEXT

            ; ROLL ( xn ... x0 n -- xn-1 ... x0 xn )
            ; Rotates the nth cell (counting TOS as 0) to the top.
            ; n=0 no-op, n=1 = SWAP, n=2 = ROT, etc.
nfa_ROLL    fcb     4
            fcc     "ROLL"
lnk_ROLL    fdb     prev_link
cfa_ROLL    fdb     code_ROLL
prev_link   set     nfa_ROLL
code_ROLL:  ldd     ,u++            ; D = n; drop the count cell
            beq     roll_done       ; n = 0 → nothing to do
            aslb
            rola                    ; D = n*2 (byte offset)
            leay    d,u             ; Y = addr of xn
            ldd     ,y              ; D = xn (value we'll move to top)
            pshs    d               ; stash xn on R
            stu     roll_u          ; snapshot U for boundary check
roll_shift: cmpy    roll_u
            beq     roll_place
            leay    -2,y
            ldd     ,y
            std     2,y             ; shift the cell up by one
            bra     roll_shift
roll_place: puls    d
            std     ,u              ; new TOS = xn
roll_done:  jmp     NEXT

roll_u      fdb     0

            ; DEPTH ( -- n )  number of cells currently on the data stack.
nfa_DEPTH   fcb     5
            fcc     "DEPTH"
lnk_DEPTH   fdb     prev_link
cfa_DEPTH   fdb     code_DEPTH
prev_link   set     nfa_DEPTH
code_DEPTH: stu     depth_u
            ldd     #PSP_TOP
            subd    depth_u          ; D = (PSP_TOP - U) = depth * 2
            lsra
            rorb
            std     ,--u
            jmp     NEXT

depth_u     fdb     0

            ; 2DUP ( a b -- a b a b )
nfa_2DUP    fcb     4
            fcc     "2DUP"
lnk_2DUP    fdb     prev_link
cfa_2DUP    fdb     code_2DUP
prev_link   set     nfa_2DUP
code_2DUP:  ldd     2,u             ; D = a
            ldy     ,u              ; Y = b
            leau    -4,u            ; make room for two copies
            std     2,u             ; new NOS = a
            sty     ,u              ; new TOS = b
            jmp     NEXT

            ; 2DROP ( a b -- )
nfa_2DROP   fcb     5
            fcc     "2DROP"
lnk_2DROP   fdb     prev_link
cfa_2DROP   fdb     code_2DROP
prev_link   set     nfa_2DROP
code_2DROP: leau    4,u
            jmp     NEXT

            ; 2SWAP ( a b c d -- c d a b )
nfa_2SWAP   fcb     5
            fcc     "2SWAP"
lnk_2SWAP   fdb     prev_link
cfa_2SWAP   fdb     code_2SWAP
prev_link   set     nfa_2SWAP
code_2SWAP: ldd     6,u             ; D = a
            ldy     2,u             ; Y = c
            sty     6,u
            std     2,u
            ldd     4,u             ; D = b
            ldy     ,u              ; Y = d
            sty     4,u
            std     ,u
            jmp     NEXT

            ; 2OVER ( a b c d -- a b c d a b )
nfa_2OVER   fcb     5
            fcc     "2OVER"
lnk_2OVER   fdb     prev_link
cfa_2OVER   fdb     code_2OVER
prev_link   set     nfa_2OVER
code_2OVER: ldd     6,u             ; D = a
            ldy     4,u             ; Y = b
            leau    -4,u
            std     2,u             ; copy a → new NOS
            sty     ,u              ; copy b → new TOS
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

            ; UM* ( u1 u2 -- ud )  unsigned 16×16 → 32.
            ; Result layout on stack: NOS = low16, TOS = high16.
nfa_UMSTAR  fcb     3
            fcc     "UM*"
lnk_UMSTAR  fdb     prev_link
cfa_UMSTAR  fdb     code_UMSTAR
prev_link   set     nfa_UMSTAR
code_UMSTAR:
            clr     um_res
            clr     um_res+1
            ; p1 = u1_lo * u2_lo → result[2:3]
            lda     3,u
            ldb     1,u
            mul
            std     um_res+2
            ; p2 = u1_lo * u2_hi → add to result[1:2]
            lda     3,u
            ldb     ,u
            mul
            addd    um_res+1
            std     um_res+1
            bcc     ums_p2_done
            inc     um_res
ums_p2_done:
            ; p3 = u1_hi * u2_lo → add to result[1:2]
            lda     2,u
            ldb     1,u
            mul
            addd    um_res+1
            std     um_res+1
            bcc     ums_p3_done
            inc     um_res
ums_p3_done:
            ; p4 = u1_hi * u2_hi → add to result[0:1]
            lda     2,u
            ldb     ,u
            mul
            addd    um_res
            std     um_res
            ; Overwrite stack: NOS = low, TOS = high
            ldd     um_res+2
            std     2,u
            ldd     um_res
            std     ,u
            jmp     NEXT

um_res      fcb     0,0,0,0

            ; M* ( n1 n2 -- d )  signed 16×16 → 32, truncate-to-zero convention.
nfa_MSTAR   fcb     2
            fcc     "M*"
lnk_MSTAR   fdb     prev_link
cfa_MSTAR   fdb     code_MSTAR
prev_link   set     nfa_MSTAR
code_MSTAR: clr     m_sign
            ldd     2,u
            bpl     m_a_pos
            coma
            comb
            addd    #1
            std     2,u
            com     m_sign
m_a_pos:    ldd     ,u
            bpl     m_b_pos
            coma
            comb
            addd    #1
            std     ,u
            com     m_sign
m_b_pos:
            ; Now operands are positive — call UM* (inline).
            lbsr    um_star_body
            ; Apply sign to the 32-bit result on the stack.
            tst     m_sign
            bpl     m_done
            ; negate (NOS=low, TOS=high) — pre-invert hi to preserve ADDD's C.
            ldd     ,u
            coma
            comb
            std     ,u
            ldd     2,u
            coma
            comb
            addd    #1
            std     2,u
            ldd     ,u
            adcb    #0
            adca    #0
            std     ,u
m_done:     jmp     NEXT

m_sign      fcb     0

; Inline UM* for callers (M*, */) — leaves ( d_lo d_hi ) on U stack.
um_star_body:
            pshs    x                    ; preserve IP
            clr     um_res
            clr     um_res+1
            lda     3,u
            ldb     1,u
            mul
            std     um_res+2
            lda     3,u
            ldb     ,u
            mul
            addd    um_res+1
            std     um_res+1
            bcc     usb_p2
            inc     um_res
usb_p2:     lda     2,u
            ldb     1,u
            mul
            addd    um_res+1
            std     um_res+1
            bcc     usb_p3
            inc     um_res
usb_p3:     lda     2,u
            ldb     ,u
            mul
            addd    um_res
            std     um_res
            ldd     um_res+2
            std     2,u
            ldd     um_res
            std     ,u
            puls    x,pc

            ; UM/MOD ( ud u -- urem uquot )  unsigned 32/16 → 16rem, 16quot.
            ; Dividend ud is ( lo hi ) with hi TOS-below-divisor.
            ; Stack on entry:  4,u=lo  2,u=hi  0,u=divisor
            ;       on exit:   2,u=rem 0,u=quot (so net -1 cell)
nfa_UMSLASH fcb     6
            fcc     "UM/MOD"
lnk_UMSLASH fdb     prev_link
cfa_UMSLASH fdb     code_UMSLASH
prev_link   set     nfa_UMSLASH
code_UMSLASH:
            pshs    x                    ; save IP
            ldd     ,u                   ; D = divisor
            beq     um_div_by_zero
            std     um_dvsr
            ldd     2,u                  ; D = hi (becomes remainder)
            ldy     4,u                  ; Y = lo (becomes quotient-in-progress)
            sty     um_qacc
            ldx     #16
um_loop:
            ; Shift [D:um_qacc] left 1 using classic asl/rol chain.
            lsl     um_qacc+1
            rol     um_qacc
            rolb
            rola
            cmpd    um_dvsr
            blo     um_no_sub
            subd    um_dvsr
            inc     um_qacc+1            ; bit 0 of quotient = 1
um_no_sub:
            leax    -1,x
            bne     um_loop
            ; D = remainder, um_qacc = quotient
            std     2,u                  ; NOS (below the divisor-slot) := rem
            ldd     um_qacc
            std     ,u                   ; replace divisor slot with — wait, quot should be TOS
            ; Actually we want ( rem quot ) with quot on TOS. Current stack is
            ; 4,u=lo (unused now), 2,u=stored rem, 0,u=divisor (old).
            ; Overwrite 4,u with divisor slot via leau, and 0,u with quot:
            ldd     2,u                  ; rem
            std     4,u                  ; move rem to the new-NOS slot
            ldd     um_qacc
            std     2,u                  ; quot below
            leau    2,u                  ; drop the now-unused high slot
            ; After the above: new 0,u was old 2,u (quot), new 2,u was old 4,u (rem)
            ; Wait, leau 2,u means U += 2 (moving up, popping). So after:
            ;   new 0,u = old 2,u (=quot)
            ;   new 2,u = old 4,u (=rem)
            ; That gives ( rem quot )  ✓
            puls    x
            jmp     NEXT
um_div_by_zero:
            ; Fail-soft: leave ( 0 0 ) — rem=0, quot=0
            ldd     #0
            std     2,u
            std     4,u
            leau    2,u                  ; same pop as success path
            puls    x
            jmp     NEXT

um_dvsr     fdb     0
um_qacc     fdb     0

            ; M+ ( d n -- d' )  add single n to double d.  Sign-extends n.
nfa_MPLUS   fcb     2
            fcc     "M+"
lnk_MPLUS   fdb     prev_link
cfa_MPLUS   fdb     code_MPLUS
prev_link   set     nfa_MPLUS
code_MPLUS: ldd     ,u++                 ; D = n (pop)
            pshs    d                    ; save n low
            ; Sign-extend n into a 16-bit high word on R:
            bpl     mp_pos
            ldd     #-1
            bra     mp_push
mp_pos:     ldd     #0
mp_push:    pshs    d                    ; R: (hi, lo)  top = hi
            ; Now stack: 2,u = d_lo, ,u = d_hi  (d_hi is TOS)
            ; Add: d_lo += lo, d_hi += hi + C
            ldd     2,u                  ; d_lo
            addd    2,s                  ; + n_lo (at 2,s: pushed first)
            std     2,u
            ldd     ,u                   ; d_hi
            adcb    1,s                  ; + n_hi low byte + C
            adca    ,s                   ; + n_hi high byte
            std     ,u
            leas    4,s                  ; drop n_lo + n_hi
            jmp     NEXT

            ; D+ ( d1 d2 -- d )
nfa_DPLUS   fcb     2
            fcc     "D+"
lnk_DPLUS   fdb     prev_link
cfa_DPLUS   fdb     code_DPLUS
prev_link   set     nfa_DPLUS
code_DPLUS: ldd     2,u                  ; D = d2_lo
            addd    6,u                  ; D += d1_lo (sets C)
            std     6,u
            ldd     ,u                   ; D = d2_hi
            adcb    5,u
            adca    4,u
            std     4,u
            leau    4,u                  ; drop d2 (2 cells)
            jmp     NEXT

            ; D- ( d1 d2 -- d )
nfa_DMINUS  fcb     2
            fcc     "D-"
lnk_DMINUS  fdb     prev_link
cfa_DMINUS  fdb     code_DMINUS
prev_link   set     nfa_DMINUS
code_DMINUS: ldd    6,u                  ; D = d1_lo
            subd    2,u                  ; D -= d2_lo (sets C as borrow)
            std     6,u
            ldd     4,u                  ; D = d1_hi
            sbcb    1,u
            sbca    ,u
            std     4,u
            leau    4,u
            jmp     NEXT

            ; DNEGATE ( d -- -d )
nfa_DNEG    fcb     7
            fcc     "DNEGATE"
lnk_DNEG    fdb     prev_link
cfa_DNEG    fdb     code_DNEG
prev_link   set     nfa_DNEG
            ; Note: COM sets C=1, so we must pre-invert the high word
            ; and then propagate the carry from the low-word ADDD into the
            ; already-inverted high word via ADCB/ADCA.
code_DNEG:  ldd     ,u                   ; pre-invert high
            coma
            comb
            std     ,u
            ldd     2,u                  ; d_lo
            coma
            comb
            addd    #1                   ; C = 1 iff lo was 0
            std     2,u
            ldd     ,u                   ; reload ~hi (ADDD's C survives LDD)
            adcb    #0
            adca    #0
            std     ,u
            jmp     NEXT

            ; DABS ( d -- |d| )
nfa_DABS    fcb     4
            fcc     "DABS"
lnk_DABS    fdb     prev_link
cfa_DABS    fdb     code_DABS
prev_link   set     nfa_DABS
code_DABS:  lda     ,u                   ; high byte of d_hi
            bpl     dabs_done
            ; Pre-invert high (COM sets C=1; we need ADDD's carry instead).
            ldd     ,u
            coma
            comb
            std     ,u
            ldd     2,u
            coma
            comb
            addd    #1
            std     2,u
            ldd     ,u
            adcb    #0
            adca    #0
            std     ,u
dabs_done:  jmp     NEXT

            ; D. ( d -- )  print a double in the current BASE, trailing space.
            ; Handles full range ±2,147,483,647.
nfa_DDOT    fcb     2
            fcc     "D."
lnk_DDOT    fdb     prev_link
cfa_DDOT    fdb     code_DDOT
prev_link   set     nfa_DDOT
code_DDOT:  pshs    x
            ; Handle sign: if d is negative, negate and remember to emit '-'.
            ; (Pre-invert the high word — COM sets C=1, which would corrupt
            ; the carry chain if done after ADDD on the low word.)
            clr     d_dot_sign
            lda     ,u
            bpl     ddot_pos
            com     d_dot_sign
            ldd     ,u
            coma
            comb
            std     ,u
            ldd     2,u
            coma
            comb
            addd    #1
            std     2,u
            ldd     ,u
            adcb    #0
            adca    #0
            std     ,u
ddot_pos:
            ; Write digits right-justified in pd_buf using repeated UM/MOD by BASE.
            ldy     #pd_buf_end
ddot_loop:
            ; divide ud by BASE (single-cell divisor).
            ; Use the UM/MOD algorithm in-place: dividend at 2,u (low) / ,u (hi).
            ; Simpler custom version since divisor is small:
            ldd     #0                   ; D = remainder (starts 0)
            pshs    d                    ; remainder on R
            ldd     ,u                   ; hi
            pshs    d                    ; dividend-hi on R
            ldd     2,u                  ; lo
            pshs    d                    ; dividend-lo on R
            ldx     #32                  ; 32 iterations for full 32-bit shift
ddot_shift_loop:
            ; shift [rem:hi:lo] left 1
            lsl     1,s                  ; lo.lo
            rol     ,s                   ; lo.hi
            rol     3,s                  ; hi.lo
            rol     2,s                  ; hi.hi
            rol     5,s                  ; rem.lo
            rol     4,s                  ; rem.hi
            ; if rem >= BASE, subtract and set bit 0 of lo
            ldd     4,s
            cmpd    var_BASE
            blo     ddot_no_sub
            subd    var_BASE
            std     4,s
            inc     1,s                  ; set new LSB of lo (previous lsl left 0)
ddot_no_sub:
            leax    -1,x
            bne     ddot_shift_loop
            ; Pop: lo = quotient_lo, hi = quotient_hi, rem = digit
            puls    d                    ; D = quotient_lo
            std     2,u
            puls    d                    ; D = quotient_hi
            std     ,u
            puls    d                    ; D = remainder (digit 0..BASE-1)
            ; Convert digit to ASCII
            cmpb    #10
            blo     ddot_num
            addb    #'A'-'0'-10
ddot_num:   addb    #'0'
            leay    -1,y
            stb     ,y
            ; Loop while quotient != 0
            ldd     ,u
            bne     ddot_loop
            ldd     2,u
            bne     ddot_loop
            ; Drop the double from the stack (2 cells)
            leau    4,u
            ; Emit sign if negative
            tst     d_dot_sign
            beq     ddot_nosign
            lda     #'-'
            lbsr    emit_a
ddot_nosign:
            ; Emit digits from Y to pd_buf_end
            lbsr    emit_range
            lda     #' '
            lbsr    emit_a
            puls    x
            jmp     NEXT

d_dot_sign  fcb     0

            ; D.R ( d w -- )  print signed double right-justified in w chars.
            ; No trailing space.  Implemented using pictured-numeric-output
            ; primitives so we don't need an extra fmt buffer.
nfa_DDOTR   fcb     3
            fcc     "D.R"
lnk_DDOTR   fdb     prev_link
cfa_DDOTR   fdb     code_DDOTR
prev_link   set     nfa_DDOTR
code_DDOTR: pshs    x
            ldd     ,u++                 ; D = w
            std     fmt_width
            ; Check sign of double, remember it, take absolute value.
            clr     d_dot_sign
            lda     ,u                   ; hi byte
            bpl     ddr_abs_done
            com     d_dot_sign
            ; negate in place
            ldd     ,u
            coma
            comb
            std     ,u
            ldd     2,u
            coma
            comb
            addd    #1
            std     2,u
            ldd     ,u
            adcb    #0
            adca    #0
            std     ,u
ddr_abs_done:
            ldd     #pd_buf_end
            std     hld_ptr
ddr_loop:   lbsr    sharp_body           ; extract one digit, leaves xd updated
            ldd     ,u
            bne     ddr_loop
            ldd     2,u
            bne     ddr_loop
            leau    4,u                  ; drop the double
            ; Prepend sign if needed
            tst     d_dot_sign
            beq     ddr_no_sign
            ldy     hld_ptr
            leay    -1,y
            sty     hld_ptr
            lda     #'-'
            sta     ,y
ddr_no_sign:
            ; Emit padding: width - length spaces, then digits.
            ldd     #pd_buf_end
            subd    hld_ptr              ; D = length
            pshs    d
            ldd     fmt_width
            subd    ,s++                 ; D = width - length
            ble     ddr_emit
            tfr     d,y
ddr_pad:    lda     #' '
            lbsr    emit_a
            leay    -1,y
            bne     ddr_pad
ddr_emit:   ldy     hld_ptr
            ldx     #pd_buf_end
ddr_eloop:  cmpy    #pd_buf_end
            bhs     ddr_done
            lda     ,y+
            lbsr    emit_a
            bra     ddr_eloop
ddr_done:   puls    x
            jmp     NEXT

            ; */ ( n1 n2 n3 -- n1*n2/n3 )  intermediate 32-bit product.
nfa_STARSL  fcb     2
            fcc     "*/"
lnk_STARSL  fdb     prev_link
cfa_STARSL  fdb     code_STARSL
prev_link   set     nfa_STARSL
code_STARSL: pshs   x
            ; Save divisor, do M* to leave ( d_lo d_hi ), then UM/MOD.
            ldd     ,u++                 ; D = n3 (divisor), drop it
            std     ss_dvsr
            ; Now stack: 2,u=n1, 0,u=n2
            ; Do M* — inline from code_MSTAR (duplicate logic for simplicity)
            clr     m_sign
            ldd     2,u
            bpl     ss_a_pos
            coma
            comb
            addd    #1
            std     2,u
            com     m_sign
ss_a_pos:   ldd     ,u
            bpl     ss_b_pos
            coma
            comb
            addd    #1
            std     ,u
            com     m_sign
ss_b_pos:
            lbsr    um_star_body         ; stack: ( d_lo d_hi )
            ; Push divisor (unsigned abs) for UM/MOD
            ldd     ss_dvsr
            bpl     ss_d_pos
            coma
            comb
            addd    #1
            com     m_sign               ; toggle quotient sign
ss_d_pos:   std     ,--u
            ; UM/MOD inline (call the body)
            lbsr    umslash_body         ; stack after: ( urem uquot )
            ; Drop remainder, apply sign to quotient
            ldd     ,u                   ; D = quot
            leau    2,u                  ; drop rem
            tst     m_sign
            bpl     ss_done
            coma
            comb
            addd    #1
ss_done:    std     ,u
            puls    x
            jmp     NEXT

ss_dvsr     fdb     0

; umslash_body: same as code_UMSLASH but reusable as a subroutine without
; going through NEXT.
umslash_body:
            pshs    x
            ldd     ,u
            beq     usb_div_zero
            std     um_dvsr
            ldd     2,u
            ldy     4,u
            sty     um_qacc
            ldx     #16
usb_loop:   lsl     um_qacc+1
            rol     um_qacc
            rolb
            rola
            cmpd    um_dvsr
            blo     usb_no_sub
            subd    um_dvsr
            inc     um_qacc+1
usb_no_sub: leax    -1,x
            bne     usb_loop
            ; At loop exit: D = remainder, um_qacc = quotient.
            ; Stack: 4,u=lo (unused), 2,u=hi (unused), 0,u=divisor (unused).
            ; Target: new 0,u=quot, new 2,u=rem (one cell popped from 3).
            std     4,u                  ; place rem where it will land after leau 2,u
            ldd     um_qacc
            std     2,u                  ; place quot where it will be new TOS
            leau    2,u                  ; drop the divisor slot
            puls    x,pc
usb_div_zero:
            ldd     #0
            std     4,u
            std     2,u
            leau    2,u
            puls    x,pc

            ; */MOD ( n1 n2 n3 -- rem quot )  like */ but also leaves remainder.
nfa_STARSLMOD fcb   5
            fcc     "*/MOD"
lnk_STARSLMOD fdb   prev_link
cfa_STARSLMOD fdb   code_STARSLMOD
prev_link     set   nfa_STARSLMOD
code_STARSLMOD:
            pshs    x
            ldd     ,u++
            std     ss_dvsr
            clr     m_sign
            ldd     2,u
            bpl     ssm_a_pos
            coma
            comb
            addd    #1
            std     2,u
            com     m_sign
ssm_a_pos:  ldd     ,u
            bpl     ssm_b_pos
            coma
            comb
            addd    #1
            std     ,u
            com     m_sign
ssm_b_pos:
            lbsr    um_star_body
            ldd     ss_dvsr
            bpl     ssm_d_pos
            coma
            comb
            addd    #1
            com     m_sign
ssm_d_pos:  std     ,--u
            lbsr    umslash_body         ; ( rem quot )
            tst     m_sign
            bpl     ssm_done
            ldd     ,u
            coma
            comb
            addd    #1
            std     ,u
            ; Also negate remainder (matches ANS SM/REM: rem has dividend's sign)
            ldd     2,u
            coma
            comb
            addd    #1
            std     2,u
ssm_done:   puls    x
            jmp     NEXT

            ; SM/REM ( d n -- rem quot )  signed 32/16 division, symmetric.
            ; Remainder sign follows the dividend (truncation toward zero).
            ; Implemented by taking absolute values, calling UM/MOD, then
            ; applying signs.
nfa_SMREM   fcb     6
            fcc     "SM/REM"
lnk_SMREM   fdb     prev_link
cfa_SMREM   fdb     code_SMREM
prev_link   set     nfa_SMREM
code_SMREM:
            pshs    x
            ; Determine sign of divisor (TOS)
            clr     sm_signq
            clr     sm_signr
            ldd     ,u
            bpl     sm_d_pos
            coma
            comb
            addd    #1
            std     ,u
            com     sm_signq
sm_d_pos:   ; Sign of dividend (double at 4,u=lo, 2,u=hi)
            lda     2,u
            bpl     sm_dv_pos
            com     sm_signq
            com     sm_signr
            ; Negate the double (2,u=hi, 4,u=lo)
            ldd     2,u
            coma
            comb
            std     2,u
            ldd     4,u
            coma
            comb
            addd    #1
            std     4,u
            ldd     2,u
            adcb    #0
            adca    #0
            std     2,u
sm_dv_pos:  ; Call UM/MOD body
            lbsr    umslash_body          ; stack now ( urem uquot )
            ; Apply signs
            tst     sm_signq
            bpl     sm_quot_ok
            ldd     ,u
            coma
            comb
            addd    #1
            std     ,u
sm_quot_ok: tst     sm_signr
            bpl     sm_rem_ok
            ldd     2,u
            coma
            comb
            addd    #1
            std     2,u
sm_rem_ok:  puls    x
            jmp     NEXT

sm_signq    fcb     0
sm_signr    fcb     0

            ; FM/MOD ( d n -- rem quot )  signed 32/16 division, FLOORED.
            ; Remainder sign follows the DIVISOR.  Adjusts SM/REM's result
            ; by subtracting one from the quotient and adding divisor to
            ; the remainder when they have different signs.
nfa_FMMOD   fcb     6
            fcc     "FM/MOD"
lnk_FMMOD   fdb     prev_link
cfa_FMMOD   fdb     code_FMMOD
prev_link   set     nfa_FMMOD
code_FMMOD:
            pshs    x
            ldd     ,u
            std     fm_dvsr                ; remember original divisor
            lbsr    smrem_body             ; ( rem quot ) on stack, D clobbered
            ; If rem != 0 AND sign(rem) != sign(dvsr), adjust:
            ;   quot -= 1, rem += dvsr
            ldd     2,u                    ; rem
            beq     fm_done
            ldd     2,u                    ; rem again
            eora    fm_dvsr                ; high-byte XOR
            bpl     fm_done                ; same sign — no adjust
            ldd     ,u                     ; quot
            subd    #1
            std     ,u
            ldd     2,u                    ; rem
            addd    fm_dvsr
            std     2,u
fm_done:    puls    x
            jmp     NEXT

fm_dvsr     fdb     0

            ; M/ ( d n -- quot )  signed 32/16 floored division, single
            ; quotient (remainder dropped).  Equivalent to  FM/MOD NIP.
nfa_MSLASH  fcb     2
            fcc     "M/"
lnk_MSLASH  fdb     prev_link
cfa_MSLASH  fdb     code_MSLASH
prev_link   set     nfa_MSLASH
code_MSLASH:
            pshs    x
            ldd     ,u
            std     fm_dvsr
            lbsr    smrem_body             ; ( rem quot )
            ldd     2,u                    ; rem
            beq     ms_no_adj
            ldd     2,u
            eora    fm_dvsr
            bpl     ms_no_adj
            ldd     ,u
            subd    #1
            std     ,u
ms_no_adj:  ; Drop remainder — stack ( rem quot ) → ( quot ).
            ldd     ,u                     ; quot
            leau    2,u                    ; drop rem slot
            std     ,u
            puls    x
            jmp     NEXT

; smrem_body: same as code_SMREM but as a callable subroutine (no
; NEXT dispatch at the end).  Expects stack ( d n ); leaves ( rem quot ).
smrem_body:
            pshs    x
            clr     sm_signq
            clr     sm_signr
            ldd     ,u
            bpl     smb_d_pos
            coma
            comb
            addd    #1
            std     ,u
            com     sm_signq
smb_d_pos:  lda     2,u
            bpl     smb_dv_pos
            com     sm_signq
            com     sm_signr
            ldd     2,u
            coma
            comb
            std     2,u
            ldd     4,u
            coma
            comb
            addd    #1
            std     4,u
            ldd     2,u
            adcb    #0
            adca    #0
            std     2,u
smb_dv_pos: lbsr    umslash_body
            tst     sm_signq
            bpl     smb_quot_ok
            ldd     ,u
            coma
            comb
            addd    #1
            std     ,u
smb_quot_ok: tst    sm_signr
            bpl     smb_rem_ok
            ldd     2,u
            coma
            comb
            addd    #1
            std     2,u
smb_rem_ok: puls    x,pc

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

            ; LSHIFT ( x u -- x<<u )  logical shift left by u bits.
nfa_LSHIFT  fcb     6
            fcc     "LSHIFT"
lnk_LSHIFT  fdb     prev_link
cfa_LSHIFT  fdb     code_LSHIFT
prev_link   set     nfa_LSHIFT
code_LSHIFT: ldd    ,u++            ; D = shift count (only low byte used)
            cmpb    #16
            bhs     lsh_zero
lsh_loop:   tstb
            beq     lsh_done
            asl     1,u             ; shift x left 1
            rol     ,u
            decb
            bra     lsh_loop
lsh_zero:   ldd     #0
            std     ,u
lsh_done:   jmp     NEXT

            ; RSHIFT ( x u -- x>>u )  logical shift right by u bits (zero-fill).
nfa_RSHIFT  fcb     6
            fcc     "RSHIFT"
lnk_RSHIFT  fdb     prev_link
cfa_RSHIFT  fdb     code_RSHIFT
prev_link   set     nfa_RSHIFT
code_RSHIFT: ldd    ,u++
            cmpb    #16
            bhs     rsh_zero
rsh_loop:   tstb
            beq     rsh_done
            lsr     ,u              ; high byte: bit 0 → C
            ror     1,u
            decb
            bra     rsh_loop
rsh_zero:   ldd     #0
            std     ,u
rsh_done:   jmp     NEXT

            ; ALIGN ( -- )  align HERE to a cell boundary.
            ; On this kernel, cells are 2-byte aligned by default; if HERE
            ; is odd, bump it by 1.
nfa_ALIGN   fcb     5
            fcc     "ALIGN"
lnk_ALIGN   fdb     prev_link
cfa_ALIGN   fdb     code_ALIGN
prev_link   set     nfa_ALIGN
code_ALIGN: ldd     var_HERE
            bitb    #1
            beq     align_ok
            addd    #1
            std     var_HERE
align_ok:   jmp     NEXT

            ; ALIGNED ( addr -- addr' )  round addr up to a cell boundary.
nfa_ALIGNED fcb     7
            fcc     "ALIGNED"
lnk_ALIGNED fdb     prev_link
cfa_ALIGNED fdb     code_ALIGNED
prev_link   set     nfa_ALIGNED
code_ALIGNED:
            ldd     ,u
            bitb    #1
            beq     aligned_ok
            addd    #1
            std     ,u
aligned_ok: jmp     NEXT

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

            ; 0> ( a -- flag )  -1 if a > 0 (signed), else 0
nfa_ZGT     fcb     2
            fcc     "0>"
lnk_ZGT     fdb     prev_link
cfa_ZGT     fdb     code_ZGT
prev_link   set     nfa_ZGT
code_ZGT:   ldd     ,u
            beq     zgt_false           ; 0 → not > 0
            bmi     zgt_false           ; negative → not > 0
            ldd     #-1
            std     ,u
            jmp     NEXT
zgt_false:  ldd     #0
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

            ; TRUE ( -- -1 )  Forth true flag
nfa_TRUE    fcb     4
            fcc     "TRUE"
lnk_TRUE    fdb     prev_link
cfa_TRUE    fdb     code_TRUE
prev_link   set     nfa_TRUE
code_TRUE:  ldd     #-1
            std     ,--u
            jmp     NEXT

            ; FALSE ( -- 0 )
nfa_FALSE   fcb     5
            fcc     "FALSE"
lnk_FALSE   fdb     prev_link
cfa_FALSE   fdb     code_FALSE
prev_link   set     nfa_FALSE
code_FALSE: ldd     #0
            std     ,--u
            jmp     NEXT

            ; BL ( -- 32 )  ASCII space
nfa_BL      fcb     2
            fcc     "BL"
lnk_BL      fdb     prev_link
cfa_BL      fdb     code_BL
prev_link   set     nfa_BL
code_BL:    ldd     #32
            std     ,--u
            jmp     NEXT

            ; .S ( -- )  non-destructive print of the data stack (bottom…top)
            ; Format: "<depth> v_bottom ... v_top"  — each cell in current BASE.
nfa_DOTS    fcb     2
            fcc     ".S"
lnk_DOTS    fdb     prev_link
cfa_DOTS    fdb     code_DOTS
prev_link   set     nfa_DOTS
code_DOTS:  pshs    x                    ; save IP
            stu     dots_u               ; snapshot U for comparisons
            ; depth = (PSP_TOP - U) / 2
            ldd     #PSP_TOP
            subd    dots_u               ; D = depth*2
            lsra
            rorb                         ; D = depth
            lda     #'<'
            lbsr    emit_a
            lbsr    fmt_ud_to_buf
            lbsr    emit_range
            lda     #'>'
            lbsr    emit_a
            lda     #' '
            lbsr    emit_a
            ; Walk from deepest (PSP_TOP-2) down through U, emitting each cell.
            ldy     #PSP_TOP
ds_loop:    leay    -2,y
            cmpy    dots_u
            blo     ds_done              ; Y < U → past the bottom
            ldd     ,y
            pshs    y
            lbsr    fmt_sd_to_buf
            lbsr    emit_range
            lda     #' '
            lbsr    emit_a
            puls    y
            bra     ds_loop
ds_done:    puls    x
            jmp     NEXT

dots_u      fdb     0

            ; WORDS ( -- )  print all dictionary entries, newest first.
nfa_WORDS   fcb     5
            fcc     "WORDS"
lnk_WORDS   fdb     prev_link
cfa_WORDS   fdb     code_WORDS
prev_link   set     nfa_WORDS
code_WORDS: pshs    x
            ldy     var_LATEST
w_loop:     cmpy    #0
            beq     w_done
            lda     ,y
            bita    #F_HIDDEN
            bne     w_skip               ; skip hidden
            anda    #F_LENMASK
            tfr     a,b
            ; print name: Y+1 for B bytes
            leax    1,y
w_emit:     tstb
            beq     w_sp
            lda     ,x+
            pshs    b
            lbsr    emit_a
            puls    b
            decb
            bra     w_emit
w_sp:       lda     #' '
            lbsr    emit_a
w_skip:     lda     ,y
            anda    #F_LENMASK
            tfr     a,b
            leay    1,y
            leay    b,y                  ; past name
            ldy     ,y                   ; follow link
            bra     w_loop
w_done:     puls    x
            jmp     NEXT

            ; DUMP ( addr u -- )  hex dump, 16 bytes per line.
            ; Output: "<aaaa>: bb bb bb ... bb"  (no ASCII panel — kept small).
nfa_DUMP    fcb     4
            fcc     "DUMP"
lnk_DUMP    fdb     prev_link
cfa_DUMP    fdb     code_DUMP
prev_link   set     nfa_DUMP
code_DUMP:  pshs    x
            ldd     ,u++                 ; D = u (count)
            std     dump_count
            ldd     ,u++                 ; D = addr
            std     dump_addr
dump_line:  ldd     dump_count
            beq     dump_exit
            lda     #$0D
            lbsr    emit_a
            lda     #$0A
            lbsr    emit_a
            ldd     dump_addr
            lbsr    emit_hex_word
            lda     #':'
            lbsr    emit_a
            lda     #' '
            lbsr    emit_a
            ; bytes-this-row = min(16, count)
            ldd     dump_count
            cmpd    #16
            blo     dump_row_have
            ldd     #16
dump_row_have:
            stb     dump_row             ; B only — row ≤ 16
            ldx     dump_addr
dump_bytes: tst     dump_row
            beq     dump_line_next
            ldb     ,x+
            lbsr    emit_hex_byte
            lda     #' '
            lbsr    emit_a
            dec     dump_row
            bra     dump_bytes
dump_line_next:
            ; addr += 16, count -= 16 (but clamp count at 0)
            ldd     dump_addr
            addd    #16
            std     dump_addr
            ldd     dump_count
            subd    #16
            bpl     dump_count_ok        ; result ≥ 0 — continue
            ldd     #0
dump_count_ok:
            std     dump_count
            bra     dump_line
dump_exit:
            puls    x
            jmp     NEXT

dump_addr   fdb     0
dump_count  fdb     0
dump_row    fcb     0

; emit_hex_nibble: B low 4 bits → one ASCII hex char. Clobbers A.
emit_hex_nibble:
            andb    #$0F
            cmpb    #10
            blo     enib_num
            addb    #'A'-10-'0'
enib_num:   addb    #'0'
            tfr     b,a
            lbsr    emit_a
            rts

; emit_hex_byte: B → two ASCII hex chars (high nibble first).
emit_hex_byte:
            pshs    b
            lsrb
            lsrb
            lsrb
            lsrb
            lbsr    emit_hex_nibble
            puls    b
            lbsr    emit_hex_nibble
            rts

; emit_hex_word: D → four ASCII hex chars.
emit_hex_word:
            pshs    b
            tfr     a,b
            lbsr    emit_hex_byte
            puls    b
            lbsr    emit_hex_byte
            rts

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

            ; U< ( a b -- flag )  unsigned less-than
nfa_ULT     fcb     2
            fcc     "U<"
lnk_ULT     fdb     prev_link
cfa_ULT     fdb     code_ULT
prev_link   set     nfa_ULT
code_ULT:   ldd     2,u             ; D = a
            cmpd    ,u
            blo     ult_true
            leau    2,u
            ldd     #0
            std     ,u
            jmp     NEXT
ult_true:   leau    2,u
            ldd     #-1
            std     ,u
            jmp     NEXT

            ; U> ( a b -- flag )  unsigned greater-than
nfa_UGT     fcb     2
            fcc     "U>"
lnk_UGT     fdb     prev_link
cfa_UGT     fdb     code_UGT
prev_link   set     nfa_UGT
code_UGT:   ldd     2,u             ; D = a
            cmpd    ,u
            bhi     ugt_true
            leau    2,u
            ldd     #0
            std     ,u
            jmp     NEXT
ugt_true:   leau    2,u
            ldd     #-1
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

            ; +! ( n addr -- )  addr has `n` added in place
nfa_PSTORE  fcb     2
            fcc     "+!"
lnk_PSTORE  fdb     prev_link
cfa_PSTORE  fdb     code_PSTORE
prev_link   set     nfa_PSTORE
code_PSTORE: ldy    ,u++            ; Y = addr
            ldd     ,u++            ; D = n
            addd    ,y              ; D = *addr + n
            std     ,y
            jmp     NEXT

            ; CELL+ ( addr -- addr+2 )  advance by one 16-bit cell
nfa_CELLP   fcb     5
            fcc     "CELL+"
lnk_CELLP   fdb     prev_link
cfa_CELLP   fdb     code_CELLP
prev_link   set     nfa_CELLP
code_CELLP: ldd     ,u
            addd    #2
            std     ,u
            jmp     NEXT

            ; CELLS ( n -- n*2 )  convert cell count to byte count
nfa_CELLS   fcb     5
            fcc     "CELLS"
lnk_CELLS   fdb     prev_link
cfa_CELLS   fdb     code_CELLS
prev_link   set     nfa_CELLS
code_CELLS: asl     1,u
            rol     ,u
            jmp     NEXT

            ; CMOVE ( src dst u -- )  copy u bytes from src to dst (low→high)
nfa_CMOVE   fcb     5
            fcc     "CMOVE"
lnk_CMOVE   fdb     prev_link
cfa_CMOVE   fdb     code_CMOVE
prev_link   set     nfa_CMOVE
code_CMOVE: pshs    x               ; save IP (X)
            ldd     ,u++            ; D = u (count)
            beq     cmove_none
            std     cm_count
            ldx     ,u++            ; X = dst
            ldd     ,u++            ; D = src
            std     cm_src
cmove_loop: ldy     cm_src
            lda     ,y+
            sty     cm_src
            sta     ,x+
            ldd     cm_count
            subd    #1
            std     cm_count
            bne     cmove_loop
            puls    x
            jmp     NEXT
cmove_none: leau    4,u             ; count was 0 — drop dst and src
            puls    x
            jmp     NEXT

cm_src      fdb     0
cm_count    fdb     0

            ; CMOVE> ( src dst u -- )  copy u bytes high → low (safe for
            ; overlapping regions where dst > src).
nfa_CMOVEUP fcb     6
            fcc     "CMOVE>"
lnk_CMOVEUP fdb     prev_link
cfa_CMOVEUP fdb     code_CMOVEUP
prev_link   set     nfa_CMOVEUP
code_CMOVEUP:
            pshs    x               ; save IP
            ldd     ,u++            ; D = u (count)
            beq     cmu_none
            std     cm_count
            ldx     ,u++            ; X = dst
            ldy     ,u++            ; Y = src  (D still = count, ldx/ldy don't touch D)
            leax    d,x             ; X = dst + count
            leay    d,y             ; Y = src + count
cmu_loop:   lda     ,-y             ; pre-decrement, then read
            sta     ,-x             ; pre-decrement, then store
            ldd     cm_count
            subd    #1
            std     cm_count
            bne     cmu_loop
            puls    x
            jmp     NEXT
cmu_none:   leau    4,u
            puls    x
            jmp     NEXT

            ; MOVE ( src dst u -- )  non-destructive copy — dispatches to
            ; CMOVE (src ≥ dst) or CMOVE> (src < dst).
nfa_MOVE    fcb     4
            fcc     "MOVE"
lnk_MOVE    fdb     prev_link
cfa_MOVE    fdb     code_MOVE
prev_link   set     nfa_MOVE
code_MOVE:  ldd     4,u             ; D = src
            cmpd    2,u             ; flags from src − dst
            bhs     code_CMOVE      ; src ≥ dst → forward copy is safe
            jmp     code_CMOVEUP    ; else reverse copy

            ; COMPARE ( a1 u1 a2 u2 -- n )
            ; Lexicographic byte-level comparison. Returns 0 if equal,
            ; -1 if a1/u1 < a2/u2, +1 if a1/u1 > a2/u2.
nfa_COMPARE fcb     7
            fcc     "COMPARE"
lnk_COMPARE fdb     prev_link
cfa_COMPARE fdb     code_COMPARE
prev_link   set     nfa_COMPARE
code_COMPARE:
            pshs    x                ; save IP
            ldd     ,u++             ; u2 (TOS count 2)
            std     cmp_len2
            ldx     ,u++             ; a2
            stx     cmp_ptr2
            ldd     ,u++             ; u1
            std     cmp_len1
            ldx     ,u++             ; a1
            stx     cmp_ptr1
cmp_loop:   ldd     cmp_len1
            bne     cmp_have_a       ; a1 still has bytes
            ldd     cmp_len2
            beq     cmp_eq           ; both exhausted simultaneously → equal
            ldd     #-1              ; a1 shorter but prefix match → a1 < a2
            bra     cmp_push
cmp_have_a: ldd     cmp_len2
            bne     cmp_both
            ldd     #1               ; a2 shorter but prefix match → a1 > a2
            bra     cmp_push
cmp_both:   ldx     cmp_ptr1
            lda     ,x+
            stx     cmp_ptr1
            ldx     cmp_ptr2
            ldb     ,x+
            stx     cmp_ptr2
            pshs    b
            cmpa    ,s+
            beq     cmp_next
            bhi     cmp_gt
            ldd     #-1
            bra     cmp_push
cmp_gt:     ldd     #1
            bra     cmp_push
cmp_next:   ldd     cmp_len1
            subd    #1
            std     cmp_len1
            ldd     cmp_len2
            subd    #1
            std     cmp_len2
            bra     cmp_loop
cmp_eq:     ldd     #0
cmp_push:   std     ,--u
            puls    x
            jmp     NEXT

cmp_ptr1    fdb     0
cmp_ptr2    fdb     0
cmp_len1    fdb     0
cmp_len2    fdb     0

            ; /STRING ( a u n -- a+n u-n )  advance a string by n bytes.
nfa_SLASHSTR fcb    7
            fcc     "/STRING"
lnk_SLASHSTR fdb    prev_link
cfa_SLASHSTR fdb    code_SLASHSTR
prev_link    set    nfa_SLASHSTR
code_SLASHSTR:
            ldd     ,u++             ; n
            pshs    d                ; save n
            ldd     ,u               ; u (TOS after n popped)
            subd    ,s               ; u - n
            std     ,u
            ldd     2,u              ; a
            addd    ,s++             ; a + n
            std     2,u
            jmp     NEXT

            ; -TRAILING ( a u -- a u' )  strip trailing spaces from the
            ; string. Only adjusts the length; the address is unchanged.
nfa_MTRAIL  fcb     9
            fcc     "-TRAILING"
lnk_MTRAIL  fdb     prev_link
cfa_MTRAIL  fdb     code_MTRAIL
prev_link   set     nfa_MTRAIL
code_MTRAIL:
            pshs    x
            ldd     ,u               ; u = length (TOS)
            beq     mt_done
            ldx     2,u              ; X = addr
            leax    d,x              ; X = addr + u (one past last byte)
mt_loop:    ldd     ,u
            beq     mt_done          ; empty → stop
            lda     ,-x              ; look at last byte
            cmpa    #' '
            bhi     mt_done
            ldd     ,u
            subd    #1
            std     ,u
            bra     mt_loop
mt_done:    puls    x
            jmp     NEXT

            ; 2@ ( addr -- d )  fetch a double.  Memory layout: low at addr,
            ; high at addr+2.  Stack layout: ( low high ) with high on TOS.
nfa_2FETCH  fcb     2
            fcc     "2@"
lnk_2FETCH  fdb     prev_link
cfa_2FETCH  fdb     code_2FETCH
prev_link   set     nfa_2FETCH
code_2FETCH: ldy    ,u                   ; Y = addr
            ldd     2,y                  ; high cell
            pshs    d
            ldd     ,y                   ; low cell
            std     ,u                   ; replace addr with low (NOS)
            puls    d
            std     ,--u                 ; push high (TOS)
            jmp     NEXT

            ; 2! ( d addr -- )  d is ( low high ), high is TOS
nfa_2STORE  fcb     2
            fcc     "2!"
lnk_2STORE  fdb     prev_link
cfa_2STORE  fdb     code_2STORE
prev_link   set     nfa_2STORE
code_2STORE: ldy    ,u++                 ; Y = addr
            ldd     ,u++                 ; D = high
            std     2,y
            ldd     ,u++                 ; D = low
            std     ,y
            jmp     NEXT

            ; ERASE ( addr u -- )  fill u bytes with 0.
nfa_ERASE   fcb     5
            fcc     "ERASE"
lnk_ERASE   fdb     prev_link
cfa_ERASE   fdb     code_ERASE
prev_link   set     nfa_ERASE
code_ERASE: ldd     #0
            std     ,--u                 ; push 0 as byte arg
            jmp     code_FILL

            ; BLANK ( addr u -- )  fill u bytes with ASCII space.
nfa_BLANK   fcb     5
            fcc     "BLANK"
lnk_BLANK   fdb     prev_link
cfa_BLANK   fdb     code_BLANK
prev_link   set     nfa_BLANK
code_BLANK: ldd     #32
            std     ,--u
            jmp     code_FILL

            ; FILL ( addr u byte -- )  write `u` copies of `byte` starting at `addr`
nfa_FILL    fcb     4
            fcc     "FILL"
lnk_FILL    fdb     prev_link
cfa_FILL    fdb     code_FILL
prev_link   set     nfa_FILL
code_FILL:  pshs    x               ; save IP
            ldd     ,u++            ; D = byte (B has the value)
            pshs    b               ; save byte on R
            ldy     ,u++            ; Y = count (LDY sets Z)
            beq     fill_none
            ldx     ,u++            ; X = addr
            puls    b               ; B = byte (LEAY in loop preserves B)
fill_loop:  stb     ,x+
            leay    -1,y            ; LEAY sets Z when Y reaches 0
            bne     fill_loop
            puls    x
            jmp     NEXT
fill_none:  leas    1,s             ; drop saved byte
            leau    2,u             ; count was 0 — drop addr
            puls    x
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

            ; LATEST ( -- addr )  address of the ACTIVE vocabulary's
            ; latest-NFA cache cell.  In a multi-vocab world, that cell
            ; is always kept in sync with the active vocab's PFA+0 by
            ; DOVOC's flush/reload sequence.
nfa_LATEST  fcb     6
            fcc     "LATEST"
lnk_LATEST  fdb     prev_link
cfa_LATEST  fdb     DOVAR
pfa_LATEST  fdb     0          ; RAM cache; active vocab's current latest
prev_link   set     nfa_LATEST

            ; BASE ( -- addr )  current input/output radix (default 10)
nfa_BASE    fcb     4
            fcc     "BASE"
lnk_BASE    fdb     prev_link
cfa_BASE    fdb     DOVAR
pfa_BASE    fdb     10                      ; boot default: decimal
prev_link   set     nfa_BASE

            ; HEX ( -- )  set BASE to 16
nfa_HEX     fcb     3
            fcc     "HEX"
lnk_HEX     fdb     prev_link
cfa_HEX     fdb     code_HEX
prev_link   set     nfa_HEX
code_HEX:   ldd     #16
            std     pfa_BASE
            jmp     NEXT

            ; DECIMAL ( -- )  set BASE to 10
nfa_DECIMAL fcb     7
            fcc     "DECIMAL"
lnk_DECIMAL fdb     prev_link
cfa_DECIMAL fdb     code_DECIMAL
prev_link   set     nfa_DECIMAL
code_DECIMAL: ldd   #10
            std     pfa_BASE
            jmp     NEXT

; var_HERE, var_STATE, var_LATEST, var_BASE — convenience aliases for kernel code
var_HERE    equ     here_var
var_STATE   equ     pfa_STATE
var_LATEST  equ     pfa_LATEST
var_BASE    equ     pfa_BASE

here_var    fdb     DICT_START              ; separate 16-bit RAM cell

; var_LATEST_PTR points at the active vocabulary's PFA+0 cell (where the
; vocab's latest NFA is stored permanently, separate from the var_LATEST
; cache).  Initialized at cold to pfa_FORTH_LATEST.
var_LATEST_PTR fdb pfa_FORTH_LATEST

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
code_TYPE:  pshs    x               ; save IP — X is repurposed as the source ptr
            ldd     ,u++            ; len
            ldx     ,u++            ; addr
            tstb                    ; zero-length?
            beq     type_done
            pshs    b               ; save low-byte length across the loop
type_loop:  lda     ,x+
            lbsr    emit_a
            dec     ,s
            bne     type_loop
            leas    1,s
type_done:  puls    x
            jmp     NEXT

            ; COUNT ( caddr -- addr len )  counted string → pair
nfa_COUNT   fcb     5
            fcc     "COUNT"
lnk_COUNT   fdb     prev_link
cfa_COUNT   fdb     code_COUNT
prev_link   set     nfa_COUNT
code_COUNT: pshs    x               ; save IP — X reused as the caddr ptr
            ldx     ,u              ; caddr
            clra
            ldb     ,x+             ; len byte, X = addr+1
            stx     ,u              ; replace with addr (past length)
            std     ,--u            ; push length
            puls    x
            jmp     NEXT

            ; . ( n -- )  print signed decimal
nfa_DOT     fcb     1
            fcc     "."
lnk_DOT     fdb     prev_link
cfa_DOT     fdb     code_DOT
prev_link   set     nfa_DOT
code_DOT:   ldd     ,u++
            lbsr    fmt_sd_to_buf       ; Y = start, digits + leading '-' in pd_buf
            lbsr    emit_range
            lda     #' '
            lbsr    emit_a
            jmp     NEXT

            ; U. ( u -- )  print unsigned in current BASE, followed by a space
nfa_UDOT    fcb     2
            fcc     "U."
lnk_UDOT    fdb     prev_link
cfa_UDOT    fdb     code_UDOT
prev_link   set     nfa_UDOT
code_UDOT:  ldd     ,u++
            lbsr    fmt_ud_to_buf
            lbsr    emit_range
            lda     #' '
            lbsr    emit_a
            jmp     NEXT

            ; .R ( n w -- )  print signed right-justified in w chars (no trailing space)
nfa_DOTR    fcb     2
            fcc     ".R"
lnk_DOTR    fdb     prev_link
cfa_DOTR    fdb     code_DOTR
prev_link   set     nfa_DOTR
code_DOTR:  pshs    x
            ldd     ,u++                 ; w
            std     fmt_width
            ldd     ,u++                 ; n
            lbsr    fmt_sd_to_buf        ; Y = start
            lbsr    emit_padded_range
            puls    x
            jmp     NEXT

            ; U.R ( u w -- )  print unsigned right-justified in w chars
nfa_UDOTR   fcb     3
            fcc     "U.R"
lnk_UDOTR   fdb     prev_link
cfa_UDOTR   fdb     code_UDOTR
prev_link   set     nfa_UDOTR
code_UDOTR: pshs    x
            ldd     ,u++                 ; w
            std     fmt_width
            ldd     ,u++                 ; u
            lbsr    fmt_ud_to_buf
            lbsr    emit_padded_range
            puls    x
            jmp     NEXT

            ; SPACES ( n -- )  emit n spaces (n<=0: no-op)
nfa_SPACES  fcb     6
            fcc     "SPACES"
lnk_SPACES  fdb     prev_link
cfa_SPACES  fdb     code_SPACES
prev_link   set     nfa_SPACES
code_SPACES: pshs   x
            ldd     ,u++
            cmpd    #0
            ble     sp_done
            tfr     d,y
sp_loop:    lda     #' '
            lbsr    emit_a
            leay    -1,y
            bne     sp_loop
sp_done:    puls    x
            jmp     NEXT

; ---------------------------------------------------------------------------
; Pictured numeric output — <# # #S HOLD SIGN #>
; All digits accumulate in pd_buf (shared with the fmt_* layer).  hld_ptr
; tracks the current head (grows downward from pd_buf_end).
; ---------------------------------------------------------------------------
hld_ptr     fdb     0

            ; <# ( xd -- xd )  reset the hold buffer; xd untouched.
nfa_LSHARP  fcb     2
            fcb     $3C,$23         ; '<#'
lnk_LSHARP  fdb     prev_link
cfa_LSHARP  fdb     code_LSHARP
prev_link   set     nfa_LSHARP
code_LSHARP: ldd    #pd_buf_end
            std     hld_ptr
            jmp     NEXT

            ; HOLD ( char -- )  prepend char to the pictured output.
nfa_HOLD    fcb     4
            fcc     "HOLD"
lnk_HOLD    fdb     prev_link
cfa_HOLD    fdb     code_HOLD
prev_link   set     nfa_HOLD
code_HOLD:  ldd     ,u++
            ldy     hld_ptr
            leay    -1,y
            sty     hld_ptr
            stb     ,y
            jmp     NEXT

            ; SIGN ( n -- )  if n negative, HOLD '-'.
nfa_SIGN    fcb     4
            fcc     "SIGN"
lnk_SIGN    fdb     prev_link
cfa_SIGN    fdb     code_SIGN
prev_link   set     nfa_SIGN
code_SIGN:  ldd     ,u++
            bpl     sign_done
            lda     #'-'
            ldy     hld_ptr
            leay    -1,y
            sty     hld_ptr
            sta     ,y
sign_done:  jmp     NEXT

            ; # ( xd1 -- xd2 )  extract one digit from xd using BASE, HOLD it.
nfa_SHARP   fcb     1
            fcb     $23             ; '#'
lnk_SHARP   fdb     prev_link
cfa_SHARP   fdb     code_SHARP
prev_link   set     nfa_SHARP
code_SHARP: pshs    x
            lbsr    sharp_body
            puls    x
            jmp     NEXT

            ; #S ( xd1 -- 0 0 )  repeatedly apply # until xd1 is zero.
            ; Always emits at least one digit (so 0 #S prints "0").
nfa_SHARPS  fcb     2
            fcc     "#S"
lnk_SHARPS  fdb     prev_link
cfa_SHARPS  fdb     code_SHARPS
prev_link   set     nfa_SHARPS
code_SHARPS: pshs   x
ss_loop:    lbsr    sharp_body       ; always run at least once
            ldd     ,u               ; xd_hi
            bne     ss_loop
            ldd     2,u              ; xd_lo
            bne     ss_loop
            puls    x
            jmp     NEXT

            ; #> ( xd -- addr u )  finish: replace xd with ( start, length ).
nfa_SHARPGT fcb     2
            fcc     "#>"
lnk_SHARPGT fdb     prev_link
cfa_SHARPGT fdb     code_SHARPGT
prev_link   set     nfa_SHARPGT
code_SHARPGT:
            ldd     hld_ptr
            std     2,u              ; xd_lo slot := start addr
            ldd     #pd_buf_end
            subd    hld_ptr
            std     ,u               ; xd_hi slot := length
            jmp     NEXT

; sharp_body: divide the 32-bit value on the data stack by var_BASE,
;   accumulate the digit into the hold buffer.  Stack layout in/out:
;   ( xd_lo xd_hi -- xd_lo' xd_hi' )  with hi at TOS.
; Preserves X (caller handles IP save/restore).
sharp_body:
            pshs    x                ; reuse X as iteration counter
            ldd     #0
            pshs    d                ; remainder on R
            ldd     ,u               ; xd_hi
            pshs    d
            ldd     2,u              ; xd_lo
            pshs    d
            ldx     #32
sb_loop:    lsl     1,s              ; shift [rem:hi:lo] left 1
            rol     ,s
            rol     3,s
            rol     2,s
            rol     5,s
            rol     4,s
            ldd     4,s              ; D = remainder
            cmpd    var_BASE
            blo     sb_no_sub
            subd    var_BASE
            std     4,s
            inc     1,s              ; set LSB of quotient
sb_no_sub:  leax    -1,x
            bne     sb_loop
            puls    d                ; quotient lo
            std     2,u
            puls    d                ; quotient hi
            std     ,u
            puls    d                ; remainder (digit 0..BASE-1)
            cmpb    #10
            blo     sb_digit
            addb    #'A'-'0'-10
sb_digit:   addb    #'0'
            ldy     hld_ptr
            leay    -1,y
            sty     hld_ptr
            stb     ,y
            puls    x
            rts

; ---------------------------------------------------------------------------
; Number formatting layer (BASE-aware).
;   fmt_ud_to_buf ( D = unsigned value )  → Y = start of digits in pd_buf
;   fmt_sd_to_buf ( D = signed value )    → Y = start (may include a leading '-')
;   emit_range                            → emit chars from Y to pd_buf_end-1
;   emit_padded_range                     → emit (fmt_width - length) spaces then emit_range
;
; Both fmt_* routines leave the string right-justified against pd_buf_end,
; growing leftward.  pd_buf is sized for 16-bit binary (16 chars + sign).
; ---------------------------------------------------------------------------
fmt_sd_to_buf:
            pshs    d                    ; save original value for later sign check
            cmpd    #0
            bge     fs_nonneg
            coma
            comb
            addd    #1                   ; D = |value|
fs_nonneg:
            lbsr    fmt_ud_to_buf        ; Y points to start of unsigned digits
            ldd     ,s++
            cmpd    #0
            bge     fs_done
            leay    -1,y
            lda     #'-'
            sta     ,y
fs_done:    rts

fmt_ud_to_buf:
            ldy     #pd_buf_end
fub_div:
            pshs    x                    ; preserve IP while we use X as digit accumulator
            ldx     #0
fub_sub:
            cmpd    var_BASE
            blo     fub_sub_done
            subd    var_BASE
            leax    1,x
            bra     fub_sub
fub_sub_done:
            ; B holds remainder digit 0..BASE-1 (A = 0 once D < 256 on some pass)
            cmpb    #10
            blo     fub_numchar
            addb    #'A'-'0'-10          ; digit≥10 → 'A'..'Z'
fub_numchar:
            addb    #'0'
            leay    -1,y
            stb     ,y
            tfr     x,d                  ; D = quotient
            puls    x
            cmpd    #0
            bne     fub_div
            rts

emit_range:
            pshs    x
            tfr     y,x
er_loop:    cmpx    #pd_buf_end
            bhs     er_done
            lda     ,x+
            lbsr    emit_a
            bra     er_loop
er_done:    puls    x,pc

emit_padded_range:
            pshs    y                    ; save start of digits
            ldd     #pd_buf_end
            pshs    y                    ; dup for subd
            subd    ,s++                 ; D = length = pd_buf_end - Y
            ; padding = fmt_width - length
            coma
            comb
            addd    #1                   ; D = -length (6809 lacks NEGD)
            addd    fmt_width            ; D = fmt_width - length = padding
            ble     epr_no_pad           ; ≤ 0 → no padding
            tfr     d,y
epr_pad:    lda     #' '
            lbsr    emit_a
            leay    -1,y
            bne     epr_pad
epr_no_pad:
            puls    y                    ; restore start of digits
            lbsr    emit_range
            rts

fmt_width   fdb     0                    ; scratch: target column width for .R / U.R
pd_buf      fcb     0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0  ; 18 bytes (16 binary digits + sign + slack)
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
            ; X is the Forth IP. Save it on the return stack so the inner
            ; loop can use X as a counter without clobbering NEXT's state.
            pshs    x
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
            puls    x,pc            ; restore IP and return

dm_div_by_zero:
            ; Leave dividend as remainder, 0 as quotient. Caller picks.
            ldd     2,u             ; D = a (unchanged)
            std     2,u             ; rem = a  (NOS)
            clra
            clrb
            std     ,u              ; quot = 0 (TOS)
            puls    x,pc            ; restore IP and return

; ---------------------------------------------------------------------------
; Outer-interpreter state + kernel helpers.
; ---------------------------------------------------------------------------
var_TOIN    fdb     0               ; >IN — offset into TIB of next unread byte
var_TIBLEN  fdb     0               ; #TIB — bytes currently valid in TIB
var_SPAN    fdb     0               ; SPAN — bytes read by last EXPECT

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
            ; Start walking from the active vocabulary (its cached head).
            ldd     var_LATEST_PTR
            std     sf_cur_voc
            ldx     var_LATEST
sf_iter:    cmpx    #0
            beq     sf_next_voc     ; end of this vocab → try parent
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
sf_next_voc:
            ; Finished current vocab's chain.  Follow the parent pointer
            ; to continue the search; if no parent, fail.
            ldy     sf_cur_voc
            ldy     2,y             ; Y = parent's PFA+0 address (or 0)
            cmpy    #0
            beq     sf_fail
            sty     sf_cur_voc
            ldx     ,y              ; X = parent vocab's latest NFA
            bra     sf_iter
sf_fail:    ldx     #0
            clrb                    ; B=0 → CC.Z=1 (and N=0)
            rts

sf_target   fdb     0
sf_cur      fdb     0
sf_cur_voc  fdb     0
sf_tlen     fcb     0
sf_flags    fcb     0

; --------- number_kernel ----------------------------------------------------
; Parse (X = addr, D = len, only B used) as a signed integer in the current
; BASE. Accepts one optional leading '-'.  Digits are 0..BASE-1, with values
; 10+ written as 'A'..'Z' (case-insensitive).
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
            ; Convert ASCII digit to numeric: '0'..'9' → 0..9,
            ;                                 'A'..'Z' or 'a'..'z' → 10..35
            suba    #'0'
            bmi     num_fail
            cmpa    #9
            bls     num_haveval      ; 0..9 digit
            ; Either an uppercase letter (A='0'+17) or a lowercase
            ; letter ('a'='0'+49) — treat both.
            suba    #7               ; A='0'+17 → -7 gives 10
            cmpa    #10
            blo     num_fail         ; something between '9'+1 and 'A'-1
            cmpa    #35
            bls     num_letter_cased
            suba    #32              ; lowercase fold: 'a'-'A'=32
            cmpa    #10
            blo     num_fail
            cmpa    #35
            bhi     num_fail
num_letter_cased:
num_haveval:
            ; A = digit value (0..35).  Must be < BASE.
            pshs    b
            cmpa    var_BASE+1       ; var_BASE is 16-bit, low byte is the cap for BASE ≤ 36
            blo     num_goodvalue
            puls    b
            bra     num_fail
num_goodvalue:
            pshs    a
            lbsr    mul_base_y       ; Y = Y * BASE
            puls    a
            leay    a,y              ; Y += digit
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

; Y = Y * BASE, clobbers D.  Uses repeated-add (BASE-1 times), fine for
; BASE ≤ 36 — the interpreter parses short tokens so extra work is trivial.
; NOTE: LEAX only affects the Z flag (not N), so the loop test uses BNE, and
; the BASE=1 edge case (0 iterations) is handled before entering the loop.
mul_base_y:
            pshs    x
            tfr     y,d              ; D = running sum, initialised to Y (×1)
            pshs    d                ; [s] = original Y (addend)
            ldx     var_BASE
            leax    -1,x             ; iterations = BASE-1 (Z set if 0)
            beq     mby_done
mby_loop:   addd    ,s               ; D += Y
            leax    -1,x             ; Z set when X reaches 0
            bne     mby_loop
mby_done:   tfr     d,y
            leas    2,s              ; drop saved Y
            puls    x,pc

; Alias — existing callers of mul10_y now use mul_base_y (base is dynamic).
mul10_y     equ     mul_base_y

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
            pshs    x                ; save IP
            lbsr    code_ACCEPT_body
            puls    x
            jmp     NEXT

; code_ACCEPT_body ( c-addr +n -- +n2 )
; Shared read-a-line-from-ACIA engine.  Callable subroutine (not a
; primitive itself).  Caller is responsible for saving the Forth IP (X);
; this routine uses X and Y as scratch.
code_ACCEPT_body:
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
            puls    d                ; discard saved maxlen
            tfr     y,d
            std     ,u               ; replace buffer addr with count
            rts

            ; EXPECT ( c-addr +n -- )  like ACCEPT but stores count in SPAN.
            ; FORTH-83 convention: no value left on the data stack; the
            ; caller fetches the actual read length from SPAN.
nfa_EXPECT  fcb     6
            fcc     "EXPECT"
lnk_EXPECT  fdb     prev_link
cfa_EXPECT  fdb     code_EXPECT
prev_link   set     nfa_EXPECT
code_EXPECT:
            pshs    x
            lbsr    code_ACCEPT_body       ; leaves count at ,u, returns count in Y
            ; ACCEPT's body leaves ( len ) on data stack; EXPECT should
            ; instead store len in SPAN and drop the cell.
            ldd     ,u++
            std     var_SPAN
            puls    x
            jmp     NEXT

            ; SPAN ( -- addr )  address of the SPAN variable.
nfa_SPAN    fcb     4
            fcc     "SPAN"
lnk_SPAN    fdb     prev_link
cfa_SPAN    fdb     code_SPAN
prev_link   set     nfa_SPAN
code_SPAN:  ldd     #var_SPAN
            std     ,--u
            jmp     NEXT

            ; QUERY ( -- )  read one line into TIB; reset #TIB / >IN.
nfa_QUERY   fcb     5
            fcc     "QUERY"
lnk_QUERY   fdb     prev_link
cfa_QUERY   fdb     code_QUERY
prev_link   set     nfa_QUERY
code_QUERY:
            ; Push (TIB_ADDR, TIB_SIZE) then call ACCEPT.
            ldd     #TIB_ADDR
            std     ,--u
            ldd     #TIB_SIZE
            std     ,--u
            pshs    x
            lbsr    code_ACCEPT_body       ; leaves count on stack
            ldd     ,u++                   ; D = count
            std     var_TIBLEN
            ldd     #0
            std     var_TOIN
            puls    x
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

            ; FIND ( c-addr -- c-addr 0 | xt 1 | xt -1 )
            ; Classic FORTH-83 signature. c-addr is a counted string.
            ;   0  : not found (c-addr preserved)
            ;   1  : found, non-IMMEDIATE (c-addr → xt)
            ;   -1 : found, IMMEDIATE     (c-addr → xt)
nfa_FIND    fcb     4
            fcc     "FIND"
lnk_FIND    fdb     prev_link
cfa_FIND    fdb     code_FIND
prev_link   set     nfa_FIND
code_FIND:
            pshs    x               ; save IP
            ldx     ,u              ; X = c-addr (counted string)
            ldb     ,x+             ; B = length; X now points at name bytes
            clra                    ; D = 0:length
            lbsr    sfind_kernel    ; X=xt (or 0), B=0 (miss) / 1 (normal) / 2 (IMMEDIATE)
            beq     find_miss
            stx     ,u              ; replace c-addr with xt
            cmpb    #2
            beq     find_immed
            ldd     #1
            std     ,--u
            puls    x
            jmp     NEXT
find_immed: ldd     #-1
            std     ,--u
            puls    x
            jmp     NEXT
find_miss:  ldd     #0
            std     ,--u
            puls    x
            jmp     NEXT

            ; WORD ( char -- c-addr )
            ; Parse text from TIB starting at >IN using `char` as delimiter.
            ; Skips leading delimiters, then accumulates chars until the
            ; next delimiter or end-of-TIB.  The result is a counted string
            ; written to HERE (length byte then chars).  Returns HERE addr.
            ; NOTE: this does NOT advance HERE permanently — the string is
            ; transient and will be overwritten by the next WORD.
nfa_WORD    fcb     4
            fcc     "WORD"
lnk_WORD    fdb     prev_link
cfa_WORD    fdb     code_WORD
prev_link   set     nfa_WORD
code_WORD:
            pshs    x                     ; save IP
            ldd     ,u                    ; D = char (low byte used)
            stb     wd_delim
            ; Scan TIB from >IN, skipping leading delimiters.
            ldx     #TIB_ADDR
            ldd     var_TOIN
            leax    d,x                   ; X = TIB + >IN
            ldd     #TIB_ADDR
            addd    var_TIBLEN
            std     wd_end
wd_skip:    cmpx    wd_end
            bhs     wd_at_end
            lda     ,x
            cmpa    wd_delim
            bne     wd_start_found
            leax    1,x
            bra     wd_skip
wd_at_end:  ; empty — return counted string with length 0 at HERE
            ldy     var_HERE
            clr     ,y
            sty     ,u                    ; replace char with addr
            tfr     x,d
            subd    #TIB_ADDR
            std     var_TOIN
            puls    x
            jmp     NEXT
wd_start_found:
            stx     wd_start
            ; Accumulate chars until delimiter or end-of-TIB.
wd_scan:    cmpx    wd_end
            bhs     wd_scan_done
            lda     ,x
            cmpa    wd_delim
            beq     wd_scan_done
            leax    1,x
            bra     wd_scan
wd_scan_done:
            ; Compute length.
            tfr     x,d
            subd    wd_start
            ; Step past the delimiter (if we stopped on one).
            pshs    d                     ; save length
            cmpx    wd_end
            bhs     wd_no_step
            leax    1,x
wd_no_step: tfr     x,d
            subd    #TIB_ADDR
            std     var_TOIN
            ; Write counted string to HERE.
            ldy     var_HERE
            puls    d                     ; length
            stb     ,y+                   ; length byte
            ldx     wd_start
            tstb
            beq     wd_write_done
wd_write:   lda     ,x+
            sta     ,y+
            decb
            bne     wd_write
wd_write_done:
            ldx     var_HERE              ; return HERE (counted-string addr)
            stx     ,u
            puls    x                     ; restore IP
            jmp     NEXT

wd_delim    fcb     0
wd_start    fdb     0
wd_end      fdb     0

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

            ; ' ( "name" -- xt )  look up the next word's execution token.
            ; Pushes 0 on failure (fail-soft, consistent with the rest of the kernel).
nfa_TICK    fcb     1
            fcc     "'"
lnk_TICK    fdb     prev_link
cfa_TICK    fdb     code_TICK
prev_link   set     nfa_TICK
code_TICK:  pshs    x                     ; save IP
            lbsr    parse_name_kernel     ; X = name addr, D = length in B
            cmpd    #0
            beq     tick_fail
            lbsr    sfind_kernel          ; → X = CFA (or 0), B = 1/2 (or 0)
            beq     tick_fail
            pshs    x
            ldd     ,s++
            std     ,--u                  ; push xt
            puls    x
            jmp     NEXT
tick_fail:
            ldd     #0
            std     ,--u
            puls    x
            jmp     NEXT

            ; CHAR ( "name" -- c )  parse next token, push first char.
nfa_CHAR    fcb     4
            fcc     "CHAR"
lnk_CHAR    fdb     prev_link
cfa_CHAR    fdb     code_CHAR
prev_link   set     nfa_CHAR
code_CHAR:  pshs    x
            lbsr    parse_name_kernel      ; X=addr, B=len
            cmpd    #0
            beq     char_fail
            ldb     ,x                     ; B = first char
            clra
            std     ,--u
            puls    x
            jmp     NEXT
char_fail:  ldd     #0
            std     ,--u
            puls    x
            jmp     NEXT

            ; [CHAR] IMMEDIATE ( "name" -- )  at compile time, compile (LIT) c.
nfa_CHARBRK fcb     F_IMMED|6
            fcc     "[CHAR]"
lnk_CHARBRK fdb     prev_link
cfa_CHARBRK fdb     code_CHARBRK
prev_link   set     nfa_CHARBRK
code_CHARBRK:
            pshs    x
            lbsr    parse_name_kernel
            cmpd    #0
            beq     charbrk_done
            ldb     ,x                     ; B = first char
            clra
            ldy     var_HERE
            ldx     #cfa_LIT
            stx     ,y++
            std     ,y++                   ; compile the char as a literal
            sty     var_HERE
charbrk_done: puls  x
            jmp     NEXT

            ; ['] IMMEDIATE ( "name" -- )
            ; Compile-time `'` — parses the next word and compiles `(LIT) xt`
            ; into the current definition. At runtime, pushes xt on the stack.
nfa_TICKBRK fcb     F_IMMED|3
            fcc     "[']"
lnk_TICKBRK fdb     prev_link
cfa_TICKBRK fdb     code_TICKBRK
prev_link   set     nfa_TICKBRK
code_TICKBRK:
            pshs    x                     ; save IP
            lbsr    parse_name_kernel     ; X = name, D/B = length
            cmpd    #0
            beq     tbk_fail
            lbsr    sfind_kernel          ; X = xt, B = 1/2 or 0
            beq     tbk_fail
            ; Compile (LIT) + xt into the user dictionary
            pshs    x                     ; stash xt on R
            ldy     var_HERE
            ldx     #cfa_LIT
            stx     ,y++
            puls    x                     ; pop xt
            stx     ,y++
            sty     var_HERE
            puls    x                     ; restore IP
            jmp     NEXT
tbk_fail:   puls    x
            jmp     NEXT

            ; IMMEDIATE ( -- )  Mark the most-recently defined word as IMMEDIATE.
nfa_IMMED   fcb     9
            fcc     "IMMEDIATE"
lnk_IMMED   fdb     prev_link
cfa_IMMED   fdb     code_IMMED
prev_link   set     nfa_IMMED
code_IMMED: ldy     var_LATEST
            lda     ,y
            ora     #F_IMMED
            sta     ,y
            jmp     NEXT

            ; LITERAL ( x -- )  IMMEDIATE
            ; Compile the TOS as a runtime literal: emit (LIT) xt, then the value.
nfa_LITERAL fcb     F_IMMED|7
            fcc     "LITERAL"
lnk_LITERAL fdb     prev_link
cfa_LITERAL fdb     code_LITERAL
prev_link   set     nfa_LITERAL
code_LITERAL:
            pshs    x
            ldy     var_HERE
            ldx     #cfa_LIT
            stx     ,y++
            ldd     ,u++                 ; value to embed
            std     ,y++
            sty     var_HERE
            puls    x
            jmp     NEXT

            ; \ ( "..." -- )  IMMEDIATE — rest-of-line comment.
            ;  Skips the TIB past the next newline (or to end of buffer).
nfa_LCOMMENT fcb    F_IMMED|1
            fcb     $5C                  ; ASCII "\"  (fcc would need escaping)
lnk_LCOMMENT fdb    prev_link
cfa_LCOMMENT fdb    code_LCOMMENT
prev_link    set    nfa_LCOMMENT
code_LCOMMENT:
            pshs    x
            ; Advance >IN to #TIB (consume rest of line).  The outer loop
            ; ACCEPTs a fresh line, so a truly multi-line comment requires
            ; `(` — but a trailing "\ ..." on a line is the common case.
            ldd     var_TIBLEN
            std     var_TOIN
            puls    x
            jmp     NEXT

            ; POSTPONE ( "name" -- )  IMMEDIATE
            ; Parses the next word; if target is IMMEDIATE, compile its xt
            ; so the current definition runs it at runtime.  If target is
            ; non-IMMEDIATE, compile "(LIT) xt ," so that at runtime, the
            ; current definition will itself compile xt.  Useful for
            ; wrapping control-flow words.
nfa_POSTP   fcb     F_IMMED|8
            fcc     "POSTPONE"
lnk_POSTP   fdb     prev_link
cfa_POSTP   fdb     code_POSTP
prev_link   set     nfa_POSTP
code_POSTP:
            pshs    x                     ; save IP
            lbsr    parse_name_kernel
            cmpd    #0
            lbeq    postp_done
            lbsr    sfind_kernel          ; X=xt, B=1 or 2 (0 if not found)
            beq     postp_done
            stx     postp_xt
            cmpb    #2
            beq     postp_imm
            ; non-IMMEDIATE: compile (LIT) xt , so that at runtime the
            ; outer word pushes xt and compiles it.
            ldy     var_HERE
            ldx     #cfa_LIT
            stx     ,y++
            ldx     postp_xt
            stx     ,y++
            ldx     #cfa_COMMA
            stx     ,y++
            sty     var_HERE
            bra     postp_done
postp_imm:  ; IMMEDIATE: compile xt directly so the outer word invokes it.
            ldy     var_HERE
            ldx     postp_xt
            stx     ,y++
            sty     var_HERE
postp_done: puls    x
            jmp     NEXT

postp_xt    fdb     0

            ; RECURSE ( -- )  IMMEDIATE
            ; Compile a call to the colon definition currently being defined.
            ; Uses col_new_nfa saved by `:`.
nfa_RECURSE fcb     F_IMMED|7
            fcc     "RECURSE"
lnk_RECURSE fdb     prev_link
cfa_RECURSE fdb     code_RECURSE
prev_link   set     nfa_RECURSE
code_RECURSE:
            pshs    x
            ldy     col_new_nfa          ; Y = nfa of current definition
            beq     recurse_done         ; not inside `:` — silently ignore
            ; CFA = nfa + 1 + namelen + 2(link)
            ldb     ,y
            andb    #$1F
            leay    1,y
            leay    b,y
            leay    2,y
            sty     rec_scratch
            ldy     var_HERE
            ldd     rec_scratch
            std     ,y++
            sty     var_HERE
recurse_done:
            puls    x
            jmp     NEXT

rec_scratch fdb     0

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
; BEGIN … WHILE … REPEAT
; WHILE compiles (0BRANCH) placeholder and pushes its addr above BEGIN's addr.
; REPEAT: compile (BRANCH) back to BEGIN, then patch WHILE to point here.
; ---------------------------------------------------------------------------
            ; WHILE ( addr_begin -- addr_while addr_begin )  IMMEDIATE
nfa_WHILE   fcb     F_IMMED|5
            fcc     "WHILE"
lnk_WHILE   fdb     prev_link
cfa_WHILE   fdb     code_WHILE
prev_link   set     nfa_WHILE
code_WHILE: pshs    x
            ; duplicate BEGIN's addr beneath the new placeholder:
            ; stack in: ( addr_begin )
            ; compile (0BRANCH) + placeholder
            ldd     ,u                   ; D = addr_begin (keep it in place)
            ldy     var_HERE
            ldx     #cfa_0BR
            stx     ,y++
            tfr     y,x                  ; X = placeholder addr
            ldd     #0
            std     ,y++                 ; placeholder offset
            sty     var_HERE
            ; Now rearrange: replace TOS with placeholder, push original BEGIN addr
            ldd     ,u                   ; D = addr_begin (still there)
            stx     ,u                   ; TOS := placeholder addr
            std     ,--u                 ; push addr_begin on top
            puls    x
            jmp     NEXT

            ; REPEAT ( addr_while addr_begin -- )  IMMEDIATE
nfa_REPEAT  fcb     F_IMMED|6
            fcc     "REPEAT"
lnk_REPEAT  fdb     prev_link
cfa_REPEAT  fdb     code_REPEAT
prev_link   set     nfa_REPEAT
code_REPEAT: pshs   x
            ldd     ,u++                 ; D = addr_begin
            std     ctrl_tmp
            ldd     ,u++                 ; D = addr_while (placeholder)
            std     ctrl_tmp2
            ; Compile (BRANCH) + offset to addr_begin
            ldy     var_HERE
            ldx     #cfa_BR
            stx     ,y++
            tfr     y,d
            pshs    d
            ldd     ctrl_tmp             ; D = addr_begin
            subd    ,s++                 ; D = begin - here (negative offset)
            std     ,y++
            sty     var_HERE
            ; Patch WHILE placeholder: offset = HERE - addr_while
            ldx     ctrl_tmp2
            tfr     y,d
            pshs    x
            subd    ,s++                 ; D = HERE - addr_while
            std     ,x
            puls    x
            jmp     NEXT

; ---------------------------------------------------------------------------
; DO / LOOP / +LOOP / I / J / LEAVE
; (DO) pushes limit and current index to return stack (R).
; (LOOP): ++index; if index == limit → drop limit+index, skip backward branch.
; (+LOOP): index += step; exit when (old_delta XOR new_delta) sign bit set.
; LEAVE: force-exit the current loop by setting index := limit on R.
; I/J read the inner/outer loop index (0,s / 4,s on the return stack).
; ---------------------------------------------------------------------------

            ; (DO) runtime — compiled by DO. Stack: ( limit start -- )
nfa_PDO     fcb     4
            fcc     "(DO)"
lnk_PDO     fdb     prev_link
cfa_PDO     fdb     code_PDO
prev_link   set     nfa_PDO
code_PDO:   ldd     ,u++                 ; D = start (TOS)
            ldy     ,u++                 ; Y = limit
            pshs    y                    ; R: push limit (deeper)
            pshs    d                    ; R: push start (becomes index)
            jmp     NEXT

            ; (LOOP) runtime — inline 16-bit offset follows; take it or skip.
nfa_PLOOP   fcb     6
            fcc     "(LOOP)"
lnk_PLOOP   fdb     prev_link
cfa_PLOOP   fdb     code_PLOOP
prev_link   set     nfa_PLOOP
code_PLOOP: ldd     ,s                   ; D = current index
            addd    #1
            std     ,s
            cmpd    2,s                  ; index == limit ?
            beq     ploop_exit
            ldd     ,x                   ; D = branch offset
            leax    d,x                  ; IP += offset
            jmp     NEXT
ploop_exit: leas    4,s                  ; drop index + limit from R
            leax    2,x                  ; skip offset cell
            jmp     NEXT

            ; (+LOOP) runtime — step on data stack. Exits when we cross limit.
nfa_PPLOOP  fcb     7
            fcc     "(+LOOP)"
lnk_PPLOOP  fdb     prev_link
cfa_PPLOOP  fdb     code_PPLOOP
prev_link   set     nfa_PPLOOP
code_PPLOOP: ldd    ,u++                 ; D = step
            ldy     ,s                   ; Y = old index
            addd    ,s                   ; D = new index
            std     ,s
            std     ppl_new
            subd    2,s                  ; D = new - limit
            std     ppl_new
            tfr     y,d
            subd    2,s                  ; D = old - limit
            eora    ppl_new              ; sign-bit XOR of old/new deltas
            bmi     pploop_exit          ; crossed → exit
            ldd     ,x
            leax    d,x
            jmp     NEXT
pploop_exit: leas   4,s
            leax    2,x
            jmp     NEXT

ppl_new     fdb     0

            ; I ( -- n )  innermost loop index
nfa_I       fcb     1
            fcc     "I"
lnk_I       fdb     prev_link
cfa_I       fdb     code_I
prev_link   set     nfa_I
code_I:     ldd     ,s
            std     ,--u
            jmp     NEXT

            ; J ( -- n )  next-outer loop index.  Inner loop pushed (index,
            ; limit) on top of the outer loop's (index, limit), so J lives at 4,s.
nfa_J       fcb     1
            fcc     "J"
lnk_J       fdb     prev_link
cfa_J       fdb     code_J
prev_link   set     nfa_J
code_J:     ldd     4,s
            std     ,--u
            jmp     NEXT

            ; LEAVE ( -- )  force-exit the current loop.
            ; Sets index := limit - 1 so the next (LOOP)'s increment brings
            ; index to limit and triggers the equality-exit.  (Code between
            ; LEAVE and LOOP still runs once on the iteration where LEAVE
            ; executed — this is fig-Forth style, not ANS's immediate-exit.)
            ; NOTE: reliable only with LOOP (+1 step).  With +LOOP,
            ; behaviour depends on the step's sign relative to limit−index.
nfa_LEAVE   fcb     5
            fcc     "LEAVE"
lnk_LEAVE   fdb     prev_link
cfa_LEAVE   fdb     code_LEAVE
prev_link   set     nfa_LEAVE
code_LEAVE: ldd     2,s                  ; D = limit
            subd    #1
            std     ,s                   ; index := limit - 1
            jmp     NEXT

            ; UNLOOP ( -- )  drop the innermost loop's index+limit from R
            ; Use before EXIT inside a DO/LOOP to leave the loop cleanly.
nfa_UNLOOP  fcb     6
            fcc     "UNLOOP"
lnk_UNLOOP  fdb     prev_link
cfa_UNLOOP  fdb     code_UNLOOP
prev_link   set     nfa_UNLOOP
code_UNLOOP: leas   4,s                  ; drop index (2) + limit (2)
            jmp     NEXT

            ; DO ( -- addr )  IMMEDIATE — compile (DO), remember HERE
nfa_DO      fcb     F_IMMED|2
            fcc     "DO"
lnk_DO      fdb     prev_link
cfa_DO      fdb     code_DO
prev_link   set     nfa_DO
code_DO:    pshs    x
            ldy     var_HERE
            ldx     #cfa_PDO
            stx     ,y++
            sty     var_HERE
            tfr     y,d
            std     ,--u                 ; push back-branch target
            puls    x
            jmp     NEXT

            ; LOOP ( addr -- )  IMMEDIATE — compile (LOOP) + offset
nfa_LOOP    fcb     F_IMMED|4
            fcc     "LOOP"
lnk_LOOP    fdb     prev_link
cfa_LOOP    fdb     code_LOOP
prev_link   set     nfa_LOOP
code_LOOP:  pshs    x
            ldd     ,u++                 ; D = DO's HERE
            std     ctrl_tmp
            ldy     var_HERE
            ldx     #cfa_PLOOP
            stx     ,y++                 ; compile (LOOP); Y → offset cell
            tfr     y,d
            pshs    d
            ldd     ctrl_tmp
            subd    ,s++                 ; offset = back-target - offset-cell
            std     ,y++
            sty     var_HERE
            puls    x
            jmp     NEXT

            ; +LOOP ( addr -- )  IMMEDIATE — compile (+LOOP) + offset
nfa_PLOOPI  fcb     F_IMMED|5
            fcc     "+LOOP"
lnk_PLOOPI  fdb     prev_link
cfa_PLOOPI  fdb     code_PLOOPI
prev_link   set     nfa_PLOOPI
code_PLOOPI: pshs   x
            ldd     ,u++
            std     ctrl_tmp
            ldy     var_HERE
            ldx     #cfa_PPLOOP
            stx     ,y++
            tfr     y,d
            pshs    d
            ldd     ctrl_tmp
            subd    ,s++
            std     ,y++
            sty     var_HERE
            puls    x
            jmp     NEXT

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

            ; CREATE ( "name" -- )  make a header with CFA=DOCREATE,
            ; DOES_ADDR=0, and an open PFA that ALLOT can extend.
nfa_CREATE  fcb     6
            fcc     "CREATE"
lnk_CREATE  fdb     prev_link
cfa_CREATE  fdb     code_CREATE
prev_link   set     nfa_CREATE
code_CREATE:
            pshs    x
            lbsr    parse_name_kernel
            cmpd    #0
            lbeq    create_done
            stx     col_name_addr
            stb     col_name_len
            ldy     var_HERE
            sty     col_new_nfa
            stb     ,y+
            ldx     col_name_addr
create_copy: lda    ,x+
            sta     ,y+
            decb
            bne     create_copy
            ldx     var_LATEST
            stx     ,y++            ; link
            ldx     #DOCREATE
            stx     ,y++            ; CFA
            ldx     #0
            stx     ,y++            ; DOES_ADDR = 0
            sty     var_HERE
            ldx     col_new_nfa
            stx     var_LATEST
create_done: puls   x
            jmp     NEXT

            ; (;DOES) runtime — patch LATEST's DOES_ADDR to current IP,
            ; then EXIT from the outer CREATE-using word.
nfa_SEMIDOES fcb    7
            fcc     "(;DOES"
            fcb     $29             ; ')'
lnk_SEMIDOES fdb    prev_link
cfa_SEMIDOES fdb    code_SEMIDOES
prev_link   set     nfa_SEMIDOES
code_SEMIDOES:
            ; Locate the DOES_ADDR slot of the most-recently-CREATEd word:
            ; NFA → flags+length → name → LNK (2) → CFA (2) → DOES_ADDR.
            ldy     var_LATEST
            ldb     ,y
            andb    #F_LENMASK
            clra                     ; D = 0:length (name length in [0, 31])
            leay    1,y              ; past flags byte
            leay    d,y              ; past name
            leay    4,y              ; past LNK + CFA
            stx     ,y               ; DOES_ADDR := current IP
            ; EXIT the outer word: pop saved IP from return stack.
            puls    x
            jmp     NEXT

            ; FORGET ( "name" -- )
            ; Rewind HERE and LATEST to the named word's NFA, discarding
            ; it and everything defined later. Refuses on built-ins (NFA
            ; below DICT_START) to protect the kernel.
nfa_FORGET  fcb     6
            fcc     "FORGET"
lnk_FORGET  fdb     prev_link
cfa_FORGET  fdb     code_FORGET
prev_link   set     nfa_FORGET
code_FORGET:
            pshs    x                      ; save IP
            lbsr    parse_name_kernel      ; X=name addr in TIB, B=length
            cmpd    #0
            lbeq    frg_done
            stb     frg_len
            lbsr    sfind_kernel           ; X=CFA, B=status (0=not found)
            lbeq    frg_done
            ; Derive NFA = CFA - 3 - len
            stx     frg_cfa                ; save CFA
            clra
            ldb     frg_len
            addd    #3                     ; D = len + 3
            pshs    d
            ldd     frg_cfa
            subd    ,s++                   ; D = CFA - (len + 3) = NFA
            cmpd    #DICT_START
            blo     frg_done               ; refuse if built-in
            tfr     d,y                    ; Y = NFA
            sty     var_HERE
            ; LATEST := word at NFA + 1 + len (the LNK cell contents)
            addd    #1
            clra
            ldb     frg_len
            ; D = len, but we need previous D (NFA+1) + len.
            ; Simpler via Y: leay 1,y ; leay d,y where D=len.
            ldy     var_HERE               ; Y = NFA again
            leay    1,y
            clra
            ldb     frg_len
            leay    d,y                    ; Y = LNK cell address
            ldy     ,y                     ; Y = previous NFA (LNK contents)
            sty     var_LATEST
frg_done:   puls    x
            jmp     NEXT

frg_len     fcb     0
frg_cfa     fdb     0

            ; MARKER ( "name" -- )
            ; Define "name" as a word that, when executed, forgets itself
            ; and everything defined after it.  Built on CREATE + DOES>.
            ; Implementation: MARKER stores current HERE and LATEST in
            ; "name"'s PFA; DOES> restores them.
nfa_MARKER  fcb     6
            fcc     "MARKER"
lnk_MARKER  fdb     prev_link
cfa_MARKER  fdb     code_MARKER
prev_link   set     nfa_MARKER
code_MARKER:
            pshs    x
            ; Call code_CREATE indirectly: we reuse its work by jumping
            ; to its body, then coming back.  Simpler: replicate here.
            lbsr    parse_name_kernel
            cmpd    #0
            lbeq    mk_done
            stx     col_name_addr
            stb     col_name_len
            ldy     var_HERE
            sty     col_new_nfa
            stb     ,y+
            ldx     col_name_addr
mk_copy:    lda     ,x+
            sta     ,y+
            decb
            bne     mk_copy
            ldx     var_LATEST
            stx     ,y++               ; link
            ldx     #DOMARKER
            stx     ,y++               ; CFA = DOMARKER
            ; Store snapshot: OLD_LATEST and OLD_HERE (HERE before this
            ; marker's header).  OLD_LATEST is what LATEST pointed to
            ; BEFORE this CREATE, which equals the LNK cell value we just
            ; wrote.  OLD_HERE is col_new_nfa (NFA of this marker).
            ldd     var_LATEST         ; but var_LATEST hasn't been updated yet
            ; Actually, the old LATEST we just stored as the link. Read it
            ; back for consistency:
            ldd     var_LATEST         ; unchanged: pre-marker LATEST
            std     ,y++               ; PFA+0 = saved LATEST
            ldd     col_new_nfa
            std     ,y++               ; PFA+2 = saved HERE
            sty     var_HERE
            ldx     col_new_nfa
            stx     var_LATEST
mk_done:    puls    x
            jmp     NEXT

            ; DOES> IMMEDIATE — at compile time emit a (;DOES) CFA into
            ; the current definition.  Everything compiled after DOES> is
            ; the instance-time runtime code.
nfa_DOES    fcb     F_IMMED|5
            fcc     "DOES>"
lnk_DOES    fdb     prev_link
cfa_DOES    fdb     code_DOES
prev_link   set     nfa_DOES
code_DOES:
            pshs    x                    ; save IP — X reused as scratch
            ldy     var_HERE
            ldx     #cfa_SEMIDOES
            stx     ,y++
            sty     var_HERE
            puls    x
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

            ; (SLITERAL) — runtime partner for S".  At the CFA, X (IP) points
            ; to the length byte; push (addr, len) and skip past the string.
nfa_SLITRT  fcb     10
            fcc     "(SLITERAL)"
lnk_SLITRT  fdb     prev_link
cfa_SLITRT  fdb     code_SLITRT
prev_link   set     nfa_SLITRT
code_SLITRT:
            ldb     ,x+              ; B = length, X → first char
            pshs    b                ; save len across ABX/LDD below
            stx     slit_addr        ; remember string start
            abx                      ; X += B  (step past the string; ABX leaves B intact)
            ldd     slit_addr
            std     ,--u             ; push addr (NOS)
            clra                     ; D.hi = 0
            puls    b                ; D.lo = len
            std     ,--u             ; push len (TOS)
            jmp     NEXT

slit_addr   fdb     0

            ; S" IMMEDIATE — parse until '"', compile (SLITERAL)+len+chars
nfa_SQUOTE  fcb     F_IMMED|2
            fcb     $53,$22           ; S"
lnk_SQUOTE  fdb     prev_link
cfa_SQUOTE  fdb     code_SQUOTE
prev_link   set     nfa_SQUOTE
code_SQUOTE:
            pshs    x
            ldx     #TIB_ADDR
            ldd     var_TOIN
            leax    d,x
            lda     ,x
            cmpa    #' '
            bne     sq_have_start
            leax    1,x
sq_have_start:
            stx     dq_start_addr
            ldy     #TIB_ADDR
            ldd     var_TIBLEN
            leay    d,y
            sty     dq_end
sq_scan:    cmpx    dq_end
            bhs     sq_no_quote
            lda     ,x
            cmpa    #'"'
            beq     sq_got_quote
            leax    1,x
            bra     sq_scan
sq_got_quote:
            tfr     x,d
            subd    dq_start_addr
            std     dq_len
            leax    1,x
            bra     sq_update_toin
sq_no_quote:
            tfr     x,d
            subd    dq_start_addr
            std     dq_len
sq_update_toin:
            tfr     x,d
            subd    #TIB_ADDR
            std     var_TOIN
            ldy     var_HERE
            ldx     #cfa_SLITRT
            stx     ,y++
            ldb     dq_len+1
            stb     ,y+
            tstb
            beq     sq_compile_done
            ldx     dq_start_addr
sq_copy:    lda     ,x+
            sta     ,y+
            decb
            bne     sq_copy
sq_compile_done:
            sty     var_HERE
            puls    x
            jmp     NEXT

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

            ; ABORT ( -- )  reset both stacks and re-enter QUIT.
            ; Does NOT print an error — higher-level ABORT" does that.
nfa_ABORT   fcb     5
            fcc     "ABORT"
lnk_ABORT   fdb     prev_link
cfa_ABORT   fdb     code_ABORT
prev_link   set     nfa_ABORT
code_ABORT: lds     #RSP_TOP
            ldu     #PSP_TOP
            ldx     #boot_code
            jmp     NEXT

            ; (ABORT") runtime — at the CFA, X (IP) points to the length
            ; byte.  Pops a flag from the data stack: if non-zero, TYPE the
            ; string then ABORT; otherwise skip past the string.
nfa_PABORTQ fcb     8
            fcc     "(ABORT"
            fcb     $22,$29          ; "),"   (name = "(ABORT\")")
lnk_PABORTQ fdb     prev_link
cfa_PABORTQ fdb     code_PABORTQ
prev_link   set     nfa_PABORTQ
code_PABORTQ:
            ldd     ,u++             ; D = flag
            pshs    d                ; save flag on R (LDB below would clobber)
            ldb     ,x+              ; B = string length; X → first char
            pshs    b                ; save length on R
            ldd     2,s              ; reload flag
            cmpd    #0
            beq     paq_skip
            ldb     ,s               ; B = length
            tstb
            beq     paq_do_abort
paq_loop:   lda     ,x+
            lbsr    emit_a
            dec     ,s
            bne     paq_loop
paq_do_abort:
            leas    3,s              ; drop saved len (1) + flag (2)
            lds     #RSP_TOP
            ldu     #PSP_TOP
            ldx     #boot_code
            jmp     NEXT
paq_skip:   ldb     ,s               ; B = length
            clra
            leax    d,x              ; X += len (skip past string)
            leas    3,s              ; drop saved len + flag
            jmp     NEXT

            ; ABORT" IMMEDIATE — compile (ABORT") + len + chars
            ; At runtime, pops a flag: if non-zero, print and ABORT.
nfa_ABORTQ  fcb     F_IMMED|6
            fcc     "ABORT"
            fcb     $22              ; '"'
lnk_ABORTQ  fdb     prev_link
cfa_ABORTQ  fdb     code_ABORTQ
prev_link   set     nfa_ABORTQ
code_ABORTQ:
            pshs    x
            ldx     #TIB_ADDR
            ldd     var_TOIN
            leax    d,x              ; X = scan pointer (TIB + >IN)
            lda     ,x
            cmpa    #' '
            bne     abq_scan_start
            leax    1,x
abq_scan_start:
            stx     abq_start
            ldy     #TIB_ADDR
            ldd     var_TIBLEN
            leay    d,y
            sty     abq_end
abq_scan:   cmpx    abq_end
            bhs     abq_no_quote
            lda     ,x
            cmpa    #'"'
            beq     abq_got_quote
            leax    1,x
            bra     abq_scan
abq_got_quote:
            tfr     x,d
            subd    abq_start
            std     abq_len
            leax    1,x              ; past the closing quote
            bra     abq_update
abq_no_quote:
            tfr     x,d
            subd    abq_start
            std     abq_len
abq_update:
            tfr     x,d
            subd    #TIB_ADDR
            std     var_TOIN
            ldy     var_HERE
            ldx     #cfa_PABORTQ
            stx     ,y++
            ldb     abq_len+1
            stb     ,y+
            tstb
            beq     abq_compile_done
            ldx     abq_start
abq_copy:   lda     ,x+
            sta     ,y+
            decb
            bne     abq_copy
abq_compile_done:
            sty     var_HERE
            puls    x
            jmp     NEXT

abq_start   fdb     0
abq_end     fdb     0
abq_len     fdb     0

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

            ; FORTH ( -- )  the base vocabulary.  Executing it selects
            ; FORTH as the active search/define vocabulary.  PFA holds
            ; the vocab's latest-NFA cell (PFA+0) and parent pointer
            ; (PFA+2, NIL for the root FORTH vocab).
nfa_FORTH   fcb     5
            fcc     "FORTH"
lnk_FORTH   fdb     prev_link
cfa_FORTH   fdb     DOVOC
pfa_FORTH_LATEST fdb 0             ; initialised at cold to last builtin link
pfa_FORTH_PARENT fdb 0             ; root: no parent
prev_link   set     nfa_FORTH

            ; CONTEXT ( -- addr )  address of the search-vocab pointer cell.
nfa_CONTEXT fcb     7
            fcc     "CONTEXT"
lnk_CONTEXT fdb     prev_link
cfa_CONTEXT fdb     code_CONTEXT
prev_link   set     nfa_CONTEXT
code_CONTEXT: ldd   #var_LATEST_PTR
            std     ,--u
            jmp     NEXT

            ; CURRENT ( -- addr )  same as CONTEXT here — this kernel does
            ; not split CURRENT / CONTEXT because DOVOC switches both at
            ; once.  Provided for FORTH-83 API compatibility.
nfa_CURRENT fcb     7
            fcc     "CURRENT"
lnk_CURRENT fdb     prev_link
cfa_CURRENT fdb     code_CURRENT
prev_link   set     nfa_CURRENT
code_CURRENT: ldd   #var_LATEST_PTR
            std     ,--u
            jmp     NEXT

            ; DEFINITIONS ( -- )  no-op on this kernel (CURRENT == CONTEXT
            ; is always true).  Present for source-level compatibility.
nfa_DEFS    fcb     11
            fcc     "DEFINITIONS"
lnk_DEFS    fdb     prev_link
cfa_DEFS    fdb     code_DEFS
prev_link   set     nfa_DEFS
code_DEFS:  jmp     NEXT

            ; ONLY ( -- )  switch to the FORTH vocabulary.  A full ONLY
            ; would install a minimal ROOT vocab; our implementation
            ; collapses ONLY to a FORTH switch which is the closest
            ; reasonable approximation with a single namespace system.
nfa_ONLY    fcb     4
            fcc     "ONLY"
lnk_ONLY    fdb     prev_link
cfa_ONLY    fdb     code_ONLY
prev_link   set     nfa_ONLY
code_ONLY:  ; Write-back current var_LATEST to current vocab, then switch
            ; to FORTH (same sequence DOVOC would do).
            ldx     var_LATEST_PTR
            ldd     var_LATEST
            std     ,x
            ldx     #pfa_FORTH_LATEST
            stx     var_LATEST_PTR
            ldd     ,x
            std     var_LATEST
            jmp     NEXT

            ; VOCABULARY ( "name" -- )
            ; Create a new named vocabulary.  Header is linked into the
            ; active (CURRENT) vocab.  PFA layout: (latest=0, parent=
            ; current_vocab).  Executing the name later selects it.
nfa_VOC     fcb     10
            fcc     "VOCABULARY"
lnk_VOC     fdb     prev_link
cfa_VOC     fdb     code_VOC
prev_link   set     nfa_VOC
code_VOC:
            pshs    x                     ; save IP
            lbsr    parse_name_kernel     ; X=name addr, B=length
            cmpd    #0
            lbeq    voc_done
            stx     col_name_addr
            stb     col_name_len
            ldy     var_HERE
            sty     col_new_nfa
            stb     ,y+                   ; flags/length byte
            ldx     col_name_addr
voc_copy:   lda     ,x+
            sta     ,y+
            decb
            bne     voc_copy
            ldx     var_LATEST            ; link into the active vocab
            stx     ,y++
            ldx     #DOVOC
            stx     ,y++                  ; CFA = DOVOC
            ldx     #0
            stx     ,y++                  ; PFA+0 = 0 (empty vocab)
            ldx     var_LATEST_PTR
            stx     ,y++                  ; PFA+2 = parent pointer
            sty     var_HERE
            ldx     col_new_nfa
            stx     var_LATEST            ; new word is now latest
voc_done:   puls    x
            jmp     NEXT

last_builtin_link equ prev_link


; ---------------------------------------------------------------------------
; Reset vector
; ---------------------------------------------------------------------------
            org     $FFFE
            fdb     cold

            end     cold
