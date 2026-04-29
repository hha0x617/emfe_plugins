; SPDX-License-Identifier: MIT OR Apache-2.0
; Copyright (c) 2026 hha0x617
;
; ---------------------------------------------------------------------------
; Hha Lisp for MC6809 — reader + printer + evaluator + GC + TCO stdlib
;
; Runs under the emfe_plugin_mc6809 environment (MC6850 ACIA at $FF00/$FF01,
; 64 KB RAM).  Classic-Lisp style: defun / T / NIL / 'x .
;
; Value encoding (16-bit):
;   bit0=1: fixnum;       value = (word as i16) >> 1 (15-bit signed)
;   value=$0000: NIL
;   value=$0002: T
;   value in $3000..$5FFF: pair pointer (pool A, 4 bytes/cell)
;   value in $6000..$6FFF: symbol pointer (pool B, var-length)
;
; Phase 1 scope: REPL that reads an S-expression, prints it back,
; with atoms (fixnum/symbol), NIL, T, lists (a b c), and quote 'x → (QUOTE x).
; ---------------------------------------------------------------------------

ACIA_SR     equ     $FF00
ACIA_DATA   equ     $FF01

TIB_ADDR    equ     $A000
TIB_SIZE    equ     512             ; enlarged to fit long stdlib macro bodies

PAIR_POOL   equ     $4C80           ; pair pool start (after code+data)
PAIR_END    equ     $7000           ; exclusive — extends through what was
                                    ; SYM_POOL ($6800..$6FFF) and the address
                                    ; range previously reserved for builtin
                                    ; tag values ($7000..$7FFF).  See
                                    ; BUILTIN_POOL note below.  9088 bytes
                                    ; = 2272 cells, up from 1760.
SYM_POOL    equ     $7000           ; relocated from $6800 — now occupies the
                                    ; range that used to be the builtin tag
SYM_END     equ     $7E00           ; range ($7000..$7FFF).  3.5 KB = ~280 syms
                                    ; max (stdlib + typical session uses ~115).
BUILTIN_POOL equ    $7E00           ; built-in primitive tag values relocated
BUILTIN_END equ     $7E80           ; from $7000..$7FFF to $7E00..$7E7F to
                                    ; free 4 KB of pair / symbol headroom.
                                    ; Tag values are LOGICAL — no physical RAM
                                    ; lives in this range, so the move is just
                                    ; an arithmetic shift of every BI_* equ
                                    ; (mechanical, ABI-invisible to the host).
                                    ; 62 entries × 2 bytes = 124 bytes used;
                                    ; 128 bytes reserved for future primitives.
STR_POOL    equ     $9000           ; string pool
STR_END     equ     $A000           ; exclusive — 4 KB of string space
INT32_POOL  equ     $B000           ; 32-bit int box pool (for overflow fixnum)
INT32_END   equ     $C000           ; exclusive — 4 KB = 1024 boxes × 4 bytes
INT32_MARK  equ     $8C00           ; mark bitmap (1024 bits = 128 bytes)
INT32_MARK_END equ  $8C80
CHAR_BASE   equ     $8E00           ; char value range (tag = CHAR_BASE + 2*code)
CHAR_END    equ     $9000           ; exclusive (256 chars × 2-byte stride)
VEC_POOL    equ     $A200           ; vector pool (layout: [len 2B][elem 2B]*)
VEC_END     equ     $B000           ; exclusive — 3.5 KB (TIB grew to 512 B)

NIL_VAL     equ     $0000
T_VAL       equ     $0002

; Built-in IDs — each is BUILTIN_POOL + 2*N so the low bit is never set
; (otherwise print_expr would mis-classify the value as a fixnum).
BI_CONS     equ     $7E00
BI_CAR      equ     $7E02
BI_CDR      equ     $7E04
BI_ATOM     equ     $7E06
BI_EQ       equ     $7E08
BI_NULL     equ     $7E0A
BI_PLUS     equ     $7E0C
BI_MINUS    equ     $7E0E
BI_LT       equ     $7E10
BI_GC       equ     $7E12
BI_MUL      equ     $7E14
BI_CADR     equ     $7E16
BI_CADDR    equ     $7E18
BI_CDDR     equ     $7E1A
BI_LENGTH   equ     $7E1C
BI_APPEND   equ     $7E1E
BI_APPLY    equ     $7E20
BI_LIST     equ     $7E22
BI_PRINT    equ     $7E24
BI_NEWLINE  equ     $7E26
BI_ASSOC    equ     $7E28
BI_GENSYM   equ     $7E2A
BI_STRLEN   equ     $7E2C
BI_STREQ    equ     $7E2E
BI_STRAPP   equ     $7E30
BI_STRREF   equ     $7E32
BI_STR2L    equ     $7E34
BI_L2STR    equ     $7E36
BI_DIV      equ     $7E38
BI_MOD      equ     $7E3A
BI_ERROR    equ     $7E3C
BI_THROW    equ     $7E3E
BI_NUM_EQ   equ     $7E40
BI_CHAR_INT equ     $7E42
BI_INT_CHAR equ     $7E44
BI_CHARP    equ     $7E46
BI_MAKE_VEC equ     $7E48
BI_VEC_LEN  equ     $7E4A
BI_VEC_REF  equ     $7E4C
BI_VEC_SET  equ     $7E4E
BI_VEC_LIST equ     $7E50
BI_LIST_VEC equ     $7E52
BI_VECP     equ     $7E54
BI_LOGAND   equ     $7E56
BI_LOGIOR   equ     $7E58
BI_LOGXOR   equ     $7E5A
BI_LOGNOT   equ     $7E5C
BI_ASH      equ     $7E5E
BI_NUM2STR  equ     $7E60
BI_STR2NUM  equ     $7E62
BI_SYM2STR  equ     $7E64
BI_STR2SYM  equ     $7E66
BI_EVAL     equ     $7E68
BI_RDSTR    equ     $7E6A
BI_LOADMEM  equ     $7E6C
BI_DISPLAY  equ     $7E6E
BI_PUTCHAR  equ     $7E70
BI_RAND     equ     $7E72
BI_SEED     equ     $7E74
BI_TICK     equ     $7E76
BI_PCASE_GET equ    $7E78           ; (print-case) → 0 (upper) or 1 (lower)
BI_PCASE_SET equ    $7E7A           ; (set-print-case! n) → n  (n must be 0 or 1)

; Mark-sweep GC — 1-byte-per-pair mark table (simpler than a bitmap, and the
; memory map has 8 KB of unused space at $8000-$9FFF anyway).  PAIR_POOL..
; PAIR_END covers 3072 pairs.
MARK_TABLE  equ     $8000
MARK_TABLE_END equ  $8C00           ; 3072 bytes

RSP_TOP     equ     $FEFE

; --- cold-start ------------------------------------------------------------
            org     $0100
cold:
            lds     #RSP_TOP
            ; ACIA init: master reset + 8N1 / div-16 / no IRQ
            lda     #$03
            sta     ACIA_SR
            lda     #$15
            sta     ACIA_SR
            ; Greeting
            ldx     #msg_banner
            lbsr    puts_native
            ; Init heap pointers and symbol table.
            ldx     #PAIR_POOL
            stx     pair_next
            ldx     #SYM_POOL
            stx     sym_next
            ldx     #0
            stx     sym_list        ; head of symbol linked list
            stx     global_env      ; global environment starts empty (NIL)
            stx     current_env     ; lambda-local env starts empty (NIL)
            stx     free_list       ; GC free list empty
            ldx     #STR_POOL
            stx     str_next        ; string allocator
            ldx     #INT32_POOL
            stx     int32_next      ; 32-bit int box allocator
            ldx     #VEC_POOL
            stx     vec_next        ; vector allocator
            ldx     #NIL_VAL
            stx     int32_free      ; no freed boxes yet
            ldx     #catch_stack
            stx     catch_sp        ; catch handler stack pointer
            ; Seed the PRNG with a non-zero constant.  Callers can
            ; (seed n) to re-seed; (seed (tick)) works once tick is
            ; wired to the cycle counter.
            ldx     #$DEAD
            stx     rand_state
            ldx     #$BEEF
            stx     rand_state+2
            ; Pre-intern QUOTE / IF / DEFVAR so eval can recognise them
            ; without searching by name each time.
            ldx     #s_quote_lit
            lbsr    intern
            stx     sym_QUOTE
            ldx     #s_if_lit
            lbsr    intern
            stx     sym_IF
            ldx     #s_defvar_lit
            lbsr    intern
            stx     sym_DEFVAR
            ldx     #s_cons_lit
            lbsr    intern
            stx     sym_CONS
            ldx     #s_car_lit
            lbsr    intern
            stx     sym_CAR
            ldx     #s_cdr_lit
            lbsr    intern
            stx     sym_CDR
            ldx     #s_atom_lit
            lbsr    intern
            stx     sym_ATOM
            ldx     #s_eq_lit
            lbsr    intern
            stx     sym_EQ
            ldx     #s_null_lit
            lbsr    intern
            stx     sym_NULL
            ldx     #s_plus_lit
            lbsr    intern
            stx     sym_PLUS
            ldx     #s_minus_lit
            lbsr    intern
            stx     sym_MINUS
            ldx     #s_lt_lit
            lbsr    intern
            stx     sym_LT
            ldx     #s_lambda_lit
            lbsr    intern
            stx     sym_LAMBDA
            ldx     #s_defun_lit
            lbsr    intern
            stx     sym_DEFUN
            ldx     #s_cond_lit
            lbsr    intern
            stx     sym_COND
            ldx     #s_let_lit
            lbsr    intern
            stx     sym_LET
            ldx     #s_setq_lit
            lbsr    intern
            stx     sym_SETQ
            ldx     #s_setbang_lit      ; SET! is a Scheme-style alias for SETQ
            lbsr    intern
            stx     sym_SETBANG
            ldx     #s_gc_lit
            lbsr    intern
            stx     sym_GC
            ldx     #s_progn_lit
            lbsr    intern
            stx     sym_PROGN
            ldx     #s_and_lit
            lbsr    intern
            stx     sym_AND
            ldx     #s_or_lit
            lbsr    intern
            stx     sym_OR
            ldx     #s_mul_lit
            lbsr    intern
            stx     sym_MUL
            ldx     #s_cadr_lit
            lbsr    intern
            stx     sym_CADR
            ldx     #s_caddr_lit
            lbsr    intern
            stx     sym_CADDR
            ldx     #s_cddr_lit
            lbsr    intern
            stx     sym_CDDR
            ldx     #s_length_lit
            lbsr    intern
            stx     sym_LENGTH
            ldx     #s_append_lit
            lbsr    intern
            stx     sym_APPEND
            ldx     #s_letstar_lit
            lbsr    intern
            stx     sym_LETSTAR
            ldx     #s_letrec_lit
            lbsr    intern
            stx     sym_LETREC
            ldx     #s_apply_lit
            lbsr    intern
            stx     sym_APPLY
            ldx     #s_list_lit
            lbsr    intern
            stx     sym_LIST
            ldx     #s_eqsym_lit
            lbsr    intern
            stx     sym_EQSYM
            ldx     #s_print_lit
            lbsr    intern
            stx     sym_PRINT
            ldx     #s_newline_lit
            lbsr    intern
            stx     sym_NEWLINE
            ldx     #s_assoc_lit
            lbsr    intern
            stx     sym_ASSOC
            ldx     #s_defmacro_lit
            lbsr    intern
            stx     sym_DEFMACRO
            ldx     #s_macro_lit
            lbsr    intern
            stx     sym_MACRO
            ldx     #s_quasiquote_lit
            lbsr    intern
            stx     sym_QUASIQUOTE
            ldx     #s_unquote_lit
            lbsr    intern
            stx     sym_UNQUOTE
            ldx     #s_unqsplice_lit
            lbsr    intern
            stx     sym_UNQSPLICE
            ldx     #s_gensym_lit
            lbsr    intern
            stx     sym_GENSYM
            ldx     #s_strlen_lit
            lbsr    intern
            stx     sym_STRLEN
            ldx     #s_streq_lit
            lbsr    intern
            stx     sym_STREQ
            ldx     #s_strapp_lit
            lbsr    intern
            stx     sym_STRAPP
            ldx     #s_strref_lit
            lbsr    intern
            stx     sym_STRREF
            ldx     #s_str2l_lit
            lbsr    intern
            stx     sym_STR2L
            ldx     #s_l2str_lit
            lbsr    intern
            stx     sym_L2STR
            ldx     #s_div_lit
            lbsr    intern
            stx     sym_DIV
            ldx     #s_mod_lit
            lbsr    intern
            stx     sym_MOD
            ldx     #s_error_lit
            lbsr    intern
            stx     sym_ERROR
            ldx     #s_catch_lit
            lbsr    intern
            stx     sym_CATCH
            ldx     #s_throw_lit
            lbsr    intern
            stx     sym_THROW
            ldx     #s_char_int_lit
            lbsr    intern
            stx     sym_CHAR_INT
            ldx     #s_int_char_lit
            lbsr    intern
            stx     sym_INT_CHAR
            ldx     #s_charp_lit
            lbsr    intern
            stx     sym_CHARP
            ldx     #s_make_vec_lit
            lbsr    intern
            stx     sym_MAKE_VEC
            ldx     #s_vec_len_lit
            lbsr    intern
            stx     sym_VEC_LEN
            ldx     #s_vec_ref_lit
            lbsr    intern
            stx     sym_VEC_REF
            ldx     #s_vec_set_lit
            lbsr    intern
            stx     sym_VEC_SET
            ldx     #s_vec_list_lit
            lbsr    intern
            stx     sym_VEC_LIST
            ldx     #s_list_vec_lit
            lbsr    intern
            stx     sym_LIST_VEC
            ldx     #s_vecp_lit
            lbsr    intern
            stx     sym_VECP
            ldx     #s_logand_lit
            lbsr    intern
            stx     sym_LOGAND
            ldx     #s_logior_lit
            lbsr    intern
            stx     sym_LOGIOR
            ldx     #s_logxor_lit
            lbsr    intern
            stx     sym_LOGXOR
            ldx     #s_lognot_lit
            lbsr    intern
            stx     sym_LOGNOT
            ldx     #s_ash_lit
            lbsr    intern
            stx     sym_ASH
            ldx     #s_num2str_lit
            lbsr    intern
            stx     sym_NUM2STR
            ldx     #s_str2num_lit
            lbsr    intern
            stx     sym_STR2NUM
            ldx     #s_sym2str_lit
            lbsr    intern
            stx     sym_SYM2STR
            ldx     #s_str2sym_lit
            lbsr    intern
            stx     sym_STR2SYM
            ldx     #s_eval_lit
            lbsr    intern
            stx     sym_EVAL
            ldx     #s_rdstr_lit
            lbsr    intern
            stx     sym_RDSTR
            ldx     #s_loadmem_lit
            lbsr    intern
            stx     sym_LOADMEM
            ldx     #s_display_lit
            lbsr    intern
            stx     sym_DISPLAY
            ldx     #s_putchar_lit
            lbsr    intern
            stx     sym_PUTCHAR
            ldx     #s_rand_lit
            lbsr    intern
            stx     sym_RAND
            ldx     #s_seed_lit
            lbsr    intern
            stx     sym_SEED
            ldx     #s_tick_lit
            lbsr    intern
            stx     sym_TICK
            ldx     #s_pcase_get_lit
            lbsr    intern
            stx     sym_PCASE_GET
            ldx     #s_pcase_set_lit
            lbsr    intern
            stx     sym_PCASE_SET
            ; Bind primitives as first-class function values in global_env.
            ldy     sym_CONS
            ldd     #BI_CONS
            lbsr    bind_global
            ldy     sym_CAR
            ldd     #BI_CAR
            lbsr    bind_global
            ldy     sym_CDR
            ldd     #BI_CDR
            lbsr    bind_global
            ldy     sym_ATOM
            ldd     #BI_ATOM
            lbsr    bind_global
            ldy     sym_EQ
            ldd     #BI_EQ
            lbsr    bind_global
            ldy     sym_NULL
            ldd     #BI_NULL
            lbsr    bind_global
            ldy     sym_PLUS
            ldd     #BI_PLUS
            lbsr    bind_global
            ldy     sym_MINUS
            ldd     #BI_MINUS
            lbsr    bind_global
            ldy     sym_LT
            ldd     #BI_LT
            lbsr    bind_global
            ldy     sym_GC
            ldd     #BI_GC
            lbsr    bind_global
            ldy     sym_MUL
            ldd     #BI_MUL
            lbsr    bind_global
            ldy     sym_CADR
            ldd     #BI_CADR
            lbsr    bind_global
            ldy     sym_CADDR
            ldd     #BI_CADDR
            lbsr    bind_global
            ldy     sym_CDDR
            ldd     #BI_CDDR
            lbsr    bind_global
            ldy     sym_LENGTH
            ldd     #BI_LENGTH
            lbsr    bind_global
            ldy     sym_APPEND
            ldd     #BI_APPEND
            lbsr    bind_global
            ldy     sym_APPLY
            ldd     #BI_APPLY
            lbsr    bind_global
            ldy     sym_LIST
            ldd     #BI_LIST
            lbsr    bind_global
            ; `=` is numeric equality.  With int32 boxes in play, identity
            ; compare (EQ) would give wrong results for same-value boxes, so
            ; bind `=` to a dedicated value-aware primitive.
            ldy     sym_EQSYM
            ldd     #BI_NUM_EQ
            lbsr    bind_global
            ldy     sym_PRINT
            ldd     #BI_PRINT
            lbsr    bind_global
            ldy     sym_NEWLINE
            ldd     #BI_NEWLINE
            lbsr    bind_global
            ldy     sym_ASSOC
            ldd     #BI_ASSOC
            lbsr    bind_global
            ldy     sym_GENSYM
            ldd     #BI_GENSYM
            lbsr    bind_global
            ldy     sym_STRLEN
            ldd     #BI_STRLEN
            lbsr    bind_global
            ldy     sym_STREQ
            ldd     #BI_STREQ
            lbsr    bind_global
            ldy     sym_STRAPP
            ldd     #BI_STRAPP
            lbsr    bind_global
            ldy     sym_STRREF
            ldd     #BI_STRREF
            lbsr    bind_global
            ldy     sym_STR2L
            ldd     #BI_STR2L
            lbsr    bind_global
            ldy     sym_L2STR
            ldd     #BI_L2STR
            lbsr    bind_global
            ldy     sym_DIV
            ldd     #BI_DIV
            lbsr    bind_global
            ldy     sym_MOD
            ldd     #BI_MOD
            lbsr    bind_global
            ldy     sym_ERROR
            ldd     #BI_ERROR
            lbsr    bind_global
            ldy     sym_THROW
            ldd     #BI_THROW
            lbsr    bind_global
            ldy     sym_CHAR_INT
            ldd     #BI_CHAR_INT
            lbsr    bind_global
            ldy     sym_INT_CHAR
            ldd     #BI_INT_CHAR
            lbsr    bind_global
            ldy     sym_CHARP
            ldd     #BI_CHARP
            lbsr    bind_global
            ldy     sym_MAKE_VEC
            ldd     #BI_MAKE_VEC
            lbsr    bind_global
            ldy     sym_VEC_LEN
            ldd     #BI_VEC_LEN
            lbsr    bind_global
            ldy     sym_VEC_REF
            ldd     #BI_VEC_REF
            lbsr    bind_global
            ldy     sym_VEC_SET
            ldd     #BI_VEC_SET
            lbsr    bind_global
            ldy     sym_VEC_LIST
            ldd     #BI_VEC_LIST
            lbsr    bind_global
            ldy     sym_LIST_VEC
            ldd     #BI_LIST_VEC
            lbsr    bind_global
            ldy     sym_VECP
            ldd     #BI_VECP
            lbsr    bind_global
            ldy     sym_LOGAND
            ldd     #BI_LOGAND
            lbsr    bind_global
            ldy     sym_LOGIOR
            ldd     #BI_LOGIOR
            lbsr    bind_global
            ldy     sym_LOGXOR
            ldd     #BI_LOGXOR
            lbsr    bind_global
            ldy     sym_LOGNOT
            ldd     #BI_LOGNOT
            lbsr    bind_global
            ldy     sym_ASH
            ldd     #BI_ASH
            lbsr    bind_global
            ldy     sym_NUM2STR
            ldd     #BI_NUM2STR
            lbsr    bind_global
            ldy     sym_STR2NUM
            ldd     #BI_STR2NUM
            lbsr    bind_global
            ldy     sym_SYM2STR
            ldd     #BI_SYM2STR
            lbsr    bind_global
            ldy     sym_STR2SYM
            ldd     #BI_STR2SYM
            lbsr    bind_global
            ldy     sym_EVAL
            ldd     #BI_EVAL
            lbsr    bind_global
            ldy     sym_RDSTR
            ldd     #BI_RDSTR
            lbsr    bind_global
            ldy     sym_LOADMEM
            ldd     #BI_LOADMEM
            lbsr    bind_global
            ldy     sym_DISPLAY
            ldd     #BI_DISPLAY
            lbsr    bind_global
            ldy     sym_PUTCHAR
            ldd     #BI_PUTCHAR
            lbsr    bind_global
            ldy     sym_RAND
            ldd     #BI_RAND
            lbsr    bind_global
            ldy     sym_SEED
            ldd     #BI_SEED
            lbsr    bind_global
            ldy     sym_TICK
            ldd     #BI_TICK
            lbsr    bind_global
            ldy     sym_PCASE_GET
            ldd     #BI_PCASE_GET
            lbsr    bind_global
            ldy     sym_PCASE_SET
            ldd     #BI_PCASE_SET
            lbsr    bind_global
            ; Bootstrap Tier 1 + Tier 2 standard library from ROM-resident
            ; Lisp source strings.
            lbsr    load_stdlib
            ; Enter REPL.
            jmp     repl

; Y = symbol, D = value → prepend (sym . val) to global_env.
bind_global:
            lbsr    alloc_pair          ; X = (sym . val)
            tfr     x,y
            ldd     global_env
            lbsr    alloc_pair          ; X = (binding . global_env)
            stx     global_env
            rts

msg_banner  fcc     "Hha Lisp for MC6809"
            fcb     $0D,$0A
            fcc     "(c) 2026 hha0x617 - MIT/Apache-2.0"
            fcb     $0D,$0A,0

s_quote_lit fcb     5
            fcc     "QUOTE"
s_if_lit    fcb     2
            fcc     "IF"
s_defvar_lit fcb    6
            fcc     "DEFVAR"
s_cons_lit  fcb     4
            fcc     "CONS"
s_car_lit   fcb     3
            fcc     "CAR"
s_cdr_lit   fcb     3
            fcc     "CDR"
s_atom_lit  fcb     4
            fcc     "ATOM"
s_eq_lit    fcb     2
            fcc     "EQ"
s_null_lit  fcb     4
            fcc     "NULL"
s_plus_lit  fcb     1
            fcb     '+'
s_minus_lit fcb     1
            fcb     '-'
s_lt_lit    fcb     1
            fcb     '<'
s_lambda_lit fcb    6
            fcc     "LAMBDA"
s_defun_lit fcb     5
            fcc     "DEFUN"
s_cond_lit  fcb     4
            fcc     "COND"
s_let_lit   fcb     3
            fcc     "LET"
s_setq_lit  fcb     4
            fcc     "SETQ"
s_setbang_lit fcb   4
            fcc     "SET!"
s_gc_lit    fcb     2
            fcc     "GC"
s_progn_lit fcb     5
            fcc     "PROGN"
s_and_lit   fcb     3
            fcc     "AND"
s_or_lit    fcb     2
            fcc     "OR"
s_mul_lit   fcb     1
            fcb     '*'
s_cadr_lit  fcb     4
            fcc     "CADR"
s_caddr_lit fcb     5
            fcc     "CADDR"
s_cddr_lit  fcb     4
            fcc     "CDDR"
s_length_lit fcb    6
            fcc     "LENGTH"
s_append_lit fcb    6
            fcc     "APPEND"
s_letstar_lit fcb   4
            fcc     "LET*"
s_letrec_lit fcb    6
            fcc     "LETREC"
s_apply_lit fcb     5
            fcc     "APPLY"
s_list_lit  fcb     4
            fcc     "LIST"
s_eqsym_lit fcb     1
            fcb     '='
s_print_lit fcb     5
            fcc     "PRINT"
s_newline_lit fcb   7
            fcc     "NEWLINE"
s_assoc_lit fcb     5
            fcc     "ASSOC"
s_defmacro_lit fcb  8
            fcc     "DEFMACRO"
s_macro_lit fcb     5
            fcc     "MACRO"
s_quasiquote_lit fcb 10
            fcc     "QUASIQUOTE"
s_unquote_lit fcb   7
            fcc     "UNQUOTE"
s_unqsplice_lit fcb 16
            fcc     "UNQUOTE-SPLICING"
s_gensym_lit fcb    6
            fcc     "GENSYM"
s_strlen_lit fcb    13
            fcc     "STRING-LENGTH"
s_streq_lit fcb     7
            fcc     "STRING="
s_strapp_lit fcb    13
            fcc     "STRING-APPEND"
s_strref_lit fcb    10
            fcc     "STRING-REF"
s_str2l_lit fcb     12
            fcc     "STRING->LIST"
s_l2str_lit fcb     12
            fcc     "LIST->STRING"
s_div_lit   fcb     1
            fcb     '/'
s_mod_lit   fcb     3
            fcc     "MOD"
s_error_lit fcb     5
            fcc     "ERROR"
s_catch_lit fcb     5
            fcc     "CATCH"
s_throw_lit fcb     5
            fcc     "THROW"
s_char_int_lit fcb  13
            fcc     "CHAR->INTEGER"
s_int_char_lit fcb  13
            fcc     "INTEGER->CHAR"
s_charp_lit fcb     5
            fcc     "CHAR?"
s_make_vec_lit fcb  11
            fcc     "MAKE-VECTOR"
s_vec_len_lit fcb   13
            fcc     "VECTOR-LENGTH"
s_vec_ref_lit fcb   10
            fcc     "VECTOR-REF"
s_vec_set_lit fcb   11
            fcc     "VECTOR-SET!"
s_vec_list_lit fcb  12
            fcc     "VECTOR->LIST"
s_list_vec_lit fcb  12
            fcc     "LIST->VECTOR"
s_vecp_lit  fcb     7
            fcc     "VECTOR?"
s_logand_lit fcb    6
            fcc     "LOGAND"
s_logior_lit fcb    6
            fcc     "LOGIOR"
s_logxor_lit fcb    6
            fcc     "LOGXOR"
s_lognot_lit fcb    6
            fcc     "LOGNOT"
s_ash_lit   fcb     3
            fcc     "ASH"
s_num2str_lit fcb   14
            fcc     "NUMBER->STRING"
s_str2num_lit fcb   14
            fcc     "STRING->NUMBER"
s_sym2str_lit fcb   14
            fcc     "SYMBOL->STRING"
s_str2sym_lit fcb   14
            fcc     "STRING->SYMBOL"
s_eval_lit  fcb     4
            fcc     "EVAL"
s_rdstr_lit fcb     11
            fcc     "READ-STRING"
s_loadmem_lit fcb   11
            fcc     "LOAD-MEMORY"
s_display_lit fcb   7
            fcc     "DISPLAY"
s_rand_lit  fcb     4
            fcc     "RAND"
s_seed_lit  fcb     4
            fcc     "SEED"
s_tick_lit  fcb     4
            fcc     "TICK"
s_putchar_lit fcb   7
            fcc     "PUTCHAR"
s_pcase_get_lit fcb 10
            fcc     "PRINT-CASE"
s_pcase_set_lit fcb 15
            fcc     "SET-PRINT-CASE!"

; --- native helpers --------------------------------------------------------

; puts_native: X -> NUL-terminated string.  Destroys X, A, B.
puts_native:
            lda     ,x+
            beq     puts_done
pn_wait:    ldb     ACIA_SR
            bitb    #$02
            beq     pn_wait
            sta     ACIA_DATA
            bra     puts_native
puts_done:  rts

; puts_cased: X -> NUL-terminated string.  Like puts_native but routes
; each byte through emit_a_cased so the printer's case-mode fold
; applies.  Used for `T` / `NIL` / `#<CLOSURE>` / `#<MACRO>` /
; `#<BUILTIN>` so they follow the user's chosen case.  Destroys X, A,
; B.
puts_cased:
            lda     ,x+
            beq     puts_cased_done
            pshs    x
            lbsr    emit_a_cased
            puls    x
            bra     puts_cased
puts_cased_done:
            rts

; emit_a_cased: emit A as one char, applying the printer's case-mode
; fold.  If print_case_mode == 1 and A is in 'A'..'Z', it is folded to
; the corresponding lowercase letter before emit.  Other bytes pass
; through unchanged so digits / punctuation / `<` / `>` / `#` etc. are
; never touched.  Preserves A on return; uses B internally.
emit_a_cased:
            tst     print_case_mode
            beq     emit_a              ; mode 0 (upper / default) → no fold
            cmpa    #'A'
            blo     emit_a
            cmpa    #'Z'
            bhi     emit_a
            adda    #$20            ; 'a' - 'A' = 0x20
            ; Fall through to emit_a — but emit_a expects to return A
            ; intact.  We've already mutated A; that's fine because
            ; callers of emit_a_cased don't read A after.
; emit_a: emit A as one char.  Preserves A; uses B.
emit_a:     pshs    a
ea_wait:    ldb     ACIA_SR
            bitb    #$02
            beq     ea_wait
            puls    a
            sta     ACIA_DATA
            rts

; key_blocking: read one char from ACIA into A.  Blocks.  Echoes.
key_blocking:
            ldb     ACIA_SR
            bitb    #$01
            beq     key_blocking
            lda     ACIA_DATA
            rts

; emit_crlf: CR LF.
emit_crlf:  lda     #$0D
            bsr     emit_a
            lda     #$0A
            bsr     emit_a
            rts

; ---------------------------------------------------------------------------
; ACCEPT — read one line into TIB with echo + BS handling.
; Sets tib_len = count.  Returns when CR/LF received.
; ---------------------------------------------------------------------------
accept:
            ldx     #TIB_ADDR
            ldy     #0              ; count
acc_rx:     ldb     ACIA_SR
            bitb    #$01
            beq     acc_rx
            ldb     ACIA_DATA
            cmpb    #$0D
            beq     acc_end
            cmpb    #$0A
            beq     acc_end
            cmpb    #$08
            beq     acc_bs
            cmpb    #$7F
            beq     acc_bs
            cmpy    #TIB_SIZE
            bhs     acc_rx          ; full → drop
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
acc_end:    lbsr    emit_crlf
            sty     tib_len
            clr     tib_pos
            clr     tib_pos+1
            rts

; accept_append — extends TIB in place. Inserts $0A as a line separator,
; then reads chars from ACIA until CR/LF. Backspace is not allowed to erase
; past the original tib_len (the anchor). Used for multi-line continuation.
accept_append:
            ldy     tib_len             ; anchor (the original length)
            pshs    y
            ; X = TIB_ADDR + Y; insert LF separator if room.
            tfr     y,d
            addd    #TIB_ADDR
            tfr     d,x
            cmpy    #TIB_SIZE
            bhs     aca_rx
            lda     #$0A
            sta     ,x+
            leay    1,y
aca_rx:     ldb     ACIA_SR
            bitb    #$01
            beq     aca_rx
            ldb     ACIA_DATA
            cmpb    #$0D
            beq     aca_end
            cmpb    #$0A
            beq     aca_end
            cmpb    #$08
            beq     aca_bs
            cmpb    #$7F
            beq     aca_bs
            cmpy    #TIB_SIZE
            bhs     aca_rx
            stb     ,x+
            leay    1,y
            tfr     b,a
            lbsr    emit_a
            bra     aca_rx
aca_bs:     cmpy    ,s                  ; don't backspace past the anchor
            bls     aca_rx
            leax    -1,x
            leay    -1,y
            lda     #$08
            lbsr    emit_a
            lda     #' '
            lbsr    emit_a
            lda     #$08
            lbsr    emit_a
            bra     aca_rx
aca_end:    lbsr    emit_crlf
            sty     tib_len
            leas    2,s                 ; drop anchor
            rts

; scan_paren_balance: walks TIB from tib_pos..tib_len, returns D = net
; open-paren count (positive => need more input). Handles strings,
; #\char literals, and ';' line comments. Preserves tib_pos.
scan_paren_balance:
            ldd     tib_pos
            pshs    d
            ldd     #0
            std     sp_depth
sp_loop:    lbsr    tib_peek
            tsta
            lbeq    sp_done
            cmpa    #'('
            bne     sp_ck_rp
            ldd     sp_depth
            addd    #1
            std     sp_depth
            lbsr    tib_getc
            bra     sp_loop
sp_ck_rp:   cmpa    #')'
            bne     sp_ck_str
            ldd     sp_depth
            subd    #1
            std     sp_depth
            lbsr    tib_getc
            bra     sp_loop
sp_ck_str:  cmpa    #'"'
            bne     sp_ck_cmt
            lbsr    tib_getc
sp_in_str:  lbsr    tib_peek
            tsta
            lbeq    sp_done
            cmpa    #'"'
            beq     sp_str_close
            cmpa    #'\'
            bne     sp_str_ord
            lbsr    tib_getc
            lbsr    tib_peek
            tsta
            lbeq    sp_done
            lbsr    tib_getc
            lbra    sp_in_str
sp_str_ord: lbsr    tib_getc
            lbra    sp_in_str
sp_str_close:
            lbsr    tib_getc
            lbra    sp_loop
sp_ck_cmt:  cmpa    #';'
            bne     sp_ck_hash
            lbsr    tib_getc
sp_cmt_l:   lbsr    tib_peek
            tsta
            lbeq    sp_done
            cmpa    #$0A
            beq     sp_cmt_end
            cmpa    #$0D
            beq     sp_cmt_end
            lbsr    tib_getc
            bra     sp_cmt_l
sp_cmt_end: lbsr    tib_getc
            lbra    sp_loop
sp_ck_hash: cmpa    #'#'
            bne     sp_default
            lbsr    tib_getc            ; consume #
            lbsr    tib_peek
            cmpa    #'\'
            lbne    sp_loop
            lbsr    tib_getc            ; consume \
            lbsr    tib_peek
            tsta
            lbeq    sp_done
            lbsr    tib_getc            ; consume char literal
            lbra    sp_loop
sp_default: lbsr    tib_getc
            lbra    sp_loop
sp_done:    puls    d
            std     tib_pos
            ldd     sp_depth
            rts

sp_depth    fdb     0

; ---------------------------------------------------------------------------
; tib_peek:  A = next unread char (or 0 at end).  Does NOT consume.
; tib_getc:  A = next unread char (or 0), consume.
; tib_skip_ws: advance tib_pos over spaces/tabs.
; ---------------------------------------------------------------------------
tib_peek:   ldd     tib_pos
            cmpd    tib_len
            bhs     tib_peek_end
            ldx     rdr_base
            leax    d,x
            lda     ,x
            rts
tib_peek_end:
            clra
            rts

tib_getc:   bsr     tib_peek
            pshs    a
            ldd     tib_pos
            addd    #1
            std     tib_pos
            puls    a
            rts

tib_skip_ws:
            bsr     tib_peek
            cmpa    #' '
            beq     tib_ws_consume
            cmpa    #$09            ; TAB
            beq     tib_ws_consume
            cmpa    #$0A            ; LF (multi-line separator)
            beq     tib_ws_consume
            cmpa    #$0D            ; CR
            beq     tib_ws_consume
            cmpa    #';'            ; line comment
            beq     tib_ws_comment
            rts
tib_ws_consume:
            bsr     tib_getc
            bra     tib_skip_ws
tib_ws_comment:
            bsr     tib_getc        ; consume ';'
tib_ws_cloop:
            bsr     tib_peek
            tsta
            beq     tib_ws_cdone    ; end of buffer
            cmpa    #$0A            ; LF ends the comment
            beq     tib_ws_cconsume
            cmpa    #$0D            ; CR ends the comment
            beq     tib_ws_cconsume
            bsr     tib_getc
            bra     tib_ws_cloop
tib_ws_cconsume:
            bsr     tib_getc
            bra     tib_skip_ws
tib_ws_cdone:
            rts

; ---------------------------------------------------------------------------
; intern:  X -> counted string (length byte + chars).  Returns X = symbol ptr.
; Searches sym_list for matching name; if found returns existing entry.
; Otherwise allocates in SYM_POOL, links to list head.
;
; Symbol table entry layout (in SYM_POOL):
;   +0..+1   next-symbol pointer (NIL if last)
;   +2       name length byte
;   +3..+n   name bytes
; ---------------------------------------------------------------------------
intern:
            stx     in_name         ; save counted-string ptr
            ldx     sym_list        ; head of list
in_loop:    cmpx    #0
            beq     in_new
            ; Compare name at X+2..X+2+len with in_name+0..+len
            ldb     2,x             ; entry name length
            ldy     in_name
            cmpb    ,y              ; len match?
            bne     in_next
            ; Compare bytes
            leay    1,y             ; Y -> entry-side bytes of target
            pshs    x               ; save entry ptr
            leax    3,x             ; X -> entry name bytes
in_cmp:     tstb
            beq     in_match
            lda     ,y+
            cmpa    ,x+
            bne     in_nomatch
            decb
            bra     in_cmp
in_match:   puls    x               ; X = entry ptr
            rts                     ; found
in_nomatch:
            puls    x               ; restore entry
in_next:    ldx     ,x              ; next
            bra     in_loop
in_new:     ; Allocate at sym_next.
            ldx     sym_next
            stx     in_new_entry
            ; Write next pointer = current head.
            ldd     sym_list
            std     ,x++
            ; Write length + name bytes.
            ldy     in_name
            ldb     ,y+             ; len
            stb     ,x+             ; store len
            tstb
            beq     in_new_done
in_new_copy:
            lda     ,y+
            sta     ,x+
            decb
            bne     in_new_copy
in_new_done:
            ; Round sym_next up to even alignment so symbol pointers never
            ; collide with the fixnum low-bit tag.
            tfr     x,d
            andb    #1
            beq     in_align_done
            leax    1,x
in_align_done:
            stx     sym_next        ; update alloc ptr
            ldx     in_new_entry
            stx     sym_list
            rts

in_name     fdb     0
in_new_entry fdb    0

; ---------------------------------------------------------------------------
; alloc_pair ( car:y, cdr:d -> cell:x )
;   In:  Y = car value, D = cdr value
;   Out: X = pointer to new pair
; ---------------------------------------------------------------------------
alloc_pair:
            ; Push the caller's car/cdr onto the S stack so that an
            ; in-flight conservative stack scan inside gc_run_safe can mark
            ; them as live (Y = car, D = cdr — both may be pair pointers
            ; that the caller intends to install into the new cell).  The
            ; saved pair stays on the stack for the duration of alloc_pair
            ; and is popped on exit.
            pshs    y,d
ap_try:     ldx     free_list
            cmpx    #NIL_VAL
            beq     ap_bump
            ; Free-list path: head chained through car; new_free = car(X).
            pshs    d
            ldd     ,x
            std     free_list
            puls    d
            sty     ,x              ; car
            std     2,x             ; cdr
            clr     alloc_gc_tried
            puls    d,y             ; discard saved originals
            rts
ap_bump:    ldx     pair_next
            cmpx    #PAIR_END
            bhs     ap_try_gc
            sty     ,x++            ; car
            std     ,x++            ; cdr
            stx     pair_next       ; advance
            leax    -4,x            ; back to start of this cell
            clr     alloc_gc_tried
            puls    d,y
            rts
ap_try_gc:  ; Pool exhausted — one GC pass and retry.  The conservative
            ; stack scan in gc_run_safe picks up the pshs'd y,d (so the
            ; caller's live car/cdr survive), plus every ev_* scratch.
            tst     alloc_gc_tried
            bne     ap_oom
            inc     alloc_gc_tried
            lbsr    gc_run_safe
            ldd     ,s              ; refetch cdr from saved slot
            ldy     2,s             ; refetch car
            bra     ap_try
ap_oom:     leas    4,s             ; drop saved y,d
            ldx     #str_oom
            lbsr    puts_native
            lbsr    emit_crlf
ap_hang:    bra     ap_hang         ; unrecoverable — hang the VM

alloc_gc_tried fcb  0

; ---------------------------------------------------------------------------
; READ — parse one S-expression from TIB.
; Returns X = tagged value.
; ---------------------------------------------------------------------------
read_expr:
            lbsr    tib_skip_ws
            lbsr    tib_peek
            tsta
            lbeq    read_eof        ; empty line → NIL for now
            cmpa    #'('
            lbeq    read_list
            cmpa    #''             ; ' (apostrophe)
            lbeq    read_quote
            cmpa    #'`'             ; backtick → (QUASIQUOTE x)
            lbeq    read_quasiquote
            cmpa    #','             ; , → (UNQUOTE x), ,@ → (UNQUOTE-SPLICING x)
            lbeq    read_unquote
            cmpa    #'"'             ; "...": string literal
            lbeq    read_string
            cmpa    #'#'             ; #\c: character literal
            lbeq    read_hash
            cmpa    #'-'
            lbeq    read_minus_or_sym
            ; Is it a digit?
            cmpa    #'0'
            lblo    read_sym_entry
            cmpa    #'9'
            lbhi    read_sym_entry
            lbra    read_number

read_eof:   ldx     #NIL_VAL
            rts

; -- ( read: list ) -- consume '(' and read until ')' --
; Recursive-safe: saves the previous head/tail on S before overwriting the
; rl_head / rl_tail_slot globals, and restores them on exit.
read_list:
            lbsr    tib_getc        ; consume '('
            ; Save caller's head/tail globals on S (for nested-list support).
            ldd     rl_head
            pshs    d
            ldd     rl_tail_slot
            pshs    d
            ; Initialize: head = NIL, tail_slot = &rl_head.
            ldd     #NIL_VAL
            std     rl_head
            ldd     #rl_head
            std     rl_tail_slot
rl_loop:    lbsr    tib_skip_ws
            lbsr    tib_peek
            cmpa    #')'
            beq     rl_end
            tsta
            beq     rl_end
            cmpa    #'.'
            lbeq    rl_maybe_dot
rl_elem:    lbsr    read_expr       ; X = elem
            tfr     x,y             ; Y = elem
            ldd     #NIL_VAL
            lbsr    alloc_pair      ; X = new pair (elem . NIL)
            ; *tail_slot = new_pair
            ldy     rl_tail_slot
            stx     ,y
            ; tail_slot = &new_pair.cdr
            leax    2,x
            stx     rl_tail_slot
            bra     rl_loop
rl_maybe_dot:
            ; A `.` is a dotted-pair marker only when followed by a
            ; delimiter (whitespace, tab, paren).  Otherwise it is part of
            ; a symbol name (e.g., `.foo` or `a.b` — our reader already
            ; includes `.` in symbol chars).
            ldd     tib_pos
            addd    #1
            cmpd    tib_len
            bhs     rl_elem         ; `.` at EOL → treat as symbol
            ldx     rdr_base
            leax    d,x
            lda     ,x
            cmpa    #' '
            beq     rl_dot
            cmpa    #$09
            beq     rl_dot
            cmpa    #'('
            beq     rl_dot
            cmpa    #')'
            beq     rl_dot
            bra     rl_elem
rl_dot:     lbsr    tib_getc        ; consume '.'
            lbsr    read_expr       ; X = cdr expression
            ldy     rl_tail_slot    ; slot where cdr goes
            stx     ,y
            lbsr    tib_skip_ws     ; allow whitespace before ')'
            bra     rl_end
rl_end:     lbsr    tib_getc        ; consume ')'  (no-op at EOF)
            ldx     rl_head         ; final list head
            ; Restore caller's globals.
            puls    d
            std     rl_tail_slot
            puls    d
            std     rl_head
            rts

rl_head     fdb     0
rl_tail_slot fdb    0

; -- ( read: quote shorthand ) -- 'x → (QUOTE x) --
read_quote:
            lbsr    tib_getc        ; consume '
            lbsr    read_expr       ; x (tagged value in X)
            ; Build (x . NIL) then (QUOTE . that)
            tfr     x,y             ; Y = x
            ldd     #NIL_VAL
            lbsr    alloc_pair      ; (x . NIL)
            tfr     x,d             ; D = that
            ldy     sym_QUOTE       ; Y = QUOTE symbol
            lbsr    alloc_pair
            rts

; -- ( read: `x → (QUASIQUOTE x) ) --
read_quasiquote:
            lbsr    tib_getc        ; consume `
            lbsr    read_expr
            tfr     x,y
            ldd     #NIL_VAL
            lbsr    alloc_pair      ; (x . NIL)
            tfr     x,d
            ldy     sym_QUASIQUOTE
            lbsr    alloc_pair
            rts

; -- ( read: ,x → (UNQUOTE x) / ,@x → (UNQUOTE-SPLICING x) ) --
read_unquote:
            lbsr    tib_getc        ; consume ,
            lbsr    tib_peek
            cmpa    #'@'
            beq     read_unqsplice
            lbsr    read_expr
            tfr     x,y
            ldd     #NIL_VAL
            lbsr    alloc_pair
            tfr     x,d
            ldy     sym_UNQUOTE
            lbsr    alloc_pair
            rts
read_unqsplice:
            lbsr    tib_getc        ; consume @
            lbsr    read_expr
            tfr     x,y
            ldd     #NIL_VAL
            lbsr    alloc_pair
            tfr     x,d
            ldy     sym_UNQSPLICE
            lbsr    alloc_pair
            rts

; "..." — read characters between quotes into a fresh string.  Supports
; simple escape sequences: \" \\ \n \t.  Uses Y as destination because
; tib_peek / tib_getc clobber X.
; -- ( read: # — currently only #\c character literals ) --
read_hash:  lbsr    tib_getc        ; consume '#'
            lbsr    tib_peek
            cmpa    #'\'
            beq     read_char_lit
            ; unknown #-syntax
            ldx     #NIL_VAL
            rts
read_char_lit:
            lbsr    tib_getc        ; consume '\'
            lbsr    tib_peek
            tsta
            lbeq    read_eof
            lbsr    tib_getc        ; A = char
            tfr     a,b
            clra
            lslb
            rola                    ; D = code * 2
            addd    #CHAR_BASE
            tfr     d,x
            rts

read_string:
            lbsr    tib_getc        ; consume opening "
            ldy     str_next
            sty     rs_start
            leay    1,y             ; past length slot
            clr     rs_count        ; byte counter in memory (D is clobbered by tib_*)
rs_sloop:   lbsr    tib_peek
            tsta
            beq     rs_sdone
            cmpa    #'"'
            beq     rs_send
            cmpa    #'\'
            beq     rs_escape
            lbsr    tib_getc
            sta     ,y+
            inc     rs_count
            bra     rs_sloop
rs_escape:  lbsr    tib_getc        ; consume backslash
            lbsr    tib_peek
            tsta
            beq     rs_sdone
            cmpa    #'n'
            beq     rs_esc_n
            cmpa    #'t'
            beq     rs_esc_t
            ; default: literal (\" → " , \\ → \)
            lbsr    tib_getc
            sta     ,y+
            inc     rs_count
            bra     rs_sloop
rs_esc_n:   lbsr    tib_getc
            lda     #$0A
            sta     ,y+
            inc     rs_count
            bra     rs_sloop
rs_esc_t:   lbsr    tib_getc
            lda     #$09
            sta     ,y+
            inc     rs_count
            bra     rs_sloop
rs_send:    lbsr    tib_getc        ; consume closing "
rs_sdone:   ldx     rs_start
            ldb     rs_count
            stb     ,x              ; length byte
            sty     str_next
            lbsr    align_str_next
            rts

; Strings must start at an even address because a pointer with bit 0 set
; would be mistaken for a fixnum.  Round str_next up to the next even byte.
align_str_next:
            ldd     str_next
            bitb    #1
            beq     asn_done
            addd    #1
            std     str_next
asn_done:   rts

rs_start    fdb     0
rs_count    fcb     0

; -- ( read: '-' could start number or symbol ) --
read_minus_or_sym:
            ; Peek next-next char: if digit, treat as negative number.
            ldd     tib_pos
            addd    #1
            cmpd    tib_len
            lbhs    read_sym_entry  ; '-' alone → symbol
            ldx     rdr_base
            leax    d,x
            lda     ,x
            cmpa    #'0'
            lblo    read_sym_entry
            cmpa    #'9'
            lbhi    read_sym_entry
            lbra    read_number

; -- ( read: number ) -- decimal integer, optional leading '-'.  Parses
; the full 32-bit value, then returns a fixnum if it fits in 15-bit signed
; range or an int32 box otherwise.
read_number:
            clr     rn_sign
            lbsr    tib_peek
            cmpa    #'-'
            bne     rn_digits
            lbsr    tib_getc
            inc     rn_sign
rn_digits:  ldd     #0
            std     i32_a
            std     i32_a+2
rn_loop:    lbsr    tib_peek
            cmpa    #'0'
            blo     rn_done
            cmpa    #'9'
            bhi     rn_done
            lbsr    tib_getc
            suba    #'0'
            pshs    a                   ; save digit
            lbsr    i32_mul10
            puls    a
            ; i32_a += digit (low word += digit, carry propagated)
            tfr     a,b
            clra
            addd    i32_a+2
            std     i32_a+2
            ldd     i32_a
            adcb    #0
            adca    #0
            std     i32_a
            bra     rn_loop
rn_done:    tst     rn_sign
            beq     rn_pos
            ; Negate i32_a.
            com     i32_a
            com     i32_a+1
            com     i32_a+2
            com     i32_a+3
            ldd     i32_a+2
            addd    #1
            std     i32_a+2
            ldd     i32_a
            adcb    #0
            adca    #0
            std     i32_a
rn_pos:     ; Copy into i32_res and let fit_and_box choose fixnum vs box.
            ldd     i32_a
            std     i32_res
            ldd     i32_a+2
            std     i32_res+2
            lbra    fit_and_box

; i32_a = i32_a * 10 (in place).  Uses i32_b as scratch.
i32_mul10:  ldd     i32_a
            std     i32_b
            ldd     i32_a+2
            std     i32_b+2
            ; i32_a <<= 3 (×8)
            asl     i32_a+3
            rol     i32_a+2
            rol     i32_a+1
            rol     i32_a
            asl     i32_a+3
            rol     i32_a+2
            rol     i32_a+1
            rol     i32_a
            asl     i32_a+3
            rol     i32_a+2
            rol     i32_a+1
            rol     i32_a
            ; i32_b <<= 1 (×2)
            asl     i32_b+3
            rol     i32_b+2
            rol     i32_b+1
            rol     i32_b
            ; i32_a += i32_b
            lda     i32_a+3
            adda    i32_b+3
            sta     i32_a+3
            lda     i32_a+2
            adca    i32_b+2
            sta     i32_a+2
            lda     i32_a+1
            adca    i32_b+1
            sta     i32_a+1
            lda     i32_a
            adca    i32_b
            sta     i32_a
            rts

rn_sign     fcb     0

; Y = Y * 10, clobbers D.
mul10_y:    tfr     y,d
            aslb
            rola                    ; D = Y*2
            pshs    d
            aslb
            rola
            aslb
            rola                    ; D = Y*8
            addd    ,s++
            tfr     d,y
            rts

; -- ( read: symbol ) -- alphanumeric + punct until whitespace/()/eof --
read_sym_entry:
read_symbol:
            ; Collect chars into sym_buf (1 byte count + up to 31 chars).
            ; Use Y as the write pointer — tib_peek / tib_getc clobber X.
            clr     sym_buf
            ldy     #sym_buf+1
rs_loop:    lbsr    tib_peek
            tsta
            beq     rs_done
            cmpa    #' '
            beq     rs_done
            cmpa    #$09
            beq     rs_done
            cmpa    #$0A            ; LF (multi-line separator)
            beq     rs_done
            cmpa    #$0D            ; CR
            beq     rs_done
            cmpa    #'('
            beq     rs_done
            cmpa    #')'
            beq     rs_done
            lbsr    tib_getc
            ; Upcase a-z (classic-Lisp convention: symbols are case-insensitive,
            ; canonical form is uppercase).
            cmpa    #'a'
            blo     rs_store
            cmpa    #'z'
            bhi     rs_store
            suba    #'a'-'A'
rs_store:   sta     ,y+
            inc     sym_buf
            lda     sym_buf
            cmpa    #31
            blo     rs_loop
rs_done:    ldx     #sym_buf
            lbsr    intern
            ; Intern returns X = symbol pointer in SYM_POOL (even address).
            ; Check for literal NIL / T.
            ldb     2,x             ; len
            cmpb    #3
            bne     rs_check_t
            ldy     3,x
            cmpy    #$4E49          ; "NI"
            bne     rs_check_t
            ldb     5,x
            cmpb    #'L'
            bne     rs_check_t
            ldx     #NIL_VAL
            rts
rs_check_t: cmpb    #1
            bne     rs_done_ret
            ldb     3,x
            cmpb    #'T'
            bne     rs_done_ret
            ldx     #T_VAL
rs_done_ret:
            rts

sym_buf     fcb     0
            rmb     31

; ---------------------------------------------------------------------------
; PRINT — emit a tagged value as text.  X = value.
; ---------------------------------------------------------------------------
print_expr:
            lda     #1
            sta     pr_quote
            bra     pe_body
; display_expr: like print_expr, but strings print without surrounding quotes
; and chars print without the `#\\` prefix.  Used by the FORMAT family.
display_expr:
            clr     pr_quote
pe_body:    stx     pr_val
            ldd     pr_val
            ; Fixnum?  (bit 0 == 1)
            andb    #1
            beq     pr_not_fix
            lbra    pr_fixnum
pr_not_fix: ldd     pr_val
            cmpd    #NIL_VAL
            lbeq    pr_nil
            cmpd    #T_VAL
            lbeq    pr_t
            cmpd    #PAIR_POOL
            lblo    pr_symbol_or_other
            cmpd    #PAIR_END
            lblo    pr_pair
pr_symbol_or_other:
            ldd     pr_val
            cmpd    #SYM_POOL
            lblo    pr_unknown
            cmpd    #SYM_END
            lblo    pr_symbol
            cmpd    #BUILTIN_END
            lblo    pr_builtin
            cmpd    #CHAR_BASE
            lblo    pr_unknown
            cmpd    #CHAR_END
            lblo    pr_char
            cmpd    #STR_POOL
            lblo    pr_unknown
            cmpd    #STR_END
            lblo    pr_string
            cmpd    #VEC_POOL
            lblo    pr_unknown
            cmpd    #VEC_END
            lblo    pr_vector
            cmpd    #INT32_POOL
            lblo    pr_unknown
            cmpd    #INT32_END
            lblo    pr_int32
pr_unknown: ; Print "#<?xxxx>"
            lda     #'#'
            lbsr    emit_a
            lda     #'<'
            lbsr    emit_a
            lda     #'?'
            lbsr    emit_a
            lda     pr_val
            lbsr    emit_hex2
            lda     pr_val+1
            lbsr    emit_hex2
            lda     #'>'
            lbsr    emit_a
            rts

pr_nil:     ldx     #str_nil
            bra     pr_puts
pr_t:       ldx     #str_t
pr_puts:    lbsr    puts_cased
            rts

str_nil     fcc     "NIL"
            fcb     0
str_t       fcc     "T"
            fcb     0

pr_fixnum:
            ; value = (pr_val as i16) >> 1
            ldd     pr_val
            asra                    ; arithmetic shift A right
            rorb
            lbsr    print_signed
            rts

; Print 16-bit signed integer in D as decimal.  Uses pd_buf scratch.
print_signed:
            cmpd    #0
            bge     ps_nonneg
            pshs    d
            lda     #'-'
            lbsr    emit_a
            ldd     ,s++
            coma
            comb
            addd    #1
ps_nonneg:  ldy     #pd_buf_end
ps_div:     pshs    x
            ldx     #0
ps_sub10:   cmpd    #10
            blo     ps_sub10_done
            subd    #10
            leax    1,x
            bra     ps_sub10
ps_sub10_done:
            addb    #'0'
            leay    -1,y
            stb     ,y
            tfr     x,d
            puls    x
            cmpd    #0
            bne     ps_div
ps_emit:    cmpy    #pd_buf_end
            bhs     ps_done
            lda     ,y+
            lbsr    emit_a
            bra     ps_emit
ps_done:    rts

pd_buf      fcb     0,0,0,0,0,0
pd_buf_end

; Print two-hex-digit byte in A.
emit_hex2:  pshs    a
            lsra
            lsra
            lsra
            lsra
            lbsr    emit_hex1
            puls    a
            anda    #$0F
emit_hex1:  cmpa    #10
            blt     eh_dig
            adda    #'A'-10
            lbra    emit_a
eh_dig:     adda    #'0'
            lbra    emit_a

pr_symbol:
            ; X = symbol entry, name at +2 (len) / +3 (bytes).  Emit
            ; each name byte through emit_a_cased so the active case
            ; mode (upper / lower) is applied.
            ldx     pr_val
            ldb     2,x             ; len
            leay    3,x             ; ptr to name bytes
ps_sym_loop:
            tstb
            beq     ps_sym_done
            lda     ,y+
            pshs    b
            lbsr    emit_a_cased
            puls    b
            decb
            bra     ps_sym_loop
ps_sym_done:
            rts

pr_pair:
            ldx     pr_val
            ldy     ,x              ; car of value
            cmpy    sym_LAMBDA
            beq     pr_closure
            cmpy    sym_MACRO
            beq     pr_macro_tag
            lda     #'('
            lbsr    emit_a
            ldx     pr_val
pp_elem:    ldy     ,x              ; car
            pshs    x
            stx     pp_save
            tfr     y,x
            lbsr    print_expr
            ldx     pp_save
            puls    x
            ldd     2,x             ; cdr
            cmpd    #NIL_VAL
            beq     pp_end
            ; Proper list? If cdr is a pair, print space + continue.
            cmpd    #PAIR_POOL
            blo     pp_dotted
            cmpd    #PAIR_END
            bhs     pp_dotted
            lda     #' '
            lbsr    emit_a
            ldx     2,x             ; advance to next pair
            stx     pr_val
            bra     pp_elem
pp_dotted:  ; Improper list: " . cdr"
            lda     #' '
            lbsr    emit_a
            lda     #'.'
            lbsr    emit_a
            lda     #' '
            lbsr    emit_a
            ldd     2,x
            std     pr_val
            ldx     pr_val
            lbsr    print_expr
pp_end:     lda     #')'
            lbsr    emit_a
            rts

pr_closure: ldx     #str_closure
            lbsr    puts_cased
            rts

pr_macro_tag:
            ldx     #str_macrotag
            lbsr    puts_cased
            rts

pr_builtin: ldx     #str_builtin
            lbsr    puts_cased
            rts

; Print int32 box as signed decimal.  pr_val = box pointer.
pr_int32:   ldx     pr_val
            ldd     ,x              ; high word
            std     pi_hi
            ldd     2,x             ; low word
            std     pi_lo
            lda     pi_hi
            bpl     pi_pos_ok
            ; Negative: emit '-' and two's-complement in place.
            lda     #'-'
            lbsr    emit_a
            ldd     pi_lo
            coma
            comb
            std     pi_lo
            ldd     pi_hi
            coma
            comb
            std     pi_hi
            ldd     pi_lo
            addd    #1
            std     pi_lo
            ldd     pi_hi
            adcb    #0
            adca    #0
            std     pi_hi
pi_pos_ok:  ldy     #pi_buf_end
pi_loop:    lbsr    pi_div10         ; remainder in B, updates pi_hi:pi_lo
            addb    #'0'
            leay    -1,y
            stb     ,y
            ldd     pi_hi
            bne     pi_loop
            ldd     pi_lo
            bne     pi_loop
pi_emit:    cmpy    #pi_buf_end
            bhs     pi_done
            lda     ,y+
            lbsr    emit_a
            bra     pi_emit
pi_done:    rts

; Divide pi_hi:pi_lo by 10 in-place.  Returns remainder in B.
; Shift-subtract algorithm: 32 iterations.
pi_div10:   ldd     #0
            std     pi_tmp
            lda     #32
            sta     pi_cnt
pd10_loop:  ldd     pi_lo
            lslb
            rola
            std     pi_lo
            ldd     pi_hi
            rolb
            rola
            std     pi_hi
            ldd     pi_tmp
            rolb
            rola
            std     pi_tmp
            cmpd    #10
            blo     pd10_skip
            subd    #10
            std     pi_tmp
            ldd     pi_lo
            orb     #1
            std     pi_lo
pd10_skip:  dec     pi_cnt
            bne     pd10_loop
            ldb     pi_tmp+1
            rts

pi_hi       fdb     0
pi_lo       fdb     0
pi_tmp      fdb     0
pi_cnt      fcb     0
pi_buf      fcb     0,0,0,0,0,0,0,0,0,0,0
pi_buf_end

; Print string: emit `"` + content bytes + `"`.  X = string pointer.
; If pr_quote is 0 (display_expr), the surrounding `"` are suppressed.
pr_string:  lda     pr_quote
            beq     ps_nobegin
            lda     #'"'
            lbsr    emit_a
ps_nobegin: ldx     pr_val
            ldb     ,x              ; length byte
            leay    1,x             ; point at first byte
ps_sloop:   tstb
            beq     ps_sdone
            pshs    b
            lda     ,y+
            lbsr    emit_a
            puls    b
            decb
            bra     ps_sloop
ps_sdone:   lda     pr_quote
            beq     ps_noend
            lda     #'"'
            lbsr    emit_a
ps_noend:   rts

; Print character as "#\c".  pr_val = CHAR_BASE + 2*code.
; If pr_quote is 0 (display_expr), only the underlying byte is emitted.
pr_char:    lda     pr_quote
            beq     pc_body
            lda     #'#'
            lbsr    emit_a
            lda     #'\'
            lbsr    emit_a
pc_body:    ldd     pr_val
            subd    #CHAR_BASE
            lsra
            rorb                    ; D = code
            tfr     b,a
            lbsr    emit_a
            rts

; Print vector as #(e0 e1 ... eN-1).  pr_val = vector pointer.
pr_vector:  lda     #'#'
            lbsr    emit_a
            lda     #'('
            lbsr    emit_a
            ldx     pr_val
            ldd     ,x              ; length
            std     pv_count
            lbeq    pv_end
            leax    2,x             ; point at elem 0
            stx     pv_walk
pv_loop:    ldx     pv_walk
            ldx     ,x              ; element value
            lbsr    print_expr
            ldx     pv_walk
            leax    2,x
            stx     pv_walk
            ldd     pv_count
            subd    #1
            std     pv_count
            beq     pv_end
            lda     #' '
            lbsr    emit_a
            bra     pv_loop
pv_end:     lda     #')'
            lbsr    emit_a
            rts

pv_walk     fdb     0
pv_count    fdb     0

str_closure  fcc    "#<CLOSURE>"
             fcb    0
str_macrotag fcc    "#<MACRO>"
             fcb    0
str_builtin  fcc    "#<BUILTIN>"
             fcb    0

pr_val      fdb     0
pr_quote    fcb     1
pp_save     fdb     0

; ---------------------------------------------------------------------------
; EVAL — evaluate one tagged expression.
;   In : X = expr
;   Out: X = result
; Handles: self-eval (fixnum/NIL/T), symbol lookup (global_env), and the
; special forms QUOTE / IF / DEFVAR.  Unknown operators print an error and
; return NIL; so do unbound symbols.
; ---------------------------------------------------------------------------
eval:
            stx     ev_expr_scratch
            ldd     ev_expr_scratch
            andb    #1
            bne     ev_self
            ldd     ev_expr_scratch
            cmpd    #NIL_VAL
            beq     ev_self
            cmpd    #T_VAL
            beq     ev_self
            cmpd    #PAIR_POOL
            lblo    ev_maybe_sym
            cmpd    #PAIR_END
            lblo    ev_form
ev_maybe_sym:
            ldd     ev_expr_scratch
            cmpd    #SYM_POOL
            lblo    ev_bad
            cmpd    #SYM_END
            lblo    ev_lookup
            cmpd    #BUILTIN_END
            lblo    ev_self         ; builtin values are self-evaluating
            cmpd    #CHAR_BASE
            lblo    ev_bad
            cmpd    #CHAR_END
            lblo    ev_self         ; char values are self-evaluating
            cmpd    #STR_POOL
            lblo    ev_bad
            cmpd    #STR_END
            lblo    ev_self         ; strings are self-evaluating
            cmpd    #VEC_POOL
            lblo    ev_bad
            cmpd    #VEC_END
            lblo    ev_self         ; vectors are self-evaluating
            cmpd    #INT32_POOL
            lblo    ev_bad
            cmpd    #INT32_END
            lblo    ev_self         ; int32 boxes are self-evaluating
            lbra    ev_bad
ev_self:    ldx     ev_expr_scratch
            rts
ev_bad:     ldx     #str_eval_err
            lbsr    puts_native
            ldx     ev_expr_scratch
            lbsr    print_expr
            lbsr    emit_crlf
            ldx     #NIL_VAL
            rts

; -- symbol lookup in global_env --
ev_lookup:
            ldx     ev_expr_scratch
            stx     ev_lk_target
            ldy     current_env     ; search local env first
ev_lk_loop: cmpy    #NIL_VAL
            beq     ev_lk_try_global
            ldx     ,y              ; X = binding pair (sym . value)
            ldx     ,x              ; X = sym of binding
            cmpx    ev_lk_target
            beq     ev_lk_found
            ldy     2,y             ; cdr of env list
            bra     ev_lk_loop
ev_lk_try_global:
            ldy     global_env
ev_lk_loop2: cmpy   #NIL_VAL
            beq     ev_lk_unbound
            ldx     ,y
            ldx     ,x
            cmpx    ev_lk_target
            beq     ev_lk_found
            ldy     2,y
            bra     ev_lk_loop2
ev_lk_found:
            ldx     ,y              ; X = binding pair
            ldx     2,x             ; X = value
            rts
ev_lk_unbound:
            ldx     #str_unbound
            lbsr    puts_native
            ldx     ev_lk_target
            lbsr    print_expr
            lbsr    emit_crlf
            ldx     #NIL_VAL
            rts

ev_lk_target fdb    0

; -- pair → form dispatch --
ev_form:
            ldx     ev_expr_scratch ; X = expr (pair)
            ldy     ,x              ; Y = operator = car(expr)
            cmpy    sym_QUOTE
            lbeq    ev_quote
            cmpy    sym_IF
            lbeq    ev_if
            cmpy    sym_DEFVAR
            lbeq    ev_defvar
            cmpy    sym_LAMBDA
            lbeq    ev_lambda
            cmpy    sym_DEFUN
            lbeq    ev_defun
            cmpy    sym_COND
            lbeq    ev_cond
            cmpy    sym_LET
            lbeq    ev_let
            cmpy    sym_SETQ
            lbeq    ev_setq
            cmpy    sym_SETBANG         ; (set! sym val) — Scheme-style alias for setq
            lbeq    ev_setq
            cmpy    sym_PROGN
            lbeq    ev_progn
            cmpy    sym_AND
            lbeq    ev_and
            cmpy    sym_OR
            lbeq    ev_or
            cmpy    sym_LETSTAR
            lbeq    ev_let_star
            cmpy    sym_LETREC
            lbeq    ev_letrec
            cmpy    sym_DEFMACRO
            lbeq    ev_defmacro
            cmpy    sym_QUASIQUOTE
            lbeq    ev_quasiquote
            cmpy    sym_CATCH
            lbeq    ev_catch
            ; Not a special form — evaluate operator as a function value and
            ; apply it.
            lbra    ev_apply

; -- (QUOTE X) → X --
ev_quote:   ldx     ev_expr_scratch
            ldx     2,x             ; cdr
            ldx     ,x              ; car(cdr) = X
            rts

; -- (IF TEST THEN ELSE) --
; IF is non-recursive-safe for now: uses ev_if_args global.  Good enough for
; Phase 2; the args pointer is only needed across one recursive eval(test).
ev_if:      ldx     ev_expr_scratch
            ldx     2,x             ; args = (test then else)
            pshs    x               ; save args on S across eval(test)
            ldx     ,x              ; X = test form
            lbsr    eval
            ; X = test result.  Pop args.
            ldy     ,s++            ; Y = args
            cmpx    #NIL_VAL
            beq     ev_if_else
            ldy     2,y             ; Y = (then else)
            ldx     ,y              ; then
            lbra    eval
ev_if_else: ldy     2,y             ; Y = (then else)
            ldy     2,y             ; Y = (else)
            ldx     ,y
            lbra    eval

; -- (DEFVAR SYM VALUE) --
ev_defvar:  ldx     ev_expr_scratch
            ldx     2,x             ; args = (sym value)
            stx     ev_dv_args
            ldx     ,x              ; SYM (unevaluated)
            stx     ev_dv_sym
            ldx     ev_dv_args
            ldx     2,x             ; (value)
            ldx     ,x              ; value form
            lbsr    eval            ; evaluate value (recurses; OK because we
                                    ; save state in memory above)
            ; X = value. Build binding = (sym . value).
            tfr     x,d             ; D = value
            ldy     ev_dv_sym
            lbsr    alloc_pair      ; X = (sym . value)
            tfr     x,y             ; Y = binding
            ldd     global_env
            lbsr    alloc_pair      ; X = (binding . global_env)
            stx     global_env
            ldx     ev_dv_sym       ; return the symbol
            rts

ev_expr_scratch fdb 0
ev_dv_args  fdb     0
ev_dv_sym   fdb     0

; ---------------------------------------------------------------------------
; Phase 3 primitives.  Convention:
;   On entry: ev_expr_scratch = full form pair (op . args)
;   On exit:  X = result value
;
; Each primitive evaluates its own arguments via recursive eval(), using the
; S stack to hold intermediate values across recursion.
; ---------------------------------------------------------------------------

; Helper macro pattern for 2-arg primitives.  Assumes ev_expr_scratch =
; (OP A B ...).  After the code below runs:
;   ,s   = A_val (2 bytes)
;   2,s  = args ptr (2 bytes)
;   X    = B_val
; Caller must `leas 4,s` before rts.
; Inlined per-primitive to avoid tail-call pitfalls with rts semantics.

; (CONS A B) → (A . B)
ev_cons:    ldx     ev_expr_scratch
            ldx     2,x
            pshs    x
            ldx     ,x
            lbsr    eval
            pshs    x
            ldx     2,s
            ldx     2,x
            ldx     ,x
            lbsr    eval
            tfr     x,d
            puls    y               ; Y = A_val
            leas    2,s             ; drop args
            lbsr    alloc_pair
            rts

; (CAR X) — X must be a pair
ev_car:     lbsr    eval_one_arg    ; X = arg value
            cmpx    #PAIR_POOL
            blo     ev_car_err
            cmpx    #PAIR_END
            bhs     ev_car_err
            ldx     ,x              ; car
            rts
ev_car_err: ldx     #str_car_err
            lbsr    puts_native
            lbsr    emit_crlf
            ldx     #NIL_VAL
            rts

; (CDR X) — X must be a pair
ev_cdr:     lbsr    eval_one_arg
            cmpx    #PAIR_POOL
            blo     ev_cdr_err
            cmpx    #PAIR_END
            bhs     ev_cdr_err
            ldx     2,x             ; cdr
            rts
ev_cdr_err: ldx     #str_cdr_err
            lbsr    puts_native
            lbsr    emit_crlf
            ldx     #NIL_VAL
            rts

; (CADR xs) = (car (cdr xs))
ev_cadr:    lbsr    eval_one_arg
            cmpx    #PAIR_POOL
            lblo    ev_cadr_err
            cmpx    #PAIR_END
            lbhs    ev_cadr_err
            ldx     2,x                 ; cdr
            cmpx    #PAIR_POOL
            lblo    ev_cadr_err
            cmpx    #PAIR_END
            lbhs    ev_cadr_err
            ldx     ,x                  ; car
            rts
ev_cadr_err:
            ldx     #str_cadr_err
            lbsr    puts_native
            lbsr    emit_crlf
            ldx     #NIL_VAL
            rts

; (CADDR xs) = (car (cdr (cdr xs)))
ev_caddr:   lbsr    eval_one_arg
            cmpx    #PAIR_POOL
            lblo    ev_cadr_err
            cmpx    #PAIR_END
            lbhs    ev_cadr_err
            ldx     2,x
            cmpx    #PAIR_POOL
            lblo    ev_cadr_err
            cmpx    #PAIR_END
            lbhs    ev_cadr_err
            ldx     2,x
            cmpx    #PAIR_POOL
            lblo    ev_cadr_err
            cmpx    #PAIR_END
            lbhs    ev_cadr_err
            ldx     ,x
            rts

; (CDDR xs) = (cdr (cdr xs))
ev_cddr:    lbsr    eval_one_arg
            cmpx    #PAIR_POOL
            lblo    ev_cadr_err
            cmpx    #PAIR_END
            lbhs    ev_cadr_err
            ldx     2,x
            cmpx    #PAIR_POOL
            lblo    ev_cadr_err
            cmpx    #PAIR_END
            lbhs    ev_cadr_err
            ldx     2,x
            rts

; (LENGTH xs) — count proper-list pairs; stops at NIL or non-pair tail.
ev_length:  lbsr    eval_one_arg
            tfr     x,y                 ; Y = walker
            ldd     #0                  ; counter
ln_loop:    cmpy    #NIL_VAL
            beq     ln_done
            cmpy    #PAIR_POOL
            lblo    ln_done
            cmpy    #PAIR_END
            lbhs    ln_done
            addd    #1
            ldy     2,y                 ; cdr
            bra     ln_loop
ln_done:    lslb
            rola
            addd    #1                  ; fixnum tag
            tfr     d,x
            rts

; (APPEND a b) — build a new list that is a's elements followed by b
; (b is shared, not copied).  If a is NIL returns b; if a is not a proper
; list the partial result is attached to b.
ev_append:  ldx     ev_expr_scratch
            ldx     2,x
            pshs    x                   ; save args ptr on S ([S+0])
            ldx     ,x                  ; a form
            lbsr    eval
            stx     ev_app_a            ; a value
            ldx     ,s                  ; args
            ldx     2,x                 ; (b)
            ldx     ,x                  ; b form
            lbsr    eval
            stx     ev_app_b
            leas    2,s                 ; drop saved args
            ; If a is NIL, return b directly.
            ldx     ev_app_a
            cmpx    #NIL_VAL
            bne     ap_build
            ldx     ev_app_b
            rts
ap_build:   cmpx    #PAIR_POOL
            lblo    ap_bad_a
            cmpx    #PAIR_END
            lbhs    ap_bad_a
            ; Build head = (car(a) . NIL)
            ldy     ,x
            ldd     #NIL_VAL
            lbsr    alloc_pair          ; X = new cell
            stx     ev_app_head
            stx     ev_app_tail
            ldx     ev_app_a
            ldx     2,x
            stx     ev_app_a
ap_loop:    ldx     ev_app_a
            cmpx    #NIL_VAL
            beq     ap_attach
            cmpx    #PAIR_POOL
            lblo    ap_attach           ; improper tail — treat as terminator
            cmpx    #PAIR_END
            lbhs    ap_attach
            ldy     ,x
            ldd     #NIL_VAL
            lbsr    alloc_pair
            ldy     ev_app_tail
            stx     2,y                 ; tail.cdr = new cell
            stx     ev_app_tail
            ldx     ev_app_a
            ldx     2,x
            stx     ev_app_a
            bra     ap_loop
ap_attach:  ldx     ev_app_tail
            ldd     ev_app_b
            std     2,x                 ; tail.cdr = b
            ldx     ev_app_head
            rts
ap_bad_a:   ; a is not a list — return b
            ldx     ev_app_b
            rts

ev_app_a     fdb    0
ev_app_b     fdb    0
ev_app_head  fdb    0
ev_app_tail  fdb    0

; (ATOM X) → T if X is not a pair, else NIL
ev_atom:    lbsr    eval_one_arg
            cmpx    #PAIR_POOL
            blo     ev_atom_true
            cmpx    #PAIR_END
            bhs     ev_atom_true
            ldx     #NIL_VAL
            rts
ev_atom_true:
            ldx     #T_VAL
            rts

; (NULL X) → T if X == NIL, else NIL
ev_null:    lbsr    eval_one_arg
            cmpx    #NIL_VAL
            beq     ev_null_true
            ldx     #NIL_VAL
            rts
ev_null_true:
            ldx     #T_VAL
            rts

; (EQ A B) → T if identical value, else NIL.
ev_eq:      ldx     ev_expr_scratch
            ldx     2,x
            pshs    x
            ldx     ,x
            lbsr    eval
            pshs    x
            ldx     2,s
            ldx     2,x
            ldx     ,x
            lbsr    eval
            cmpx    ,s
            beq     ev_eq_true
            leas    4,s
            ldx     #NIL_VAL
            rts
ev_eq_true: leas    4,s
            ldx     #T_VAL
            rts

; (+ A B) — fast path: both fixnums and result fits 15-bit.  Otherwise
; promote to 32-bit and return either a re-tagged fixnum or a fresh box.
ev_plus:    ldx     ev_expr_scratch
            ldx     2,x
            pshs    x
            ldx     ,x
            lbsr    eval            ; A_val
            pshs    x
            ldx     2,s
            ldx     2,x
            ldx     ,x
            lbsr    eval            ; B_val in X
            ; Fast-path check: both fixnum?
            ldd     ,s              ; A_val
            andb    #1
            beq     pl_slow
            tfr     x,d             ; B_val
            andb    #1
            beq     pl_slow
            ; Both fixnum.  Compute untagged sum; check range.
            tfr     x,d
            subd    #1
            asra
            rorb                    ; D = b (16-bit signed)
            pshs    d               ; save b
            ldd     2,s             ; A_val (original was at [S+2] before this pshs; now at [S+2])
            subd    #1
            asra
            rorb                    ; D = a
            addd    ,s++            ; D = a + b (drop b slot)
            ; Now [S+0] = original A_val still (we haven't popped).  Check range.
            cmpd    #16383
            bgt     pl_overflow_at_stack
            cmpd    #-16384
            blt     pl_overflow_at_stack
            ; Fits: tag and return.
            lslb
            rola
            addd    #1
            tfr     d,x
            leas    4,s             ; drop A_val + args
            rts
pl_overflow_at_stack:
            ; Need box.  Stack still has A_val at [S+0], args at [S+2].
            ; Slow path below expects ev_ap scratch + stack; prefer to re-eval
            ; through a unified helper.
            ; Easier: jump into slow path with the already-eval'd values.
            ; We still have A_val on S and B_val in X.
            ; Fall through to pl_slow with X = B_val, [S] = A_val.
pl_slow:    ; General path: convert both to i32, add, fit_and_box.
            ; X = B_val, [S+0] = A_val (args at [S+2]).
            lbsr    untag_to_i32    ; X → i32_scratch
            ldd     i32_scratch
            std     i32_b
            ldd     i32_scratch+2
            std     i32_b+2
            puls    x               ; X = A_val
            lbsr    untag_to_i32
            ldd     i32_scratch
            std     i32_a
            ldd     i32_scratch+2
            std     i32_a+2
            leas    2,s             ; drop args
            lbsr    i32_add
            lbsr    fit_and_box
            rts

; (- A B)
ev_minus:   ldx     ev_expr_scratch
            ldx     2,x
            pshs    x
            ldx     ,x
            lbsr    eval
            pshs    x
            ldx     2,s
            ldx     2,x
            ldx     ,x
            lbsr    eval
            ; Fast path: both fixnum?
            ldd     ,s
            andb    #1
            beq     mi_slow
            tfr     x,d
            andb    #1
            beq     mi_slow
            ; Both fixnum: A - B in untagged.
            ldd     ,s
            subd    #1
            asra
            rorb                    ; D = a
            pshs    d
            tfr     x,d
            subd    #1
            asra
            rorb                    ; D = b
            pshs    d
            ldd     2,s             ; a
            subd    ,s++            ; D = a - b (drop b).  S now: [a, A_val, args]
            cmpd    #16383
            bgt     mi_slow_atS
            cmpd    #-16384
            blt     mi_slow_atS
            lslb
            rola
            addd    #1
            tfr     d,x
            leas    6,s             ; drop a + A_val + args
            rts
mi_slow_atS:
            leas    2,s             ; drop stored a; now [S+0]=A_val, [S+2]=args
            ; fall through to mi_slow with X untouched (B_val).
mi_slow:    lbsr    untag_to_i32
            ldd     i32_scratch
            std     i32_b
            ldd     i32_scratch+2
            std     i32_b+2
            puls    x
            lbsr    untag_to_i32
            ldd     i32_scratch
            std     i32_a
            ldd     i32_scratch+2
            std     i32_a+2
            leas    2,s
            lbsr    i32_sub
            lbsr    fit_and_box
            rts

; (< A B) — works for fixnum/fixnum fast path (cmpx is signed 16-bit on
; tagged values, and tagged values preserve ordering relative to untagged
; integers since shift is monotonic).  For mixed types uses i32_cmp.
ev_lt:      ldx     ev_expr_scratch
            ldx     2,x
            pshs    x
            ldx     ,x
            lbsr    eval
            pshs    x
            ldx     2,s
            ldx     2,x
            ldx     ,x
            lbsr    eval
            ; Fast path: both fixnum?
            ldd     ,s
            andb    #1
            beq     lt_slow
            tfr     x,d
            andb    #1
            beq     lt_slow
            ; Tagged fixnums preserve signed ordering — cmpx works directly.
            cmpx    ,s
            bgt     lt_true
            leas    4,s
            ldx     #NIL_VAL
            rts
lt_true:    leas    4,s
            ldx     #T_VAL
            rts
lt_slow:    lbsr    untag_to_i32
            ldd     i32_scratch
            std     i32_b
            ldd     i32_scratch+2
            std     i32_b+2
            puls    x
            lbsr    untag_to_i32
            ldd     i32_scratch
            std     i32_a
            ldd     i32_scratch+2
            std     i32_a+2
            leas    2,s
            lbsr    i32_cmp
            tstb
            bmi     lt_true_slow
            ldx     #NIL_VAL
            rts
lt_true_slow:
            ldx     #T_VAL
            rts

; (= A B) — numeric equality, value-aware (box equality compares contents,
; not pointers).
ev_num_eq:  ldx     ev_expr_scratch
            ldx     2,x
            pshs    x
            ldx     ,x
            lbsr    eval
            pshs    x
            ldx     2,s
            ldx     2,x
            ldx     ,x
            lbsr    eval
            ldd     ,s
            andb    #1
            beq     neq_slow
            tfr     x,d
            andb    #1
            beq     neq_slow
            ; Both fixnum: cmpx.
            cmpx    ,s
            beq     neq_true
            leas    4,s
            ldx     #NIL_VAL
            rts
neq_true:   leas    4,s
            ldx     #T_VAL
            rts
neq_slow:   lbsr    untag_to_i32
            ldd     i32_scratch
            std     i32_b
            ldd     i32_scratch+2
            std     i32_b+2
            puls    x
            lbsr    untag_to_i32
            ldd     i32_scratch
            std     i32_a
            ldd     i32_scratch+2
            std     i32_a+2
            leas    2,s
            lbsr    i32_cmp
            tstb
            beq     neq_true_slow
            ldx     #NIL_VAL
            rts
neq_true_slow:
            ldx     #T_VAL
            rts

; Helper: evaluate the single argument of a 1-arg primitive.
; In:  ev_expr_scratch = (OP X)
; Out: X = value of X (arg evaluated)
;      ev_expr_scratch clobbered (caller must not rely on it afterwards)
eval_one_arg:
            ldx     ev_expr_scratch
            ldx     2,x             ; (X)
            ldx     ,x              ; X form
            lbra    eval

ev_pr_args  fdb     0
ev_pr_tmp   fdb     0
ev_pr_tmp2  fdb     0

; ---------------------------------------------------------------------------
; Phase 4 — LAMBDA / DEFUN / function application.
;
; Closure representation: a 3-pair chain
;     outer = (sym_LAMBDA . mid)
;     mid   = (params     . inner)
;     inner = (body       . captured_env)
; The closure *value* is the outer pair pointer.  Check `car(v) == sym_LAMBDA`
; to distinguish a closure from a regular pair.
;
; Environment model: two chains.
;   current_env — prepended to by lambda-application param binding, captured
;                 by LAMBDA.  Popped when the call returns.
;   global_env  — targeted by DEFVAR / DEFUN (even inside a function body).
; Lookup walks current_env first then falls back to global_env.
; ---------------------------------------------------------------------------

; (LAMBDA (params) body1 body2 ... bodyN) → closure
; Multiple body expressions are wrapped in an implicit PROGN at closure
; creation time so the call-time interpreter can keep a single-expression
; body invariant.
ev_lambda:
            ldx     ev_expr_scratch
            ldx     2,x                 ; args = ((params) body...)
            stx     ev_lm_args
            ldx     ,x                  ; params
            stx     ev_lm_params
            ldx     ev_lm_args
            ldx     2,x                 ; (body...)
            lbsr    wrap_progn          ; X = (PROGN body1 body2 ...)
            stx     ev_lm_body
            ldx     sym_LAMBDA
            stx     ev_lm_tag
            lbsr    build_closure
            rts

; (DEFUN name (params) body1 body2 ... bodyN) → name, with global_env
; extended with the binding.  Multiple body expressions → implicit PROGN.
ev_defun:   ldx     ev_expr_scratch
            ldx     2,x                 ; args = (name (params) body...)
            stx     ev_lm_args
            ldx     ,x                  ; name
            stx     ev_df_sym
            ldx     ev_lm_args
            ldx     2,x                 ; ((params) body...)
            stx     ev_lm_args
            ldx     ,x                  ; params
            stx     ev_lm_params
            ldx     ev_lm_args
            ldx     2,x                 ; (body...)
            lbsr    wrap_progn          ; X = (PROGN body1 body2 ...)
            stx     ev_lm_body
            ldx     sym_LAMBDA
            stx     ev_lm_tag
            lbsr    build_closure       ; X = closure
            tfr     x,d                 ; D = closure (for binding cdr)
            ldy     ev_df_sym
            lbsr    alloc_pair          ; X = (name . closure)
            tfr     x,y                 ; Y = binding
            ldd     global_env
            lbsr    alloc_pair          ; X = (binding . global_env)
            stx     global_env
            ldx     ev_df_sym           ; return name
            rts

; (DEFMACRO name (params) body...) — like DEFUN but creates a macro value
; (head tag sym_MACRO).  When invoked, args are bound UNEVALUATED and the
; body's result is evaluated as a form (one-step macro expansion).
ev_defmacro:
            ldx     ev_expr_scratch
            ldx     2,x                 ; args = (name (params) body...)
            stx     ev_lm_args
            ldx     ,x                  ; name
            stx     ev_df_sym
            ldx     ev_lm_args
            ldx     2,x                 ; ((params) body...)
            stx     ev_lm_args
            ldx     ,x                  ; params
            stx     ev_lm_params
            ldx     ev_lm_args
            ldx     2,x                 ; (body...)
            lbsr    wrap_progn
            stx     ev_lm_body
            ldx     sym_MACRO
            stx     ev_lm_tag
            lbsr    build_closure
            tfr     x,d
            ldy     ev_df_sym
            lbsr    alloc_pair          ; (name . macro)
            tfr     x,y
            ldd     global_env
            lbsr    alloc_pair
            stx     global_env
            ldx     ev_df_sym
            rts

; wrap_progn — given a body list in X, return a single form in X that
; evaluates the body's expressions in order (implicit PROGN).  Specifically
; returns (PROGN . body-list).  If body-list is NIL, returns NIL.
wrap_progn:
            cmpx    #NIL_VAL
            beq     wp_nil
            tfr     x,d                 ; D = body list
            ldy     sym_PROGN
            lbsr    alloc_pair          ; X = (PROGN . body)
            rts
wp_nil:     ldx     #NIL_VAL
            rts

; Build a closure (or macro) from ev_lm_params / ev_lm_body / ev_lm_tag,
; capturing current_env.  ev_lm_tag selects the head symbol — sym_LAMBDA
; for regular closures, sym_MACRO for macros.
; Out: X = closure.  Clobbers D, Y.
build_closure:
            ldy     ev_lm_body
            ldd     current_env
            lbsr    alloc_pair          ; X = (body . env)
            tfr     x,d
            ldy     ev_lm_params
            lbsr    alloc_pair          ; X = (params . inner)
            tfr     x,d
            ldy     ev_lm_tag
            lbsr    alloc_pair          ; X = (TAG . mid)
            rts

; Fall-through from ev_form when operator is not a known special form.
; Evaluates the operator expression, expects a closure value, evaluates the
; arguments, binds params, and runs the body in the extended env.
ev_apply:   ldx     ev_expr_scratch
            stx     ev_ap_expr
            ldx     ,x                  ; operator form (symbol or expression)
            lbsr    eval
            stx     ev_ap_fn
            ; Builtin function value?  ($7000..$7FFF)
            cmpx    #BUILTIN_POOL
            lblo    ev_ap_try_closure
            cmpx    #BUILTIN_END
            lbhs    ev_ap_err
            ; Restore ev_expr_scratch to the whole form so each primitive can
            ; pull its own args out via ev_expr_scratch+2.
            ldx     ev_ap_expr
            stx     ev_expr_scratch
            ldd     ev_ap_fn
            cmpd    #BI_CONS
            lbeq    ev_cons
            cmpd    #BI_CAR
            lbeq    ev_car
            cmpd    #BI_CDR
            lbeq    ev_cdr
            cmpd    #BI_ATOM
            lbeq    ev_atom
            cmpd    #BI_EQ
            lbeq    ev_eq
            cmpd    #BI_NULL
            lbeq    ev_null
            cmpd    #BI_PLUS
            lbeq    ev_plus
            cmpd    #BI_MINUS
            lbeq    ev_minus
            cmpd    #BI_LT
            lbeq    ev_lt
            cmpd    #BI_GC
            lbeq    ev_gc
            cmpd    #BI_MUL
            lbeq    ev_mul
            cmpd    #BI_CADR
            lbeq    ev_cadr
            cmpd    #BI_CADDR
            lbeq    ev_caddr
            cmpd    #BI_CDDR
            lbeq    ev_cddr
            cmpd    #BI_LENGTH
            lbeq    ev_length
            cmpd    #BI_APPEND
            lbeq    ev_append
            cmpd    #BI_APPLY
            lbeq    ev_apply_fn
            cmpd    #BI_LIST
            lbeq    ev_list
            cmpd    #BI_PRINT
            lbeq    ev_print
            cmpd    #BI_NEWLINE
            lbeq    ev_newline
            cmpd    #BI_ASSOC
            lbeq    ev_assoc
            cmpd    #BI_GENSYM
            lbeq    ev_gensym
            cmpd    #BI_STRLEN
            lbeq    ev_strlen
            cmpd    #BI_STREQ
            lbeq    ev_streq
            cmpd    #BI_STRAPP
            lbeq    ev_strapp
            cmpd    #BI_STRREF
            lbeq    ev_strref
            cmpd    #BI_STR2L
            lbeq    ev_str2l
            cmpd    #BI_L2STR
            lbeq    ev_l2str
            cmpd    #BI_DIV
            lbeq    ev_div
            cmpd    #BI_MOD
            lbeq    ev_mod
            cmpd    #BI_ERROR
            lbeq    ev_error
            cmpd    #BI_THROW
            lbeq    ev_throw
            cmpd    #BI_NUM_EQ
            lbeq    ev_num_eq
            cmpd    #BI_CHAR_INT
            lbeq    ev_char_int
            cmpd    #BI_INT_CHAR
            lbeq    ev_int_char
            cmpd    #BI_CHARP
            lbeq    ev_charp
            cmpd    #BI_MAKE_VEC
            lbeq    ev_make_vec
            cmpd    #BI_VEC_LEN
            lbeq    ev_vec_len
            cmpd    #BI_VEC_REF
            lbeq    ev_vec_ref
            cmpd    #BI_VEC_SET
            lbeq    ev_vec_set
            cmpd    #BI_VEC_LIST
            lbeq    ev_vec_list
            cmpd    #BI_LIST_VEC
            lbeq    ev_list_vec
            cmpd    #BI_VECP
            lbeq    ev_vecp
            cmpd    #BI_LOGAND
            lbeq    ev_logand
            cmpd    #BI_LOGIOR
            lbeq    ev_logior
            cmpd    #BI_LOGXOR
            lbeq    ev_logxor
            cmpd    #BI_LOGNOT
            lbeq    ev_lognot
            cmpd    #BI_ASH
            lbeq    ev_ash
            cmpd    #BI_NUM2STR
            lbeq    ev_num2str
            cmpd    #BI_STR2NUM
            lbeq    ev_str2num
            cmpd    #BI_SYM2STR
            lbeq    ev_sym2str
            cmpd    #BI_STR2SYM
            lbeq    ev_str2sym
            cmpd    #BI_EVAL
            lbeq    ev_eval_prim
            cmpd    #BI_RDSTR
            lbeq    ev_read_string
            cmpd    #BI_LOADMEM
            lbeq    ev_load_memory
            cmpd    #BI_DISPLAY
            lbeq    ev_display
            cmpd    #BI_PUTCHAR
            lbeq    ev_putchar
            cmpd    #BI_RAND
            lbeq    ev_rand
            cmpd    #BI_SEED
            lbeq    ev_seed
            cmpd    #BI_TICK
            lbeq    ev_tick
            cmpd    #BI_PCASE_GET
            lbeq    ev_pcase_get
            cmpd    #BI_PCASE_SET
            lbeq    ev_pcase_set
            lbra    ev_ap_err
ev_ap_try_closure:
            ; Must be a pair.  Tagged via car: sym_LAMBDA = closure (evaluate
            ; args then run body), sym_MACRO = macro (pass raw arg forms,
            ; evaluate body, then evaluate that expansion).
            cmpx    #PAIR_POOL
            lblo    ev_ap_err
            cmpx    #PAIR_END
            lbhs    ev_ap_err
            ldy     ,x
            cmpy    sym_LAMBDA
            lbeq    ev_ap_closure_proper
            cmpy    sym_MACRO
            lbeq    ev_ap_macro
            lbra    ev_ap_err
ev_ap_closure_proper:
            ; Walk closure: outer=(LAMBDA . mid), mid=(params . inner),
            ; inner=(body . env).
            ldx     ev_ap_fn
            ldx     2,x                 ; mid
            ldd     ,x
            std     ev_ap_params
            ldx     2,x                 ; inner
            ldd     ,x
            std     ev_ap_body
            ldd     2,x
            std     ev_ap_env           ; captured env
            ; Pick up the argument forms: cdr of the outer expression.
            ldx     ev_ap_expr
            ldx     2,x
            stx     ev_ap_args
            ; Walk params and argument forms in parallel, evaluating each
            ; argument and prepending (param . value) onto ev_ap_env.
            ldx     ev_ap_params
            stx     ev_ap_pp
            ldx     ev_ap_args
            stx     ev_ap_ap
ev_ap_loop:
            ldx     ev_ap_pp
            cmpx    #NIL_VAL
            beq     ev_ap_done_bind
            ; If ev_ap_pp is not a pair it is the "rest" param (dotted-tail
            ; `(a b . rest)` or pure-varargs `args` form).  Bind it to a
            ; fresh list of *evaluated* remaining args and finish.
            cmpx    #PAIR_POOL
            lblo    ev_ap_rest
            cmpx    #PAIR_END
            lbhs    ev_ap_rest
            ldy     ev_ap_ap
            cmpy    #NIL_VAL
            lbeq    ev_ap_arity_err
            ldx     ,y                  ; arg form
            ; Save loop state across eval, because the arg form may recurse
            ; into another ev_apply that clobbers ev_ap_body / ev_ap_env /
            ; ev_ap_pp / ev_ap_ap.  (Bug reproducer:
            ; `(append (reverse xs) ys)` — arg-1's eval recurses into
            ; reverse's ev_ap_loop, which then rewrites our ev_ap_body
            ; pointer so we end up evaluating the wrong closure body.)
            ldd     ev_ap_body
            pshs    d
            ldd     ev_ap_env
            pshs    d
            ldd     ev_ap_pp
            pshs    d
            ldd     ev_ap_ap
            pshs    d
            ldd     ev_ap_fn
            pshs    d
            lbsr    eval                ; X = arg value
            stx     ev_ap_val
            puls    d
            std     ev_ap_fn
            puls    d
            std     ev_ap_ap
            puls    d
            std     ev_ap_pp
            puls    d
            std     ev_ap_env
            puls    d
            std     ev_ap_body
            ldx     ev_ap_pp
            ldy     ,x                  ; param symbol (car)
            ldd     ev_ap_val
            lbsr    alloc_pair          ; X = (param . value)
            tfr     x,y
            ldd     ev_ap_env
            lbsr    alloc_pair          ; X = (binding . env)
            stx     ev_ap_env
            ldx     ev_ap_pp
            ldx     2,x
            stx     ev_ap_pp
            ldx     ev_ap_ap
            ldx     2,x
            stx     ev_ap_ap
            lbra    ev_ap_loop
ev_ap_done_bind:
            ldx     ev_ap_ap
            cmpx    #NIL_VAL
            lbne    ev_ap_arity_err
            ; Tail-call re-entry: skip the env-save pshs (we reuse the one
            ; stacked by the outer call) but still switch current_env.
            ; Also track current_closure so self-TCO can recognise a recursive
            ; call even after nested eval has overwritten ev_ap_fn.
            lda     ev_tail_mode
            bne     ev_ap_tskip
            ldd     current_env
            pshs    d
            ldd     current_closure
            pshs    d
            bra     ev_ap_after_save
ev_ap_tskip:
            clr     ev_tail_mode
ev_ap_after_save:
            ldd     ev_ap_fn
            std     current_closure
            ldd     ev_ap_env
            std     current_env
ev_ap_body_start:
            ; Re-fetch body from current_closure.  Nested closure applies
            ; (e.g. a LET wrapping body code) overwrite ev_ap_body with
            ; their own body, so we can't rely on the scratch holding our
            ; body across sub-expression evaluation.
            ldx     current_closure
            ldx     2,x                 ; mid = (params . inner)
            ldx     2,x                 ; inner = (body . env)
            ldd     ,x
            std     ev_ap_body
            ; Body is normally (PROGN e1 e2 ... eN).  Walk manually so the
            ; final expression can be handled as a tail position (TCO).
            ldx     ev_ap_body
            cmpx    #PAIR_POOL
            lblo    ev_ap_body_single
            cmpx    #PAIR_END
            lbhs    ev_ap_body_single
            ldy     ,x
            cmpy    sym_PROGN
            lbne    ev_ap_body_single
            ; PROGN body.  X = (PROGN e1 e2 ... eN).  Walk cdr.
            ldx     2,x
ev_ap_body_loop:
            cmpx    #NIL_VAL
            lbeq    ev_ap_body_nil
            ldd     2,x
            cmpd    #NIL_VAL
            beq     ev_ap_body_tail
            ; Non-last form: eval and discard.
            pshs    x
            ldx     ,x
            lbsr    eval
            puls    x
            ldx     2,x
            bra     ev_ap_body_loop
ev_ap_body_tail:
            ldx     ,x                  ; X = tail expression
ev_ap_body_single:
ev_ap_tail_dispatch:
            ; X = the final form to evaluate.  Some special forms are
            ; tail-transparent: their final subexpression is itself a tail
            ; position, so we recurse here instead of calling eval.
            cmpx    #PAIR_POOL
            lblo    ev_ap_tail_reg
            cmpx    #PAIR_END
            lbhs    ev_ap_tail_reg
            ldy     ,x                  ; operator form
            cmpy    #SYM_POOL
            lblo    ev_ap_tail_check
            cmpy    #SYM_END
            lbhs    ev_ap_tail_check
            cmpy    sym_IF
            lbeq    ev_ap_tail_if
            cmpy    sym_PROGN
            lbeq    ev_ap_tail_progn
            cmpy    sym_COND
            lbeq    ev_ap_tail_cond
            ; Reject other special forms — evaluate normally via ev_form.
            cmpy    sym_QUOTE
            lbeq    ev_ap_tail_reg
            cmpy    sym_DEFVAR
            lbeq    ev_ap_tail_reg
            cmpy    sym_LAMBDA
            lbeq    ev_ap_tail_reg
            cmpy    sym_DEFUN
            lbeq    ev_ap_tail_reg
            cmpy    sym_LET
            lbeq    ev_ap_tail_reg
            cmpy    sym_SETQ
            lbeq    ev_ap_tail_reg
            cmpy    sym_SETBANG         ; SET! also opts out of TCO — it's a special form
            lbeq    ev_ap_tail_reg
            cmpy    sym_PROGN
            lbeq    ev_ap_tail_reg
            cmpy    sym_AND
            lbeq    ev_ap_tail_reg
            cmpy    sym_OR
            lbeq    ev_ap_tail_reg
            cmpy    sym_LETSTAR
            lbeq    ev_ap_tail_reg
            cmpy    sym_LETREC
            lbeq    ev_ap_tail_reg
            cmpy    sym_DEFMACRO
            lbeq    ev_ap_tail_reg
            cmpy    sym_QUASIQUOTE
            lbeq    ev_ap_tail_reg
            cmpy    sym_CATCH
            lbeq    ev_ap_tail_reg
ev_ap_tail_check:
            ; Evaluate the operator.  Save the whole call form first.
            pshs    x
            tfr     y,x
            lbsr    eval
            ; X = operator value.  Closure?
            cmpx    #PAIR_POOL
            lblo    ev_ap_tail_pop
            cmpx    #PAIR_END
            lbhs    ev_ap_tail_pop
            ldd     ,x
            cmpd    sym_LAMBDA
            lbne    ev_ap_tail_pop
            ; TCO!  Prefer self-tail: if the target closure is the same
            ; one we are currently running, we can re-evaluate args and
            ; mutate the existing binding pairs in place — no new pair
            ; allocations, so deep tail recursion does not grow garbage.
            cmpx    current_closure
            beq     ev_ap_self_tco
            ; General TCO: different closure.  Allocate a fresh frame
            ; (accepts some garbage per iteration).
            stx     ev_ap_fn
            puls    x
            stx     ev_ap_expr
            lda     #1
            sta     ev_tail_mode
            lbra    ev_ap_closure_proper
ev_ap_self_tco:
            ; Self-tail call: current closure recurses.  Evaluate ALL new
            ; args first (buffered in stco_vals[]), THEN mutate the bindings.
            ; Doing this in two phases means later args see the OLD values
            ; of earlier params (the correct semantic).
            puls    x
            stx     ev_ap_expr
            ; Set up param / arg walkers.
            ldx     current_closure
            ldx     2,x                 ; mid
            ldd     ,x
            std     stco_params
            ldx     ev_ap_expr
            ldx     2,x
            stx     stco_args
            ldy     #stco_vals
            sty     stco_vy
            ; Phase 1: evaluate each arg in current_env, buffer into stco_vals.
stco_eval_loop:
            ldx     stco_params
            cmpx    #NIL_VAL
            beq     stco_eval_done
            cmpx    #PAIR_POOL
            lblo    stco_fallback
            cmpx    #PAIR_END
            lbhs    stco_fallback
            ldy     stco_args
            cmpy    #NIL_VAL
            lbeq    ev_ap_arity_err
            cmpy    #PAIR_POOL
            lblo    stco_fallback
            cmpy    #PAIR_END
            lbhs    stco_fallback
            ldx     ,y                  ; arg form
            lbsr    eval
            ldy     stco_vy
            cmpy    #stco_vals_end
            lbhs    stco_fallback       ; too many args for buffer
            stx     ,y++
            sty     stco_vy
            ldx     stco_params
            ldx     2,x
            stx     stco_params
            ldx     stco_args
            ldx     2,x
            stx     stco_args
            lbra    stco_eval_loop
stco_eval_done:
            ldx     stco_args
            cmpx    #NIL_VAL
            lbne    ev_ap_arity_err
            ; Phase 2: walk params again, mutate each binding to stco_vals[i].
            ldx     current_closure
            ldx     2,x
            ldd     ,x
            std     stco_params
            ldy     #stco_vals
            sty     stco_vy
stco_mutate_loop:
            ldx     stco_params
            cmpx    #NIL_VAL
            lbeq    ev_ap_body_start
            ldy     ,x                  ; param sym
            sty     stco_psym
            ldy     stco_vy
            ldd     ,y++
            std     stco_val
            sty     stco_vy
            ldx     current_env
stco_find:  cmpx    #NIL_VAL
            lbeq    stco_fallback       ; shouldn't happen — bail out
            ldy     ,x                  ; binding pair
            ldd     ,y
            cmpd    stco_psym
            beq     stco_mutate_one
            ldx     2,x
            bra     stco_find
stco_mutate_one:
            ldd     stco_val
            std     2,y
            ldx     stco_params
            ldx     2,x
            stx     stco_params
            lbra    stco_mutate_loop
stco_fallback:
            ; Dotted params, varargs, or too many args — fall back to
            ; general TCO (allocates a fresh frame).  current_closure and
            ; ev_ap_expr are already set.
            ldd     current_closure
            std     ev_ap_fn
            lda     #1
            sta     ev_tail_mode
            lbra    ev_ap_closure_proper
ev_ap_tail_pop:
            puls    x
ev_ap_tail_reg:
            lbsr    eval
            stx     ev_ap_val
            puls    d
            std     current_closure
            puls    d
            std     current_env
            ldx     ev_ap_val
            rts
ev_ap_body_nil:
            ldx     #NIL_VAL
            stx     ev_ap_val
            puls    d
            std     current_closure
            puls    d
            std     current_env
            ldx     ev_ap_val
            rts

; Tail-transparent IF: evaluate the condition, dispatch to the chosen
; branch, and re-enter ev_ap_tail_dispatch so further tail forms nest.
ev_ap_tail_if:
            ; X = (IF cond then . else-or-nil)
            ldx     2,x                 ; X = (cond . rest)
            pshs    x                   ; save pointer to cond-and-rest
            ldx     ,x                  ; cond form
            lbsr    eval
            cmpx    #NIL_VAL
            beq     ev_ap_tif_else
            ; true branch = car(cdr(rest))
            puls    x
            ldx     2,x                 ; (then . (else . nil))
            ldx     ,x
            lbra    ev_ap_tail_dispatch
ev_ap_tif_else:
            puls    x
            ldx     2,x                 ; (then . (else . nil))
            ldx     2,x                 ; (else . nil)
            cmpx    #NIL_VAL
            beq     ev_ap_tif_no_else
            ldx     ,x
            lbra    ev_ap_tail_dispatch
ev_ap_tif_no_else:
            ldx     #NIL_VAL
            stx     ev_ap_val
            puls    d
            std     current_closure
            puls    d
            std     current_env
            ldx     ev_ap_val
            rts

; Tail-transparent PROGN: evaluate all-but-last, last expression is still
; in tail position.
ev_ap_tail_progn:
            ldx     2,x                 ; rest = (e1 . e2 . ... . eN . NIL)
; Tail-progn entry that takes the body list directly in X instead of a
; (PROGN . body) form.  Used by ev_ap_tail_cond so a cond match doesn't
; have to alloc_pair a temporary `(PROGN . body)` wrapper — saves one
; pair per matched cond clause, which adds up fast in deep search.
ev_ap_tail_progn_rest:
ev_ap_tp_loop:
            cmpx    #NIL_VAL
            beq     ev_ap_tp_empty
            ldd     2,x
            cmpd    #NIL_VAL
            beq     ev_ap_tp_last
            ; Non-last: eval and discard.
            pshs    x
            ldx     ,x
            lbsr    eval
            puls    x
            ldx     2,x
            bra     ev_ap_tp_loop
ev_ap_tp_last:
            ldx     ,x                  ; tail expr
            lbra    ev_ap_tail_dispatch
ev_ap_tp_empty:
            ldx     #NIL_VAL
            stx     ev_ap_val
            puls    d
            std     current_closure
            puls    d
            std     current_env
            ldx     ev_ap_val
            rts

; Tail-transparent COND: walk clauses, evaluate each test reentrantly
; (loop state on S stack), and tail-dispatch the matching clause body.
; Without this, cond bodies whose last form is a self-tail-call lost
; their TCO opportunity (issue #14) and ran with non-tail recursion,
; quickly blowing the pair pool on search algorithms like 8-queens.
ev_ap_tail_cond:
            ldx     2,x                 ; X = clause list
            pshs    x                   ; [S+0] = current args
ev_ap_tcn_loop:
            ldx     ,s
            cmpx    #NIL_VAL
            beq     ev_ap_tcn_none
            ldx     ,x                  ; X = clause = (test . body)
            pshs    x                   ; [S+0] = clause, [S+2] = args
            ldx     ,x                  ; test form
            lbsr    eval                ; reentrant test eval
            cmpx    #NIL_VAL
            beq     ev_ap_tcn_next
            ; Match: get body and tail-dispatch through ev_ap_tail_progn_rest
            ; directly with the body list — skipping the wrap_progn alloc that
            ; would otherwise create a transient (PROGN . body) pair on every
            ; matched clause.  Significant under deep recursion (queens(8)
            ; saves tens of thousands of transient pairs vs the wrap path).
            puls    x                   ; X = clause
            leas    2,s                 ; drop args
            ldx     2,x                 ; (body...)
            lbra    ev_ap_tail_progn_rest
ev_ap_tcn_next:
            puls    x                   ; drop clause
            ldx     ,s
            ldx     2,x
            stx     ,s
            bra     ev_ap_tcn_loop
ev_ap_tcn_none:
            leas    2,s                 ; drop args
            ; No clause matched — return NIL through the function-exit
            ; path so the saved env / closure get restored.
            ldx     #NIL_VAL
            stx     ev_ap_val
            puls    d
            std     current_closure
            puls    d
            std     current_env
            ldx     ev_ap_val
            rts

ev_tail_mode fcb    0
current_closure fdb 0
stco_params  fdb    0
stco_args    fdb    0
stco_val     fdb    0
stco_psym    fdb    0
stco_vy      fdb    0
stco_vals    rmb    32          ; up to 16 evaluated args buffered here
stco_vals_end

; Closure rest-param bind: ev_ap_pp is a symbol (not a pair).  Evaluate all
; remaining arg forms and bind ev_ap_pp to a fresh list of those values.
ev_ap_rest: ldx     #NIL_VAL
            stx     ev_ap_rhead
            stx     ev_ap_rtail
apr_loop:   ldx     ev_ap_ap
            cmpx    #NIL_VAL
            beq     apr_bind
            cmpx    #PAIR_POOL
            lblo    apr_bind
            cmpx    #PAIR_END
            lbhs    apr_bind
            ldx     ,x                  ; arg form
            ldd     ev_ap_body
            pshs    d
            ldd     ev_ap_env
            pshs    d
            ldd     ev_ap_pp
            pshs    d
            ldd     ev_ap_ap
            pshs    d
            ldd     ev_ap_rhead
            pshs    d
            ldd     ev_ap_rtail
            pshs    d
            lbsr    eval
            stx     ev_ap_val
            puls    d
            std     ev_ap_rtail
            puls    d
            std     ev_ap_rhead
            puls    d
            std     ev_ap_ap
            puls    d
            std     ev_ap_pp
            puls    d
            std     ev_ap_env
            puls    d
            std     ev_ap_body
            ldy     ev_ap_val
            ldd     #NIL_VAL
            lbsr    alloc_pair          ; (val . NIL)
            ldy     ev_ap_rtail
            cmpy    #NIL_VAL
            bne     apr_append
            stx     ev_ap_rhead
            stx     ev_ap_rtail
            bra     apr_advance
apr_append: stx     2,y
            stx     ev_ap_rtail
apr_advance:
            ldx     ev_ap_ap
            ldx     2,x
            stx     ev_ap_ap
            lbra    apr_loop
apr_bind:   ldy     ev_ap_pp            ; rest symbol
            ldd     ev_ap_rhead
            lbsr    alloc_pair          ; (rest-sym . list)
            tfr     x,y
            ldd     ev_ap_env
            lbsr    alloc_pair
            stx     ev_ap_env
            lbra    ev_ap_done_bind

ev_ap_err:  ldx     #str_apply_err
            lbsr    puts_native
            lbsr    emit_crlf
            ldx     #NIL_VAL
            rts
ev_ap_arity_err:
            ldx     #str_arity_err
            lbsr    puts_native
            lbsr    emit_crlf
            ldx     #NIL_VAL
            rts

; Macro application path.  Identical to the closure path except
;   (1) arg binding stores the RAW unevaluated form (no eval call), and
;   (2) after the body evaluates to an expansion form, we eval that form
;       one more time in the caller's env.
ev_ap_macro:
            ldx     ev_ap_fn
            ldx     2,x                 ; mid
            ldd     ,x
            std     ev_ap_params
            ldx     2,x                 ; inner
            ldd     ,x
            std     ev_ap_body
            ldd     2,x
            std     ev_ap_env
            ldx     ev_ap_expr
            ldx     2,x
            stx     ev_ap_args
            ldx     ev_ap_params
            stx     ev_ap_pp
            ldx     ev_ap_args
            stx     ev_ap_ap
mac_loop:   ldx     ev_ap_pp
            cmpx    #NIL_VAL
            beq     mac_done_bind
            cmpx    #PAIR_POOL
            lblo    mac_rest
            cmpx    #PAIR_END
            lbhs    mac_rest
            ldy     ev_ap_ap
            cmpy    #NIL_VAL
            lbeq    ev_ap_arity_err
            ldd     ,y                  ; arg form (NOT evaluated)
            std     ev_ap_val
            ldx     ev_ap_pp
            ldy     ,x                  ; param symbol
            ldd     ev_ap_val
            lbsr    alloc_pair          ; (param . arg-form)
            tfr     x,y
            ldd     ev_ap_env
            lbsr    alloc_pair
            stx     ev_ap_env
            ldx     ev_ap_pp
            ldx     2,x
            stx     ev_ap_pp
            ldx     ev_ap_ap
            ldx     2,x
            stx     ev_ap_ap
            bra     mac_loop
mac_rest:   ; ev_ap_pp is a symbol.  Macros take RAW forms, so the rest
            ; binding is just `ev_ap_ap` itself (the remaining arg list) —
            ; no evaluation, no copy needed.
            ldy     ev_ap_pp
            ldd     ev_ap_ap
            lbsr    alloc_pair          ; (rest-sym . arg-forms)
            tfr     x,y
            ldd     ev_ap_env
            lbsr    alloc_pair
            stx     ev_ap_env
            ldx     #NIL_VAL            ; signal "all args consumed"
            stx     ev_ap_ap
            lbra    mac_done_bind
mac_done_bind:
            ldx     ev_ap_ap
            cmpx    #NIL_VAL
            lbne    ev_ap_arity_err
            ldd     current_env
            pshs    d                   ; save outer env
            ldd     ev_ap_env
            std     current_env
            ldx     ev_ap_body
            lbsr    eval                ; macro body → expansion form
            stx     ev_ap_val
            puls    d
            std     current_env        ; restore caller's env
            ldx     ev_ap_val
            lbsr    eval                ; evaluate the expansion
            rts

; ---------------------------------------------------------------------------
; Phase 5 — COND / LET / SETQ
; ---------------------------------------------------------------------------

; (COND (test1 expr1) (test2 expr2) ... ) — evaluate tests in order; the
; first clause whose test is non-NIL has its body (implicit PROGN)
; evaluated and returned.  If no test succeeds the result is NIL.
;
; Loop state lives on the S stack so nested COND evaluation (e.g. the
; test form contains another COND) can not corrupt the outer iteration.
; The previous global-scratch implementation (ev_cn_args / ev_cn_clause)
; was clobbered by recursive cond calls, which silently bypassed the
; outer clause body — see issue #14.
ev_cond:    ldx     ev_expr_scratch
            ldx     2,x                 ; X = clause list
            pshs    x                   ; [S+0] = current args
ev_cn_loop: ldx     ,s                  ; reload current args
            cmpx    #NIL_VAL
            beq     ev_cn_none
            ldx     ,x                  ; X = clause = (test . body)
            pshs    x                   ; [S+0] = clause, [S+2] = args
            ldx     ,x                  ; test form
            lbsr    eval                ; recursive eval — safely reentrant
            cmpx    #NIL_VAL
            beq     ev_cn_next
            ; Non-NIL: evaluate the body (implicit PROGN) and tail-eval.
            puls    x                   ; X = clause (saved)
            leas    2,s                 ; drop args
            ldx     2,x                 ; (body...)
            lbsr    wrap_progn
            lbra    eval
ev_cn_next: puls    x                   ; drop saved clause
            ldx     ,s                  ; current args
            ldx     2,x                 ; cdr → next clause list
            stx     ,s
            bra     ev_cn_loop
ev_cn_none: leas    2,s                 ; drop args
            ldx     #NIL_VAL
            rts

; (LET ((v1 e1) (v2 e2) ...) body) — evaluate each ei in the outer env,
; bind the collected pairs onto a fresh env, evaluate body in it.
ev_let:     ldx     ev_expr_scratch
            ldx     2,x                 ; (bindings body...)
            stx     ev_lt_args
            ldx     ,x                  ; bindings list
            stx     ev_lt_bindings
            ldx     ev_lt_args
            ldx     2,x                 ; (body...)
            lbsr    wrap_progn
            stx     ev_lt_body
            ; Seed new_env with current_env; accumulate bindings without
            ; touching current_env itself so each ei is evaluated in the
            ; OUTER env (parallel let semantics).
            ldd     current_env
            std     ev_lt_newenv
ev_lt_loop: ldx     ev_lt_bindings
            cmpx    #NIL_VAL
            beq     ev_lt_done
            ldx     ,x                  ; binding = (var expr)
            stx     ev_lt_cur
            ldx     2,x                 ; (expr)
            ldx     ,x                  ; expr form
            ; Save loop state across eval — a nested LET / LET* / LETREC
            ; inside the expr evaluation reuses these memory scratches
            ; and would otherwise clobber them (reproducible via
            ; `(let ((i (inner-let-fn))) i)`).
            ldd     ev_lt_bindings
            pshs    d
            ldd     ev_lt_cur
            pshs    d
            ldd     ev_lt_newenv
            pshs    d
            ldd     ev_lt_body
            pshs    d
            lbsr    eval                ; value in outer env
            stx     ev_lt_val
            puls    d
            std     ev_lt_body
            puls    d
            std     ev_lt_newenv
            puls    d
            std     ev_lt_cur
            puls    d
            std     ev_lt_bindings
            ldx     ev_lt_cur
            ldy     ,x                  ; var symbol
            ldd     ev_lt_val
            lbsr    alloc_pair          ; (var . val)
            tfr     x,y
            ldd     ev_lt_newenv
            lbsr    alloc_pair          ; (binding . new_env)
            stx     ev_lt_newenv
            ldx     ev_lt_bindings
            ldx     2,x
            stx     ev_lt_bindings
            bra     ev_lt_loop
ev_lt_done: ldd     current_env
            pshs    d                   ; save outer env
            ldd     ev_lt_newenv
            std     current_env
            ldx     ev_lt_body
            lbsr    eval
            stx     ev_lt_result
            puls    d
            std     current_env
            ldx     ev_lt_result
            rts

; (LET* ((v1 e1) (v2 e2) ...) body) — sequential binding.
; Each ei is evaluated in the env extended with v1..v(i-1) already bound,
; so later initialisers may refer to earlier ones.
ev_let_star:
            ldx     ev_expr_scratch
            ldx     2,x
            stx     ev_lts_args
            ldx     ,x
            stx     ev_lts_bindings
            ldx     ev_lts_args
            ldx     2,x                 ; (body...)
            lbsr    wrap_progn
            stx     ev_lts_body
            ldd     current_env
            pshs    d                   ; save outer env
lts_loop:   ldx     ev_lts_bindings
            cmpx    #NIL_VAL
            lbeq    lts_body
            ldx     ,x                  ; binding = (v e)
            stx     ev_lts_cur
            ldx     2,x
            ldx     ,x                  ; e form
            ldd     ev_lts_body
            pshs    d
            ldd     ev_lts_bindings
            pshs    d
            ldd     ev_lts_cur
            pshs    d
            lbsr    eval
            stx     ev_lts_val
            puls    d
            std     ev_lts_cur
            puls    d
            std     ev_lts_bindings
            puls    d
            std     ev_lts_body
            ldx     ev_lts_cur
            ldy     ,x                  ; v symbol
            ldd     ev_lts_val
            lbsr    alloc_pair          ; (v . val)
            tfr     x,y
            ldd     current_env
            lbsr    alloc_pair          ; (binding . env) — update in place
            stx     current_env
            ldx     ev_lts_bindings
            ldx     2,x
            stx     ev_lts_bindings
            lbra    lts_loop
lts_body:   ldx     ev_lts_body
            lbsr    eval
            stx     ev_lts_result
            puls    d
            std     current_env
            ldx     ev_lts_result
            rts

; (LETREC ((v1 e1) ...) body) — mutually-recursive binding.  Pre-binds
; every vi to NIL so the initialisers can refer to each other (and
; themselves), then evaluates the ei and mutates the corresponding
; binding's cdr with the real value.  Classical trick from Lisp 1.5.
ev_letrec:  ldx     ev_expr_scratch
            ldx     2,x
            stx     ev_ltr_args
            ldx     ,x
            stx     ev_ltr_bindings
            ldx     ev_ltr_args
            ldx     2,x                 ; (body...)
            lbsr    wrap_progn
            stx     ev_ltr_body
            ldd     current_env
            pshs    d                   ; save outer env
            ldx     ev_ltr_bindings
            stx     ev_ltr_walk
ltr_pre:    ldx     ev_ltr_walk
            cmpx    #NIL_VAL
            lbeq    ltr_evals
            ldx     ,x                  ; binding (v e)
            ldy     ,x                  ; v
            ldd     #NIL_VAL
            lbsr    alloc_pair          ; (v . NIL)
            tfr     x,y
            ldd     current_env
            lbsr    alloc_pair          ; (binding . env)
            stx     current_env
            ldx     ev_ltr_walk
            ldx     2,x
            stx     ev_ltr_walk
            lbra    ltr_pre
ltr_evals:  ldx     ev_ltr_bindings
            stx     ev_ltr_walk
ltr_ev_lp:  ldx     ev_ltr_walk
            cmpx    #NIL_VAL
            lbeq    ltr_body
            ldx     ,x
            stx     ev_ltr_cur
            ldx     2,x
            ldx     ,x                  ; e form
            ldd     ev_ltr_body
            pshs    d
            ldd     ev_ltr_walk
            pshs    d
            ldd     ev_ltr_cur
            pshs    d
            lbsr    eval
            stx     ev_ltr_val
            puls    d
            std     ev_ltr_cur
            puls    d
            std     ev_ltr_walk
            puls    d
            std     ev_ltr_body
            ldx     ev_ltr_cur
            ldx     ,x                  ; v symbol
            stx     ev_ltr_target
            ldy     current_env
ltr_find:   ldx     ,y                  ; binding pair
            ldd     ,x
            cmpd    ev_ltr_target
            beq     ltr_mutate
            ldy     2,y
            bra     ltr_find            ; pre-bind guarantees we find it
ltr_mutate: ldd     ev_ltr_val
            std     2,x                 ; binding.cdr = val
            ldx     ev_ltr_walk
            ldx     2,x
            stx     ev_ltr_walk
            lbra    ltr_ev_lp
ltr_body:   ldx     ev_ltr_body
            lbsr    eval
            stx     ev_ltr_val
            puls    d
            std     current_env
            ldx     ev_ltr_val
            rts

; (APPLY f args-list) — call the function value in f with the elements of
; args-list as arguments.  Works for both builtins and closures by
; synthesising the form ((QUOTE f-val) (QUOTE v1) ... (QUOTE vN)) and
; evaluating it: each (QUOTE x) is self-returning, and ev_apply's normal
; dispatch then handles both function kinds.  Costs ~3(N+1) temporary
; pairs that become garbage after the call.
ev_apply_fn:
            ldx     ev_expr_scratch
            ldx     2,x                 ; (f args-expr)
            pshs    x
            ldx     ,x
            lbsr    eval                ; f value
            stx     ev_af_fn
            ldx     ,s
            ldx     2,x
            ldx     ,x
            lbsr    eval                ; args list
            stx     ev_af_argvals
            leas    2,s
            ldx     #NIL_VAL
            stx     ev_af_qargs
            stx     ev_af_qtail
            ldx     ev_af_argvals
            stx     ev_af_walk
af_qloop:   ldx     ev_af_walk
            cmpx    #NIL_VAL
            beq     af_qdone
            cmpx    #PAIR_POOL
            lblo    af_qdone
            cmpx    #PAIR_END
            lbhs    af_qdone
            ldy     ,x                  ; current value
            ldd     #NIL_VAL
            lbsr    alloc_pair          ; (val . NIL)
            tfr     x,d
            ldy     sym_QUOTE
            lbsr    alloc_pair          ; (QUOTE . (val . NIL))
            ldy     ev_af_qtail
            cmpy    #NIL_VAL
            bne     af_append
            tfr     x,y
            ldd     #NIL_VAL
            lbsr    alloc_pair          ; (quoted-form . NIL)
            stx     ev_af_qargs
            stx     ev_af_qtail
            bra     af_qnext
af_append:  tfr     x,y
            ldd     #NIL_VAL
            lbsr    alloc_pair
            ldy     ev_af_qtail
            stx     2,y
            stx     ev_af_qtail
af_qnext:   ldx     ev_af_walk
            ldx     2,x
            stx     ev_af_walk
            bra     af_qloop
af_qdone:   ldy     ev_af_fn
            ldd     #NIL_VAL
            lbsr    alloc_pair          ; (f-val . NIL)
            tfr     x,d
            ldy     sym_QUOTE
            lbsr    alloc_pair          ; (QUOTE . (f-val . NIL))
            tfr     x,y
            ldd     ev_af_qargs
            lbsr    alloc_pair          ; ((quote f-val) . qargs)
            lbsr    eval
            rts

ev_lts_args  fdb    0
ev_lts_bindings fdb 0
ev_lts_body  fdb    0
ev_lts_cur   fdb    0
ev_lts_val   fdb    0
ev_lts_result fdb   0
ev_ltr_args  fdb    0
ev_ltr_bindings fdb 0
ev_ltr_body  fdb    0
ev_ltr_cur   fdb    0
ev_ltr_val   fdb    0
ev_ltr_walk  fdb    0
ev_ltr_target fdb   0
ev_af_fn     fdb    0
ev_af_argvals fdb   0
ev_af_qargs  fdb    0
ev_af_qtail  fdb    0
ev_af_walk   fdb    0
ev_ap_rhead  fdb    0
ev_ap_rtail  fdb    0

; ---------------------------------------------------------------------------
; Phase 9 — usability builtins: LIST / PRINT / NEWLINE / ASSOC
; ---------------------------------------------------------------------------

; (LIST e1 e2 ... eN) — evaluate all args, return a fresh list of values.
; Variable arity; zero args returns NIL.
ev_list:    ldx     ev_expr_scratch
            ldx     2,x                 ; arg forms
            stx     ev_ls_args
            ldx     #NIL_VAL
            stx     ev_ls_head
            stx     ev_ls_tail
ls_loop:    ldx     ev_ls_args
            cmpx    #NIL_VAL
            beq     ls_done
            ldx     ,x                  ; current arg form
            ; Save loop state across eval.
            ldd     ev_ls_args
            pshs    d
            ldd     ev_ls_head
            pshs    d
            ldd     ev_ls_tail
            pshs    d
            lbsr    eval
            stx     ev_ls_val
            puls    d
            std     ev_ls_tail
            puls    d
            std     ev_ls_head
            puls    d
            std     ev_ls_args
            ; Build new cell (val . NIL) and link to tail.
            ldy     ev_ls_val
            ldd     #NIL_VAL
            lbsr    alloc_pair          ; X = (val . NIL)
            ldy     ev_ls_tail
            cmpy    #NIL_VAL
            bne     ls_append
            stx     ev_ls_head
            stx     ev_ls_tail
            bra     ls_next
ls_append:  stx     2,y                 ; tail.cdr = new cell
            stx     ev_ls_tail
ls_next:    ldx     ev_ls_args
            ldx     2,x
            stx     ev_ls_args
            bra     ls_loop
ls_done:    ldx     ev_ls_head
            rts

; (PRINT x) — eval x, print value + CRLF, return the value.
ev_print:   lbsr    eval_one_arg
            stx     ev_pn_val_t
            lbsr    print_expr
            lbsr    emit_crlf
            ldx     ev_pn_val_t
            rts

; (NEWLINE) — emit CRLF, return NIL.
ev_newline: lbsr    emit_crlf
            ldx     #NIL_VAL
            rts

; (ASSOC key alist) — find the first binding pair whose car eq key.
; Returns the binding pair, or NIL if not found.
ev_assoc:   ldx     ev_expr_scratch
            ldx     2,x
            pshs    x                   ; args
            ldx     ,x
            lbsr    eval                ; key value
            stx     ev_as_key
            ldx     ,s
            ldx     2,x
            ldx     ,x
            lbsr    eval                ; alist value
            leas    2,s                 ; drop args
            stx     ev_as_walk
as_loop:    ldx     ev_as_walk
            cmpx    #NIL_VAL
            beq     as_nil
            cmpx    #PAIR_POOL
            lblo    as_nil
            cmpx    #PAIR_END
            lbhs    as_nil
            ldy     ,x                  ; binding pair
            cmpy    #PAIR_POOL
            lblo    as_skip
            cmpy    #PAIR_END
            lbhs    as_skip
            ldd     ,y                  ; key' of binding
            cmpd    ev_as_key
            beq     as_found
as_skip:    ldx     ev_as_walk
            ldx     2,x
            stx     ev_as_walk
            bra     as_loop
as_found:   tfr     y,x
            rts
as_nil:     ldx     #NIL_VAL
            rts

ev_ls_args   fdb    0
ev_ls_head   fdb    0
ev_ls_tail   fdb    0
ev_ls_val    fdb    0
ev_pn_val_t  fdb    0
ev_as_key    fdb    0
ev_as_walk   fdb    0

; (GENSYM) — return a fresh symbol unique across the session.  Implemented
; as "g<4-hex>" where the leading lowercase `g` guarantees no collision
; with reader-produced symbols (the reader upcases a-z before intern).
ev_gensym:  ldd     gensym_counter
            addd    #1
            std     gensym_counter
            lda     #5
            sta     sym_buf             ; length byte
            lda     #'g'
            sta     sym_buf+1
            ldx     #sym_buf+2
            lda     gensym_counter      ; high byte
            lbsr    hex_byte_to_x
            lda     gensym_counter+1
            lbsr    hex_byte_to_x
            ldx     #sym_buf
            lbra    intern

; ---------------------------------------------------------------------------
; String primitives.  Strings are stored in $9000-$9FFF as:
;   [length byte] [content bytes ...]
; Allocation is bump-only (no GC for strings in this first version).
; ---------------------------------------------------------------------------

; (STRING-LENGTH s) — s must be a string value.  Returns fixnum length.
ev_strlen:  lbsr    eval_one_arg
            stx     ev_str_a
            cmpx    #STR_POOL
            lblo    ev_str_err
            cmpx    #STR_END
            lbhs    ev_str_err
            clra
            ldb     ,x              ; length byte
            lslb
            rola
            addd    #1              ; fixnum tag
            tfr     d,x
            rts

; (STRING= s1 s2) — byte-wise equality.
ev_streq:   ldx     ev_expr_scratch
            ldx     2,x
            pshs    x
            ldx     ,x
            lbsr    eval
            stx     ev_str_a
            ldx     ,s
            ldx     2,x
            ldx     ,x
            lbsr    eval
            stx     ev_str_b
            leas    2,s
            ldx     ev_str_a
            cmpx    #STR_POOL
            lblo    ev_str_err
            cmpx    #STR_END
            lbhs    ev_str_err
            ldy     ev_str_b
            cmpy    #STR_POOL
            lblo    ev_str_err
            cmpy    #STR_END
            lbhs    ev_str_err
            ldb     ,x              ; len a
            cmpb    ,y              ; len b
            bne     se_false
            tstb
            beq     se_true         ; both empty
se_loop:    lda     1,x
            cmpa    1,y
            bne     se_false
            leax    1,x
            leay    1,y
            decb
            bne     se_loop
se_true:    ldx     #T_VAL
            rts
se_false:   ldx     #NIL_VAL
            rts

; (STRING-APPEND s1 s2) — concatenate.  Allocates in the string pool.
ev_strapp:  ldx     ev_expr_scratch
            ldx     2,x
            pshs    x
            ldx     ,x
            lbsr    eval
            stx     ev_str_a
            ldx     ,s
            ldx     2,x
            ldx     ,x
            lbsr    eval
            stx     ev_str_b
            leas    2,s
            ldx     ev_str_a
            cmpx    #STR_POOL
            lblo    ev_str_err
            cmpx    #STR_END
            lbhs    ev_str_err
            ldy     ev_str_b
            cmpy    #STR_POOL
            lblo    ev_str_err
            cmpy    #STR_END
            lbhs    ev_str_err
            ldy     str_next
            sty     ev_str_dst
            leay    1,y             ; past length slot
            ldx     ev_str_a
            ldb     ,x+             ; len a
            tstb
            beq     sa_a_done
sa_a_loop:  lda     ,x+
            sta     ,y+
            decb
            bne     sa_a_loop
sa_a_done:  ldx     ev_str_b
            ldb     ,x+
            tstb
            beq     sa_b_done
sa_b_loop:  lda     ,x+
            sta     ,y+
            decb
            bne     sa_b_loop
sa_b_done:  ; total length = len_a + len_b
            ldx     ev_str_a
            ldb     ,x
            ldx     ev_str_b
            addb    ,x
            ldx     ev_str_dst
            stb     ,x              ; write length byte
            clra
            addd    #1              ; +1 for length byte
            addd    ev_str_dst
            std     str_next
            lbsr    align_str_next
            rts

; (STRING-REF s n) — return fixnum char code at index n.
ev_strref:  ldx     ev_expr_scratch
            ldx     2,x
            pshs    x
            ldx     ,x
            lbsr    eval
            stx     ev_str_a
            ldx     ,s
            ldx     2,x
            ldx     ,x
            lbsr    eval            ; X = fixnum index
            leas    2,s
            ; Untag index.
            tfr     x,d
            subd    #1
            asra
            rorb
            ; B = index (assume < 256).
            ldx     ev_str_a
            cmpx    #STR_POOL
            lblo    ev_str_err
            cmpx    #STR_END
            lbhs    ev_str_err
            abx                     ; X = str_ptr + index
            leax    1,x             ; +1 to skip length byte
            lda     ,x              ; A = char code
            tfr     a,b
            clra
            lslb
            rola
            addd    #1              ; fixnum tag
            tfr     d,x
            rts

ev_str_err: ldx     #str_strerr
            lbsr    puts_native
            lbsr    emit_crlf
            ldx     #NIL_VAL
            rts

; (STRING->LIST s) — return list of fixnum char codes.
ev_str2l:   lbsr    eval_one_arg
            stx     ev_str_a
            cmpx    #STR_POOL
            lblo    ev_str_err
            cmpx    #STR_END
            lbhs    ev_str_err
            ldb     ,x              ; length
            leax    1,x
            stx     ev_str_p        ; byte ptr
            stb     ev_str_n
            ldx     #NIL_VAL
            stx     ev_str_head
            stx     ev_str_tail
sl_loop:    ldb     ev_str_n
            tstb
            beq     sl_done
            ldx     ev_str_p
            lda     ,x+
            stx     ev_str_p
            tfr     a,b
            clra
            lslb
            rola
            addd    #1
            tfr     d,y             ; Y = tagged fixnum
            ldd     #NIL_VAL
            lbsr    alloc_pair      ; (char . NIL)
            ldy     ev_str_tail
            cmpy    #NIL_VAL
            bne     sl_append
            stx     ev_str_head
            stx     ev_str_tail
            bra     sl_next
sl_append:  stx     2,y
            stx     ev_str_tail
sl_next:    dec     ev_str_n
            bra     sl_loop
sl_done:    ldx     ev_str_head
            rts

; (LIST->STRING lst) — build a string from a list of fixnum char codes.
ev_l2str:   lbsr    eval_one_arg
            stx     ev_str_p
            ldx     str_next
            stx     ev_str_dst
            leax    1,x             ; skip length slot
            clr     ev_str_n        ; counter in memory (B is reused for char code)
l2s_loop:   ldy     ev_str_p
            cmpy    #NIL_VAL
            beq     l2s_done
            cmpy    #PAIR_POOL
            lblo    l2s_done
            cmpy    #PAIR_END
            lbhs    l2s_done
            ldd     ,y              ; car fixnum
            subd    #1
            asra
            rorb
            stb     ,x+             ; write char byte
            inc     ev_str_n
            ldy     ev_str_p
            ldy     2,y
            sty     ev_str_p
            bra     l2s_loop
l2s_done:   ldy     ev_str_dst
            ldb     ev_str_n
            stb     ,y              ; length
            clra
            addd    #1              ; +1 for length byte
            addd    ev_str_dst
            std     str_next
            lbsr    align_str_next
            tfr     y,x
            rts

ev_str_a    fdb     0
ev_str_b    fdb     0
ev_str_dst  fdb     0
ev_str_p    fdb     0
ev_str_n    fcb     0
ev_str_head fdb     0
ev_str_tail fdb     0

str_strerr  fcc     "STRING: bad arg"
            fcb     0

; ---------------------------------------------------------------------------
; 32-bit int box pool ($B000-$BFFF, 1024 boxes × 4 bytes).  Bump-allocated
; with a free list (populated by GC sweep — see gc_run32).  Each box holds a
; signed 32-bit integer in big-endian (hi word / lo word).
; ---------------------------------------------------------------------------

; alloc_int32: allocate a box and store i32_scratch (4 bytes) in it.
;   In:  i32_scratch (4 bytes, big-endian signed int32)
;   Out: X = box pointer (in $B000..$BFFF)
alloc_int32:
            ldx     int32_free
            cmpx    #NIL_VAL
            beq     ai32_bump
            ldd     ,x               ; next-free pointer in first 2 bytes
            std     int32_free
            bra     ai32_fill
ai32_bump:  ldx     int32_next
            cmpx    #INT32_END
            lbhs    int32_oom
            leax    4,x
            stx     int32_next
            leax    -4,x
ai32_fill:  ldd     i32_scratch
            std     ,x
            ldd     i32_scratch+2
            std     2,x
            rts
int32_oom:  ldx     #str_i32_oom
            lbsr    puts_native
            lbsr    emit_crlf
            ldx     #NIL_VAL
            rts
str_i32_oom fcc     "ALLOC: int32 pool exhausted"
            fcb     0

; untag_to_i32: X = fixnum or int32 box → i32_scratch = signed 32-bit.
untag_to_i32:
            tfr     x,d
            andb    #1
            beq     ui_box
            ; fixnum: sign-extend
            tfr     x,d
            subd    #1
            asra
            rorb
            std     i32_scratch+2
            bpl     ui_pos_hi
            ldd     #$FFFF
            bra     ui_store_hi
ui_pos_hi:  ldd     #0
ui_store_hi:
            std     i32_scratch
            rts
ui_box:     ldd     ,x
            std     i32_scratch
            ldd     2,x
            std     i32_scratch+2
            rts

; fit_and_box: examine i32_res (4-byte signed int32).
;   If value fits in 15-bit signed (-16384..16383) return as fixnum.
;   Otherwise allocate an int32 box and return pointer to it.
;   Out: X = fixnum or box pointer
fit_and_box:
            ldd     i32_res          ; high word
            beq     fab_pos
            cmpd    #$FFFF
            bne     fab_box
            ; hi == $FFFF: negative. Low word must be >= $C000 unsigned.
            ldd     i32_res+2
            cmpd    #$C000
            blo     fab_box
            bra     fab_tag
fab_pos:    ; hi == 0: non-negative. Low word must be <= $3FFF.
            ldd     i32_res+2
            cmpd    #$4000
            bhs     fab_box
fab_tag:    ldd     i32_res+2
            lslb
            rola
            addd    #1
            tfr     d,x
            rts
fab_box:    ldd     i32_res
            std     i32_scratch
            ldd     i32_res+2
            std     i32_scratch+2
            lbra    alloc_int32

; i32_add: i32_res = i32_a + i32_b
i32_add:    lda     i32_a+3
            adda    i32_b+3
            sta     i32_res+3
            lda     i32_a+2
            adca    i32_b+2
            sta     i32_res+2
            lda     i32_a+1
            adca    i32_b+1
            sta     i32_res+1
            lda     i32_a
            adca    i32_b
            sta     i32_res
            rts

; i32_sub: i32_res = i32_a - i32_b
i32_sub:    lda     i32_a+3
            suba    i32_b+3
            sta     i32_res+3
            lda     i32_a+2
            sbca    i32_b+2
            sta     i32_res+2
            lda     i32_a+1
            sbca    i32_b+1
            sta     i32_res+1
            lda     i32_a
            sbca    i32_b
            sta     i32_res
            rts

; i32_cmp: compare i32_a vs i32_b as signed 32-bit.
;   Out: B = -1 if a < b, 0 if a == b, 1 if a > b.
;   Note: mutates i32_res.
i32_cmp:    lbsr    i32_sub
            lda     i32_res
            ora     i32_res+1
            ora     i32_res+2
            ora     i32_res+3
            beq     i32_cmp_eq
            lda     i32_res
            bpl     i32_cmp_gt
            ldb     #$FF            ; a < b
            rts
i32_cmp_eq: clrb
            rts
i32_cmp_gt: ldb     #1
            rts

; i32_negate: i32_res = -i32_res (two's complement in place).
i32_negate: com     i32_res
            com     i32_res+1
            com     i32_res+2
            com     i32_res+3
            ldd     i32_res+2
            addd    #1
            std     i32_res+2
            ldd     i32_res
            adcb    #0
            adca    #0
            std     i32_res
            rts

; i32_neg_a: in-place negate i32_a.
i32_neg_a:  com     i32_a
            com     i32_a+1
            com     i32_a+2
            com     i32_a+3
            ldd     i32_a+2
            addd    #1
            std     i32_a+2
            ldd     i32_a
            adcb    #0
            adca    #0
            std     i32_a
            rts

; i32_neg_b: in-place negate i32_b.
i32_neg_b:  com     i32_b
            com     i32_b+1
            com     i32_b+2
            com     i32_b+3
            ldd     i32_b+2
            addd    #1
            std     i32_b+2
            ldd     i32_b
            adcb    #0
            adca    #0
            std     i32_b
            rts

; i32_mul: i32_res = i32_a * i32_b (32x32 → 32-bit signed, truncating).
; Uses shift-and-add on absolute values, then applies sign.
i32_mul:    clr     mul32_sign
            lda     i32_a
            bpl     m32_ap
            lbsr    i32_neg_a
            com     mul32_sign
m32_ap:     lda     i32_b
            bpl     m32_bp
            lbsr    i32_neg_b
            com     mul32_sign
m32_bp:     ldd     #0
            std     i32_res
            std     i32_res+2
            lda     #32
            sta     mul32_cnt
m32_loop:   lsr     i32_b           ; shift i32_b right 1, lsb → C
            ror     i32_b+1
            ror     i32_b+2
            ror     i32_b+3
            bcc     m32_no_add
            ; res += a (32-bit)
            lda     i32_a+3
            adda    i32_res+3
            sta     i32_res+3
            lda     i32_a+2
            adca    i32_res+2
            sta     i32_res+2
            lda     i32_a+1
            adca    i32_res+1
            sta     i32_res+1
            lda     i32_a
            adca    i32_res
            sta     i32_res
m32_no_add: ; a <<= 1
            asl     i32_a+3
            rol     i32_a+2
            rol     i32_a+1
            rol     i32_a
            dec     mul32_cnt
            bne     m32_loop
            lda     mul32_sign
            beq     m32_done
            lbsr    i32_negate
m32_done:   rts

; i32_divmod: quotient → i32_res, remainder → i32_rem.
; Inputs: i32_a = dividend, i32_b = divisor.  Truncation semantics.
i32_divmod: clr     dv32_nq
            clr     dv32_nr
            lda     i32_a
            bpl     d32_ap
            lbsr    i32_neg_a
            com     dv32_nq
            com     dv32_nr
d32_ap:     lda     i32_b
            bpl     d32_bp
            lbsr    i32_neg_b
            com     dv32_nq
d32_bp:     ldd     #0
            std     i32_res
            std     i32_res+2
            std     i32_rem
            std     i32_rem+2
            lda     #32
            sta     dv32_cnt
d32_loop:   ; Shift top bit of i32_a into i32_rem (left-shift both 32-bit).
            asl     i32_a+3
            rol     i32_a+2
            rol     i32_a+1
            rol     i32_a
            rol     i32_rem+3
            rol     i32_rem+2
            rol     i32_rem+1
            rol     i32_rem
            ; Compare i32_rem vs i32_b (unsigned, byte-by-byte).
            lda     i32_rem
            cmpa    i32_b
            blo     d32_noSub
            bhi     d32_sub
            lda     i32_rem+1
            cmpa    i32_b+1
            blo     d32_noSub
            bhi     d32_sub
            lda     i32_rem+2
            cmpa    i32_b+2
            blo     d32_noSub
            bhi     d32_sub
            lda     i32_rem+3
            cmpa    i32_b+3
            blo     d32_noSub
d32_sub:    ; rem -= b (32-bit)
            lda     i32_rem+3
            suba    i32_b+3
            sta     i32_rem+3
            lda     i32_rem+2
            sbca    i32_b+2
            sta     i32_rem+2
            lda     i32_rem+1
            sbca    i32_b+1
            sta     i32_rem+1
            lda     i32_rem
            sbca    i32_b
            sta     i32_rem
            ; Set low bit of quotient (shifted)
            asl     i32_res+3
            rol     i32_res+2
            rol     i32_res+1
            rol     i32_res
            lda     i32_res+3
            ora     #1
            sta     i32_res+3
            bra     d32_next
d32_noSub:  asl     i32_res+3
            rol     i32_res+2
            rol     i32_res+1
            rol     i32_res
d32_next:   dec     dv32_cnt
            lbne    d32_loop
            lda     dv32_nq
            beq     d32_qp
            lbsr    i32_negate
d32_qp:     lda     dv32_nr
            beq     d32_rp
            ; Negate i32_rem
            com     i32_rem
            com     i32_rem+1
            com     i32_rem+2
            com     i32_rem+3
            ldd     i32_rem+2
            addd    #1
            std     i32_rem+2
            ldd     i32_rem
            adcb    #0
            adca    #0
            std     i32_rem
d32_rp:     rts

mul32_sign  fcb     0
mul32_cnt   fcb     0
dv32_cnt    fcb     0
dv32_nq     fcb     0
dv32_nr     fcb     0
i32_rem     fdb     0,0

; Scratch area for 32-bit arithmetic (4 bytes each, big-endian).
i32_a       fdb     0,0
i32_b       fdb     0,0
i32_res     fdb     0,0
i32_scratch fdb     0,0

; ---------------------------------------------------------------------------
; (/) and (MOD) — 16-bit signed integer division.  Truncation semantics:
; quotient rounds toward zero, remainder has the sign of the dividend.
; ---------------------------------------------------------------------------

; Evaluate both args and store into i32_a / i32_b (fixnum or int32 box).
divmod_eval:
            ldx     ev_expr_scratch
            ldx     2,x
            pshs    x
            ldx     ,x
            lbsr    eval
            pshs    x
            ldx     2,s
            ldx     2,x
            ldx     ,x
            lbsr    eval                ; B_val in X
            lbsr    untag_to_i32
            ldd     i32_scratch
            std     i32_b
            ldd     i32_scratch+2
            std     i32_b+2
            puls    x
            lbsr    untag_to_i32
            ldd     i32_scratch
            std     i32_a
            ldd     i32_scratch+2
            std     i32_a+2
            leas    2,s
            rts

; (/ a b) → truncated quotient.
ev_div:     lbsr    divmod_eval
            lbsr    i32_divmod
            lbsr    fit_and_box
            rts

; (MOD a b) → remainder (sign follows dividend).
ev_mod:     lbsr    divmod_eval
            lbsr    i32_divmod
            ldd     i32_rem
            std     i32_res
            ldd     i32_rem+2
            std     i32_res+2
            lbsr    fit_and_box
            rts

; ---------------------------------------------------------------------------
; Bitwise logical ops on 32-bit integers (LOGAND / LOGIOR / LOGXOR / LOGNOT).
; Values are promoted to i32 first, then the op is applied byte-wise, then
; fit_and_box picks the narrowest representation.
; ---------------------------------------------------------------------------
ev_logand:  lbsr    divmod_eval
            lda     i32_a
            anda    i32_b
            sta     i32_res
            lda     i32_a+1
            anda    i32_b+1
            sta     i32_res+1
            lda     i32_a+2
            anda    i32_b+2
            sta     i32_res+2
            lda     i32_a+3
            anda    i32_b+3
            sta     i32_res+3
            lbra    fit_and_box

ev_logior:  lbsr    divmod_eval
            lda     i32_a
            ora     i32_b
            sta     i32_res
            lda     i32_a+1
            ora     i32_b+1
            sta     i32_res+1
            lda     i32_a+2
            ora     i32_b+2
            sta     i32_res+2
            lda     i32_a+3
            ora     i32_b+3
            sta     i32_res+3
            lbra    fit_and_box

ev_logxor:  lbsr    divmod_eval
            lda     i32_a
            eora    i32_b
            sta     i32_res
            lda     i32_a+1
            eora    i32_b+1
            sta     i32_res+1
            lda     i32_a+2
            eora    i32_b+2
            sta     i32_res+2
            lda     i32_a+3
            eora    i32_b+3
            sta     i32_res+3
            lbra    fit_and_box

ev_lognot:  lbsr    eval_one_arg
            lbsr    untag_to_i32
            lda     i32_scratch
            coma
            sta     i32_res
            lda     i32_scratch+1
            coma
            sta     i32_res+1
            lda     i32_scratch+2
            coma
            sta     i32_res+2
            lda     i32_scratch+3
            coma
            sta     i32_res+3
            lbra    fit_and_box

; (ASH n shift) — arithmetic shift.  shift > 0 = left; shift < 0 = right
; (sign-preserving).  Large shifts saturate (0 for positive/left-out, -1 for
; negative/right-with-sign).
ev_ash:     ldx     ev_expr_scratch
            ldx     2,x
            pshs    x
            ldx     ,x
            lbsr    eval                ; value
            lbsr    untag_to_i32
            ldd     i32_scratch
            std     i32_res
            ldd     i32_scratch+2
            std     i32_res+2
            puls    x
            ldx     2,x
            ldx     ,x
            lbsr    eval                ; X = shift amount
            tfr     x,d
            andb    #1
            beq     ash_big_shift       ; shift is an int32 box: saturate
            ; Untag shift (fixnum, signed 16-bit in small range).
            tfr     x,d
            subd    #1
            asra
            rorb                        ; D = signed shift count
            tsta
            bmi     ash_right_from_d
            ; Positive (left) shift.
            cmpd    #32
            bhs     ash_ls_sat
            tstb
            beq     ash_return
ash_ls_loop:
            asl     i32_res+3
            rol     i32_res+2
            rol     i32_res+1
            rol     i32_res
            decb
            bne     ash_ls_loop
            lbra    fit_and_box
ash_right_from_d:
            ; Negate D to get positive right-shift count.
            coma
            comb
            addd    #1
            cmpd    #32
            bhs     ash_rs_sat
            tstb
            beq     ash_return
ash_rs_loop:
            asr     i32_res
            ror     i32_res+1
            ror     i32_res+2
            ror     i32_res+3
            decb
            bne     ash_rs_loop
            lbra    fit_and_box
ash_big_shift:
            ; X is an int32 box. Dispatch on its sign.
            lda     ,x
            bmi     ash_rs_sat
            bra     ash_ls_sat
ash_ls_sat: ldd     #0
            std     i32_res
            std     i32_res+2
            lbra    fit_and_box
ash_rs_sat: lda     i32_res
            bmi     ash_rs_sat_neg
            ldd     #0
            std     i32_res
            std     i32_res+2
            lbra    fit_and_box
ash_rs_sat_neg:
            ldd     #$FFFF
            std     i32_res
            std     i32_res+2
            lbra    fit_and_box
ash_return: lbra    fit_and_box

; ---------------------------------------------------------------------------
; PRNG: xorshift32.  rand_state is 4 bytes; step is:
;     s ^= s << 13; s ^= s >> 17; s ^= s << 5
; We split each shift into a byte-level pre-shift plus a smaller bit shift:
;   << 13 = << 8 (byte shift) then << 5
;   >> 17 = >> 16 (two-byte shift) then >> 1
;   << 5 = five iterations
; ---------------------------------------------------------------------------
xorshift32:
            ; Step 1: tmp = state << 13.  Load state shifted by 1 byte.
            lda     rand_state+1
            sta     rs_tmp
            lda     rand_state+2
            sta     rs_tmp+1
            lda     rand_state+3
            sta     rs_tmp+2
            clr     rs_tmp+3
            ldb     #5                  ; now shift tmp left 5 bits
xs_left_1:  asl     rs_tmp+3
            rol     rs_tmp+2
            rol     rs_tmp+1
            rol     rs_tmp
            decb
            bne     xs_left_1
            ; state ^= tmp
            lda     rand_state
            eora    rs_tmp
            sta     rand_state
            lda     rand_state+1
            eora    rs_tmp+1
            sta     rand_state+1
            lda     rand_state+2
            eora    rs_tmp+2
            sta     rand_state+2
            lda     rand_state+3
            eora    rs_tmp+3
            sta     rand_state+3
            ; Step 2: tmp = state >> 17.  Load state shifted right by 16 bits.
            clr     rs_tmp
            clr     rs_tmp+1
            lda     rand_state
            sta     rs_tmp+2
            lda     rand_state+1
            sta     rs_tmp+3
            ; Shift right 1 more bit.
            lsr     rs_tmp
            ror     rs_tmp+1
            ror     rs_tmp+2
            ror     rs_tmp+3
            ; state ^= tmp
            lda     rand_state
            eora    rs_tmp
            sta     rand_state
            lda     rand_state+1
            eora    rs_tmp+1
            sta     rand_state+1
            lda     rand_state+2
            eora    rs_tmp+2
            sta     rand_state+2
            lda     rand_state+3
            eora    rs_tmp+3
            sta     rand_state+3
            ; Step 3: tmp = state << 5.
            lda     rand_state
            sta     rs_tmp
            lda     rand_state+1
            sta     rs_tmp+1
            lda     rand_state+2
            sta     rs_tmp+2
            lda     rand_state+3
            sta     rs_tmp+3
            ldb     #5
xs_left_3:  asl     rs_tmp+3
            rol     rs_tmp+2
            rol     rs_tmp+1
            rol     rs_tmp
            decb
            bne     xs_left_3
            lda     rand_state
            eora    rs_tmp
            sta     rand_state
            lda     rand_state+1
            eora    rs_tmp+1
            sta     rand_state+1
            lda     rand_state+2
            eora    rs_tmp+2
            sta     rand_state+2
            lda     rand_state+3
            eora    rs_tmp+3
            sta     rand_state+3
            rts

; (RAND) — advance PRNG, return low 14 bits of new state as a fixnum
; in the range 0..16383.
ev_rand:    lbsr    xorshift32
            ldd     rand_state+2
            anda    #$3F                ; mask to 14 bits
            aslb
            rola
            addd    #1                  ; fixnum tag (bit 0 = 1)
            tfr     d,x
            rts

; (SEED n) — set PRNG state to n (fixnum or int32).  Returns n.
ev_seed:    lbsr    eval_one_arg
            pshs    x                   ; save original for return
            lbsr    untag_to_i32
            ldd     i32_scratch
            std     rand_state
            ldd     i32_scratch+2
            std     rand_state+2
            ; If seed is all zeroes, bump it so xorshift doesn't get stuck
            ; at the 0 fixed point.
            ldd     rand_state
            bne     sd_ok
            ldd     rand_state+2
            bne     sd_ok
            ldd     #1
            std     rand_state+2
sd_ok:      puls    x
            rts

; (TICK) — wall-clock cycle counter from the MMIO register at $FF02.
; Returns low 14 bits as a fixnum.  The em6809 plugin fills this in on each
; read; on an older plugin the read returns a constant (still usable as a
; cheap, monotonic counter source for seeding).
ev_tick:    ldd     $FF02
            anda    #$3F
            aslb
            rola
            addd    #1
            tfr     d,x
            rts

; (PRINT-CASE) — return current printer case mode as a fixnum
; (0 = upper / default, 1 = lower).  Use SET-PRINT-CASE! to change.
ev_pcase_get:
            clra
            ldb     print_case_mode
            aslb
            rola
            addd    #1                  ; fixnum tag (bit 0 = 1)
            tfr     d,x
            rts

; (SET-PRINT-CASE! n) — set printer case mode to fixnum n
; (0 = upper, 1 = lower).  Non-fixnum / non-{0,1} args are silently
; coerced to 0.  Returns the canonical fixnum 0 or 1.
ev_pcase_set:
            lbsr    eval_one_arg        ; X = arg value
            tfr     x,d
            andb    #1
            beq     pcs_zero            ; bit 0 == 0 → not a fixnum → upper
            tfr     x,d
            asra
            rorb                        ; D = signed value
            tstb
            beq     pcs_store           ; 0 stays 0
            ldb     #1                  ; anything else normalised to 1
            bra     pcs_store
pcs_zero:   clrb
pcs_store:  stb     print_case_mode
            clra
            aslb
            rola
            addd    #1                  ; fixnum tag
            tfr     d,x
            rts

rand_state  fdb     0,0
rs_tmp      fdb     0,0

; ---------------------------------------------------------------------------
; Type <-> string primitives: NUMBER->STRING, STRING->NUMBER,
; SYMBOL->STRING, STRING->SYMBOL.
; ---------------------------------------------------------------------------

; (NUMBER->STRING n) — fixnum or int32 -> new string of decimal digits.
ev_num2str: lbsr    eval_one_arg
            ; X is fixnum or int32 box. Write signed decimal into the string
            ; pool, using pi_hi/pi_lo as the divide-by-10 register.
            lbsr    untag_to_i32
            ldd     i32_scratch
            std     i32_res
            ldd     i32_scratch+2
            std     i32_res+2
            ldx     str_next
            pshs    x                   ; save string start (length byte slot)
            leax    1,x                 ; skip length byte
            clr     n2s_len
            lda     i32_res
            bpl     n2s_nonneg
            lda     #'-'
            sta     ,x+
            inc     n2s_len
            ldd     i32_res+2
            coma
            comb
            std     i32_res+2
            ldd     i32_res
            coma
            comb
            std     i32_res
            ldd     i32_res+2
            addd    #1
            std     i32_res+2
            ldd     i32_res
            adcb    #0
            adca    #0
            std     i32_res
n2s_nonneg: ldd     i32_res
            std     pi_hi
            ldd     i32_res+2
            std     pi_lo
            ldy     #pi_buf_end
            ldd     pi_hi
            bne     n2s_div
            ldd     pi_lo
            bne     n2s_div
            ; Value is zero.
            leay    -1,y
            lda     #'0'
            sta     ,y
            bra     n2s_copy
n2s_div:    lbsr    pi_div10
            addb    #'0'
            leay    -1,y
            stb     ,y
            ldd     pi_hi
            bne     n2s_div
            ldd     pi_lo
            bne     n2s_div
n2s_copy:   cmpy    #pi_buf_end
            bhs     n2s_done
            lda     ,y+
            sta     ,x+
            inc     n2s_len
            bra     n2s_copy
n2s_done:   stx     str_next
            lbsr    align_str_next
            ldx     ,s                  ; X = string start
            lda     n2s_len
            sta     ,x
            puls    x
            rts

n2s_len     fcb     0

; (STRING->NUMBER s) — parse decimal. Returns fixnum/int32 on success, NIL on fail.
ev_str2num: lbsr    eval_one_arg
            cmpx    #STR_POOL
            lblo    s2n_fail
            cmpx    #STR_END
            lbhs    s2n_fail
            ldb     ,x                  ; length
            tstb
            lbeq    s2n_fail
            leay    1,x                 ; Y = content
            clr     s2n_sign
            lda     ,y                  ; first char
            cmpa    #'-'
            bne     s2n_nosign
            inc     s2n_sign
            leay    1,y
            decb
            lbeq    s2n_fail            ; "-" alone is not a number
s2n_nosign: stb     s2n_cnt
            ldd     #0
            std     i32_a
            std     i32_a+2
s2n_loop:   lda     s2n_cnt
            beq     s2n_ok
            lda     ,y+
            cmpa    #'0'
            lblo    s2n_fail
            cmpa    #'9'
            lbhi    s2n_fail
            suba    #'0'
            sta     s2n_digit
            sty     s2n_y_save
            lbsr    i32_mul10
            ldy     s2n_y_save
            ldb     s2n_digit
            clra
            addd    i32_a+2
            std     i32_a+2
            ldd     i32_a
            adcb    #0
            adca    #0
            std     i32_a
            dec     s2n_cnt
            bra     s2n_loop
s2n_ok:     tst     s2n_sign
            beq     s2n_positive
            com     i32_a
            com     i32_a+1
            com     i32_a+2
            com     i32_a+3
            ldd     i32_a+2
            addd    #1
            std     i32_a+2
            ldd     i32_a
            adcb    #0
            adca    #0
            std     i32_a
s2n_positive:
            ldd     i32_a
            std     i32_res
            ldd     i32_a+2
            std     i32_res+2
            lbra    fit_and_box
s2n_fail:   ldx     #NIL_VAL
            rts

s2n_sign    fcb     0
s2n_cnt     fcb     0
s2n_digit   fcb     0
s2n_y_save  fdb     0

; (SYMBOL->STRING sym) — copy the symbol's name into a new string.
ev_sym2str: lbsr    eval_one_arg
            cmpx    #SYM_POOL
            lblo    s2s_fail
            cmpx    #SYM_END
            lbhs    s2s_fail
            stx     s2s_sym
            ldb     2,x                 ; len
            stb     s2s_len
            ldx     str_next
            stx     s2s_str
            stb     ,x+
            ldy     s2s_sym
            leay    3,y
            tstb
            beq     s2s_done
s2s_loop:   lda     ,y+
            sta     ,x+
            decb
            bne     s2s_loop
s2s_done:   stx     str_next
            lbsr    align_str_next
            ldx     s2s_str
            rts
s2s_fail:   ldx     #NIL_VAL
            rts

s2s_sym     fdb     0
s2s_str     fdb     0
s2s_len     fcb     0

; (STRING->SYMBOL s) — intern as a symbol, preserving case as-is.
ev_str2sym: lbsr    eval_one_arg
            cmpx    #STR_POOL
            lblo    t2s_fail
            cmpx    #STR_END
            lbhs    t2s_fail
            ; Counted-string layout already matches intern's expectation.
            lbra    intern
t2s_fail:   ldx     #NIL_VAL
            rts

; (EVAL form) — evaluate `form` in the global environment.
ev_eval_prim:
            lbsr    eval_one_arg
            lbra    eval

; (READ-STRING s) — parse one S-expression from `s`, return the form.
; Temporarily redirects the reader at the string content and restores afterwards.
ev_read_string:
            lbsr    eval_one_arg
            cmpx    #STR_POOL
            lblo    rds_bad
            cmpx    #STR_END
            lbhs    rds_bad
            ; Save current reader state.
            ldd     rdr_base
            pshs    d
            ldd     tib_pos
            pshs    d
            ldd     tib_len
            pshs    d
            ; Set up: base = content, len = string length, pos = 0.
            ldb     ,x                  ; B = length
            pshs    b
            tfr     x,d
            addd    #1
            std     rdr_base
            puls    b
            clra
            std     tib_len
            ldd     #0
            std     tib_pos
            lbsr    read_expr
            puls    d
            std     tib_len
            puls    d
            std     tib_pos
            puls    d
            std     rdr_base
            rts
rds_bad:    ldx     #NIL_VAL
            rts

; (LOAD-MEMORY addr) — read and evaluate every form starting at addr,
; stopping at the first NUL byte (or after 4 KB, whichever comes first).
; addr may be a fixnum or int32 value.  Returns the last form's value
; (or NIL if the buffer is empty).
ev_load_memory:
            lbsr    eval_one_arg
            lbsr    untag_to_i32
            ldd     i32_scratch+2       ; D = low 16 bits = address
            std     lm_addr
            ; Save current reader state.
            ldd     rdr_base
            pshs    d
            ldd     tib_pos
            pshs    d
            ldd     tib_len
            pshs    d
            ; Compute buffer length by scanning for NUL (max 4096).
            ldx     lm_addr
            ldd     #0
            std     lm_len
lm_scan:    lda     ,x+
            beq     lm_scan_done
            ldd     lm_len
            addd    #1
            std     lm_len
            cmpd    #4096
            blo     lm_scan
lm_scan_done:
            ; Redirect the reader at [lm_addr .. lm_addr + lm_len).
            ldd     lm_addr
            std     rdr_base
            ldd     lm_len
            std     tib_len
            ldd     #0
            std     tib_pos
            ldx     #NIL_VAL
            stx     lm_last
lm_loop:    lbsr    tib_skip_ws
            lbsr    tib_peek
            tsta
            beq     lm_done
            lbsr    read_expr
            lbsr    eval
            stx     lm_last
            bra     lm_loop
lm_done:    puls    d
            std     tib_len
            puls    d
            std     tib_pos
            puls    d
            std     rdr_base
            ldx     lm_last
            rts

lm_addr     fdb     0
lm_len      fdb     0
lm_last     fdb     0

; (DISPLAY x) — emit the value without the surrounding quotes a string/char
; would get from PRINT.  No trailing newline.  Returns the value.
ev_display: lbsr    eval_one_arg
            stx     ev_pn_val_t
            lbsr    display_expr
            ldx     ev_pn_val_t
            rts

; (PUTCHAR n) — emit the byte whose code is n (fixnum 0..255).  Accepts a
; char value too — it is converted to its underlying code.  Returns the arg.
ev_putchar: lbsr    eval_one_arg
            stx     ev_pn_val_t
            tfr     x,d
            cmpd    #CHAR_BASE
            blo     pch_fix
            cmpd    #CHAR_END
            bhs     pch_fix
            subd    #CHAR_BASE
            lsra
            rorb
            tfr     b,a
            lbsr    emit_a
            ldx     ev_pn_val_t
            rts
pch_fix:    tfr     x,d
            andb    #1
            beq     pch_err
            tfr     x,d
            subd    #1
            asra
            rorb
            tfr     b,a
            lbsr    emit_a
            ldx     ev_pn_val_t
            rts
pch_err:    ldx     #NIL_VAL
            rts

; ---------------------------------------------------------------------------
; Error handling — (error msg), (catch tag body...), (throw tag value).
; (error) and uncaught (throw) unwind to the REPL entry (repl_init_s).
; (catch ...) installs a handler frame in catch_stack; (throw ...) walks
; the stack for a matching tag and longjmps to that frame.
; ---------------------------------------------------------------------------

; (ERROR msg) — print "ERROR: <msg>" and unwind to REPL.
ev_error:   lbsr    eval_one_arg
            stx     ev_err_val
            ldx     #str_errprefix
            lbsr    puts_native
            ldx     ev_err_val
            lbsr    print_expr
            lbsr    emit_crlf
            ldx     #NIL_VAL
            stx     current_env
            stx     catch_sp        ; reset catch stack too (it was below this frame)
            ldx     #catch_stack
            stx     catch_sp
            lds     repl_init_s
            lbra    repl_loop

; (CATCH tag body...) — evaluate tag, then body in implicit PROGN.  If body
; completes normally, return its value.  If (throw tag value) fires during
; body evaluation with a matching tag, restore S to this catch's saved
; level and return the thrown value.
ev_catch:   ldx     ev_expr_scratch
            ldx     2,x
            pshs    x                   ; args
            ldx     ,x
            lbsr    eval                ; tag value
            stx     ev_cth_tag
            ldx     ,s
            ldx     2,x                 ; body list
            lbsr    wrap_progn
            stx     ev_cth_body
            leas    2,s
            ; Push catch frame onto catch_stack.
            ; Layout: [tag (2)][saved_s (2)][saved_env (2)]
            ldy     catch_sp
            ldd     ev_cth_tag
            std     ,y++
            sts     ,y++                ; saved S (right before lbsr eval below)
            ldd     current_env
            std     ,y++
            sty     catch_sp
            ; Eval body.  A matching throw will restore S to what we just
            ; stored, which is THIS point (pre-lbsr push).  The rts that
            ; eval (or throw) does will then pop the return address that
            ; lbsr eval placed on the stack — pointing at the instruction
            ; right after lbsr eval.  That's the pop/clean-up path.
            ldx     ev_cth_body
            lbsr    eval
            ; Normal completion: pop catch frame, return X.
            ldy     catch_sp
            leay    -6,y
            sty     catch_sp
            rts

; (THROW tag value) — find the innermost matching catch and unwind.
ev_throw:   ldx     ev_expr_scratch
            ldx     2,x
            pshs    x
            ldx     ,x
            lbsr    eval
            stx     ev_cth_tag
            ldx     ,s
            ldx     2,x
            ldx     ,x
            lbsr    eval
            stx     catch_thrown
            leas    2,s
            ; Walk catch_stack from top down.
            ldy     catch_sp
thr_loop:   cmpy    #catch_stack
            lbeq    thr_nomatch
            leay    -6,y
            ldd     ,y
            cmpd    ev_cth_tag
            beq     thr_match
            bra     thr_loop
thr_match:  ; Restore env, S.  Adjust catch_sp to drop this frame.
            sty     catch_sp
            ldd     4,y                 ; saved env
            std     current_env
            ldd     2,y                 ; saved S
            tfr     d,s
            ldx     catch_thrown
            rts                         ; pops the ret addr of ev_catch's lbsr eval
thr_nomatch: ; Uncaught throw — treat like error: print and unwind to REPL.
            ldx     #str_uncaught
            lbsr    puts_native
            ldx     ev_cth_tag
            lbsr    print_expr
            lbsr    emit_crlf
            ldx     #NIL_VAL
            stx     current_env
            ldx     #catch_stack
            stx     catch_sp
            lds     repl_init_s
            lbra    repl_loop

str_errprefix fcc   "ERROR: "
              fcb   0
str_uncaught  fcc   "UNCAUGHT THROW: "
              fcb   0

ev_err_val   fdb    0
ev_cth_tag   fdb    0
ev_cth_body  fdb    0

; Fixed 8-entry catch frame stack (6 bytes each = 48 bytes).
catch_stack  fdb    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0

; ---------------------------------------------------------------------------
; Character primitives.  Chars are tagged values in CHAR_BASE..CHAR_END
; (CHAR_BASE + code).  No box needed — the tag encodes the whole value.
; ---------------------------------------------------------------------------

; (CHAR->INTEGER c) — tagged char → fixnum code.
ev_char_int:
            lbsr    eval_one_arg
            cmpx    #CHAR_BASE
            lblo    ev_char_err
            cmpx    #CHAR_END
            lbhs    ev_char_err
            tfr     x,d
            subd    #CHAR_BASE          ; D = 2*code
            ; Tag as fixnum: (code << 1) | 1 = (2*code) | 1
            addd    #1
            tfr     d,x
            rts

; (INTEGER->CHAR n) — fixnum code → tagged char.  Error if code out of
; range (0..255).
ev_int_char:
            lbsr    eval_one_arg
            tfr     x,d
            andb    #1
            beq     ev_char_err         ; not a fixnum
            tfr     x,d
            subd    #1
            asra
            rorb                        ; D = code
            cmpd    #0
            blt     ev_char_err
            cmpd    #256
            bge     ev_char_err
            lslb
            rola                        ; D = 2*code
            addd    #CHAR_BASE
            tfr     d,x
            rts

; (CHAR? x) — T if x is a character value, else NIL.
ev_charp:   lbsr    eval_one_arg
            cmpx    #CHAR_BASE
            blo     ev_charp_no
            cmpx    #CHAR_END
            bhs     ev_charp_no
            ldx     #T_VAL
            rts
ev_charp_no:
            ldx     #NIL_VAL
            rts

ev_char_err:
            ldx     #str_char_err
            lbsr    puts_native
            lbsr    emit_crlf
            ldx     #NIL_VAL
            rts
str_char_err fcc    "CHAR: bad arg"
             fcb    0

; ---------------------------------------------------------------------------
; Vector primitives.  Vectors live in VEC_POOL..VEC_END.  Layout:
;   [length word (2 B)][elem 0 (2 B)][elem 1 (2 B)]...[elem N-1 (2 B)]
; Allocation is bump-only (no GC for vectors yet).  Elements hold any
; tagged value (2 bytes each).
; ---------------------------------------------------------------------------

; (MAKE-VECTOR n fill) — allocate vector of length n, filled with `fill`.
ev_make_vec:
            ldx     ev_expr_scratch
            ldx     2,x
            pshs    x                   ; args
            ldx     ,x
            lbsr    eval                ; n (fixnum)
            tfr     x,d
            andb    #1
            beq     ev_vec_err_pop      ; n must be fixnum
            tfr     x,d
            subd    #1
            asra
            rorb                        ; D = n
            cmpd    #0
            blt     ev_vec_err_pop
            std     mv_n
            ldx     ,s
            ldx     2,x
            ldx     ,x
            lbsr    eval                ; fill value
            stx     mv_fill
            leas    2,s
            ; Check pool space.
            ldd     mv_n
            aslb
            rola                        ; D = 2n
            addd    #2                  ; + length field
            addd    vec_next
            cmpd    #VEC_END
            bhi     ev_vec_oom
            ; Allocate.
            ldx     vec_next
            ldd     mv_n
            std     ,x                  ; length
            pshs    x                   ; save vector start
            leax    2,x                 ; point at elem 0
            ldd     mv_n
            cmpd    #0
            beq     mv_done
mv_fill_loop:
            ldy     mv_fill
            sty     ,x++
            subd    #1
            bne     mv_fill_loop
mv_done:    stx     vec_next
            puls    x                   ; X = vector pointer
            rts
ev_vec_err_pop:
            leas    2,s
            lbra    ev_vec_err
ev_vec_oom: ldx     #str_vec_oom
            lbsr    puts_native
            lbsr    emit_crlf
            ldx     #NIL_VAL
            rts

; (VECTOR-LENGTH v) — length as fixnum.
ev_vec_len: lbsr    eval_one_arg
            cmpx    #VEC_POOL
            lblo    ev_vec_err
            cmpx    #VEC_END
            lbhs    ev_vec_err
            ldd     ,x
            lslb
            rola
            addd    #1
            tfr     d,x
            rts

; (VECTOR-REF v i) — element at index i.
ev_vec_ref:
            ldx     ev_expr_scratch
            ldx     2,x
            pshs    x
            ldx     ,x
            lbsr    eval                ; vector in X
            stx     mv_vec
            cmpx    #VEC_POOL
            lblo    ev_vec_err_pop
            cmpx    #VEC_END
            lbhs    ev_vec_err_pop
            ldx     ,s
            ldx     2,x
            ldx     ,x
            lbsr    eval                ; index
            leas    2,s
            tfr     x,d
            andb    #1
            lbeq    ev_vec_err
            tfr     x,d
            subd    #1
            asra
            rorb                        ; D = index
            ldx     mv_vec
            cmpd    ,x                  ; must be < length
            lbhs    ev_vec_err
            aslb
            rola                        ; D = 2*i
            addd    #2                  ; + length field
            leax    d,x                 ; X = &elem
            ldx     ,x                  ; X = element value
            rts

; (VECTOR-SET! v i x) — update element at index; return x.
ev_vec_set:
            ldx     ev_expr_scratch
            ldx     2,x
            pshs    x
            ldx     ,x
            lbsr    eval
            stx     mv_vec
            ldx     ,s
            ldx     2,x
            stx     ,s                  ; overwrite args ptr with (i x) pair
            ldx     ,x
            lbsr    eval                ; index
            stx     mv_idx
            ldx     ,s
            ldx     2,x
            ldx     ,x
            lbsr    eval                ; new value
            stx     mv_val
            leas    2,s
            ; Validate vector.
            ldx     mv_vec
            cmpx    #VEC_POOL
            lblo    ev_vec_err
            cmpx    #VEC_END
            lbhs    ev_vec_err
            ; Validate index.
            ldd     mv_idx
            andb    #1
            lbeq    ev_vec_err
            ldd     mv_idx
            subd    #1
            asra
            rorb
            cmpd    ,x
            lbhs    ev_vec_err
            ; Compute element address and write.
            aslb
            rola
            addd    #2
            leax    d,x
            ldd     mv_val
            std     ,x
            ldx     mv_val
            rts

; (VECTOR->LIST v) — new list with the same elements.
ev_vec_list:
            lbsr    eval_one_arg
            cmpx    #VEC_POOL
            lblo    ev_vec_err
            cmpx    #VEC_END
            lbhs    ev_vec_err
            stx     mv_vec
            ldd     ,x
            std     mv_n
            ldx     #NIL_VAL
            stx     mv_head
            stx     mv_tail
            ldd     mv_n
            beq     vl_done
            ldx     mv_vec
            leax    2,x                 ; point at elem 0
            stx     mv_p
vl_loop:    ldx     mv_p
            ldy     ,x                  ; element
            ldd     #NIL_VAL
            lbsr    alloc_pair          ; (elem . NIL)
            ldy     mv_tail
            cmpy    #NIL_VAL
            bne     vl_append
            stx     mv_head
            stx     mv_tail
            bra     vl_next
vl_append:  stx     2,y
            stx     mv_tail
vl_next:    ldx     mv_p
            leax    2,x
            stx     mv_p
            ldd     mv_n
            subd    #1
            std     mv_n
            bne     vl_loop
vl_done:    ldx     mv_head
            rts

; (LIST->VECTOR lst) — allocate vector from a list of values.
ev_list_vec:
            lbsr    eval_one_arg
            stx     mv_head             ; remember list head
            stx     mv_p                ; walker
            ; First pass: count elements.
            ldd     #0
            std     mv_n
lv_count:   ldx     mv_p
            cmpx    #NIL_VAL
            beq     lv_count_done
            cmpx    #PAIR_POOL
            lblo    lv_count_done
            cmpx    #PAIR_END
            lbhs    lv_count_done
            ldd     mv_n
            addd    #1
            std     mv_n
            ldx     2,x
            stx     mv_p
            bra     lv_count
lv_count_done:
            ; Check capacity.
            ldd     mv_n
            aslb
            rola
            addd    #2
            addd    vec_next
            cmpd    #VEC_END
            lbhi    ev_vec_oom
            ; Allocate header.
            ldx     vec_next
            stx     mv_vec
            ldd     mv_n
            std     ,x
            ; Second pass: copy, starting from saved head.
            ldx     mv_head
            stx     mv_p
            ldx     mv_vec
            leax    2,x
lv_copy:    ldy     mv_p
            cmpy    #NIL_VAL
            beq     lv_copy_done
            cmpy    #PAIR_POOL
            lblo    lv_copy_done
            cmpy    #PAIR_END
            lbhs    lv_copy_done
            ldd     ,y                  ; car
            std     ,x++
            ldy     2,y                 ; cdr
            sty     mv_p
            bra     lv_copy
lv_copy_done:
            stx     vec_next
            ldx     mv_vec
            rts

; (VECTOR? x) — T if x is a vector, else NIL.
ev_vecp:    lbsr    eval_one_arg
            cmpx    #VEC_POOL
            blo     ev_vecp_no
            cmpx    #VEC_END
            bhs     ev_vecp_no
            ldx     #T_VAL
            rts
ev_vecp_no: ldx     #NIL_VAL
            rts

ev_vec_err: ldx     #str_vec_err
            lbsr    puts_native
            lbsr    emit_crlf
            ldx     #NIL_VAL
            rts

str_vec_err  fcc    "VECTOR: bad arg"
             fcb    0
str_vec_oom  fcc    "VECTOR: pool exhausted"
             fcb    0

mv_n         fdb    0
mv_fill      fdb    0
mv_vec       fdb    0
mv_idx       fdb    0
mv_val       fdb    0
mv_p         fdb    0
mv_head      fdb    0
mv_tail      fdb    0

; Write the two hex digits of A into [X], X += 2.  Uppercase.
hex_byte_to_x:
            pshs    a
            lsra
            lsra
            lsra
            lsra
            bsr     hex_nibble_to_x
            puls    a
            anda    #$0F
            bsr     hex_nibble_to_x
            rts
hex_nibble_to_x:
            cmpa    #10
            blt     hex_dig
            adda    #'A'-10
            bra     hex_emit
hex_dig:    adda    #'0'
hex_emit:   sta     ,x+
            rts

; ---------------------------------------------------------------------------
; Quasi-quotation — (QUASIQUOTE template) expands the template, with proper
; DEPTH tracking for nested `` ` ``.  Depth starts at 1; QUASIQUOTE
; increments it (keeping the marker literal); UNQUOTE / UNQUOTE-SPLICING
; decrement it but only actually EVALUATE their body when depth reaches 1.
; ---------------------------------------------------------------------------
ev_quasiquote:
            ldx     ev_expr_scratch
            ldx     2,x                 ; (template)
            ldx     ,x                  ; template
            ldd     #1
            std     qq_depth
            lbsr    qq_expand
            rts

; qq_expand: X = template.  Returns X = expanded value.  qq_depth must be
; unchanged on return (increments/decrements are paired here).
qq_expand:  cmpx    #PAIR_POOL
            blo     qq_atom_ret
            cmpx    #PAIR_END
            bhs     qq_atom_ret
            ldy     ,x
            cmpy    sym_UNQUOTE
            lbeq    qq_h_unquote
            cmpy    sym_UNQSPLICE
            lbeq    qq_h_unqsplice
            cmpy    sym_QUASIQUOTE
            lbeq    qq_h_quasi
            lbra    qq_walk
qq_atom_ret: rts

; Head is (UNQUOTE x).  At depth 1, evaluate x.  Otherwise rebuild
; (UNQUOTE qq_expand(x)) with depth-1 during recursion.
qq_h_unquote:
            ldd     qq_depth
            cmpd    #1
            lbeq    qq_do_eval
            ; Depth > 1: keep marker literal.
            subd    #1
            std     qq_depth
            ldx     2,x
            ldx     ,x
            lbsr    qq_expand
            ldd     qq_depth
            addd    #1
            std     qq_depth
            ; Build (UNQUOTE . (expanded . NIL))
            tfr     x,y
            ldd     #NIL_VAL
            lbsr    alloc_pair
            tfr     x,d
            ldy     sym_UNQUOTE
            lbsr    alloc_pair
            rts

; Head is (UNQUOTE-SPLICING x) at top-level of a quasi template (not inside
; a list — list walkers handle the splice-at-depth-1 case directly).  At
; depth 1 behaves like UNQUOTE; at depth > 1 keeps marker.
qq_h_unqsplice:
            ldd     qq_depth
            cmpd    #1
            lbeq    qq_do_eval
            subd    #1
            std     qq_depth
            ldx     2,x
            ldx     ,x
            lbsr    qq_expand
            ldd     qq_depth
            addd    #1
            std     qq_depth
            tfr     x,y
            ldd     #NIL_VAL
            lbsr    alloc_pair
            tfr     x,d
            ldy     sym_UNQSPLICE
            lbsr    alloc_pair
            rts

; Head is (QUASIQUOTE x).  Depth++, recurse, rebuild with QUASIQUOTE marker.
qq_h_quasi:
            ldd     qq_depth
            addd    #1
            std     qq_depth
            ldx     2,x
            ldx     ,x
            lbsr    qq_expand
            ldd     qq_depth
            subd    #1
            std     qq_depth
            tfr     x,y
            ldd     #NIL_VAL
            lbsr    alloc_pair
            tfr     x,d
            ldy     sym_QUASIQUOTE
            lbsr    alloc_pair
            rts

qq_do_eval: ldx     2,x
            ldx     ,x
            lbra    eval

; qq_walk: build new list element-by-element.  Handles UNQUOTE (one elem) /
; UNQUOTE-SPLICING (splice list) / normal (recurse qq_expand) per element.
qq_walk:    stx     qq_wp
            ldx     #NIL_VAL
            stx     qq_head
            stx     qq_tail
qw_loop:    ldx     qq_wp
            cmpx    #NIL_VAL
            lbeq    qw_done
            cmpx    #PAIR_POOL
            lblo    qw_dot
            cmpx    #PAIR_END
            lbhs    qw_dot
            ldx     ,x                  ; head element
            stx     qq_cur
            ; At depth == 1, an element whose car is UNQUOTE-SPLICING needs
            ; the splicing behaviour (insert multiple elements).  All other
            ; cases — UNQUOTE, nested QUASIQUOTE, plain list, atom — go via
            ; qq_expand which knows how to treat markers per depth.
            cmpx    #PAIR_POOL
            lblo    qw_normal
            cmpx    #PAIR_END
            lbhs    qw_normal
            ldy     ,x                  ; car(head)
            cmpy    sym_UNQSPLICE
            bne     qw_normal
            ldd     qq_depth
            cmpd    #1
            bne     qw_normal
            lbra    qw_splice
qw_normal:  ldd     qq_wp
            pshs    d
            ldd     qq_head
            pshs    d
            ldd     qq_tail
            pshs    d
            ldx     qq_cur
            lbsr    qq_expand
            stx     qq_cur
            puls    d
            std     qq_tail
            puls    d
            std     qq_head
            puls    d
            std     qq_wp
            ldx     qq_cur
            lbsr    qq_append_one
            lbra    qw_advance
qw_splice:  ldx     qq_cur
            ldx     2,x
            ldx     ,x
            ldd     qq_wp
            pshs    d
            ldd     qq_head
            pshs    d
            ldd     qq_tail
            pshs    d
            lbsr    eval                ; X = list to splice
            stx     qq_cur
            puls    d
            std     qq_tail
            puls    d
            std     qq_head
            puls    d
            std     qq_wp
            ldx     qq_cur
            stx     qq_sp
qs_loop:    ldx     qq_sp
            cmpx    #NIL_VAL
            beq     qw_advance
            cmpx    #PAIR_POOL
            lblo    qw_advance
            cmpx    #PAIR_END
            lbhs    qw_advance
            ldy     ,x
            ldd     #NIL_VAL
            lbsr    alloc_pair
            lbsr    qq_link_cell
            ldx     qq_sp
            ldx     2,x
            stx     qq_sp
            bra     qs_loop
qw_advance: ldx     qq_wp
            ldx     2,x
            stx     qq_wp
            lbra    qw_loop
qw_dot:     ldd     qq_wp
            pshs    d
            ldd     qq_head
            pshs    d
            ldd     qq_tail
            pshs    d
            ldx     qq_wp
            lbsr    qq_expand
            stx     qq_cur
            puls    d
            std     qq_tail
            puls    d
            std     qq_head
            puls    d
            std     qq_wp
            ldy     qq_tail
            cmpy    #NIL_VAL
            bne     qw_dot_attach
            ldx     qq_cur
            rts
qw_dot_attach:
            ldx     qq_cur
            stx     2,y
            ldx     qq_head
            rts
qw_done:    ldx     qq_head
            rts

; Append X (single value) to qq_head/qq_tail chain as a new cell.
qq_append_one:
            tfr     x,y
            ldd     #NIL_VAL
            lbsr    alloc_pair
; Link pre-allocated cell X to qq_tail chain.
qq_link_cell:
            ldy     qq_tail
            cmpy    #NIL_VAL
            bne     qlc_app
            stx     qq_head
            stx     qq_tail
            rts
qlc_app:    stx     2,y
            stx     qq_tail
            rts

qq_wp        fdb    0
qq_cur       fdb    0
qq_head      fdb    0
qq_tail      fdb    0
qq_sp        fdb    0
qq_depth     fdb    0

; (SETQ var expr) — mutate existing binding for var.  Searches current_env
; first, then global_env.  Errors on unbound.
ev_setq:    ldx     ev_expr_scratch
            ldx     2,x                 ; (var expr)
            stx     ev_sq_args
            ldx     ,x                  ; var symbol (unevaluated)
            stx     ev_sq_sym
            ldx     ev_sq_args
            ldx     2,x                 ; (expr)
            ldx     ,x                  ; expr form
            lbsr    eval
            stx     ev_sq_val
            ldy     current_env
ev_sq_l1:   cmpy    #NIL_VAL
            beq     ev_sq_try_g
            ldx     ,y                  ; binding pair
            ldd     ,x                  ; var of binding
            cmpd    ev_sq_sym
            beq     ev_sq_found
            ldy     2,y
            bra     ev_sq_l1
ev_sq_try_g: ldy    global_env
ev_sq_l2:   cmpy    #NIL_VAL
            lbeq    ev_sq_unbound
            ldx     ,y
            ldd     ,x
            cmpd    ev_sq_sym
            beq     ev_sq_found
            ldy     2,y
            bra     ev_sq_l2
ev_sq_found:
            ldd     ev_sq_val
            std     2,x                 ; mutate binding's cdr
            ldx     ev_sq_val
            rts
ev_sq_unbound:
            ldx     #str_unbound
            lbsr    puts_native
            ldx     ev_sq_sym
            lbsr    print_expr
            lbsr    emit_crlf
            ldx     #NIL_VAL
            rts

; ---------------------------------------------------------------------------
; Phase 7 — PROGN / AND / OR sequencing forms.
; ---------------------------------------------------------------------------

; (PROGN e1 e2 ... eN) — evaluate in order, return the last value.  Empty
; body evaluates to NIL.  Uses S stack for loop state so nested PROGNs
; (implicit from multi-body defun/let) stay reentrant.
ev_progn:   ldx     ev_expr_scratch
            ldx     2,x                 ; rest
            pshs    x                   ; [S+0] = rest
            ldx     #NIL_VAL
            pshs    x                   ; [S+0] = last, [S+2] = rest
pn_loop:    ldx     2,s                 ; rest
            cmpx    #NIL_VAL
            beq     pn_done
            ldx     ,x                  ; current form
            lbsr    eval
            stx     ,s                  ; last = X
            ldx     2,s
            ldx     2,x                 ; advance rest
            stx     2,s
            bra     pn_loop
pn_done:    puls    x                   ; X = last
            leas    2,s                 ; drop rest
            rts

; (AND e1 e2 ... eN) — short-circuit NIL, else last value.  Empty = T.
ev_and:     ldx     ev_expr_scratch
            ldx     2,x
            pshs    x                   ; rest
            ldx     #T_VAL
            pshs    x                   ; last (initial T)
an_loop:    ldx     2,s                 ; rest
            cmpx    #NIL_VAL
            beq     an_done
            ldx     ,x
            lbsr    eval
            cmpx    #NIL_VAL
            beq     an_nil
            stx     ,s                  ; last = X
            ldx     2,s
            ldx     2,x
            stx     2,s
            bra     an_loop
an_nil:     leas    4,s
            ldx     #NIL_VAL
            rts
an_done:    puls    x
            leas    2,s
            rts

; (OR e1 e2 ... eN) — short-circuit non-NIL.  Empty = NIL.
ev_or:      ldx     ev_expr_scratch
            ldx     2,x
            pshs    x                   ; rest
or_loop:    ldx     ,s                  ; rest
            cmpx    #NIL_VAL
            beq     or_done
            ldx     ,x
            lbsr    eval
            cmpx    #NIL_VAL
            bne     or_return
            ldx     ,s
            ldx     2,x
            stx     ,s
            bra     or_loop
or_return:  leas    2,s
            rts
or_done:    leas    2,s
            ldx     #NIL_VAL
            rts

; ---------------------------------------------------------------------------
; Phase 7 — 16-bit signed multiplication primitive (*).
;
; Builtin value = BI_MUL.  Entered via ev_apply's dispatch just like the
; other primitives.  Uses the existing ev_pr_args / ev_pr_tmp scratch.
; Algorithm: normalise both operands to non-negative, shift-and-add loop,
; negate result if exactly one operand was negative.
; ---------------------------------------------------------------------------
ev_mul:     ldx     ev_expr_scratch
            ldx     2,x
            pshs    x
            ldx     ,x
            lbsr    eval
            pshs    x                   ; [S+0]=A_val, [S+2]=args
            ldx     2,s
            ldx     2,x
            ldx     ,x
            lbsr    eval                ; B_val in X
            ; Convert both to i32 and multiply.  (Fast path not taken here
            ; because detecting fixnum×fixnum overflow in the middle of
            ; shift-add is tricky; always using 32-bit is simpler and
            ; correct for all cases.  Fixnum×fixnum is a small constant
            ; overhead but still fast relative to the REPL.)
            lbsr    untag_to_i32
            ldd     i32_scratch
            std     i32_b
            ldd     i32_scratch+2
            std     i32_b+2
            puls    x
            lbsr    untag_to_i32
            ldd     i32_scratch
            std     i32_a
            ldd     i32_scratch+2
            std     i32_a+2
            leas    2,s                 ; drop args
            lbsr    i32_mul
            lbsr    fit_and_box
            rts

; ---------------------------------------------------------------------------
; Phase 7 — stdlib bootstrap.
;
; load_stdlib iterates a ROM-resident table of pointers to null-terminated
; Lisp source strings; for each string it copies into TIB, resets the parser
; state, reads one expression, and evaluates it (discarding the result).
; This is how PROGN/AND/OR/* primitives enable user-mode definitions of
; the Tier-1/Tier-2 standard library functions without bloating the ASM.
; ---------------------------------------------------------------------------
load_stdlib:
            ldx     #stdlib_table
ld_loop:    ldy     ,x++                ; Y = next source ptr
            cmpy    #0
            beq     ld_done
            ; Copy Y's null-terminated bytes into TIB.  Guard against
            ; overflow: stop at TIB_SIZE bytes (a too-long source would
            ; silently corrupt the vector pool just past TIB_ADDR).
            pshs    x                   ; save table cursor
            ldx     #TIB_ADDR
            ldu     #TIB_ADDR+TIB_SIZE  ; end sentinel (guard against overrun)
ld_copy:    lda     ,y+
            beq     ld_copy_done
            cmpx    #TIB_ADDR+TIB_SIZE
            bhs     ld_copy_done        ; TIB full — truncate
            sta     ,x+
            bra     ld_copy
ld_copy_done:
            tfr     x,d
            subd    #TIB_ADDR
            std     tib_len
            ldd     #0
            std     tib_pos
            lbsr    read_expr
            lbsr    eval
            puls    x
            bra     ld_loop
ld_done:    rts

; Pointer table — null-terminated list of source strings to evaluate.  Order
; matters when definitions reference earlier ones (e.g. reverse uses append).
stdlib_table
            fdb     sl_not
            fdb     sl_zerop
            fdb     sl_1plus
            fdb     sl_1minus
            fdb     sl_gt
            fdb     sl_abs
            fdb     sl_max
            fdb     sl_min
            fdb     sl_revacc
            fdb     sl_reverse
            fdb     sl_nth
            fdb     sl_last
            fdb     sl_member
            fdb     sl_mapcar
            fdb     sl_filter
            fdb     sl_reduce
            fdb     sl_any
            fdb     sl_all
            fdb     sl_equal
            fdb     sl_when
            fdb     sl_unless
            fdb     sl_swap
            fdb     sl_wgensyms
            fdb     sl_while
            fdb     sl_dolist
            fdb     sl_funcall
            fdb     sl_le
            fdb     sl_ge
            fdb     sl_case_expand
            fdb     sl_case
            fdb     sl_format_step
            fdb     sl_format
            fdb     sl_trace_wrap
            fdb     sl_trace
            fdb     sl_untrace
            fdb     sl_symcat
            fdb     sl_ds_acc
            fdb     sl_ds_set
            fdb     sl_defstruct
            fdb     sl_q_from
            fdb     sl_q_to
            fdb     sl_q_mul
            fdb     sl_q_div
            fdb     sl_make_ht
            fdb     sl_ht_hash
            fdb     sl_ht_get
            fdb     sl_ht_put
            ; Scheme-style ?-suffix aliases for the CL-style bare predicates.
            ; Hha Lisp's surface is Common Lisp (defun/setq/t/nil) but Scheme
            ; users coming from SICP / Racket / Clojure expect (null? x) and
            ; (eq? a b).  First-class primitives let us alias for free via
            ; defvar.  Original names (NULL/ATOM/EQ/ZEROP) remain bound for
            ; CL-style callers.
            fdb     sl_null_q
            fdb     sl_atom_q
            fdb     sl_eq_q
            fdb     sl_zero_q
            fdb     0

sl_not      fcc     "(defun not (x) (if x nil t))"
            fcb     0
sl_zerop    fcc     "(defun zerop (n) (eq n 0))"
            fcb     0
sl_1plus    fcc     "(defun inc (n) (+ n 1))"
            fcb     0
sl_1minus   fcc     "(defun dec (n) (- n 1))"
            fcb     0
sl_gt       fcc     "(defun > (a b) (< b a))"
            fcb     0
sl_abs      fcc     "(defun abs (n) (if (< n 0) (- 0 n) n))"
            fcb     0
sl_max      fcc     "(defun max (a b) (if (< a b) b a))"
            fcb     0
sl_min      fcc     "(defun min (a b) (if (< a b) a b))"
            fcb     0
; O(n) reverse via accumulator — avoids the O(n^2) append cascade.
sl_revacc   fcc     "(defun rev-acc (xs acc) (if (null xs) acc (rev-acc (cdr xs) (cons (car xs) acc))))"
            fcb     0
sl_reverse  fcc     "(defun reverse (xs) (rev-acc xs nil))"
            fcb     0
sl_nth      fcc     "(defun nth (n xs) (if (< n 1) (car xs) (nth (- n 1) (cdr xs))))"
            fcb     0
sl_last     fcc     "(defun last (xs) (if (null (cdr xs)) (car xs) (last (cdr xs))))"
            fcb     0
sl_member   fcc     "(defun member (x xs) (cond ((null xs) nil) ((eq (car xs) x) xs) (t (member x (cdr xs)))))"
            fcb     0
sl_mapcar   fcc     "(defun mapcar (f xs) (if (null xs) nil (cons (f (car xs)) (mapcar f (cdr xs)))))"
            fcb     0
sl_filter   fcc     "(defun filter (p xs) (cond ((null xs) nil) ((p (car xs)) (cons (car xs) (filter p (cdr xs)))) (t (filter p (cdr xs)))))"
            fcb     0
sl_reduce   fcc     "(defun reduce (f init xs) (if (null xs) init (f (car xs) (reduce f init (cdr xs)))))"
            fcb     0
sl_any      fcc     "(defun any (p xs) (cond ((null xs) nil) ((p (car xs)) t) (t (any p (cdr xs)))))"
            fcb     0
sl_all      fcc     "(defun all (p xs) (cond ((null xs) t) ((p (car xs)) (all p (cdr xs))) (t nil)))"
            fcb     0
sl_equal    fcc     "(defun equal (a b) (cond ((eq a b) t) ((atom a) nil) ((atom b) nil) (t (if (equal (car a) (car b)) (equal (cdr a) (cdr b)) nil))))"
            fcb     0

; --- hygienic macros -------------------------------------------------------
sl_when     fcc     "(defmacro when (test . body) `(if ,test (progn ,@body) nil))"
            fcb     0
sl_unless   fcc     "(defmacro unless (test . body) `(if ,test nil (progn ,@body)))"
            fcb     0
; Swap values of two variables using a gensym-guarded temporary.  Safe even
; when either variable happens to be named `tmp`.
sl_swap     fcc     "(defmacro swap (a b) (let ((tmp (gensym))) `(let ((,tmp ,a)) (setq ,a ,b) (setq ,b ,tmp))))"
            fcb     0
; with-gensyms: bind each name in NAMES to a fresh gensym while expanding
; body.  Recursive macro — expansion invokes with-gensyms on cdr of names.
sl_wgensyms fcc     "(defmacro with-gensyms (names . body) (if (null names) `(progn ,@body) `(let ((,(car names) (gensym))) (with-gensyms ,(cdr names) ,@body))))"
            fcb     0
; while: loop while test is non-NIL, executing body each iteration.  Uses a
; gensym'd `loop` name so user variables named `loop` don't get captured.
sl_while    fcc     "(defmacro while (test . body) (let ((loop (gensym))) `(letrec ((,loop (lambda () (if ,test (progn ,@body (,loop)) nil)))) (,loop))))"
            fcb     0
; dolist: iterate var over list, running body each iteration.
;   (dolist (v '(1 2 3)) (print v))
sl_dolist   fcc     "(defmacro dolist (spec . body) (let ((xs (gensym))) `(let ((,xs ,(cadr spec))) (while (not (null ,xs)) (let ((,(car spec) (car ,xs))) ,@body) (setq ,xs (cdr ,xs))))))"
            fcb     0
; funcall: call f with explicit arguments.  Sugar over APPLY.
sl_funcall  fcc     "(defmacro funcall (f . args) `(apply ,f (list ,@args)))"
            fcb     0
sl_le       fcc     "(defun <= (a b) (not (< b a)))"
            fcb     0
sl_ge       fcc     "(defun >= (a b) (not (< a b)))"
            fcb     0
; case: classic multi-branch on key value.  Each clause is (keys body...),
; where keys is either a list of match values or the symbol t for default.
sl_case_expand
            fcc     "(defun case-expand (k clauses) (if (null clauses) nil (let ((c (car clauses))) (if (eq (car c) t) (cons (quote progn) (cdr c)) `(if (member ,k ',(car c)) (progn ,@(cdr c)) ,(case-expand k (cdr clauses)))))))"
            fcb     0
sl_case     fcc     "(defmacro case (key . clauses) (let ((k (gensym))) `(let ((,k ,key)) ,(case-expand k clauses))))"
            fcb     0
; format: minimal formatted output.  On `~` the next char is skipped as a
; directive and one argument is DISPLAY'ed.  Any char literally emits.  No
; ~% / ~~ shortcuts — use (newline) / (putchar 126) instead.  ASCII 126='~'.
sl_format_step
            fcc     "(defun format-step (fmt args i n) (if (< i n) (if (eq (string-ref fmt i) 126) (progn (display (car args)) (format-step fmt (cdr args) (+ i 2) n)) (progn (putchar (string-ref fmt i)) (format-step fmt args (+ i 1) n)))))"
            fcb     0
sl_format   fcc     "(defun format (fmt . args) (format-step fmt args 0 (string-length fmt)))"
            fcb     0
; trace: wrap a function so that each call logs entry and exit on the console.
; Stores the original value under SYM-ORIG via (defvar sym-orig sym).  The
; wrapper forwards the real call through (eval backup), and untrace restores
; the binding by mutating the live value back to backup.
sl_trace_wrap
            fcc     "(defun trace-wrap (sym args backup) (format "
            fcb     $22
            fcc     "ENTER ~A ~A"
            fcb     $22
            fcc     " sym args) (newline) (let ((r (apply (eval backup) args))) (format "
            fcb     $22
            fcc     "EXIT  ~A -> ~A"
            fcb     $22
            fcc     " sym r) (newline) r))"
            fcb     0
sl_trace    fcc     "(defun trace (sym) (let ((backup (string->symbol (string-append (symbol->string sym) "
            fcb     $22
            fcc     "-ORIG"
            fcb     $22
            fcc     ")))) (eval `(defvar ,backup ,sym)) (eval `(defun ,sym args (trace-wrap ',sym args ',backup)))))"
            fcb     0
sl_untrace  fcc     "(defun untrace (sym) (let ((backup (string->symbol (string-append (symbol->string sym) "
            fcb     $22
            fcc     "-ORIG"
            fcb     $22
            fcc     ")))) (eval `(setq ,sym ,backup))))"
            fcb     0
; defstruct: vector-backed records.  (defstruct point x y) generates
; make-point / point? / point-x / point-y / set-point-x! / set-point-y!.
; The vector's first slot holds the struct name symbol as a tag.
sl_symcat   fcc     "(defun symcat (a b) (string->symbol (string-append (symbol->string a) (symbol->string b))))"
            fcb     0
sl_ds_acc   fcc     "(defun ds-acc (prefix fields i) (if (null fields) nil (cons `(defun ,(symcat prefix (car fields)) (o) (vector-ref o ,i)) (ds-acc prefix (cdr fields) (+ i 1)))))"
            fcb     0
sl_ds_set   fcc     "(defun ds-set (prefix fields i) (if (null fields) nil (cons `(defun ,(symcat 'set- (symcat prefix (car fields))) (o v) (vector-set! o ,i v)) (ds-set prefix (cdr fields) (+ i 1)))))"
            fcb     0
sl_defstruct
            fcc     "(defmacro defstruct (name . fields) `(progn (defun ,(symcat 'make- name) ,fields (list->vector (cons ',name (list ,@fields)))) (defun ,(symcat name '?) (o) (and (vector? o) (eq (vector-ref o 0) ',name))) ,@(ds-acc (symcat name '-) fields 1) ,@(ds-set (symcat name '-) fields 1)))"
            fcb     0
; Hashtable stdlib was prototyped here but pulled — it pushes the code image
; past the memory budget and drops pair-pool cells below the level several
; regression tests depend on.  A native primitive implementation (backed by
; a dedicated pool rather than stdlib-on-vector) is recommended instead.

; Q8.8 fixed-point math.  Value is int32 interpreted as raw/256.
; + and - work directly on raw values; * and / scale.  Range: -128..127.996
; in steps of 1/256.  (* a b) auto-promotes to int32 so the 16-bit product
; doesn't overflow prematurely.
sl_q_from   fcc     "(defun q-from (n) (ash n 8))"
            fcb     0
sl_q_to     fcc     "(defun q-to (q) (ash q -8))"
            fcb     0
sl_q_mul    fcc     "(defun q* (a b) (ash (* a b) -8))"
            fcb     0
sl_q_div    fcc     "(defun q/ (a b) (/ (ash a 8) b))"
            fcb     0
; Hashtable: vector with 'ht tag + 8 alist buckets.  Hash = first char of
; (symbol->string key) mod 8 (symbols only; non-symbol keys → bucket 0).
; ht-put prepends (key . val); the bucket's alist acts as a shadow list.
sl_make_ht  fcc     "(defun make-ht () (let ((v (make-vector 9 nil))) (vector-set! v 0 'ht) v))"
            fcb     0
sl_ht_hash  fcc     "(defun ht-hash (k) (let ((s (symbol->string k))) (if (null s) 0 (if (zerop (string-length s)) 0 (mod (string-ref s 0) 8)))))"
            fcb     0
sl_ht_get   fcc     "(defun ht-get (h k) (let ((e (assoc k (vector-ref h (+ 1 (ht-hash k)))))) (if e (cdr e) nil)))"
            fcb     0
sl_ht_put   fcc     "(defun ht-put (h k v) (let ((i (+ 1 (ht-hash k)))) (vector-set! h i (cons (cons k v) (vector-ref h i))) v))"
            fcb     0
; Scheme-style ?-suffix predicate aliases.  These rebind the existing
; built-in / stdlib predicates under their Scheme-conventional names so
; users coming from SICP / Racket / Clojure can write (null? xs) and
; (eq? a b) naturally.  defvar is enough — first-class primitives mean
; the right-hand side evaluates to the same callable value.
sl_null_q   fcc     "(defvar null? null)"
            fcb     0
sl_atom_q   fcc     "(defvar atom? atom)"
            fcb     0
sl_eq_q     fcc     "(defvar eq? eq)"
            fcb     0
sl_zero_q   fcc     "(defvar zero? zerop)"
            fcb     0

; ev_cn_args / ev_cn_clause removed: ev_cond now uses the S stack for
; loop state to stay reentrant across nested cond evaluation (issue #14).
ev_pn_rest   fdb    0
ev_pn_last   fdb    0
ev_lt_args   fdb    0
ev_lt_bindings fdb  0
ev_lt_body   fdb    0
ev_lt_newenv fdb    0
ev_lt_cur    fdb    0
ev_lt_val    fdb    0
ev_lt_result fdb    0
ev_sq_args   fdb    0
ev_sq_sym    fdb    0
ev_sq_val    fdb    0

ev_lm_args   fdb    0
ev_lm_params fdb    0
ev_lm_body   fdb    0
ev_lm_tag    fdb    0
ev_df_sym    fdb    0
ev_ap_expr   fdb    0
ev_ap_fn     fdb    0
ev_ap_params fdb    0
ev_ap_body   fdb    0
ev_ap_env    fdb    0
ev_ap_args   fdb    0
ev_ap_pp     fdb    0
ev_ap_ap     fdb    0
ev_ap_val    fdb    0

str_apply_err fcc   "APPLY: not a function"
              fcb   0
str_arity_err fcc   "APPLY: arity mismatch"
              fcb   0

str_car_err fcc     "CAR: not a pair"
            fcb     0
str_cdr_err fcc     "CDR: not a pair"
            fcb     0
str_cadr_err fcc    "CxxR: not a pair"
             fcb    0

str_eval_err fcc    "EVAL?: "
             fcb    0
str_unk_op   fcc    "UNKNOWN-OP: "
             fcb    0
str_unbound  fcc    "UNBOUND: "
             fcb    0
str_oom      fcc    "ALLOC: pool exhausted"
             fcb    0

; ---------------------------------------------------------------------------
; Phase 6 — mark-sweep garbage collector.
;
; Roots: global_env + current_env.  Because `(gc)` is only invoked from the
; REPL (manually, between expressions), all other scratch variables are
; already stale and need not be traced.
; ---------------------------------------------------------------------------

; gc_mark — recursively mark pair X (and its transitive contents).
; If X is not a pair pointer, returns immediately.
gc_mark:
            cmpx    #PAIR_POOL
            lblo    gm_ret
            cmpx    #PAIR_END
            lbhs    gm_ret
            pshs    x               ; save input pair ptr
            tfr     x,d
            subd    #PAIR_POOL
            lsra
            rorb
            lsra
            rorb                    ; D = pair_idx (0..3071)
            addd    #MARK_TABLE
            tfr     d,x             ; X = &mark[pair_idx]
            tst     ,x
            bne     gm_already
            ldb     #1
            stb     ,x              ; mark
            ldx     ,s              ; recover input
            ldx     ,x              ; car
            lbsr    gc_mark
            ldx     ,s
            ldx     2,x             ; cdr
            lbsr    gc_mark
            puls    x
            rts
gm_already: puls    x
gm_ret:     rts

; gc_sweep — walk pair pool [PAIR_POOL, pair_next), push unmarked cells onto
; the free list, clear marks on live ones.  gc_freed = count swept.
gc_sweep:   ldd     #0
            std     gc_freed
            ldx     #NIL_VAL
            stx     free_list
            ldy     #PAIR_POOL
sw_loop:    cmpy    pair_next
            lbhs    sw_done
            tfr     y,d
            subd    #PAIR_POOL
            lsra
            rorb
            lsra
            rorb                    ; D = pair_idx
            addd    #MARK_TABLE
            tfr     d,x
            lda     ,x
            beq     sw_free
            clr     ,x              ; clear mark for next cycle
            bra     sw_next
sw_free:    ldd     free_list
            std     ,y              ; chain through car
            sty     free_list
            ldd     gc_freed
            addd    #1
            std     gc_freed
sw_next:    leay    4,y
            bra     sw_loop
sw_done:    rts

; gc_run — clear mark table, trace roots, sweep.
gc_run:     ldx     #MARK_TABLE
            ldd     #0
gr_clr:     std     ,x++
            cmpx    #MARK_TABLE_END
            blo     gr_clr
            ldx     global_env
            lbsr    gc_mark
            ldx     current_env
            lbsr    gc_mark
            lbsr    gc_mark_vec_pool
            lbsr    gc_sweep
            rts

; gc_mark_vec_pool — scan every 16-bit word in the live vector pool
; [VEC_POOL, vec_next) and mark any value that lands in the pair range.
; Vectors store their elements inline (length field + elems), and those
; elements may be pair pointers that are only reachable through the
; vector.  Without this pass, mutator code like (vector-set! v 0 (cons 1 2))
; would see the pair swept on the next GC.  Length fields and non-pair
; elements (fixnum, symbol, etc.) are ignored because they fall outside the
; pair range.
gc_mark_vec_pool:
            ldx     #VEC_POOL
gmvp_loop:  cmpx    vec_next
            bhs     gmvp_done
            ldy     ,x              ; Y = 16-bit word
            cmpy    #PAIR_POOL
            blo     gmvp_next
            cmpy    #PAIR_END
            bhs     gmvp_next
            pshs    x
            tfr     y,x
            lbsr    gc_mark
            puls    x
gmvp_next:  leax    2,x
            bra     gmvp_loop
gmvp_done:  rts

; gc_run_safe — like gc_run but also roots every ev_* scratch pair pointer
; and conservatively scans the hardware stack for values that land in the
; pair-pool range.  Used by alloc_pair when the pool is otherwise
; exhausted — running GC here can reclaim garbage from in-flight macro
; expansions without losing live pairs that only exist on the S stack.
gc_run_safe:
            ldx     #MARK_TABLE
            ldd     #0
grs_clr:    std     ,x++
            cmpx    #MARK_TABLE_END
            blo     grs_clr
            ldx     global_env
            lbsr    gc_mark
            ldx     current_env
            lbsr    gc_mark
            ldx     current_closure
            lbsr    gc_mark
            ldx     ev_expr_scratch
            lbsr    gc_mark
            ldx     ev_ap_body
            lbsr    gc_mark
            ldx     ev_ap_env
            lbsr    gc_mark
            ldx     ev_ap_params
            lbsr    gc_mark
            ldx     ev_ap_args
            lbsr    gc_mark
            ldx     ev_ap_pp
            lbsr    gc_mark
            ldx     ev_ap_ap
            lbsr    gc_mark
            ldx     ev_ap_val
            lbsr    gc_mark
            ldx     ev_ap_rhead
            lbsr    gc_mark
            ldx     ev_ap_rtail
            lbsr    gc_mark
            ldx     ev_ap_fn
            lbsr    gc_mark
            lbsr    gc_mark_vec_pool
            ; Conservative stack scan: walk every 16-bit word between the
            ; current S and repl_init_s.  Any value that lands in the pair
            ; range is treated as a live reference.  This catches car/cdr
            ; that alloc_pair pushed onto the stack on entry so that the
            ; caller's intended fields survive the GC pass, plus any pair
            ; pointer that happens to be saved via `pshs d` across a
            ; recursive eval.
            ldd     repl_init_s
            beq     grs_sweep           ; REPL not yet initialised — skip
            tfr     s,d
            addd    #2                  ; skip our return address
            tfr     d,x
grs_scan:   cmpx    repl_init_s
            bhs     grs_sweep
            ldy     ,x++
            cmpy    #PAIR_POOL
            blo     grs_scan
            cmpy    #PAIR_END
            bhs     grs_scan
            pshs    x
            tfr     y,x
            lbsr    gc_mark
            puls    x
            bra     grs_scan
grs_sweep:  lbsr    gc_sweep
            rts

; (GC) builtin — run GC, return freed count as a fixnum.
ev_gc:      lbsr    gc_run
            ldd     gc_freed
            lslb
            rola                    ; D <<= 1
            addd    #1              ; | 1 (fixnum tag)
            tfr     d,x
            rts

; ---------------------------------------------------------------------------
; REPL — prompt, ACCEPT, read all forms in the line and echo each.
; ---------------------------------------------------------------------------
repl:
            sts     repl_init_s             ; anchor for (error ...) longjmp
repl_loop:  ldd     #TIB_ADDR               ; ensure reader base is reset after
            std     rdr_base                ; any (read-string) error unwind.
            ; Auto-GC at REPL top: all ev_* scratch is stale, current_env is
            ; empty, and the hardware stack holds only the REPL's own frame,
            ; so gc_run sees global_env + current_env as the complete root
            ; set.  This reclaims macro-expansion garbage from the previous
            ; line before the next one gets a chance to exhaust the pool.
            lbsr    gc_run
            lda     #'>'
            lbsr    emit_a
            lda     #' '
            lbsr    emit_a
            lbsr    accept
repl_check_bal:
            lbsr    scan_paren_balance
            cmpd    #0
            ble     repl_forms
            lda     #'>'
            lbsr    emit_a
            lda     #'>'
            lbsr    emit_a
            lda     #' '
            lbsr    emit_a
            lbsr    accept_append
            bra     repl_check_bal
repl_forms: lbsr    tib_skip_ws
            lbsr    tib_peek
            tsta
            beq     repl_line_done
            lbsr    read_expr
            lbsr    eval
            lbsr    print_expr
            lbsr    emit_crlf
            bra     repl_forms
repl_line_done:
            bra     repl_loop

; ---------------------------------------------------------------------------
; RAM variables
; ---------------------------------------------------------------------------
tib_len     fdb     0
tib_pos     fdb     0
pair_next   fdb     0
sym_next    fdb     0
sym_list    fdb     0
sym_QUOTE   fdb     0
sym_IF      fdb     0
sym_DEFVAR  fdb     0
sym_CONS    fdb     0
sym_CAR     fdb     0
sym_CDR     fdb     0
sym_ATOM    fdb     0
sym_EQ      fdb     0
sym_NULL    fdb     0
sym_PLUS    fdb     0
sym_MINUS   fdb     0
sym_LT      fdb     0
sym_LAMBDA  fdb     0
sym_DEFUN   fdb     0
sym_COND    fdb     0
sym_LET     fdb     0
sym_SETQ    fdb     0
sym_SETBANG fdb     0
sym_GC      fdb     0
sym_PROGN   fdb     0
sym_AND     fdb     0
sym_OR      fdb     0
sym_MUL     fdb     0
sym_CADR    fdb     0
sym_CADDR   fdb     0
sym_CDDR    fdb     0
sym_LENGTH  fdb     0
sym_APPEND  fdb     0
sym_LETSTAR fdb     0
sym_LETREC  fdb     0
sym_APPLY   fdb     0
sym_LIST    fdb     0
sym_EQSYM   fdb     0
sym_PRINT   fdb     0
sym_NEWLINE fdb     0
sym_ASSOC   fdb     0
sym_DEFMACRO fdb    0
sym_MACRO   fdb     0
sym_QUASIQUOTE fdb  0
sym_UNQUOTE fdb     0
sym_UNQSPLICE fdb   0
sym_GENSYM  fdb     0
sym_STRLEN  fdb     0
sym_STREQ   fdb     0
sym_STRAPP  fdb     0
sym_STRREF  fdb     0
sym_STR2L   fdb     0
sym_L2STR   fdb     0
sym_DIV     fdb     0
sym_MOD     fdb     0
sym_ERROR   fdb     0
sym_CATCH   fdb     0
sym_THROW   fdb     0
sym_CHAR_INT fdb    0
sym_INT_CHAR fdb    0
sym_CHARP   fdb     0
sym_MAKE_VEC fdb    0
sym_VEC_LEN fdb     0
sym_VEC_REF fdb     0
sym_VEC_SET fdb     0
sym_VEC_LIST fdb    0
sym_LIST_VEC fdb    0
sym_VECP    fdb     0
sym_LOGAND  fdb     0
sym_LOGIOR  fdb     0
sym_LOGXOR  fdb     0
sym_LOGNOT  fdb     0
sym_ASH     fdb     0
sym_NUM2STR fdb     0
sym_STR2NUM fdb     0
sym_SYM2STR fdb     0
sym_STR2SYM fdb     0
sym_EVAL    fdb     0
sym_RDSTR   fdb     0
sym_LOADMEM fdb     0
sym_DISPLAY fdb     0
sym_PUTCHAR fdb     0
sym_RAND    fdb     0
sym_SEED    fdb     0
sym_TICK    fdb     0
sym_PCASE_GET fdb   0
sym_PCASE_SET fdb   0
print_case_mode fcb 0               ; 0 = upper (default), 1 = lower
                                    ; — toggles the printer's case folding for
                                    ; symbol names and #<TYPE>-style displays
gensym_counter fdb  0
repl_init_s fdb     0
rdr_base    fdb     TIB_ADDR
catch_sp    fdb     0
catch_thrown fdb    0
vec_next    fdb     0
str_next    fdb     0
int32_next  fdb     0
int32_free  fdb     0
global_env  fdb     0
current_env fdb     0
free_list   fdb     0
gc_freed    fdb     0

; ---------------------------------------------------------------------------
; Reset vector
; ---------------------------------------------------------------------------
            org     $FFFE
            fdb     cold

            end     cold
