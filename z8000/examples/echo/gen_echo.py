#!/usr/bin/env python3
"""
Generate echo.bin — Z8000 polling-based UART echo program.

Prints a prompt, then polls the UART status register for RX-ready, reads the
incoming byte, writes it back to the TX port, and repeats. Carriage-return
is expanded to CR+LF and followed by a fresh "> " prompt.

Binary format: 64KB raw image, code at $0100, PSA at $0000.

Layout: subroutines (print_str, wait_rx) are emitted FIRST so that the main
loop can reach them with backward CALR disp12 (Z8000 CALR is backward-only).
Strings follow the main loop.
"""

import os

UART_DATA   = 0xFE00
UART_STATUS = 0xFE01
CODE_START  = 0x0100
INITIAL_FCW = 0x2000
INITIAL_SP  = 0xF000


def emit(mem, addr, *data):
    for i, b in enumerate(data):
        mem[addr + i] = b & 0xFF
    return addr + len(data)


def emit_word_be(mem, addr, word):
    mem[addr]     = (word >> 8) & 0xFF
    mem[addr + 1] = word & 0xFF


def calr_disp(from_pc_after_fetch, target):
    """CALR disp12: target = PC_after_fetch - 2*disp12 (disp12 unsigned, backward)."""
    delta = from_pc_after_fetch - target
    assert delta >= 0 and delta % 2 == 0, f"CALR target must be ≤ caller: delta={delta}"
    disp12 = delta // 2
    assert 0 <= disp12 < 0x1000, f"CALR disp12 out of range: {disp12}"
    return disp12


def main():
    mem = bytearray(65536)
    pc = CODE_START

    # ------------------------------------------------------------------
    # Subroutine: print_str (R2=pointer, returns when *R2==0)
    # ------------------------------------------------------------------
    print_str_addr = pc
    # loop:
    pr_loop = pc
    pc = emit(mem, pc, 0x20, 0x28)          # LDB RL0,@R2    (s=2,d=8)
    pc = emit(mem, pc, 0x84, 0x88)          # ORB RL0,RL0
    pc = emit(mem, pc, 0x9E, 0x06)          # RET Z
    pc = emit(mem, pc, 0x3A, 0x86)          # OUTB #UART_DATA,RL0
    emit_word_be(mem, pc, UART_DATA); pc += 2
    pc = emit(mem, pc, 0x81, 0x12)          # ADD R2,R1       (R1 = 1)
    disp = (pr_loop - (pc + 2)) // 2
    pc = emit(mem, pc, 0xE8, disp & 0xFF)   # JR T,pr_loop

    # ------------------------------------------------------------------
    # Subroutine: wait_rx (spin until UART SR bit0 set)
    # ------------------------------------------------------------------
    wait_rx_addr = pc
    wr_loop = pc
    pc = emit(mem, pc, 0x3A, 0x84)          # INB RL0,#UART_STATUS
    emit_word_be(mem, pc, UART_STATUS); pc += 2
    pc = emit(mem, pc, 0x86, 0xD8)          # ANDB RL0,RL5    (RL5=byte reg 13)
    disp = (wr_loop - (pc + 2)) // 2
    pc = emit(mem, pc, 0xE6, disp & 0xFF)   # JR Z,wr_loop
    pc = emit(mem, pc, 0x9E, 0x08)          # RET T

    # ------------------------------------------------------------------
    # Main program
    # ------------------------------------------------------------------
    main_start = pc

    # Init regs
    pc = emit(mem, pc, 0x21, 0x0F)          # LD R15,#$F000
    emit_word_be(mem, pc, INITIAL_SP); pc += 2
    pc = emit(mem, pc, 0x21, 0x01)          # LD R1,#1  (increment)
    emit_word_be(mem, pc, 0x0001); pc += 2
    pc = emit(mem, pc, 0x21, 0x05)          # LD R5,#1  (status mask)
    emit_word_be(mem, pc, 0x0001); pc += 2

    # Print initial banner
    ld_banner_addr = pc
    pc = emit(mem, pc, 0x21, 0x02, 0x00, 0x00)   # LD R2,#banner (patch)
    # CALR print_str
    disp12 = calr_disp(pc + 2, print_str_addr)
    pc = emit(mem, pc, 0xD0 | ((disp12 >> 8) & 0x0F), disp12 & 0xFF)

    # Idle loop
    idle_loop = pc

    # CALR wait_rx
    disp12 = calr_disp(pc + 2, wait_rx_addr)
    pc = emit(mem, pc, 0xD0 | ((disp12 >> 8) & 0x0F), disp12 & 0xFF)

    # INB RL0,#UART_DATA
    pc = emit(mem, pc, 0x3A, 0x84); emit_word_be(mem, pc, UART_DATA); pc += 2
    # OUTB #UART_DATA,RL0
    pc = emit(mem, pc, 0x3A, 0x86); emit_word_be(mem, pc, UART_DATA); pc += 2

    # If char == CR, also emit LF and reprint "> ".
    pc = emit(mem, pc, 0xC9, 0x0D)          # LDB RL1,#$0D
    pc = emit(mem, pc, 0x8A, 0x98)          # CPB RL0,RL1  (s=9,d=8)
    jr_skip_cr = pc
    pc = emit(mem, pc, 0xEE, 0x00)          # JR NE,skip_cr (patch)

    pc = emit(mem, pc, 0xC8, 0x0A)          # LDB RL0,#$0A (LF)
    pc = emit(mem, pc, 0x3A, 0x86); emit_word_be(mem, pc, UART_DATA); pc += 2

    ld_prompt2_addr = pc
    pc = emit(mem, pc, 0x21, 0x02, 0x00, 0x00)   # LD R2,#prompt2 (patch)
    disp12 = calr_disp(pc + 2, print_str_addr)
    pc = emit(mem, pc, 0xD0 | ((disp12 >> 8) & 0x0F), disp12 & 0xFF)

    skip_cr = pc
    disp = (skip_cr - (jr_skip_cr + 2)) // 2
    mem[jr_skip_cr + 1] = disp & 0xFF

    # JR T,idle_loop
    disp_back = (idle_loop - (pc + 2)) // 2
    pc = emit(mem, pc, 0xE8, disp_back & 0xFF)

    # ------------------------------------------------------------------
    # Strings
    # ------------------------------------------------------------------
    banner_addr = pc
    for ch in b"Z8000 Echo. Type any character; Enter for a new line.\r\n> \x00":
        mem[pc] = ch; pc += 1

    prompt2_addr = pc
    for ch in b"> \x00":
        mem[pc] = ch; pc += 1

    # Patch string references
    emit_word_be(mem, ld_banner_addr  + 2, banner_addr)
    emit_word_be(mem, ld_prompt2_addr + 2, prompt2_addr)

    # ------------------------------------------------------------------
    # PSA
    # ------------------------------------------------------------------
    emit_word_be(mem, 0x0002, INITIAL_FCW)
    emit_word_be(mem, 0x0004, main_start)

    out_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "echo.bin")
    with open(out_path, "wb") as f:
        f.write(mem)

    print(f"Generated {out_path}")
    print(f"  print_str: ${print_str_addr:04X}")
    print(f"  wait_rx:   ${wait_rx_addr:04X}")
    print(f"  main:      ${main_start:04X}")
    print(f"  banner:    ${banner_addr:04X}")
    print(f"  prompt2:   ${prompt2_addr:04X}")
    print(f"  code end:  ${pc - 1:04X}")


if __name__ == "__main__":
    main()
