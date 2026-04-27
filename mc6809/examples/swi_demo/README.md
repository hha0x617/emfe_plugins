# SWI / call-stack demo

A tiny program that exercises **BSR** and **SWI** on the same execution
thread, so the emfe Call Stack pane has a clean target for verifying
that the Phase C consolidation reports interrupt frames correctly.

## What it does

```
loop:
    lda  #'A'
    bsr  putc       ; explicit subroutine call (CALL frame)
    swi             ; transfer to swi_handler  (EXCEPTION frame)
    lda  #$0A
    bsr  putc
    bra  loop

swi_handler:
    lda  #'S'
    bsr  putc       ; nested CALL frame inside the EXCEPTION
    rti
```

ACIA layout matches the rest of the examples (status @ `$FF00`, data
@ `$FF01`). Output stream is a continuous `AS\nAS\nAS\n...`.

## Build

```sh
lwasm -9 -f srec -o swi_demo.s19 --list=swi_demo.lst swi_demo.asm
```

The committed `swi_demo.s19` / `swi_demo.lst` were produced this way;
re-build only if you edit the source.

## What to look for

Load `swi_demo.s19` in emfe_WinUI3Cpp / emfe_CsWPF and:

1. Open **View → Call Stack** (or the equivalent pane in emfe).
2. Set a breakpoint at the `sta ACIA_DATA` line inside `putc`
   (address `$1022` per the listing).
3. Press **Run**.

When the breakpoint fires *during the SWI sub-call* (every other
iteration), the Call Stack pane should show two frames:

| #  | Kind        | Call PC         | Target PC    | Return PC    |
|----|-------------|-----------------|--------------|--------------|
| 2  | **CALL**    | `$1029` (BSR)   | `$1019`      | `$102B`      |
| 1  | **EXCEPTION** | `$1012` (SWI) | `$1027`      | `$1013`      |

The **EXCEPTION** entry is the new behaviour from Phase C — before the
plugin migrated to the em6809 core's shadow stack, only the BSR/JSR
opcodes were tracked, so the SWI frame was missing entirely.

When the breakpoint fires *during the main-loop sub-call* (the other
iteration), only the top **CALL** frame appears (no EXCEPTION),
matching the un-elevated state.

## Why a tiny example

`lisp.s19` and the other large examples don't issue `swi` and don't
take ACIA RX interrupts, so they exercise only the BSR/JSR/RTS path
the plugin already handled before Phase C. This demo isolates the
interrupt-frame case in ~25 lines so the regression test is fast to
inspect by eye.
