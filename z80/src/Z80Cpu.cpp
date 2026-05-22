// SPDX-License-Identifier: MIT OR Apache-2.0
// Copyright (c) 2026 hha0x617
//
// Z80Cpu.cpp - Z80 CPU core.
//
// Phase 1.7: Phase 1.6 + INC/DEC (8-bit r and 16-bit pair) + IN/OUT
// immediate. With this set the plugin can execute a hand-written hello-
// world ROM that walks a string, OUTs each byte to the UART and loops
// via INC HL — that's the Phase 1 acceptance demo.
// Prefix opcodes (CB / DD / ED / FD), HALT, EX/EXX, and 16-bit ALU land
// in Phase 2.

#include "pch.h"
#include "Z80Cpu.h"
#include "Z80Memory.h"

namespace {
    // Even-parity check on an 8-bit value. Returns true when the popcount
    // of `v` is even (matches the Zilog "P" flag for AND/OR/XOR).
    inline bool parity8(uint8_t v) {
        v ^= static_cast<uint8_t>(v >> 4);
        v ^= static_cast<uint8_t>(v >> 2);
        v ^= static_cast<uint8_t>(v >> 1);
        return (v & 1) == 0;
    }
}

void Z80Cpu::Reset() {
    // Z80 power-on / RESET state per Zilog UM008011 §2.4:
    //   PC = 0x0000, I = 0, R = 0, IFF1 = IFF2 = 0, IM = 0.
    // Other registers are undefined on real hardware; we clear them for
    // determinism inside the emulator.
    AF = BC = DE = HL = 0;
    AF_ = BC_ = DE_ = HL_ = 0;
    IX = IY = SP = PC = 0;
    I = 0;
    R = 0;
    IFF1 = IFF2 = false;
    IM = 0;
    Halted = false;
    CycleCount = 0;
    InstructionCount = 0;
    NmiPending = false;
    IrqLine = false;
}

uint8_t Z80Cpu::FetchByte() {
    uint8_t v = Memory->ReadByte(PC);
    PC = static_cast<uint16_t>(PC + 1);
    return v;
}

uint16_t Z80Cpu::FetchWord() {
    uint16_t lo = FetchByte();
    uint16_t hi = FetchByte();
    return static_cast<uint16_t>(lo | (hi << 8));
}

uint8_t Z80Cpu::Reg8Read(int code) {
    switch (code) {
    case 0: return B();
    case 1: return C();
    case 2: return D();
    case 3: return E();
    case 4: return H();
    case 5: return L();
    case 6: return Memory->ReadByte(HL);
    case 7: return A();
    default: return 0;
    }
}

void Z80Cpu::Reg8Write(int code, uint8_t val) {
    switch (code) {
    case 0: SetB(val); break;
    case 1: SetC(val); break;
    case 2: SetD(val); break;
    case 3: SetE(val); break;
    case 4: SetH(val); break;
    case 5: SetL(val); break;
    case 6: Memory->WriteByte(HL, val); break;
    case 7: SetA(val); break;
    }
}

// ---- 8-bit ALU helpers ---------------------------------------------
// All update F via SetF. The "Zilog peculiarity" handled here:
//   - Y / X come from the result for ADD/ADC/SUB/SBC/AND/OR/XOR.
//   - Y / X come from the OPERAND for CP (since CP doesn't write a
//     result back to A, the convention reuses the operand's bits 5/3).
// Signed-overflow (P/V) is computed via the standard same-sign / sign-
// flip identity.

uint8_t Z80Cpu::AluAdd(uint8_t a, uint8_t b, bool carry) {
    int cy = carry ? 1 : 0;
    int full = static_cast<int>(a) + static_cast<int>(b) + cy;
    int half = (a & 0x0F) + (b & 0x0F) + cy;
    uint8_t result = static_cast<uint8_t>(full & 0xFF);
    uint8_t f = 0;
    if (result & 0x80)                                f |= F_S;
    if (result == 0)                                  f |= F_Z;
    if (result & 0x20)                                f |= F_Y;
    if (half > 0x0F)                                  f |= F_H;
    if (result & 0x08)                                f |= F_X;
    if (((a ^ result) & (b ^ result) & 0x80) != 0)    f |= F_PV;
    // N = 0 for add — already cleared.
    if (full > 0xFF)                                  f |= F_C;
    SetF(f);
    return result;
}

uint8_t Z80Cpu::AluSub(uint8_t a, uint8_t b, bool borrow) {
    int by = borrow ? 1 : 0;
    int full = static_cast<int>(a) - static_cast<int>(b) - by;
    int half = (a & 0x0F) - (b & 0x0F) - by;
    uint8_t result = static_cast<uint8_t>(full & 0xFF);
    uint8_t f = F_N;
    if (result & 0x80)                                f |= F_S;
    if (result == 0)                                  f |= F_Z;
    if (result & 0x20)                                f |= F_Y;
    if (half < 0)                                     f |= F_H;
    if (result & 0x08)                                f |= F_X;
    if (((a ^ b) & (a ^ result) & 0x80) != 0)         f |= F_PV;
    if (full < 0)                                     f |= F_C;
    SetF(f);
    return result;
}

uint8_t Z80Cpu::AluAnd(uint8_t a, uint8_t b) {
    uint8_t result = static_cast<uint8_t>(a & b);
    uint8_t f = F_H;                                  // AND sets H
    if (result & 0x80)                                f |= F_S;
    if (result == 0)                                  f |= F_Z;
    if (result & 0x20)                                f |= F_Y;
    if (result & 0x08)                                f |= F_X;
    if (parity8(result))                              f |= F_PV;
    // N = 0, C = 0 — already cleared.
    SetF(f);
    return result;
}

uint8_t Z80Cpu::AluOr(uint8_t a, uint8_t b) {
    uint8_t result = static_cast<uint8_t>(a | b);
    uint8_t f = 0;                                    // OR clears H/N/C
    if (result & 0x80)                                f |= F_S;
    if (result == 0)                                  f |= F_Z;
    if (result & 0x20)                                f |= F_Y;
    if (result & 0x08)                                f |= F_X;
    if (parity8(result))                              f |= F_PV;
    SetF(f);
    return result;
}

uint8_t Z80Cpu::AluXor(uint8_t a, uint8_t b) {
    uint8_t result = static_cast<uint8_t>(a ^ b);
    uint8_t f = 0;                                    // XOR clears H/N/C
    if (result & 0x80)                                f |= F_S;
    if (result == 0)                                  f |= F_Z;
    if (result & 0x20)                                f |= F_Y;
    if (result & 0x08)                                f |= F_X;
    if (parity8(result))                              f |= F_PV;
    SetF(f);
    return result;
}

// ---- Stack helpers --------------------------------------------------
// Z80 push convention: SP pre-decrements by 2, then the word is stored
// little-endian (low byte at SP, high byte at SP+1). Pop is the inverse.

void Z80Cpu::Push16(uint16_t val) {
    SP = static_cast<uint16_t>(SP - 2);
    Memory->WriteWord(SP, val);
}

uint16_t Z80Cpu::Pop16() {
    uint16_t v = Memory->ReadWord(SP);
    SP = static_cast<uint16_t>(SP + 2);
    return v;
}

// ---- Condition-code evaluator (Zilog 3-bit ccc) ---------------------

bool Z80Cpu::CheckCond(int ccc) const {
    uint8_t f = F();
    switch (ccc & 0x07) {
    case 0: return (f & F_Z)  == 0;  // NZ
    case 1: return (f & F_Z)  != 0;  // Z
    case 2: return (f & F_C)  == 0;  // NC
    case 3: return (f & F_C)  != 0;  // C
    case 4: return (f & F_PV) == 0;  // PO (parity odd)
    case 5: return (f & F_PV) != 0;  // PE (parity even)
    case 6: return (f & F_S)  == 0;  // P  (positive)
    case 7: return (f & F_S)  != 0;  // M  (minus)
    }
    return false;
}

void Z80Cpu::AluCp(uint8_t a, uint8_t b) {
    int full = static_cast<int>(a) - static_cast<int>(b);
    int half = (a & 0x0F) - (b & 0x0F);
    uint8_t result = static_cast<uint8_t>(full & 0xFF);
    uint8_t f = F_N;
    if (result & 0x80)                                f |= F_S;
    if (result == 0)                                  f |= F_Z;
    if (b & 0x20)                                     f |= F_Y;  // from operand
    if (half < 0)                                     f |= F_H;
    if (b & 0x08)                                     f |= F_X;  // from operand
    if (((a ^ b) & (a ^ result) & 0x80) != 0)         f |= F_PV;
    if (full < 0)                                     f |= F_C;
    SetF(f);
}

int Z80Cpu::ExecuteOne() {
    if (!Memory) return 0;

    uint16_t opcodePC = PC;            // for rollback on trap
    uint8_t opcode = FetchByte();

    // The R register's lower 7 bits increment on every instruction
    // fetch (Zilog UM008011 §2.7.6 — bit 7 of R is preserved). Bumped
    // BEFORE dispatch so an unrecognised opcode still leaves R in the
    // post-fetch state that real hardware would have.
    R = static_cast<uint8_t>((R & 0x80) | ((R + 1) & 0x7F));

    // ----- 0x00 NOP ---------------------------------------------------
    if (opcode == 0x00) {
        CycleCount += 4;
        InstructionCount += 1;
        return 4;
    }

    // ----- 0x01 / 0x11 / 0x21 / 0x31 : LD rr, nn ----------------------
    // Pattern 00 pp 0001. pp: 00=BC 01=DE 10=HL 11=SP.
    if ((opcode & 0xCF) == 0x01) {
        uint16_t nn = FetchWord();
        int pp = (opcode >> 4) & 0x03;
        switch (pp) {
        case 0: BC = nn; break;
        case 1: DE = nn; break;
        case 2: HL = nn; break;
        case 3: SP = nn; break;
        }
        CycleCount += 10;
        InstructionCount += 1;
        return 10;
    }

    // ----- 0x03 / 0x13 / 0x23 / 0x33 : INC rr  (no flag effects) ------
    // Pattern 00 pp 0011. pp: 00=BC 01=DE 10=HL 11=SP.
    if ((opcode & 0xCF) == 0x03) {
        int pp = (opcode >> 4) & 0x03;
        switch (pp) {
        case 0: BC = static_cast<uint16_t>(BC + 1); break;
        case 1: DE = static_cast<uint16_t>(DE + 1); break;
        case 2: HL = static_cast<uint16_t>(HL + 1); break;
        case 3: SP = static_cast<uint16_t>(SP + 1); break;
        }
        CycleCount += 6; InstructionCount++; return 6;
    }
    // ----- 0x0B / 0x1B / 0x2B / 0x3B : DEC rr  (no flag effects) ------
    if ((opcode & 0xCF) == 0x0B) {
        int pp = (opcode >> 4) & 0x03;
        switch (pp) {
        case 0: BC = static_cast<uint16_t>(BC - 1); break;
        case 1: DE = static_cast<uint16_t>(DE - 1); break;
        case 2: HL = static_cast<uint16_t>(HL - 1); break;
        case 3: SP = static_cast<uint16_t>(SP - 1); break;
        }
        CycleCount += 6; InstructionCount++; return 6;
    }

    // ----- 0x04/0C/14/1C/24/2C/34/3C : INC r  (8-bit) -----------------
    // Pattern 00 ddd 100. Flags: S/Z/Y/H/X from result; P/V set when
    // the result is 0x80 (signed overflow on add of 1); N = 0; C is
    // preserved (per Zilog UM008011 §5).
    if ((opcode & 0xC7) == 0x04) {
        int dst = (opcode >> 3) & 0x07;
        uint8_t orig = Reg8Read(dst);
        uint8_t result = static_cast<uint8_t>(orig + 1);
        Reg8Write(dst, result);
        uint8_t f = static_cast<uint8_t>(F() & F_C);   // preserve C
        if (result & 0x80) f |= F_S;
        if (result == 0)   f |= F_Z;
        if (result & 0x20) f |= F_Y;
        if ((orig & 0x0F) == 0x0F) f |= F_H;
        if (result & 0x08) f |= F_X;
        if (result == 0x80) f |= F_PV;
        // N already 0.
        SetF(f);
        int cycles = (dst == 6) ? 11 : 4;
        CycleCount += cycles; InstructionCount++; return cycles;
    }
    // ----- 0x05/0D/15/1D/25/2D/35/3D : DEC r  (8-bit) -----------------
    // Pattern 00 ddd 101. Flags: as INC but N=1, P/V set on 0x7F
    // (signed overflow on subtract of 1), H = 1 when low nibble was 0.
    if ((opcode & 0xC7) == 0x05) {
        int dst = (opcode >> 3) & 0x07;
        uint8_t orig = Reg8Read(dst);
        uint8_t result = static_cast<uint8_t>(orig - 1);
        Reg8Write(dst, result);
        uint8_t f = static_cast<uint8_t>((F() & F_C) | F_N);   // preserve C, set N
        if (result & 0x80) f |= F_S;
        if (result == 0)   f |= F_Z;
        if (result & 0x20) f |= F_Y;
        if ((orig & 0x0F) == 0x00) f |= F_H;
        if (result & 0x08) f |= F_X;
        if (result == 0x7F) f |= F_PV;
        SetF(f);
        int cycles = (dst == 6) ? 11 : 4;
        CycleCount += cycles; InstructionCount++; return cycles;
    }

    // ----- 0x02 LD (BC), A    0x12 LD (DE), A -------------------------
    if (opcode == 0x02) { Memory->WriteByte(BC, A()); CycleCount += 7; InstructionCount++; return 7; }
    if (opcode == 0x12) { Memory->WriteByte(DE, A()); CycleCount += 7; InstructionCount++; return 7; }

    // ----- 0x0A LD A, (BC)    0x1A LD A, (DE) -------------------------
    if (opcode == 0x0A) { SetA(Memory->ReadByte(BC)); CycleCount += 7; InstructionCount++; return 7; }
    if (opcode == 0x1A) { SetA(Memory->ReadByte(DE)); CycleCount += 7; InstructionCount++; return 7; }

    // ----- 0x22 LD (nn), HL --------------------------------------------
    if (opcode == 0x22) {
        uint16_t nn = FetchWord();
        Memory->WriteWord(nn, HL);
        CycleCount += 16; InstructionCount++; return 16;
    }
    // ----- 0x2A LD HL, (nn) --------------------------------------------
    if (opcode == 0x2A) {
        uint16_t nn = FetchWord();
        HL = Memory->ReadWord(nn);
        CycleCount += 16; InstructionCount++; return 16;
    }
    // ----- 0x32 LD (nn), A ---------------------------------------------
    if (opcode == 0x32) {
        uint16_t nn = FetchWord();
        Memory->WriteByte(nn, A());
        CycleCount += 13; InstructionCount++; return 13;
    }
    // ----- 0x3A LD A, (nn) ---------------------------------------------
    if (opcode == 0x3A) {
        uint16_t nn = FetchWord();
        SetA(Memory->ReadByte(nn));
        CycleCount += 13; InstructionCount++; return 13;
    }

    // ----- 0x06 / 0x0E / ... / 0x3E : LD r, n -------------------------
    // Pattern 00 ddd 110. dst code: 0=B 1=C 2=D 3=E 4=H 5=L 6=(HL) 7=A.
    if ((opcode & 0xC7) == 0x06) {
        int dst = (opcode >> 3) & 0x07;
        uint8_t imm = FetchByte();
        Reg8Write(dst, imm);
        int cycles = (dst == 6) ? 10 : 7;
        CycleCount += cycles;
        InstructionCount += 1;
        return cycles;
    }

    // ----- 0x40 .. 0x7F : LD r, r' (except 0x76 = HALT) ---------------
    // Pattern 01 ddd sss. The (HL),(HL) encoding (0x76) is reused as
    // HALT, which Phase 1.4 does not yet implement.
    if (opcode >= 0x40 && opcode <= 0x7F && opcode != 0x76) {
        int dst = (opcode >> 3) & 0x07;
        int src = opcode & 0x07;
        uint8_t v = Reg8Read(src);
        Reg8Write(dst, v);
        bool touchesMem = (src == 6) || (dst == 6);
        int cycles = touchesMem ? 7 : 4;
        CycleCount += cycles;
        InstructionCount += 1;
        return cycles;
    }

    // ----- 0x80 .. 0xBF : 8-bit ALU on A,r ----------------------------
    // Pattern 10 ooo sss. ooo selects the operation:
    //   000 ADD  001 ADC  010 SUB  011 SBC
    //   100 AND  101 XOR  110 OR   111 CP
    // sss selects the source register code (0=B .. 7=A; code 6 = (HL)).
    if ((opcode & 0xC0) == 0x80) {
        int op  = (opcode >> 3) & 0x07;
        int src = opcode & 0x07;
        uint8_t val = Reg8Read(src);
        bool cy  = (F() & F_C) != 0;
        uint8_t a = A();
        switch (op) {
        case 0: SetA(AluAdd(a, val, false)); break;
        case 1: SetA(AluAdd(a, val, cy));    break;
        case 2: SetA(AluSub(a, val, false)); break;
        case 3: SetA(AluSub(a, val, cy));    break;
        case 4: SetA(AluAnd(a, val));        break;
        case 5: SetA(AluXor(a, val));        break;
        case 6: SetA(AluOr (a, val));        break;
        case 7: AluCp(a, val);               break;   // no writeback
        }
        int cycles = (src == 6) ? 7 : 4;
        CycleCount += cycles;
        InstructionCount += 1;
        return cycles;
    }

    // ----- 0xC6 / 0xCE / 0xD6 / 0xDE / 0xE6 / 0xEE / 0xF6 / 0xFE -------
    // 8-bit ALU on A,n — pattern 11 ooo 110.
    if ((opcode & 0xC7) == 0xC6) {
        int op = (opcode >> 3) & 0x07;
        uint8_t val = FetchByte();
        bool cy  = (F() & F_C) != 0;
        uint8_t a = A();
        switch (op) {
        case 0: SetA(AluAdd(a, val, false)); break;
        case 1: SetA(AluAdd(a, val, cy));    break;
        case 2: SetA(AluSub(a, val, false)); break;
        case 3: SetA(AluSub(a, val, cy));    break;
        case 4: SetA(AluAnd(a, val));        break;
        case 5: SetA(AluXor(a, val));        break;
        case 6: SetA(AluOr (a, val));        break;
        case 7: AluCp(a, val);               break;
        }
        CycleCount += 7;
        InstructionCount += 1;
        return 7;
    }

    // ----- Control flow ------------------------------------------------

    // 0x18 JR e  (unconditional relative jump)
    if (opcode == 0x18) {
        int8_t e = static_cast<int8_t>(FetchByte());
        PC = static_cast<uint16_t>(PC + e);
        CycleCount += 12; InstructionCount++; return 12;
    }
    // 0x10 DJNZ e (B := B - 1; if B != 0 jump)
    if (opcode == 0x10) {
        int8_t e = static_cast<int8_t>(FetchByte());
        SetB(static_cast<uint8_t>(B() - 1));
        if (B() != 0) {
            PC = static_cast<uint16_t>(PC + e);
            CycleCount += 13; InstructionCount++; return 13;
        }
        CycleCount += 8; InstructionCount++; return 8;
    }
    // 0x20 / 0x28 / 0x30 / 0x38 : JR cc, e — pattern 001 cc 000
    // cc takes only 2 bits here (NZ / Z / NC / C — same as cond 0..3).
    if ((opcode & 0xE7) == 0x20) {
        int cc = (opcode >> 3) & 0x03;
        int8_t e = static_cast<int8_t>(FetchByte());
        if (CheckCond(cc)) {
            PC = static_cast<uint16_t>(PC + e);
            CycleCount += 12; InstructionCount++; return 12;
        }
        CycleCount += 7; InstructionCount++; return 7;
    }

    // 0xC3 JP nn (unconditional)
    if (opcode == 0xC3) {
        PC = FetchWord();
        CycleCount += 10; InstructionCount++; return 10;
    }
    // 0xE9 JP (HL) — PC := HL (no indirect despite the name)
    if (opcode == 0xE9) {
        PC = HL;
        CycleCount += 4; InstructionCount++; return 4;
    }
    // 0xC2/CA/D2/DA/E2/EA/F2/FA : JP cc, nn — pattern 11 ccc 010
    if ((opcode & 0xC7) == 0xC2) {
        int cc = (opcode >> 3) & 0x07;
        uint16_t nn = FetchWord();
        if (CheckCond(cc)) PC = nn;
        CycleCount += 10; InstructionCount++; return 10;
    }

    // 0xCD CALL nn (unconditional)
    if (opcode == 0xCD) {
        uint16_t nn = FetchWord();
        Push16(PC);
        PC = nn;
        CycleCount += 17; InstructionCount++; return 17;
    }
    // 0xC4/CC/D4/DC/E4/EC/F4/FC : CALL cc, nn — pattern 11 ccc 100
    if ((opcode & 0xC7) == 0xC4) {
        int cc = (opcode >> 3) & 0x07;
        uint16_t nn = FetchWord();
        if (CheckCond(cc)) {
            Push16(PC);
            PC = nn;
            CycleCount += 17; InstructionCount++; return 17;
        }
        CycleCount += 10; InstructionCount++; return 10;
    }

    // 0xC9 RET (unconditional)
    if (opcode == 0xC9) {
        PC = Pop16();
        CycleCount += 10; InstructionCount++; return 10;
    }
    // 0xC0/C8/D0/D8/E0/E8/F0/F8 : RET cc — pattern 11 ccc 000
    if ((opcode & 0xC7) == 0xC0) {
        int cc = (opcode >> 3) & 0x07;
        if (CheckCond(cc)) {
            PC = Pop16();
            CycleCount += 11; InstructionCount++; return 11;
        }
        CycleCount += 5; InstructionCount++; return 5;
    }

    // 0xC7/CF/D7/DF/E7/EF/F7/FF : RST nnn (push PC, jump to nnn*8)
    if ((opcode & 0xC7) == 0xC7) {
        int n = (opcode >> 3) & 0x07;
        Push16(PC);
        PC = static_cast<uint16_t>(n * 8);
        CycleCount += 11; InstructionCount++; return 11;
    }

    // PUSH rr — pattern 11 pp 0101 (0xC5/D5/E5/F5). pp: 00 BC 01 DE 10 HL 11 AF.
    if ((opcode & 0xCF) == 0xC5) {
        int pp = (opcode >> 4) & 0x03;
        uint16_t val = 0;
        switch (pp) {
        case 0: val = BC; break;
        case 1: val = DE; break;
        case 2: val = HL; break;
        case 3: val = AF; break;
        }
        Push16(val);
        CycleCount += 11; InstructionCount++; return 11;
    }
    // POP rr — pattern 11 pp 0001 (0xC1/D1/E1/F1).
    if ((opcode & 0xCF) == 0xC1) {
        int pp = (opcode >> 4) & 0x03;
        uint16_t val = Pop16();
        switch (pp) {
        case 0: BC = val; break;
        case 1: DE = val; break;
        case 2: HL = val; break;
        case 3: AF = val; break;
        }
        CycleCount += 10; InstructionCount++; return 10;
    }

    // ----- IN A, (n) / OUT (n), A  (immediate-port I/O) ----------------
    // 0xDB IN  A, (n) : A := port[n] — 11 T
    // 0xD3 OUT (n), A : port[n] := A — 11 T
    // Phase 1.7 wires these straight through to Z80Memory's I/O space,
    // which the UART hooks into at ports 0xFE / 0xFF (see docs/mmio.md).
    if (opcode == 0xDB) {
        uint8_t port = FetchByte();
        SetA(Memory->ReadIo(port));
        CycleCount += 11; InstructionCount++; return 11;
    }
    if (opcode == 0xD3) {
        uint8_t port = FetchByte();
        Memory->WriteIo(port, A());
        CycleCount += 11; InstructionCount++; return 11;
    }

    // ----- unrecognised opcode: rollback PC, surface trap -------------
    PC = opcodePC;
    return 0;
}
