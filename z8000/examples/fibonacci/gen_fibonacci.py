#!/usr/bin/env python3
"""
Generate fibonacci.bin — Z8000 program that computes fib(N) iteratively and
prints the first N+1 values in decimal to the UART.

Output:
  Fibonacci:
  0
  1
  1
  2
  3
  5
  8
  13
  21
  34
  55
  HALT

Design notes:
  - Iterative fib: R1=a, R2=b, each iteration (R4=a+b; a:=b; b:=R4).
  - Integer-to-decimal: only registers (no memory buffer). A subroutine
    peels digits by repeated subtraction against powers of 10 (10000, 1000,
    100, 10, 1), emitting each digit (suppressing leading zeros except the
    last). Values stay within 16-bit unsigned (fib(24) = 46368 < 65536).
  - Phase 1 ISA limits: no INC/DEC-Rd-#n, no ADDB/SUBB immediate, no shifts.
    Everything is reg-reg ALU + LD imm.
"""

import os

UART_DATA  = 0xFE00
CODE_START = 0x0100
INITIAL_FCW = 0x2000
INITIAL_SP  = 0xF000

# How many Fibonacci terms to print (0..N inclusive).
N_TERMS = 11


def emit(mem, addr, *data):
    for i, b in enumerate(data):
        mem[addr + i] = b & 0xFF
    return addr + len(data)


def emit_word_be(mem, addr, word):
    mem[addr]     = (word >> 8) & 0xFF
    mem[addr + 1] = word & 0xFF


def calr_disp(from_pc_after_fetch, target):
    delta = from_pc_after_fetch - target
    assert delta >= 0 and delta % 2 == 0, f"CALR target must be ≤ caller: delta={delta}"
    disp12 = delta // 2
    assert 0 <= disp12 < 0x1000, f"CALR disp12 out of range: {disp12}"
    return disp12


def main():
    mem = bytearray(65536)
    pc = CODE_START

    # ==================================================================
    # Subroutines first (so the main loop can CALR backward to reach them)
    # ==================================================================

    # ------------------------------------------------------------------
    # print_str  (R2 = pointer, R1 = 1)
    # ------------------------------------------------------------------
    print_str_addr = pc
    ps_loop = pc
    pc = emit(mem, pc, 0x20, 0x28)          # LDB RL0,@R2
    pc = emit(mem, pc, 0x84, 0x88)          # ORB RL0,RL0
    pc = emit(mem, pc, 0x9E, 0x06)          # RET Z
    pc = emit(mem, pc, 0x3A, 0x86)          # OUTB #UART_DATA,RL0
    emit_word_be(mem, pc, UART_DATA); pc += 2
    pc = emit(mem, pc, 0x81, 0x12)          # ADD R2,R1
    disp = (ps_loop - (pc + 2)) // 2
    pc = emit(mem, pc, 0xE8, disp & 0xFF)   # JR T,ps_loop

    # ------------------------------------------------------------------
    # print_digit (RL0 = digit value 0..9 on entry; emits '0'..'9')
    # ------------------------------------------------------------------
    print_digit_addr = pc
    # Add '0' (=$30) to RL0 by ADDB with RL7 (which holds $30).
    # Byte reg 15 = RL7.
    pc = emit(mem, pc, 0x80, 0xF8)          # ADDB RL0,RL7   (s=15,d=8)
    pc = emit(mem, pc, 0x3A, 0x86)          # OUTB #UART_DATA,RL0
    emit_word_be(mem, pc, UART_DATA); pc += 2
    pc = emit(mem, pc, 0x9E, 0x08)          # RET T

    # ------------------------------------------------------------------
    # print_u16_decimal
    #   On entry: R3 = value (uint16)
    #   Uses:    R1=1, R4=digit accumulator, R5=power-of-10 const, R6=scratch
    #   Strategy: for each power-of-10 (10000,1000,100,10,1), count how many
    #   times it fits into R3; that is the digit. Suppress leading zeros
    #   except in the final (ones) place.
    # ------------------------------------------------------------------
    print_u16_addr = pc

    # R13 = 0 (leading-zero flag — "have we printed a nonzero digit yet?")
    # NOTE: R7 is reserved for RL7 = '0' constant used by print_digit. Using
    # R7 here would trash it mid-routine.
    pc = emit(mem, pc, 0x21, 0x0D)          # LD R13,#0
    emit_word_be(mem, pc, 0x0000); pc += 2

    # Helper macro: process one power-of-10 constant (value in R5).
    # Produces digit in RL0, optionally prints it.
    # Inline each of the 5 places (10000,1000,100,10,1).
    for pwr_idx, pwr in enumerate([10000, 1000, 100, 10, 1]):
        is_last = (pwr == 1)

        # R5 = pwr
        pc = emit(mem, pc, 0x21, 0x05)
        emit_word_be(mem, pc, pwr); pc += 2
        # R4 = 0   (digit counter)
        pc = emit(mem, pc, 0x21, 0x04)
        emit_word_be(mem, pc, 0x0000); pc += 2

        # count_loop: while R3 >= R5  { R3 -= R5; R4 += 1 }
        count_loop = pc
        # LD R6,R3 ; copy value
        pc = emit(mem, pc, 0xA1, 0x36)      # LD R6,R3  (s=3,d=6)
        # SUB R6,R5  -> sets C if R3 < R5 (unsigned)
        pc = emit(mem, pc, 0x83, 0x56)      # SUB R6,R5
        # JR C,count_done  (cc=7=C/ULT)
        jr_done_c = pc
        pc = emit(mem, pc, 0xE7, 0x00)      # patch

        # R3 = R6 (the subtracted value)
        pc = emit(mem, pc, 0xA1, 0x63)      # LD R3,R6 (s=6,d=3)
        # R4 += R1 (=1)
        pc = emit(mem, pc, 0x81, 0x14)      # ADD R4,R1
        # JR T,count_loop
        disp = (count_loop - (pc + 2)) // 2
        pc = emit(mem, pc, 0xE8, disp & 0xFF)

        count_done = pc
        disp = (count_done - (jr_done_c + 2)) // 2
        mem[jr_done_c + 1] = disp & 0xFF

        # RL0 = low byte of R4 (digit is 0..9)
        # RL4 is byte-register 12 (= $C); RL0 is byte-register 8. Encoding $A0[s][d].
        pc = emit(mem, pc, 0xA0, 0xC8)      # LDB RL0,RL4

        if not is_last:
            # Suppress leading zero: if digit==0 AND R7==0 (no prior nonzero),
            # skip emitting. Otherwise: print and set R7 to 1.
            # ORB RL0,RL0
            pc = emit(mem, pc, 0x84, 0x88)  # ORB RL0,RL0
            # JR NZ,emit_now
            jr_nz = pc
            pc = emit(mem, pc, 0xEE, 0x00)  # JR NE

            # digit == 0: check R13
            # OR R13,R13
            pc = emit(mem, pc, 0x85, 0xDD)  # OR R13,R13
            # JR Z,skip (skip = fall through past emit block; patched below)
            jr_z_skip = pc
            pc = emit(mem, pc, 0xE6, 0x00)  # JR Z

            emit_now = pc
            disp = (emit_now - (jr_nz + 2)) // 2
            mem[jr_nz + 1] = disp & 0xFF

            # R13 = 1 — "have printed a digit"
            pc = emit(mem, pc, 0x21, 0x0D)
            emit_word_be(mem, pc, 0x0001); pc += 2

            # CALR print_digit
            disp12 = calr_disp(pc + 2, print_digit_addr)
            pc = emit(mem, pc, 0xD0 | ((disp12 >> 8) & 0x0F), disp12 & 0xFF)

            skip = pc
            disp = (skip - (jr_z_skip + 2)) // 2
            mem[jr_z_skip + 1] = disp & 0xFF
        else:
            # Last place: always print, even if 0 (in case value was 0).
            disp12 = calr_disp(pc + 2, print_digit_addr)
            pc = emit(mem, pc, 0xD0 | ((disp12 >> 8) & 0x0F), disp12 & 0xFF)

    # Print newline: CR+LF
    pc = emit(mem, pc, 0xC8, 0x0D)          # LDB RL0,#$0D
    pc = emit(mem, pc, 0x3A, 0x86)          # OUTB #UART_DATA,RL0
    emit_word_be(mem, pc, UART_DATA); pc += 2
    pc = emit(mem, pc, 0xC8, 0x0A)          # LDB RL0,#$0A
    pc = emit(mem, pc, 0x3A, 0x86)          # OUTB
    emit_word_be(mem, pc, UART_DATA); pc += 2
    # RET
    pc = emit(mem, pc, 0x9E, 0x08)

    # ==================================================================
    # Main program
    # ==================================================================
    main_start = pc

    # Init:
    #   R15 = SP
    #   R1 = 1  (increment)
    #   RL7 = '0' (=$30) for digit emit
    pc = emit(mem, pc, 0x21, 0x0F); emit_word_be(mem, pc, INITIAL_SP); pc += 2
    pc = emit(mem, pc, 0x21, 0x01); emit_word_be(mem, pc, 0x0001); pc += 2
    # LD R7,#$3030  (RL7 = $30 = '0'; RH7 unused)
    pc = emit(mem, pc, 0x21, 0x07); emit_word_be(mem, pc, 0x3030); pc += 2

    # Print banner
    ld_banner = pc
    pc = emit(mem, pc, 0x21, 0x02, 0x00, 0x00)  # LD R2,#banner (patch)
    disp12 = calr_disp(pc + 2, print_str_addr)
    pc = emit(mem, pc, 0xD0 | ((disp12 >> 8) & 0x0F), disp12 & 0xFF)

    # Fibonacci: R8 = a, R9 = b, R10 = N (remaining terms)
    pc = emit(mem, pc, 0x21, 0x08); emit_word_be(mem, pc, 0x0000); pc += 2  # a = 0
    pc = emit(mem, pc, 0x21, 0x09); emit_word_be(mem, pc, 0x0001); pc += 2  # b = 1
    pc = emit(mem, pc, 0x21, 0x0A); emit_word_be(mem, pc, N_TERMS); pc += 2 # count

    fib_loop = pc

    # Print a (the current term): LD R3,R8; CALR print_u16_addr
    pc = emit(mem, pc, 0xA1, 0x83)          # LD R3,R8  (s=8,d=3)
    disp12 = calr_disp(pc + 2, print_u16_addr)
    pc = emit(mem, pc, 0xD0 | ((disp12 >> 8) & 0x0F), disp12 & 0xFF)

    # tmp = a + b (R11 = a + b)
    pc = emit(mem, pc, 0xA1, 0x8B)          # LD R11,R8
    pc = emit(mem, pc, 0x81, 0x9B)          # ADD R11,R9
    # a = b
    pc = emit(mem, pc, 0xA1, 0x98)          # LD R8,R9
    # b = tmp
    pc = emit(mem, pc, 0xA1, 0xB9)          # LD R9,R11

    # R10 -= R1 ; loop if nonzero
    pc = emit(mem, pc, 0x83, 0x1A)          # SUB R10,R1 (s=1,d=10)
    jr_back = pc
    disp = (fib_loop - (jr_back + 2)) // 2
    pc = emit(mem, pc, 0xEE, disp & 0xFF)   # JR NE,fib_loop

    # Print final "HALT\r\n" banner
    ld_end = pc
    pc = emit(mem, pc, 0x21, 0x02, 0x00, 0x00)
    disp12 = calr_disp(pc + 2, print_str_addr)
    pc = emit(mem, pc, 0xD0 | ((disp12 >> 8) & 0x0F), disp12 & 0xFF)

    pc = emit(mem, pc, 0x7A, 0x00)          # HALT

    # ==================================================================
    # Strings
    # ==================================================================
    banner_addr = pc
    for ch in b"Fibonacci:\r\n\x00":
        mem[pc] = ch; pc += 1

    end_addr = pc
    for ch in b"HALT\r\n\x00":
        mem[pc] = ch; pc += 1

    emit_word_be(mem, ld_banner + 2, banner_addr)
    emit_word_be(mem, ld_end    + 2, end_addr)

    # PSA
    emit_word_be(mem, 0x0002, INITIAL_FCW)
    emit_word_be(mem, 0x0004, main_start)

    out_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fibonacci.bin")
    with open(out_path, "wb") as f:
        f.write(mem)

    print(f"Generated {out_path}")
    print(f"  print_str:  ${print_str_addr:04X}")
    print(f"  print_digit:${print_digit_addr:04X}")
    print(f"  print_u16:  ${print_u16_addr:04X}")
    print(f"  main:       ${main_start:04X}")
    print(f"  banner:     ${banner_addr:04X}")
    print(f"  end_msg:    ${end_addr:04X}")
    print(f"  code end:   ${pc - 1:04X}")


if __name__ == "__main__":
    main()
