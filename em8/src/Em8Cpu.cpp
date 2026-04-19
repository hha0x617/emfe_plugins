/*
 * Em8Cpu.cpp - EM8 Custom 8-bit CPU Core Implementation
 *
 * Full instruction set: ~70 opcodes via switch dispatch.
 * 6502-style ADC/SBC semantics for ADD/SUB.
 *
 * Copyright (c) 2026 emfe Project
 * SPDX-License-Identifier: MIT
 */
#include "pch.h"
#include "Em8Cpu.h"
#include "Em8Memory.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void Em8Cpu::SetNZ(uint8_t val)
{
    SetFlag(FLAG_Z, val == 0);
    SetFlag(FLAG_N, (val & 0x80) != 0);
}

void Em8Cpu::Push8(uint8_t val)
{
    Memory->Write(0x0100 + SP, val);
    SP--;
}

uint8_t Em8Cpu::Pull8()
{
    SP++;
    return Memory->Read(0x0100 + SP);
}

void Em8Cpu::Push16(uint16_t val)
{
    Push8(static_cast<uint8_t>(val >> 8));   // high byte first
    Push8(static_cast<uint8_t>(val & 0xFF)); // low byte second
}

uint16_t Em8Cpu::Pull16()
{
    uint8_t lo = Pull8();
    uint8_t hi = Pull8();
    return static_cast<uint16_t>((hi << 8) | lo);
}

uint8_t Em8Cpu::ReadByte(uint16_t addr)
{
    return Memory->Read(addr);
}

void Em8Cpu::WriteByte(uint16_t addr, uint8_t val)
{
    Memory->Write(addr, val);
}

uint8_t Em8Cpu::FetchByte()
{
    return ReadByte(PC++);
}

uint16_t Em8Cpu::FetchWord()
{
    uint8_t lo = FetchByte();
    uint8_t hi = FetchByte();
    return static_cast<uint16_t>((hi << 8) | lo);
}

// ---------------------------------------------------------------------------
// Addressing modes
// ---------------------------------------------------------------------------

uint8_t Em8Cpu::AddrZpg()
{
    return FetchByte();
}

uint8_t Em8Cpu::AddrZpgX()
{
    return static_cast<uint8_t>(FetchByte() + X);
}

uint16_t Em8Cpu::AddrAbs()
{
    return FetchWord();
}

uint16_t Em8Cpu::AddrAbsX()
{
    return static_cast<uint16_t>(FetchWord() + X);
}

uint16_t Em8Cpu::AddrAbsY()
{
    return static_cast<uint16_t>(FetchWord() + Y);
}

uint16_t Em8Cpu::AddrInd()
{
    uint8_t zp = FetchByte();
    uint8_t lo = ReadByte(zp);
    uint8_t hi = ReadByte(static_cast<uint8_t>(zp + 1));
    return static_cast<uint16_t>((hi << 8) | lo);
}

// ---------------------------------------------------------------------------
// ALU operations
// ---------------------------------------------------------------------------

void Em8Cpu::DoAdd(uint8_t val)
{
    // ADC: A = A + val + C
    uint16_t sum = static_cast<uint16_t>(A) + val + (GetFlag(FLAG_C) ? 1 : 0);
    // Overflow: sign of result differs from both operands' sign
    bool overflow = (~(A ^ val) & (A ^ static_cast<uint8_t>(sum)) & 0x80) != 0;
    SetFlag(FLAG_C, sum > 0xFF);
    SetFlag(FLAG_V, overflow);
    A = static_cast<uint8_t>(sum);
    SetNZ(A);
}

void Em8Cpu::DoSub(uint8_t val)
{
    // SBC: A = A - val - !C  (same as A + ~val + C)
    uint16_t diff = static_cast<uint16_t>(A) + static_cast<uint8_t>(~val) + (GetFlag(FLAG_C) ? 1 : 0);
    bool overflow = ((A ^ val) & (A ^ static_cast<uint8_t>(diff)) & 0x80) != 0;
    SetFlag(FLAG_C, diff > 0xFF); // No borrow
    SetFlag(FLAG_V, overflow);
    A = static_cast<uint8_t>(diff);
    SetNZ(A);
}

void Em8Cpu::DoCmp(uint8_t reg, uint8_t val)
{
    uint16_t diff = static_cast<uint16_t>(reg) - val;
    SetFlag(FLAG_C, reg >= val);
    SetFlag(FLAG_Z, reg == val);
    SetFlag(FLAG_N, (static_cast<uint8_t>(diff) & 0x80) != 0);
}

// ---------------------------------------------------------------------------
// Reset
// ---------------------------------------------------------------------------

void Em8Cpu::Reset()
{
    A = 0;
    X = 0;
    Y = 0;
    SP = 0xFF;
    FL = FLAG_I; // I=1 on reset
    Halted = false;
    CycleCount = 0;
    InstructionCount = 0;
    NmiPending = false;
    IrqLine = false;
    ShadowStackTop = 0;

    // Load PC from reset vector $FFFC
    if (Memory) {
        uint8_t lo = Memory->Peek(0xFFFC);
        uint8_t hi = Memory->Peek(0xFFFD);
        PC = static_cast<uint16_t>((hi << 8) | lo);
    }
}

// ---------------------------------------------------------------------------
// Interrupt handling
// ---------------------------------------------------------------------------

int Em8Cpu::CheckInterrupts()
{
    if (Halted)
        return 0;

    // NMI has priority
    if (NmiPending) {
        NmiPending = false;
        Push16(PC);
        Push8(FL & ~FLAG_B); // B=0 for hardware interrupt
        SetFlag(FLAG_I, true);
        uint8_t lo = ReadByte(0xFFFA);
        uint8_t hi = ReadByte(0xFFFB);
        PC = static_cast<uint16_t>((hi << 8) | lo);
        return 7;
    }

    // IRQ: only if I flag is clear
    if (IrqLine && !GetFlag(FLAG_I)) {
        Push16(PC);
        Push8(FL & ~FLAG_B); // B=0 for hardware interrupt
        SetFlag(FLAG_I, true);
        uint8_t lo = ReadByte(0xFFFE);
        uint8_t hi = ReadByte(0xFFFF);
        PC = static_cast<uint16_t>((hi << 8) | lo);
        return 7;
    }

    return 0;
}

// ---------------------------------------------------------------------------
// Execute one instruction
// ---------------------------------------------------------------------------

int Em8Cpu::ExecuteOne()
{
    if (Halted)
        return 0;

    uint8_t opcode = FetchByte();
    int cycles = 2; // Default cycle count

    switch (opcode) {

    // =====================================================================
    // ALU: ADD (ADC semantics)
    // =====================================================================
    case 0x10: { // ADD imm
        DoAdd(FetchByte());
        cycles = 2;
        break;
    }
    case 0x11: { // ADD zpg
        uint8_t addr = AddrZpg();
        DoAdd(ReadByte(addr));
        cycles = 3;
        break;
    }
    case 0x12: { // ADD abs
        uint16_t addr = AddrAbs();
        DoAdd(ReadByte(addr));
        cycles = 4;
        break;
    }

    // =====================================================================
    // ALU: SUB (SBC semantics)
    // =====================================================================
    case 0x14: { // SUB imm
        DoSub(FetchByte());
        cycles = 2;
        break;
    }
    case 0x15: { // SUB zpg
        uint8_t addr = AddrZpg();
        DoSub(ReadByte(addr));
        cycles = 3;
        break;
    }
    case 0x16: { // SUB abs
        uint16_t addr = AddrAbs();
        DoSub(ReadByte(addr));
        cycles = 4;
        break;
    }

    // =====================================================================
    // ALU: AND
    // =====================================================================
    case 0x18: { // AND imm
        A &= FetchByte();
        SetNZ(A);
        cycles = 2;
        break;
    }
    case 0x19: { // AND zpg
        uint8_t addr = AddrZpg();
        A &= ReadByte(addr);
        SetNZ(A);
        cycles = 3;
        break;
    }

    // =====================================================================
    // ALU: ORA
    // =====================================================================
    case 0x1C: { // ORA imm
        A |= FetchByte();
        SetNZ(A);
        cycles = 2;
        break;
    }
    case 0x1D: { // ORA zpg
        uint8_t addr = AddrZpg();
        A |= ReadByte(addr);
        SetNZ(A);
        cycles = 3;
        break;
    }

    // =====================================================================
    // ALU: XOR
    // =====================================================================
    case 0x20: { // XOR imm
        A ^= FetchByte();
        SetNZ(A);
        cycles = 2;
        break;
    }
    case 0x21: { // XOR zpg
        uint8_t addr = AddrZpg();
        A ^= ReadByte(addr);
        SetNZ(A);
        cycles = 3;
        break;
    }

    // =====================================================================
    // ALU: CMP/CPX/CPY
    // =====================================================================
    case 0x24: { // CMP imm
        DoCmp(A, FetchByte());
        cycles = 2;
        break;
    }
    case 0x25: { // CMP zpg
        uint8_t addr = AddrZpg();
        DoCmp(A, ReadByte(addr));
        cycles = 3;
        break;
    }
    case 0x26: { // CMP abs
        uint16_t addr = AddrAbs();
        DoCmp(A, ReadByte(addr));
        cycles = 4;
        break;
    }
    case 0x38: { // CPX imm
        DoCmp(X, FetchByte());
        cycles = 2;
        break;
    }
    case 0x3C: { // CPY imm
        DoCmp(Y, FetchByte());
        cycles = 2;
        break;
    }

    // =====================================================================
    // Shifts (accumulator, implied)
    // =====================================================================
    case 0x40: { // ASL A
        bool carry = (A & 0x80) != 0;
        A <<= 1;
        SetFlag(FLAG_C, carry);
        SetNZ(A);
        cycles = 2;
        break;
    }
    case 0x41: { // LSR A
        bool carry = (A & 0x01) != 0;
        A >>= 1;
        SetFlag(FLAG_C, carry);
        SetNZ(A);
        cycles = 2;
        break;
    }
    case 0x42: { // ROL A
        bool oldCarry = GetFlag(FLAG_C);
        bool newCarry = (A & 0x80) != 0;
        A = static_cast<uint8_t>((A << 1) | (oldCarry ? 1 : 0));
        SetFlag(FLAG_C, newCarry);
        SetNZ(A);
        cycles = 2;
        break;
    }
    case 0x43: { // ROR A
        bool oldCarry = GetFlag(FLAG_C);
        bool newCarry = (A & 0x01) != 0;
        A = static_cast<uint8_t>((A >> 1) | (oldCarry ? 0x80 : 0));
        SetFlag(FLAG_C, newCarry);
        SetNZ(A);
        cycles = 2;
        break;
    }

    // =====================================================================
    // Inc/Dec (implied)
    // =====================================================================
    case 0x48: { // INC A
        A++;
        SetNZ(A);
        cycles = 2;
        break;
    }
    case 0x49: { // DEC A
        A--;
        SetNZ(A);
        cycles = 2;
        break;
    }
    case 0x4A: { // INX
        X++;
        SetNZ(X);
        cycles = 2;
        break;
    }
    case 0x4B: { // DEX
        X--;
        SetNZ(X);
        cycles = 2;
        break;
    }
    case 0x4C: { // INY
        Y++;
        SetNZ(Y);
        cycles = 2;
        break;
    }
    case 0x4D: { // DEY
        Y--;
        SetNZ(Y);
        cycles = 2;
        break;
    }

    // =====================================================================
    // Branches (relative)
    // =====================================================================
    case 0x50: { // BEQ rel
        int8_t off = static_cast<int8_t>(FetchByte());
        if (GetFlag(FLAG_Z)) { PC = static_cast<uint16_t>(PC + off); cycles = 3; }
        else cycles = 2;
        break;
    }
    case 0x51: { // BNE rel
        int8_t off = static_cast<int8_t>(FetchByte());
        if (!GetFlag(FLAG_Z)) { PC = static_cast<uint16_t>(PC + off); cycles = 3; }
        else cycles = 2;
        break;
    }
    case 0x52: { // BCS rel
        int8_t off = static_cast<int8_t>(FetchByte());
        if (GetFlag(FLAG_C)) { PC = static_cast<uint16_t>(PC + off); cycles = 3; }
        else cycles = 2;
        break;
    }
    case 0x53: { // BCC rel
        int8_t off = static_cast<int8_t>(FetchByte());
        if (!GetFlag(FLAG_C)) { PC = static_cast<uint16_t>(PC + off); cycles = 3; }
        else cycles = 2;
        break;
    }
    case 0x54: { // BMI rel
        int8_t off = static_cast<int8_t>(FetchByte());
        if (GetFlag(FLAG_N)) { PC = static_cast<uint16_t>(PC + off); cycles = 3; }
        else cycles = 2;
        break;
    }
    case 0x55: { // BPL rel
        int8_t off = static_cast<int8_t>(FetchByte());
        if (!GetFlag(FLAG_N)) { PC = static_cast<uint16_t>(PC + off); cycles = 3; }
        else cycles = 2;
        break;
    }
    case 0x56: { // BVS rel
        int8_t off = static_cast<int8_t>(FetchByte());
        if (GetFlag(FLAG_V)) { PC = static_cast<uint16_t>(PC + off); cycles = 3; }
        else cycles = 2;
        break;
    }
    case 0x57: { // BVC rel
        int8_t off = static_cast<int8_t>(FetchByte());
        if (!GetFlag(FLAG_V)) { PC = static_cast<uint16_t>(PC + off); cycles = 3; }
        else cycles = 2;
        break;
    }
    case 0x58: { // BRA rel (unconditional)
        int8_t off = static_cast<int8_t>(FetchByte());
        PC = static_cast<uint16_t>(PC + off);
        cycles = 3;
        break;
    }

    // =====================================================================
    // Jump/Call
    // =====================================================================
    case 0x60: { // JMP abs
        PC = FetchWord();
        cycles = 3;
        break;
    }
    case 0x61: { // JSR abs
        uint16_t target = FetchWord();
        // Push return address (PC-1 = last byte of JSR instruction)
        Push16(static_cast<uint16_t>(PC - 1));
        // Shadow stack
        if (ShadowStackTop < MaxShadowStack) {
            ShadowStack[ShadowStackTop++] = PC; // Return address (actual)
        }
        PC = target;
        cycles = 6;
        break;
    }
    case 0x62: { // RTS
        PC = static_cast<uint16_t>(Pull16() + 1);
        if (ShadowStackTop > 0) {
            ShadowStackTop--;
        }
        cycles = 6;
        break;
    }
    case 0x63: { // RTI
        FL = Pull8();
        PC = Pull16();
        cycles = 7;
        break;
    }

    // =====================================================================
    // Stack
    // =====================================================================
    case 0x70: { // PHA
        Push8(A);
        cycles = 3;
        break;
    }
    case 0x71: { // PLA
        A = Pull8();
        SetNZ(A);
        cycles = 4;
        break;
    }
    case 0x72: { // PHF (push flags)
        Push8(FL);
        cycles = 3;
        break;
    }
    case 0x73: { // PLF (pull flags)
        FL = Pull8();
        cycles = 4;
        break;
    }

    // =====================================================================
    // Transfer
    // =====================================================================
    case 0x80: { // TAX
        X = A;
        SetNZ(X);
        cycles = 2;
        break;
    }
    case 0x81: { // TXA
        A = X;
        SetNZ(A);
        cycles = 2;
        break;
    }
    case 0x82: { // TAY
        Y = A;
        SetNZ(Y);
        cycles = 2;
        break;
    }
    case 0x83: { // TYA
        A = Y;
        SetNZ(A);
        cycles = 2;
        break;
    }
    case 0x84: { // TXS
        SP = X;
        // TXS does not set flags (same as 6502)
        cycles = 2;
        break;
    }
    case 0x85: { // TSX
        X = SP;
        SetNZ(X);
        cycles = 2;
        break;
    }

    // =====================================================================
    // System
    // =====================================================================
    case 0x90: { // CLC
        SetFlag(FLAG_C, false);
        cycles = 2;
        break;
    }
    case 0x91: { // SEC
        SetFlag(FLAG_C, true);
        cycles = 2;
        break;
    }
    case 0x92: { // CLI
        SetFlag(FLAG_I, false);
        cycles = 2;
        break;
    }
    case 0x93: { // SEI
        SetFlag(FLAG_I, true);
        cycles = 2;
        break;
    }
    case 0x94: { // CLV
        SetFlag(FLAG_V, false);
        cycles = 2;
        break;
    }
    case 0x98: { // NOP
        cycles = 2;
        break;
    }
    case 0x99: { // BRK
        PC++; // BRK skips one byte (signature byte)
        Push16(PC);
        Push8(FL | FLAG_B); // B=1 for BRK
        SetFlag(FLAG_I, true);
        uint8_t lo = ReadByte(0xFFFE);
        uint8_t hi = ReadByte(0xFFFF);
        PC = static_cast<uint16_t>((hi << 8) | lo);
        cycles = 7;
        break;
    }
    case 0x9A: { // HLT
        Halted = true;
        cycles = 2;
        break;
    }

    // =====================================================================
    // Load/Store
    // =====================================================================

    // --- LDA ---
    case 0xA0: { // LDA imm
        A = FetchByte();
        SetNZ(A);
        cycles = 2;
        break;
    }
    case 0xA1: { // LDA zpg
        uint8_t addr = AddrZpg();
        A = ReadByte(addr);
        SetNZ(A);
        cycles = 3;
        break;
    }
    case 0xA2: { // LDA abs
        uint16_t addr = AddrAbs();
        A = ReadByte(addr);
        SetNZ(A);
        cycles = 4;
        break;
    }
    case 0xA3: { // LDA zpg,X
        uint8_t addr = AddrZpgX();
        A = ReadByte(addr);
        SetNZ(A);
        cycles = 4;
        break;
    }
    case 0xA4: { // LDA abs,X
        uint16_t addr = AddrAbsX();
        A = ReadByte(addr);
        SetNZ(A);
        cycles = 4;
        break;
    }
    case 0xA5: { // LDA (ind)
        uint16_t addr = AddrInd();
        A = ReadByte(addr);
        SetNZ(A);
        cycles = 5;
        break;
    }

    // --- LDX ---
    case 0xA6: { // LDX imm
        X = FetchByte();
        SetNZ(X);
        cycles = 2;
        break;
    }
    case 0xA7: { // LDX zpg
        uint8_t addr = AddrZpg();
        X = ReadByte(addr);
        SetNZ(X);
        cycles = 3;
        break;
    }
    case 0xA8: { // LDX abs
        uint16_t addr = AddrAbs();
        X = ReadByte(addr);
        SetNZ(X);
        cycles = 4;
        break;
    }

    // --- LDY ---
    case 0xA9: { // LDY imm
        Y = FetchByte();
        SetNZ(Y);
        cycles = 2;
        break;
    }
    case 0xAA: { // LDY zpg
        uint8_t addr = AddrZpg();
        Y = ReadByte(addr);
        SetNZ(Y);
        cycles = 3;
        break;
    }
    case 0xAB: { // LDY abs
        uint16_t addr = AddrAbs();
        Y = ReadByte(addr);
        SetNZ(Y);
        cycles = 4;
        break;
    }

    // --- STA ---
    case 0xAC: { // STA zpg
        uint8_t addr = AddrZpg();
        WriteByte(addr, A);
        cycles = 3;
        break;
    }
    case 0xAD: { // STA abs
        uint16_t addr = AddrAbs();
        WriteByte(addr, A);
        cycles = 4;
        break;
    }
    case 0xAE: { // STA zpg,X
        uint8_t addr = AddrZpgX();
        WriteByte(addr, A);
        cycles = 4;
        break;
    }
    case 0xAF: { // STA abs,X
        uint16_t addr = AddrAbsX();
        WriteByte(addr, A);
        cycles = 5;
        break;
    }
    case 0xB0: { // STA (ind)
        uint16_t addr = AddrInd();
        WriteByte(addr, A);
        cycles = 6;
        break;
    }

    // --- STX ---
    case 0xB1: { // STX zpg
        uint8_t addr = AddrZpg();
        WriteByte(addr, X);
        cycles = 3;
        break;
    }
    case 0xB2: { // STX abs
        uint16_t addr = AddrAbs();
        WriteByte(addr, X);
        cycles = 4;
        break;
    }

    // --- STY ---
    case 0xB3: { // STY zpg
        uint8_t addr = AddrZpg();
        WriteByte(addr, Y);
        cycles = 3;
        break;
    }
    case 0xB4: { // STY abs
        uint16_t addr = AddrAbs();
        WriteByte(addr, Y);
        cycles = 4;
        break;
    }

    // =====================================================================
    // Undefined opcode
    // =====================================================================
    default:
        Halted = true;
        PC--; // Point back at the undefined opcode
        cycles = 2;
        break;
    }

    CycleCount += cycles;
    InstructionCount++;
    return cycles;
}

// ---------------------------------------------------------------------------
// Disassembler
// ---------------------------------------------------------------------------

Em8Cpu::DisasmResult Em8Cpu::Disassemble(uint16_t addr) const
{
    DisasmResult result;
    uint8_t op = Memory->Peek(addr);

    auto peekByte = [&](uint16_t a) -> uint8_t { return Memory->Peek(a); };
    auto peekWord = [&](uint16_t a) -> uint16_t {
        return static_cast<uint16_t>(peekByte(a) | (peekByte(static_cast<uint16_t>(a + 1)) << 8));
    };

    auto hexByte = [](uint8_t v) -> std::string {
        return std::format("{:02X}", v);
    };
    auto hexWord = [](uint16_t v) -> std::string {
        return std::format("{:04X}", v);
    };

    // Format branch target: PC + 2 + signed offset
    auto branchTarget = [&](uint16_t base) -> std::string {
        int8_t off = static_cast<int8_t>(peekByte(static_cast<uint16_t>(base + 1)));
        uint16_t target = static_cast<uint16_t>(base + 2 + off);
        return "$" + hexWord(target);
    };

    // Helper: immediate operand
    auto imm8 = [&]() -> std::string {
        return "#$" + hexByte(peekByte(static_cast<uint16_t>(addr + 1)));
    };
    // Helper: zero page operand
    auto zpg = [&]() -> std::string {
        return "$" + hexByte(peekByte(static_cast<uint16_t>(addr + 1)));
    };
    // Helper: zero page,X operand
    auto zpgX = [&]() -> std::string {
        return "$" + hexByte(peekByte(static_cast<uint16_t>(addr + 1))) + ",X";
    };
    // Helper: absolute operand
    auto abs16 = [&]() -> std::string {
        return "$" + hexWord(peekWord(static_cast<uint16_t>(addr + 1)));
    };
    // Helper: absolute,X operand
    auto absX = [&]() -> std::string {
        return "$" + hexWord(peekWord(static_cast<uint16_t>(addr + 1))) + ",X";
    };
    // Helper: absolute,Y operand
    auto absY = [&]() -> std::string {
        return "$" + hexWord(peekWord(static_cast<uint16_t>(addr + 1))) + ",Y";
    };
    // Helper: indirect operand
    auto ind = [&]() -> std::string {
        return "($" + hexByte(peekByte(static_cast<uint16_t>(addr + 1))) + ")";
    };

    // Raw bytes builder
    auto raw1 = [&]() -> std::string { return hexByte(op); };
    auto raw2 = [&]() -> std::string {
        return hexByte(op) + " " + hexByte(peekByte(static_cast<uint16_t>(addr + 1)));
    };
    auto raw3 = [&]() -> std::string {
        return hexByte(op) + " " + hexByte(peekByte(static_cast<uint16_t>(addr + 1)))
            + " " + hexByte(peekByte(static_cast<uint16_t>(addr + 2)));
    };

    switch (op) {
    // ALU: ADD
    case 0x10: result = {"ADD", imm8(), raw2(), 2}; break;
    case 0x11: result = {"ADD", zpg(), raw2(), 2}; break;
    case 0x12: result = {"ADD", abs16(), raw3(), 3}; break;

    // ALU: SUB
    case 0x14: result = {"SUB", imm8(), raw2(), 2}; break;
    case 0x15: result = {"SUB", zpg(), raw2(), 2}; break;
    case 0x16: result = {"SUB", abs16(), raw3(), 3}; break;

    // ALU: AND
    case 0x18: result = {"AND", imm8(), raw2(), 2}; break;
    case 0x19: result = {"AND", zpg(), raw2(), 2}; break;

    // ALU: ORA
    case 0x1C: result = {"ORA", imm8(), raw2(), 2}; break;
    case 0x1D: result = {"ORA", zpg(), raw2(), 2}; break;

    // ALU: XOR
    case 0x20: result = {"XOR", imm8(), raw2(), 2}; break;
    case 0x21: result = {"XOR", zpg(), raw2(), 2}; break;

    // ALU: CMP
    case 0x24: result = {"CMP", imm8(), raw2(), 2}; break;
    case 0x25: result = {"CMP", zpg(), raw2(), 2}; break;
    case 0x26: result = {"CMP", abs16(), raw3(), 3}; break;

    // CPX/CPY
    case 0x38: result = {"CPX", imm8(), raw2(), 2}; break;
    case 0x3C: result = {"CPY", imm8(), raw2(), 2}; break;

    // Shifts
    case 0x40: result = {"ASL", "A", raw1(), 1}; break;
    case 0x41: result = {"LSR", "A", raw1(), 1}; break;
    case 0x42: result = {"ROL", "A", raw1(), 1}; break;
    case 0x43: result = {"ROR", "A", raw1(), 1}; break;

    // Inc/Dec
    case 0x48: result = {"INC", "A", raw1(), 1}; break;
    case 0x49: result = {"DEC", "A", raw1(), 1}; break;
    case 0x4A: result = {"INX", "", raw1(), 1}; break;
    case 0x4B: result = {"DEX", "", raw1(), 1}; break;
    case 0x4C: result = {"INY", "", raw1(), 1}; break;
    case 0x4D: result = {"DEY", "", raw1(), 1}; break;

    // Branches
    case 0x50: result = {"BEQ", branchTarget(addr), raw2(), 2}; break;
    case 0x51: result = {"BNE", branchTarget(addr), raw2(), 2}; break;
    case 0x52: result = {"BCS", branchTarget(addr), raw2(), 2}; break;
    case 0x53: result = {"BCC", branchTarget(addr), raw2(), 2}; break;
    case 0x54: result = {"BMI", branchTarget(addr), raw2(), 2}; break;
    case 0x55: result = {"BPL", branchTarget(addr), raw2(), 2}; break;
    case 0x56: result = {"BVS", branchTarget(addr), raw2(), 2}; break;
    case 0x57: result = {"BVC", branchTarget(addr), raw2(), 2}; break;
    case 0x58: result = {"BRA", branchTarget(addr), raw2(), 2}; break;

    // Jump/Call
    case 0x60: result = {"JMP", abs16(), raw3(), 3}; break;
    case 0x61: result = {"JSR", abs16(), raw3(), 3}; break;
    case 0x62: result = {"RTS", "", raw1(), 1}; break;
    case 0x63: result = {"RTI", "", raw1(), 1}; break;

    // Stack
    case 0x70: result = {"PHA", "", raw1(), 1}; break;
    case 0x71: result = {"PLA", "", raw1(), 1}; break;
    case 0x72: result = {"PHF", "", raw1(), 1}; break;
    case 0x73: result = {"PLF", "", raw1(), 1}; break;

    // Transfer
    case 0x80: result = {"TAX", "", raw1(), 1}; break;
    case 0x81: result = {"TXA", "", raw1(), 1}; break;
    case 0x82: result = {"TAY", "", raw1(), 1}; break;
    case 0x83: result = {"TYA", "", raw1(), 1}; break;
    case 0x84: result = {"TXS", "", raw1(), 1}; break;
    case 0x85: result = {"TSX", "", raw1(), 1}; break;

    // System
    case 0x90: result = {"CLC", "", raw1(), 1}; break;
    case 0x91: result = {"SEC", "", raw1(), 1}; break;
    case 0x92: result = {"CLI", "", raw1(), 1}; break;
    case 0x93: result = {"SEI", "", raw1(), 1}; break;
    case 0x94: result = {"CLV", "", raw1(), 1}; break;
    case 0x98: result = {"NOP", "", raw1(), 1}; break;
    case 0x99: result = {"BRK", "", raw1(), 1}; break;
    case 0x9A: result = {"HLT", "", raw1(), 1}; break;

    // Load: LDA
    case 0xA0: result = {"LDA", imm8(), raw2(), 2}; break;
    case 0xA1: result = {"LDA", zpg(), raw2(), 2}; break;
    case 0xA2: result = {"LDA", abs16(), raw3(), 3}; break;
    case 0xA3: result = {"LDA", zpgX(), raw2(), 2}; break;
    case 0xA4: result = {"LDA", absX(), raw3(), 3}; break;
    case 0xA5: result = {"LDA", ind(), raw2(), 2}; break;

    // Load: LDX
    case 0xA6: result = {"LDX", imm8(), raw2(), 2}; break;
    case 0xA7: result = {"LDX", zpg(), raw2(), 2}; break;
    case 0xA8: result = {"LDX", abs16(), raw3(), 3}; break;

    // Load: LDY
    case 0xA9: result = {"LDY", imm8(), raw2(), 2}; break;
    case 0xAA: result = {"LDY", zpg(), raw2(), 2}; break;
    case 0xAB: result = {"LDY", abs16(), raw3(), 3}; break;

    // Store: STA
    case 0xAC: result = {"STA", zpg(), raw2(), 2}; break;
    case 0xAD: result = {"STA", abs16(), raw3(), 3}; break;
    case 0xAE: result = {"STA", zpgX(), raw2(), 2}; break;
    case 0xAF: result = {"STA", absX(), raw3(), 3}; break;
    case 0xB0: result = {"STA", ind(), raw2(), 2}; break;

    // Store: STX
    case 0xB1: result = {"STX", zpg(), raw2(), 2}; break;
    case 0xB2: result = {"STX", abs16(), raw3(), 3}; break;

    // Store: STY
    case 0xB3: result = {"STY", zpg(), raw2(), 2}; break;
    case 0xB4: result = {"STY", abs16(), raw3(), 3}; break;

    default:
        result = {"???", std::format("${:02X}", op), raw1(), 1};
        break;
    }

    return result;
}
