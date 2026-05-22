// SPDX-License-Identifier: MIT OR Apache-2.0
// Copyright (c) 2026 hha0x617
//
// Z80Cpu.cpp - Z80 CPU core.
//
// Phase 1.5: NOP + LD family + 8-bit ALU (ADD/ADC/SUB/SBC/AND/OR/XOR/CP)
// against A,r / A,(HL) / A,n. Control flow lands in Phase 1.6, prefix
// opcodes (CB / DD / ED / FD) and HALT in Phase 2.

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

    // ----- unrecognised opcode: rollback PC, surface trap -------------
    PC = opcodePC;
    return 0;
}
