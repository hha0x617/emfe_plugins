; SPDX-License-Identifier: MIT OR Apache-2.0
; Copyright (c) 2026 hha0x617
;
; ---------------------------------------------------------------------------
; SWI demo for emfe_plugin_mc6809
;
; Tiny program that exercises BSR (CALL frame) and SWI (EXCEPTION frame)
; on the same execution thread, so the emfe Call Stack pane has a
; clean target for the Phase C consolidation work to be visible.
;
; Talks to the same MC6850 ACIA layout the rest of the examples use
; (status @ $FF00, data @ $FF01).
;
; Main loop:
;   - emit 'A' via a BSR-called subroutine    -> CALL frame
;   - issue SWI                              -> EXCEPTION frame
;     swi_handler: emit 'S' via BSR          -> CALL frame nested
;                                              under the EXCEPTION
;     RTI                                     -> EXCEPTION frame popped
;   - emit '\n' via the same subroutine      -> CALL frame
;   - back to top of loop
;
; To exercise the Call Stack pane, set a breakpoint inside `putc` (e.g.,
; at the `sta ACIA_DATA` line). When execution halts there during the
; SWI sub-call, the Call Stack pane should show three frames:
;
;   #3   CALL       inside putc (sta ACIA_DATA)     <- top
;   #2   EXCEPTION  swi_handler -> putc
;   #1   CALL       main loop -> swi (return after SWI)
;
; Without Phase C the EXCEPTION frame would be missing entirely.
; ---------------------------------------------------------------------------

ACIA_SR     equ     $FF00
ACIA_DATA   equ     $FF01

ROM_BASE    equ     $1000
STACK_TOP   equ     $2000

            org     ROM_BASE

;; ---------------------------------------------------------------
;; Entry point. Set up S, then loop forever printing "AS\n".
;; ---------------------------------------------------------------
start:
            lds     #STACK_TOP
loop:
            lda     #'A'
            bsr     putc            ; explicit subroutine call
            swi                     ; transfer through swi_handler
            lda     #$0A            ; '\n'
            bsr     putc
            bra     loop

;; ---------------------------------------------------------------
;; putc — busy-wait write the byte in A to the ACIA. Preserves B.
;; Loops until TDRE (bit 1 of status) goes high, then writes.
;; ---------------------------------------------------------------
putc:
            pshs    b
putc_wait:
            ldb     ACIA_SR
            bitb    #$02            ; TDRE = bit 1
            beq     putc_wait
            sta     ACIA_DATA
            puls    b,pc

;; ---------------------------------------------------------------
;; SWI handler. Emits 'S' through the same putc helper so we end up
;; with a CALL frame nested under the EXCEPTION frame the SWI
;; pushed. Returns via RTI, which restores the saved PC and CC.
;; ---------------------------------------------------------------
swi_handler:
            lda     #'S'
            bsr     putc
            rti

            ;; ----- vectors -----
            org     $FFFA
            fdb     swi_handler     ; $FFFA-FFFB : SWI vector
            org     $FFFE
            fdb     start           ; $FFFE-FFFF : reset vector

            end
