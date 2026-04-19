#!/usr/bin/env python3
"""
Generate stack.s19 — recursive descent printer for exercising BSR/RTS and
PSHS/PULS machinery.  A stress test for the call-stack debugger view.

Program outline:
  reset:
      ACIA master reset + 8N1/div-16
      LDX #6
      BSR recurse
      BRA *               ; idiomatic halt

  recurse:               ; X = depth remaining (0..6)
      CMPX #0
      BEQ  ret
      PSHS X              ; save depth on S
      Emit  'E' + ('0'+depth) + CR LF    ; "E6", "E5", ...
      LDX  ,S
      LEAX -1,X
      BSR  recurse        ; dive
      Emit  'X' + ('0'+depth) + CR LF    ; "X1", "X2", ...
      PULS X
  ret:
      RTS

  putc:                  ; A = byte to write; preserves B
      PSHS B
      wait: LDB $FF00 / BITB #$02 / BEQ wait
      STA $FF01
      PULS B
      RTS

With depth=6 the console shows:
  E6 E5 E4 E3 E2 E1 X1 X2 X3 X4 X5 X6
and the call-stack panel peaks at 6 nested BSR frames at the innermost
recursion (plus main's BSR, so 7 total entries as seen by emfe_get_call_stack).

Entry $0100; reset vector at $FFFE/$FFFF set to $0100.
"""

import os

ACIA_CR_SR = 0xFF00
ACIA_DATA  = 0xFF01
CODE_START = 0x0100
RESET_VEC  = 0xFFFE
MAX_DEPTH  = 6


def emit(mem, addr, *data):
    for i, b in enumerate(data):
        mem[addr + i] = b & 0xFF
    return addr + len(data)


def emit_be(mem, addr, word):
    mem[addr]     = (word >> 8) & 0xFF
    mem[addr + 1] = word & 0xFF


def rel8(from_pc_after_branch, target):
    off = target - from_pc_after_branch
    assert -128 <= off <= 127, f"branch out of range: {off}"
    return off & 0xFF


def to_srec(mem, segments, entry):
    lines = ["S0100000656D66655F6D633638303960"]  # header
    for start, length in segments:
        i = 0
        while i < length:
            chunk = min(24, length - i)
            addr = start + i
            row = [chunk + 3, (addr >> 8) & 0xFF, addr & 0xFF] + list(mem[addr:addr + chunk])
            cksum = (~(sum(row) & 0xFF)) & 0xFF
            lines.append("S1" + "".join(f"{b:02X}" for b in row) + f"{cksum:02X}")
            i += chunk
    row = [3, (entry >> 8) & 0xFF, entry & 0xFF]
    cksum = (~(sum(row) & 0xFF)) & 0xFF
    lines.append("S9" + "".join(f"{b:02X}" for b in row) + f"{cksum:02X}")
    return "\n".join(lines) + "\n"


def main():
    mem = bytearray(0x10000)
    pc = CODE_START

    # ---------------- main ----------------
    # ACIA: $03 master reset, $15 8N1/div-16/no IRQ
    pc = emit(mem, pc, 0x86, 0x03)                     # LDA #$03
    pc = emit(mem, pc, 0xB7, 0xFF, 0x00)               # STA $FF00
    pc = emit(mem, pc, 0x86, 0x15)                     # LDA #$15
    pc = emit(mem, pc, 0xB7, 0xFF, 0x00)               # STA $FF00

    # LDX #MAX_DEPTH
    pc = emit(mem, pc, 0x8E, (MAX_DEPTH >> 8) & 0xFF, MAX_DEPTH & 0xFF)
    # BSR recurse (patched later)
    pc = emit(mem, pc, 0x8D, 0x00)
    bsr_main = pc - 1
    # BRA * (halt)
    halt_addr = pc
    pc = emit(mem, pc, 0x20, 0xFE)

    # ---------------- recurse ----------------
    # Input: X = depth; preserves X on return; trashes A, B, CC
    recurse = pc
    # CMPX #0
    pc = emit(mem, pc, 0x8C, 0x00, 0x00)
    # BEQ ret (patch)
    pc = emit(mem, pc, 0x27, 0x00)
    beq_ret = pc - 1
    # PSHS X        (postbyte $10 = X)
    pc = emit(mem, pc, 0x34, 0x10)

    # Emit 'E'
    pc = emit(mem, pc, 0x86, ord('E'))                 # LDA #'E'
    pc = emit(mem, pc, 0x8D, 0x00)                     # BSR putc (patch)
    bsr_putc_e = pc - 1

    # Emit '0' + depth  (depth is in X; low byte via TFR X,D then ADDB #'0')
    pc = emit(mem, pc, 0x1F, 0x10)                     # TFR X,D
    pc = emit(mem, pc, 0xCB, ord('0'))                 # ADDB #'0'
    pc = emit(mem, pc, 0x1F, 0x98)                     # TFR B,A (src=B=9, dst=A=8 → $98)
    pc = emit(mem, pc, 0x8D, 0x00)                     # BSR putc (patch)
    bsr_putc_d1 = pc - 1

    # Emit CR
    pc = emit(mem, pc, 0x86, 0x0D)                     # LDA #$0D
    pc = emit(mem, pc, 0x8D, 0x00)
    bsr_putc_cr1 = pc - 1
    # Emit LF
    pc = emit(mem, pc, 0x86, 0x0A)                     # LDA #$0A
    pc = emit(mem, pc, 0x8D, 0x00)
    bsr_putc_lf1 = pc - 1

    # Reload X from stack, decrement, recurse
    # LDX ,S        (postbyte $60 = n=0, R=S, 5-bit form)
    pc = emit(mem, pc, 0xAE, 0x60)
    # LEAX -1,X     (postbyte $1F = n=-1 in 5-bit, R=X)
    pc = emit(mem, pc, 0x30, 0x1F)
    # BSR recurse (self, patched later)
    pc = emit(mem, pc, 0x8D, 0x00)
    bsr_self = pc - 1

    # Emit 'X' exit marker
    pc = emit(mem, pc, 0x86, ord('X'))                 # LDA #'X'
    pc = emit(mem, pc, 0x8D, 0x00)                     # BSR putc (patch)
    bsr_putc_x = pc - 1

    # Emit depth digit again — reload from stack since X/D are trashed
    pc = emit(mem, pc, 0xAE, 0x60)                     # LDX ,S
    pc = emit(mem, pc, 0x1F, 0x10)                     # TFR X,D
    pc = emit(mem, pc, 0xCB, ord('0'))                 # ADDB #'0'
    pc = emit(mem, pc, 0x1F, 0x98)                     # TFR B,A
    pc = emit(mem, pc, 0x8D, 0x00)                     # BSR putc (patch)
    bsr_putc_d2 = pc - 1

    # CR LF
    pc = emit(mem, pc, 0x86, 0x0D)
    pc = emit(mem, pc, 0x8D, 0x00)
    bsr_putc_cr2 = pc - 1
    pc = emit(mem, pc, 0x86, 0x0A)
    pc = emit(mem, pc, 0x8D, 0x00)
    bsr_putc_lf2 = pc - 1

    # PULS X, RTS
    pc = emit(mem, pc, 0x35, 0x10)                     # PULS X
    ret_addr = pc
    mem[beq_ret] = rel8(beq_ret + 1, ret_addr)
    pc = emit(mem, pc, 0x39)                           # RTS

    # Self-patch: BSR recurse targets
    mem[bsr_main] = rel8(bsr_main + 1, recurse)
    mem[bsr_self] = rel8(bsr_self + 1, recurse)

    # ---------------- putc ----------------
    # A = char to write; preserves B
    putc = pc
    pc = emit(mem, pc, 0x34, 0x04)                     # PSHS B (bit 2 = B)
    pc_wait = pc
    pc = emit(mem, pc, 0xF6, 0xFF, 0x00)               # LDB $FF00 (SR)
    pc = emit(mem, pc, 0xC5, 0x02)                     # BITB #$02 (TDRE)
    pc = emit(mem, pc, 0x27, rel8(pc + 2, pc_wait))    # BEQ wait
    pc = emit(mem, pc, 0xB7, 0xFF, 0x01)               # STA $FF01 (TDR)
    pc = emit(mem, pc, 0x35, 0x04)                     # PULS B
    pc = emit(mem, pc, 0x39)                           # RTS

    # Patch all putc BSRs now that putc address is known
    for patch_site in (bsr_putc_e, bsr_putc_d1, bsr_putc_cr1, bsr_putc_lf1,
                       bsr_putc_x, bsr_putc_d2, bsr_putc_cr2, bsr_putc_lf2):
        mem[patch_site] = rel8(patch_site + 1, putc)

    code_end = pc

    # Reset vector → CODE_START
    emit_be(mem, RESET_VEC, CODE_START)

    srec = to_srec(
        mem,
        [(CODE_START, code_end - CODE_START), (RESET_VEC, 2)],
        CODE_START,
    )

    out_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "stack.s19")
    with open(out_path, "w") as f:
        f.write(srec)

    print(f"Generated {out_path}")
    print(f"  recurse:  ${recurse:04X}")
    print(f"  putc:     ${putc:04X}")
    print(f"  halt:     ${halt_addr:04X}")
    print(f"  code end: ${code_end - 1:04X}  ({code_end - CODE_START} bytes)")
    print(f"  max depth: {MAX_DEPTH} (call stack peaks at {MAX_DEPTH + 1} frames)")


if __name__ == "__main__":
    main()
