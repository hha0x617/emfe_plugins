/*
 * Z8000Cpu.cpp - Zilog Z8000 Family CPU Core Implementation
 *
 * Phase 1 scope: Z8002 (non-segmented, no VM abort) only. Variant-specific
 * paths for Z8001/Z8003/Z8004 are stubbed and will be filled in Phase 2/3.
 *
 * Copyright (c) 2026 emfe Project
 * SPDX-License-Identifier: MIT
 */
#include "pch.h"
#include "Z8000Cpu.h"
#include "Z8000Memory.h"

// ============================================================================
// Reset
// ============================================================================

void Z8000Cpu::Reset() {
    for (auto& r : R) r = 0;
    ShadowR14 = ShadowR15 = 0;
    PC = 0;
    PCSegment = 0;
    // On reset: System mode, interrupts disabled.
    FCW = FCW_SN;
    if (m_segmented) FCW |= FCW_SEG;
    PSAPOffset = 0;
    PSAPSegment = 0;
    Refresh = 0;

    Halted = false;
    CycleCount = 0;
    InstructionCount = 0;

    NmiPending = false;
    NviLine    = false;
    ViLine     = false;
    ViVector   = 0;

    ShadowStackTop = 0;

    // Reset vector: Z8002 reads FCW from $0002 and PC from $0004 (PSA offsets).
    // If memory is wired and contains a vector, load it; otherwise leave PC=0.
    if (Memory) {
        FCW = Memory->PeekWord(0x0002);
        PC  = Memory->PeekWord(0x0004);
        // Force system mode on reset regardless of vector content.
        FCW |= FCW_SN;
        if (m_segmented) FCW |= FCW_SEG;
    }
}

// ============================================================================
// Stack-bank exchange (invoked on S/N transitions)
// ============================================================================

void Z8000Cpu::SwapStackBanks() {
    std::swap(R[14], ShadowR14);
    std::swap(R[15], ShadowR15);
}

// ============================================================================
// Instruction fetch / stack helpers
// ============================================================================

uint16_t Z8000Cpu::FetchWord() {
    uint16_t w = Memory ? Memory->ReadWord(PC) : 0;
    PC = static_cast<uint16_t>(PC + 2);
    return w;
}

void Z8000Cpu::Push16(uint16_t val) {
    R[15] = static_cast<uint16_t>(R[15] - 2);
    if (Memory) Memory->WriteWord(R[15], val);
}

uint16_t Z8000Cpu::Pop16() {
    uint16_t v = Memory ? Memory->ReadWord(R[15]) : 0;
    R[15] = static_cast<uint16_t>(R[15] + 2);
    return v;
}

void Z8000Cpu::Push32(uint32_t val) {
    // Z8000 pushes high word first, so low word ends up at lower address.
    Push16(static_cast<uint16_t>(val & 0xFFFF));
    Push16(static_cast<uint16_t>(val >> 16));
}

uint32_t Z8000Cpu::Pop32() {
    uint16_t hi = Pop16();
    uint16_t lo = Pop16();
    return (static_cast<uint32_t>(hi) << 16) | lo;
}

// ============================================================================
// Flag updates
// ============================================================================

static bool ParityEven8(uint8_t v) {
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return (v & 1) == 0;
}

static bool ParityEven16(uint16_t v) {
    v ^= v >> 8;
    return ParityEven8(static_cast<uint8_t>(v));
}

void Z8000Cpu::UpdateFlagsByte(uint8_t result) {
    SetFlag(FCW_Z, result == 0);
    SetFlag(FCW_S, (result & 0x80) != 0);
    SetFlag(FCW_PV, ParityEven8(result));  // Logical ops: PV = parity
}

void Z8000Cpu::UpdateFlagsWord(uint16_t result) {
    SetFlag(FCW_Z, result == 0);
    SetFlag(FCW_S, (result & 0x8000) != 0);
    SetFlag(FCW_PV, ParityEven16(result));
}

void Z8000Cpu::UpdateFlagsLong(uint32_t result) {
    SetFlag(FCW_Z, result == 0);
    SetFlag(FCW_S, (result & 0x80000000u) != 0);
    // P/V is undefined for long ops on Z8000 (ADDL/SUBL set V based on overflow).
}

// ============================================================================
// Arithmetic primitives
// ============================================================================

uint8_t Z8000Cpu::DoAddByte(uint8_t a, uint8_t b, bool withCarry) {
    uint16_t c = withCarry && GetFlag(FCW_C) ? 1 : 0;
    uint16_t r = static_cast<uint16_t>(a) + b + c;
    uint8_t res = static_cast<uint8_t>(r);
    SetFlag(FCW_Z, res == 0);
    SetFlag(FCW_S, (res & 0x80) != 0);
    SetFlag(FCW_C, r > 0xFF);
    bool signedOv = ((~(a ^ b) & (a ^ res)) & 0x80) != 0;
    SetFlag(FCW_PV, signedOv);
    SetFlag(FCW_H, (((a & 0x0F) + (b & 0x0F) + c) & 0x10) != 0);
    SetFlag(FCW_DA, false); // DA=0 means last op was addition
    return res;
}

uint16_t Z8000Cpu::DoAddWord(uint16_t a, uint16_t b, bool withCarry) {
    uint32_t c = withCarry && GetFlag(FCW_C) ? 1 : 0;
    uint32_t r = static_cast<uint32_t>(a) + b + c;
    uint16_t res = static_cast<uint16_t>(r);
    SetFlag(FCW_Z, res == 0);
    SetFlag(FCW_S, (res & 0x8000) != 0);
    SetFlag(FCW_C, r > 0xFFFF);
    bool signedOv = ((~(a ^ b) & (a ^ res)) & 0x8000) != 0;
    SetFlag(FCW_PV, signedOv);
    SetFlag(FCW_H, (((a & 0x0FFF) + (b & 0x0FFF) + c) & 0x1000) != 0);
    SetFlag(FCW_DA, false);
    return res;
}

uint8_t Z8000Cpu::DoSubByte(uint8_t a, uint8_t b, bool withCarry) {
    uint16_t c = withCarry && GetFlag(FCW_C) ? 1 : 0;
    uint16_t r = static_cast<uint16_t>(a) - b - c;
    uint8_t res = static_cast<uint8_t>(r);
    SetFlag(FCW_Z, res == 0);
    SetFlag(FCW_S, (res & 0x80) != 0);
    SetFlag(FCW_C, (r & 0x100) != 0); // borrow
    bool signedOv = (((a ^ b) & (a ^ res)) & 0x80) != 0;
    SetFlag(FCW_PV, signedOv);
    SetFlag(FCW_H, (((a & 0x0F) - (b & 0x0F) - c) & 0x10) != 0);
    SetFlag(FCW_DA, true); // DA=1 means last op was subtraction
    return res;
}

uint16_t Z8000Cpu::DoSubWord(uint16_t a, uint16_t b, bool withCarry) {
    uint32_t c = withCarry && GetFlag(FCW_C) ? 1 : 0;
    uint32_t r = static_cast<uint32_t>(a) - b - c;
    uint16_t res = static_cast<uint16_t>(r);
    SetFlag(FCW_Z, res == 0);
    SetFlag(FCW_S, (res & 0x8000) != 0);
    SetFlag(FCW_C, (r & 0x10000u) != 0);
    bool signedOv = (((a ^ b) & (a ^ res)) & 0x8000) != 0;
    SetFlag(FCW_PV, signedOv);
    SetFlag(FCW_H, (((a & 0x0FFF) - (b & 0x0FFF) - c) & 0x1000) != 0);
    SetFlag(FCW_DA, true);
    return res;
}

// ============================================================================
// Exception/trap entry
// ============================================================================

void Z8000Cpu::TakeException(uint16_t vectorOffset, bool pushIdentifier, uint16_t identifier) {
    // Switch to System mode before pushing (pushes go to SSP).
    bool wasSystem = IsSystem();
    if (!wasSystem) {
        SwapStackBanks();
        FCW |= FCW_SN;
    }

    if (pushIdentifier) Push16(identifier);
    Push16(PC);
    Push16(FCW);

    if (Memory) {
        FCW = Memory->ReadWord(static_cast<uint16_t>(PSAPOffset + vectorOffset));
        PC  = Memory->ReadWord(static_cast<uint16_t>(PSAPOffset + vectorOffset + 2));
    }
    Halted = false;
}

// ============================================================================
// Condition code evaluation (Z8000 4-bit cc field)
// ============================================================================

bool Z8000Cpu_TestCondition(const Z8000Cpu& cpu, uint8_t cc) {
    bool Z = cpu.GetFlag(Z8000Cpu::FCW_Z);
    bool C = cpu.GetFlag(Z8000Cpu::FCW_C);
    bool S = cpu.GetFlag(Z8000Cpu::FCW_S);
    bool PV = cpu.GetFlag(Z8000Cpu::FCW_PV);
    switch (cc & 0x0F) {
    case 0x0: return false;              // F (false / never)
    case 0x1: return PV != S;            // LT  (S != V — signed less than)
    case 0x2: return Z || (PV != S);     // LE
    case 0x3: return C || Z;             // ULE (unsigned less or equal)
    case 0x4: return PV;                 // OV / PE
    case 0x5: return S;                  // MI
    case 0x6: return Z;                  // Z / EQ
    case 0x7: return C;                  // C / ULT
    case 0x8: return true;               // T / always
    case 0x9: return PV == S;            // GE
    case 0xA: return !Z && (PV == S);    // GT
    case 0xB: return !C && !Z;           // UGT
    case 0xC: return !PV;                // NOV / PO
    case 0xD: return !S;                 // PL
    case 0xE: return !Z;                 // NZ / NE
    case 0xF: return !C;                 // NC / UGE
    }
    return false;
}

// ============================================================================
// Operand decoding helpers (first-word format for reg-reg ALU / LD)
// Layout: [opcode:8][src:4][dst:4]
// ============================================================================

static inline int OperandSrc(uint16_t op) { return (op >> 4) & 0x0F; }
static inline int OperandDst(uint16_t op) { return op & 0x0F; }

// ============================================================================
// Instruction handlers (Phase 1a)
//
// Encodings are taken from the Zilog Z8000 CPU Technical Manual
// (1980/1982 revisions). Register-to-register ALU ops all use the
// [op:8][src:4][dst:4] layout. Byte variants operate on RH0..RL7 (byte
// register numbers 0..15); word variants operate on R0..R15.
// ============================================================================

// --- Word ALU (register to register) ---
static int Exec_ADD_r_r(Z8000Cpu& c, uint16_t op) {
    int s = OperandSrc(op), d = OperandDst(op);
    c.R[d] = c.DoAddWord(c.R[d], c.R[s], false);
    return 4;
}
static int Exec_SUB_r_r(Z8000Cpu& c, uint16_t op) {
    int s = OperandSrc(op), d = OperandDst(op);
    c.R[d] = c.DoSubWord(c.R[d], c.R[s], false);
    return 4;
}
static int Exec_OR_r_r(Z8000Cpu& c, uint16_t op) {
    int s = OperandSrc(op), d = OperandDst(op);
    c.R[d] = c.R[d] | c.R[s];
    c.UpdateFlagsWord(c.R[d]);
    c.SetFlag(Z8000Cpu::FCW_C, false);
    return 4;
}
static int Exec_AND_r_r(Z8000Cpu& c, uint16_t op) {
    int s = OperandSrc(op), d = OperandDst(op);
    c.R[d] = c.R[d] & c.R[s];
    c.UpdateFlagsWord(c.R[d]);
    c.SetFlag(Z8000Cpu::FCW_C, false);
    return 4;
}
static int Exec_XOR_r_r(Z8000Cpu& c, uint16_t op) {
    int s = OperandSrc(op), d = OperandDst(op);
    c.R[d] = c.R[d] ^ c.R[s];
    c.UpdateFlagsWord(c.R[d]);
    c.SetFlag(Z8000Cpu::FCW_C, false);
    return 4;
}
static int Exec_CP_r_r(Z8000Cpu& c, uint16_t op) {
    int s = OperandSrc(op), d = OperandDst(op);
    (void)c.DoSubWord(c.R[d], c.R[s], false); // flags only, discard result
    return 4;
}
static int Exec_LD_r_r(Z8000Cpu& c, uint16_t op) {
    int s = OperandSrc(op), d = OperandDst(op);
    c.R[d] = c.R[s];
    return 3;
}

// --- Byte ALU (register to register) ---
static int Exec_ADDB_r_r(Z8000Cpu& c, uint16_t op) {
    int s = OperandSrc(op), d = OperandDst(op);
    c.SetByteReg(d, c.DoAddByte(c.GetByteReg(d), c.GetByteReg(s), false));
    return 4;
}
static int Exec_SUBB_r_r(Z8000Cpu& c, uint16_t op) {
    int s = OperandSrc(op), d = OperandDst(op);
    c.SetByteReg(d, c.DoSubByte(c.GetByteReg(d), c.GetByteReg(s), false));
    return 4;
}
static int Exec_ORB_r_r(Z8000Cpu& c, uint16_t op) {
    int s = OperandSrc(op), d = OperandDst(op);
    uint8_t r = c.GetByteReg(d) | c.GetByteReg(s);
    c.SetByteReg(d, r);
    c.UpdateFlagsByte(r);
    c.SetFlag(Z8000Cpu::FCW_C, false);
    return 4;
}
static int Exec_ANDB_r_r(Z8000Cpu& c, uint16_t op) {
    int s = OperandSrc(op), d = OperandDst(op);
    uint8_t r = c.GetByteReg(d) & c.GetByteReg(s);
    c.SetByteReg(d, r);
    c.UpdateFlagsByte(r);
    c.SetFlag(Z8000Cpu::FCW_C, false);
    return 4;
}
static int Exec_XORB_r_r(Z8000Cpu& c, uint16_t op) {
    int s = OperandSrc(op), d = OperandDst(op);
    uint8_t r = c.GetByteReg(d) ^ c.GetByteReg(s);
    c.SetByteReg(d, r);
    c.UpdateFlagsByte(r);
    c.SetFlag(Z8000Cpu::FCW_C, false);
    return 4;
}
static int Exec_CPB_r_r(Z8000Cpu& c, uint16_t op) {
    int s = OperandSrc(op), d = OperandDst(op);
    (void)c.DoSubByte(c.GetByteReg(d), c.GetByteReg(s), false);
    return 4;
}
static int Exec_LDB_r_r(Z8000Cpu& c, uint16_t op) {
    int s = OperandSrc(op), d = OperandDst(op);
    c.SetByteReg(d, c.GetByteReg(s));
    return 3;
}

// --- Byte immediate: LDB Rbd,#data  encoding: 1100 dddd nnnn nnnn = $Cdnn ---
static int Exec_LDB_r_imm(Z8000Cpu& c, uint16_t op) {
    int d = (op >> 8) & 0x0F;
    uint8_t imm = static_cast<uint8_t>(op & 0xFF);
    c.SetByteReg(d, imm);
    return 5;
}

// --- Word immediate: LD Rd,#data  encoding: 0010 0001 0000 dddd + imm16 = $210d imm16 ---
static int Exec_LD_r_imm(Z8000Cpu& c, uint16_t op) {
    int d = op & 0x0F;
    uint16_t imm = c.FetchWord();
    c.R[d] = imm;
    return 7;
}

// --- JR cc,disp  encoding: 1110 cccc dddd dddd = $Ecdd (disp is signed 8-bit word count) ---
static int Exec_JR(Z8000Cpu& c, uint16_t op) {
    uint8_t cc = (op >> 8) & 0x0F;
    int8_t disp = static_cast<int8_t>(op & 0xFF);
    if (Z8000Cpu_TestCondition(c, cc)) {
        c.PC = static_cast<uint16_t>(c.PC + static_cast<int16_t>(disp) * 2);
        return 6;
    }
    return 6;
}

// --- HALT encoding: 0111 1010 0000 0000 = $7A00 (privileged) ---
static int Exec_HALT(Z8000Cpu& c, uint16_t /*op*/) {
    if (!c.IsSystem()) {
        c.TakeException(0x10, true, 0x7A00); // privileged instruction trap
        return 8;
    }
    c.Halted = true;
    return 8;
}

// ============================================================================
// Phase 1b handlers — memory access, stack, subroutines, I/O, control
// Encodings verified against MAME src/devices/cpu/z8000/ op table.
// ============================================================================

// --- Memory load via register indirect: LDB Rbd,@Rs = $20[s][d], LD = $21[s][d] ---
static int Exec_LDB_r_ind(Z8000Cpu& c, uint16_t op) {
    int s = OperandSrc(op), d = OperandDst(op);
    if (s == 0 || !c.Memory) return 7;  // invalid pointer; MAME also gates on s≠0
    c.SetByteReg(d, c.Memory->ReadByte(c.R[s]));
    return 7;
}
static int Exec_LD_r_ind(Z8000Cpu& c, uint16_t op) {
    int s = OperandSrc(op), d = OperandDst(op);
    if (s == 0 || !c.Memory) return 7;
    c.R[d] = c.Memory->ReadWord(c.R[s]);
    return 7;
}

// --- Memory store via register indirect: LDB @Rd,Rbs = $2E[d][s], LD = $2F[d][s] ---
static int Exec_LDB_ind_r(Z8000Cpu& c, uint16_t op) {
    int d = (op >> 4) & 0x0F;
    int s = op & 0x0F;
    if (d == 0 || !c.Memory) return 8;
    c.Memory->WriteByte(c.R[d], c.GetByteReg(s));
    return 8;
}
static int Exec_LD_ind_r(Z8000Cpu& c, uint16_t op) {
    int d = (op >> 4) & 0x0F;
    int s = op & 0x0F;
    if (d == 0 || !c.Memory) return 8;
    c.Memory->WriteWord(c.R[d], c.R[s]);
    return 8;
}

// --- Memory load from direct address ---
// LDB Rbd,addr = $70[0][d] addr16  ;  LD Rd,addr = $71[0][d] addr16
static int Exec_LDB_r_dir(Z8000Cpu& c, uint16_t op) {
    int d = op & 0x0F;
    uint16_t addr = c.FetchWord();
    if (c.Memory) c.SetByteReg(d, c.Memory->ReadByte(addr));
    return 9;
}
static int Exec_LD_r_dir(Z8000Cpu& c, uint16_t op) {
    int d = op & 0x0F;
    uint16_t addr = c.FetchWord();
    if (c.Memory) c.R[d] = c.Memory->ReadWord(addr);
    return 9;
}

// --- Memory store to direct address ---
// LDB addr,Rbs = $78[0][s] addr16  ;  LD addr,Rs = $79[0][s] addr16
static int Exec_LDB_dir_r(Z8000Cpu& c, uint16_t op) {
    int s = op & 0x0F;
    uint16_t addr = c.FetchWord();
    if (c.Memory) c.Memory->WriteByte(addr, c.GetByteReg(s));
    return 11;
}
static int Exec_LD_dir_r(Z8000Cpu& c, uint16_t op) {
    int s = op & 0x0F;
    uint16_t addr = c.FetchWord();
    if (c.Memory) c.Memory->WriteWord(addr, c.R[s]);
    return 11;
}

// --- PUSH @Rd,Rs = $13[d][s] (pre-decrement *Rd then write Rs) ---
static int Exec_PUSH(Z8000Cpu& c, uint16_t op) {
    int d = (op >> 4) & 0x0F;
    int s = op & 0x0F;
    if (d == 0 || !c.Memory) return 9;
    c.R[d] = static_cast<uint16_t>(c.R[d] - 2);
    c.Memory->WriteWord(c.R[d], c.R[s]);
    return 9;
}

// --- POP Rd,@Rs = $17[s][d] (read *Rs then post-increment Rs) ---
static int Exec_POP(Z8000Cpu& c, uint16_t op) {
    int s = (op >> 4) & 0x0F;
    int d = op & 0x0F;
    if (s == 0 || !c.Memory) return 8;
    c.R[d] = c.Memory->ReadWord(c.R[s]);
    c.R[s] = static_cast<uint16_t>(c.R[s] + 2);
    return 8;
}

// --- CALL addr = $5F00 addr16 ---
static int Exec_CALL_addr(Z8000Cpu& c, uint16_t /*op*/) {
    uint16_t addr = c.FetchWord();
    // PC now points past the CALL instruction — push that as return address.
    c.Push16(c.PC);
    if (c.ShadowStackTop < Z8000Cpu::MaxShadowStack)
        c.ShadowStack[c.ShadowStackTop++] = c.PC;
    c.PC = addr;
    return 20;
}

// --- CALL @Rd = $5F[d][0] (d≠0) ---
static int Exec_CALL_ind(Z8000Cpu& c, uint16_t op) {
    int d = (op >> 4) & 0x0F;
    if (d == 0) { c.TakeException(0x10, true, op); return 16; }
    c.Push16(c.PC);
    if (c.ShadowStackTop < Z8000Cpu::MaxShadowStack)
        c.ShadowStack[c.ShadowStackTop++] = c.PC;
    c.PC = c.R[d];
    return 15;
}

// --- CALR disp12 = $D[disp12] (target = PC_after_fetch - 2*disp12, disp12 unsigned) ---
static int Exec_CALR(Z8000Cpu& c, uint16_t op) {
    uint16_t disp = op & 0x0FFF;
    // PC currently points past the CALR instruction (after FetchWord).
    c.Push16(c.PC);
    if (c.ShadowStackTop < Z8000Cpu::MaxShadowStack)
        c.ShadowStack[c.ShadowStackTop++] = c.PC;
    c.PC = static_cast<uint16_t>(c.PC - disp * 2);
    return 10;
}

// --- RET cc = $9E0[cc] ---
static int Exec_RET(Z8000Cpu& c, uint16_t op) {
    uint8_t cc = op & 0x0F;
    if (Z8000Cpu_TestCondition(c, cc)) {
        c.PC = c.Pop16();
        if (c.ShadowStackTop > 0) c.ShadowStackTop--;
        return 10;
    }
    return 7;
}

// --- JP cc,addr = $5E0[cc] addr16 ---
static int Exec_JP_addr(Z8000Cpu& c, uint16_t op) {
    uint8_t cc = op & 0x0F;
    uint16_t target = c.FetchWord();
    if (Z8000Cpu_TestCondition(c, cc)) c.PC = target;
    return 7;
}

// --- JP cc,@Rd = $1E[d][cc] (d≠0) ---
static int Exec_JP_ind(Z8000Cpu& c, uint16_t op) {
    int d = (op >> 4) & 0x0F;
    uint8_t cc = op & 0x0F;
    if (d == 0) { c.TakeException(0x10, true, op); return 16; }
    if (Z8000Cpu_TestCondition(c, cc)) c.PC = c.R[d];
    return 8;
}

// --- IRET = $7B00 (privileged; pop FCW then PC) ---
static int Exec_IRET(Z8000Cpu& c, uint16_t /*op*/) {
    if (!c.IsSystem()) {
        c.TakeException(0x10, true, 0x7B00);
        return 16;
    }
    c.FCW = c.Pop16();
    c.PC  = c.Pop16();
    // If mode flipped, swap stack banks.
    // (Exception entry had forced system mode; if FCW now says normal, swap back.)
    if (!c.IsSystem()) c.SwapStackBanks();
    return 13;
}

// --- EI / DI = $7C0[n] where low 2 bits mask NVI/VI sources ---
// EI: $7C04..$7C07 — the low 2 bits are (VIE, NVIE) enable flags to set
// DI: $7C00..$7C03 — the low 2 bits are (VIE, NVIE) flags to clear
// (In MAME: bit0=NVIE, bit1=VIE; EI if bit2=1, DI if bit2=0)
static int Exec_EI_DI(Z8000Cpu& c, uint16_t op) {
    if (!c.IsSystem()) {
        c.TakeException(0x10, true, op);
        return 16;
    }
    bool enable = (op & 0x04) != 0; // bit 2 = EI(1) / DI(0)
    bool nvi = (op & 0x01) != 0;
    bool vi  = (op & 0x02) != 0;
    if (enable) {
        if (nvi) c.FCW |= Z8000Cpu::FCW_NVIE;
        if (vi)  c.FCW |= Z8000Cpu::FCW_VIE;
    } else {
        if (nvi) c.FCW &= ~Z8000Cpu::FCW_NVIE;
        if (vi)  c.FCW &= ~Z8000Cpu::FCW_VIE;
    }
    return 7;
}

// --- I/O port immediate ---
// INB Rbd,#port  = $3A [d]4 imm16   (sub-op 4 in low nibble)
// IN  Rd, #port  = $3B [d]4 imm16
// OUTB #port,Rbs = $3A [s]6 imm16
// OUT  #port,Rs  = $3B [s]6 imm16
static int Exec_IO_imm(Z8000Cpu& c, uint16_t op) {
    if (!c.IsSystem()) {
        c.TakeException(0x10, true, op);
        return 16;
    }
    bool word = (op & 0x0100) != 0; // $3B -> word, $3A -> byte
    uint8_t sub = op & 0x0F;
    int reg = (op >> 4) & 0x0F;
    uint16_t port = c.FetchWord();

    switch (sub) {
    case 0x4: // IN / INB — read from port into register
        if (word) {
            if (c.ReadIoWord) c.R[reg] = c.ReadIoWord(port);
        } else {
            if (c.ReadIoByte) c.SetByteReg(reg, c.ReadIoByte(port));
        }
        return 12;
    case 0x6: // OUT / OUTB — write register to port
        if (word) {
            if (c.WriteIoWord) c.WriteIoWord(port, c.R[reg]);
        } else {
            if (c.WriteIoByte) c.WriteIoByte(port, c.GetByteReg(reg));
        }
        return 12;
    default:
        // Other sub-ops (special I/O, repeat forms) not implemented Phase 1b.
        c.TakeException(0x10, true, op);
        return 16;
    }
}

// --- I/O port indirect ---
// INB Rbd,@Rs  = $3C [s][d]
// IN  Rd, @Rs  = $3D [s][d]
// OUTB @Rd,Rbs = $3E [d][s]
// OUT  @Rd,Rs  = $3F [d][s]
static int Exec_IO_ind(Z8000Cpu& c, uint16_t op) {
    if (!c.IsSystem()) {
        c.TakeException(0x10, true, op);
        return 16;
    }
    uint8_t hi = static_cast<uint8_t>(op >> 8);
    int a = (op >> 4) & 0x0F;  // first reg field
    int b = op & 0x0F;          // second reg field
    switch (hi) {
    case 0x3C: // INB Rbd,@Rs : a=s (pointer), b=d (byte dst)
        if (a != 0 && c.ReadIoByte) c.SetByteReg(b, c.ReadIoByte(c.R[a]));
        return 10;
    case 0x3D: // IN Rd,@Rs
        if (a != 0 && c.ReadIoWord) c.R[b] = c.ReadIoWord(c.R[a]);
        return 10;
    case 0x3E: // OUTB @Rd,Rbs : a=d (pointer), b=s (byte src)
        if (a != 0 && c.WriteIoByte) c.WriteIoByte(c.R[a], c.GetByteReg(b));
        return 10;
    case 0x3F: // OUT @Rd,Rs
        if (a != 0 && c.WriteIoWord) c.WriteIoWord(c.R[a], c.R[b]);
        return 10;
    }
    return 10;
}

// ============================================================================
// ExecuteOne - dispatch on high byte of opcode word
// ============================================================================

int Z8000Cpu::ExecuteOne() {
    if (Halted) return 0;

    uint16_t op = FetchWord();
    uint8_t hi = static_cast<uint8_t>(op >> 8);
    int cycles = 0;
    bool handled = true;

    // Byte ALU register-register: $80..$8B (byte variants = even, word = odd)
    // NOTE: within the $8x range, high-byte selects operation while the full
    // word's low 8 bits carry [src:4][dst:4].
    switch (hi) {
    // Phase 1a: register-to-register ALU
    case 0x80: cycles = Exec_ADDB_r_r(*this, op); break;
    case 0x81: cycles = Exec_ADD_r_r (*this, op); break;
    case 0x82: cycles = Exec_SUBB_r_r(*this, op); break;
    case 0x83: cycles = Exec_SUB_r_r (*this, op); break;
    case 0x84: cycles = Exec_ORB_r_r (*this, op); break;
    case 0x85: cycles = Exec_OR_r_r  (*this, op); break;
    case 0x86: cycles = Exec_ANDB_r_r(*this, op); break;
    case 0x87: cycles = Exec_AND_r_r (*this, op); break;
    case 0x88: cycles = Exec_XORB_r_r(*this, op); break;
    case 0x89: cycles = Exec_XOR_r_r (*this, op); break;
    case 0x8A: cycles = Exec_CPB_r_r (*this, op); break;
    case 0x8B: cycles = Exec_CP_r_r  (*this, op); break;
    case 0xA0: cycles = Exec_LDB_r_r (*this, op); break;
    case 0xA1: cycles = Exec_LD_r_r  (*this, op); break;

    // Phase 1b: memory via register indirect
    case 0x20: cycles = Exec_LDB_r_ind(*this, op); break;
    case 0x21:
        if ((op & 0xF0) == 0x00) cycles = Exec_LD_r_imm(*this, op); // immediate form
        else                     cycles = Exec_LD_r_ind(*this, op); // indirect form
        break;
    case 0x2E: cycles = Exec_LDB_ind_r(*this, op); break;
    case 0x2F: cycles = Exec_LD_ind_r (*this, op); break;

    // Phase 1b: memory via direct address (s/d nibble in low byte must have high nibble 0)
    case 0x70: cycles = Exec_LDB_r_dir(*this, op); break;
    case 0x71: cycles = Exec_LD_r_dir (*this, op); break;
    case 0x78: cycles = Exec_LDB_dir_r(*this, op); break;
    case 0x79: cycles = Exec_LD_dir_r (*this, op); break;

    // Phase 1b: stack
    case 0x13: cycles = Exec_PUSH(*this, op); break;
    case 0x17: cycles = Exec_POP (*this, op); break;

    // Phase 1b: I/O indirect
    case 0x3C:
    case 0x3D:
    case 0x3E:
    case 0x3F: cycles = Exec_IO_ind(*this, op); break;

    // Phase 1b: I/O immediate (INB/IN/OUTB/OUT with port=imm16)
    case 0x3A:
    case 0x3B: cycles = Exec_IO_imm(*this, op); break;

    default:
        handled = false;
        break;
    }

    if (!handled) {
        // Secondary dispatch on wider patterns
        if ((hi & 0xF0) == 0xC0) {
            // LDB Rbd,#imm8  = $C[d][imm8]
            cycles = Exec_LDB_r_imm(*this, op);
            handled = true;
        } else if ((hi & 0xF0) == 0xE0) {
            // JR cc,disp
            cycles = Exec_JR(*this, op);
            handled = true;
        } else if ((hi & 0xF0) == 0xD0) {
            // CALR disp12 = $D[disp12]
            cycles = Exec_CALR(*this, op);
            handled = true;
        } else if (hi == 0x5E) {
            // JP cc,addr  = $5E[0][cc] addr   (non-indexed form only for Phase 1b)
            cycles = Exec_JP_addr(*this, op);
            handled = true;
        } else if (hi == 0x5F) {
            // CALL addr = $5F00 addr   OR   CALL @Rd = $5F[d][0] with d≠0
            // op low byte = $00 for direct form, $[d][0] for indirect.
            if ((op & 0x00FF) == 0) {
                cycles = Exec_CALL_addr(*this, op);
            } else {
                cycles = Exec_CALL_ind(*this, op);
            }
            handled = true;
        } else if (hi == 0x1E) {
            // JP cc,@Rd = $1E[d][cc] with d≠0
            cycles = Exec_JP_ind(*this, op);
            handled = true;
        } else if (hi == 0x9E) {
            // RET cc = $9E0[cc]
            cycles = Exec_RET(*this, op);
            handled = true;
        } else if (hi == 0x7C && (op & 0xF8) == 0x00) {
            // EI/DI = $7C0[n] where n<8
            cycles = Exec_EI_DI(*this, op);
            handled = true;
        } else if (op == 0x8D07) {
            cycles = 7; // NOP
            handled = true;
        } else if (op == 0x7A00) {
            cycles = Exec_HALT(*this, op);
            handled = true;
        } else if (op == 0x7B00) {
            cycles = Exec_IRET(*this, op);
            handled = true;
        }
    }

    if (!handled) {
        // Unrecognised opcode → Privileged Instruction trap (PSA offset $10).
        // This keeps state well-defined until we grow the decode table.
        TakeException(0x10, true, op);
        cycles = 16;
    }

    InstructionCount++;
    CycleCount += cycles;
    return cycles;
}

// ============================================================================
// CheckInterrupts
// ============================================================================

int Z8000Cpu::CheckInterrupts() {
    // Priority: NMI > VI > NVI. NMI is not maskable.
    int cycles = 0;
    if (NmiPending) {
        NmiPending = false;
        Halted = false;
        TakeException(0x08, /*pushIdentifier=*/false, 0);
        cycles = 39;
    } else if (ViLine && GetFlag(FCW_VIE)) {
        Halted = false;
        uint8_t v = ViVector;
        TakeException(static_cast<uint16_t>(0x1C + v * 2), /*pushIdentifier=*/true, v);
        cycles = 39;
    } else if (NviLine && GetFlag(FCW_NVIE)) {
        Halted = false;
        TakeException(0x0C, /*pushIdentifier=*/false, 0);
        cycles = 39;
    }
    CycleCount += cycles;
    return cycles;
}
