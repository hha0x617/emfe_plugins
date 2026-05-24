/*
 * Z80Cpu.h - Zilog Z80 8-bit CPU core
 *
 * Memory: 64 KiB flat via Z80Memory.
 * I/O space: 256-byte via Z80Memory::ReadIo / WriteIo.
 *
 * Register file: AF / BC / DE / HL (with shadow AF' / BC' / DE' / HL'),
 *                IX / IY / SP / PC, I / R, IFF1 / IFF2 / IM.
 * Pairs are stored as uint16_t and exposed as 8-bit halves via inline
 * accessors (A/F, B/C, D/E, H/L) — the host's register pane lists each
 * pair plus its halves so users can edit at either granularity.
 *
 * Flags register F (Zilog UM008011-0816 §2.4):
 *   bit 7 S   bit 6 Z   bit 5 Y*  bit 4 H
 *   bit 3 X*  bit 2 P/V bit 1 N   bit 0 C
 *   (* = undocumented; tracked correctly from Phase 3 onward)
 *
 * Interrupts: INT (modes 0 / 1 / 2) + NMI.
 *
 * SPDX-License-Identifier: MIT OR Apache-2.0
 * Copyright (c) 2026 hha0x617
 */
#pragma once
#include <cstdint>
#include <unordered_set>

class Z80Memory;

class Z80Cpu {
public:
    // ---- Main register file (16-bit storage; halves via accessors) ---
    uint16_t AF = 0, BC = 0, DE = 0, HL = 0;
    uint16_t AF_ = 0, BC_ = 0, DE_ = 0, HL_ = 0;   // shadow set
    uint16_t IX = 0, IY = 0, SP = 0, PC = 0;
    uint8_t  I = 0, R = 0;

    // ---- Interrupt state --------------------------------------------
    bool IFF1 = false, IFF2 = false;
    uint8_t IM = 0;          // interrupt mode 0 / 1 / 2
    bool Halted = false;

    // ---- Counters ----------------------------------------------------
    uint64_t CycleCount = 0;
    uint64_t InstructionCount = 0;

    // ---- Pending interrupt lines (set externally) -------------------
    bool NmiPending = false;
    bool IrqLine = false;

    // True for exactly one ExecuteOne call after EI runs — interrupts
    // are masked during that step (the Zilog "EI delay" rule that
    // prevents an EI;RETI pattern from re-firing immediately).
    bool EiDelayPending = false;

    Z80Memory* Memory = nullptr;

    // ---- 8-bit half accessors ---------------------------------------
    // Pair layout: high byte = A/B/D/H, low byte = F/C/E/L. This is the
    // Zilog-documented byte ordering inside the pairs (e.g. `LD A, B`
    // moves the high byte of BC into the high byte of AF).
    uint8_t  A() const { return static_cast<uint8_t>(AF >> 8); }
    uint8_t  F() const { return static_cast<uint8_t>(AF & 0xFF); }
    uint8_t  B() const { return static_cast<uint8_t>(BC >> 8); }
    uint8_t  C() const { return static_cast<uint8_t>(BC & 0xFF); }
    uint8_t  D() const { return static_cast<uint8_t>(DE >> 8); }
    uint8_t  E() const { return static_cast<uint8_t>(DE & 0xFF); }
    uint8_t  H() const { return static_cast<uint8_t>(HL >> 8); }
    uint8_t  L() const { return static_cast<uint8_t>(HL & 0xFF); }

    void SetA(uint8_t v) { AF = static_cast<uint16_t>((AF & 0x00FF) | (uint16_t(v) << 8)); }
    void SetF(uint8_t v) { AF = static_cast<uint16_t>((AF & 0xFF00) | v); }
    void SetB(uint8_t v) { BC = static_cast<uint16_t>((BC & 0x00FF) | (uint16_t(v) << 8)); }
    void SetC(uint8_t v) { BC = static_cast<uint16_t>((BC & 0xFF00) | v); }
    void SetD(uint8_t v) { DE = static_cast<uint16_t>((DE & 0x00FF) | (uint16_t(v) << 8)); }
    void SetE(uint8_t v) { DE = static_cast<uint16_t>((DE & 0xFF00) | v); }
    void SetH(uint8_t v) { HL = static_cast<uint16_t>((HL & 0x00FF) | (uint16_t(v) << 8)); }
    void SetL(uint8_t v) { HL = static_cast<uint16_t>((HL & 0xFF00) | v); }

    // ---- F (flags) register bit constants ---------------------------
    // Layout per Zilog UM008011 §2.4. Y and X are undocumented copies
    // of bits 5 and 3 of either the result (most ALU ops) or the
    // operand (CP — see Z80Cpu::AluCp). For normal 8-bit ALU these
    // are straightforward result-bit copies; Phase 2 deals with the
    // weirder propagation rules for BIT / SCF / CCF / 16-bit ops.
    static constexpr uint8_t F_S  = 0x80;
    static constexpr uint8_t F_Z  = 0x40;
    static constexpr uint8_t F_Y  = 0x20;
    static constexpr uint8_t F_H  = 0x10;
    static constexpr uint8_t F_X  = 0x08;
    static constexpr uint8_t F_PV = 0x04;
    static constexpr uint8_t F_N  = 0x02;
    static constexpr uint8_t F_C  = 0x01;

    // ---- Execution surface ------------------------------------------
    // Reset to power-on state (Zilog UM008011 §2.4).
    void Reset();

    // Execute one instruction at PC. Returns:
    //   > 0 : T-states consumed; CPU state advanced.
    //     0 : unrecognised opcode; PC rolled back to the opcode address
    //         so the host's disassembly pane still shows the offending
    //         byte. The R register is left in its incremented state to
    //         match the real hardware behaviour (R bumps per M1 fetch).
    //         The caller (emfe_step) translates 0 into
    //         EMFE_ERR_UNSUPPORTED.
    //  -1 : breakpoint hit at PC. PC is left untouched (so the next
    //         emfe_step / emfe_run after the host clears or steps past
    //         the BP resumes at the same address). The worker loop
    //         (Phase 6.1) treats this the same as the 0 case — break
    //         out of the run loop and let state→STOPPED. emfe_step
    //         translates this into EMFE_OK (no-op for the user; PC
    //         didn't move). See InStepMode / SetInStepMode for the
    //         "step past a BP" escape.
    // Cycle counts follow the Zilog UM008011 reference values.
    int ExecuteOne();

    // ---- Phase 6.2 breakpoint state ---------------------------------
    // Two sets so emfe_enable_breakpoint can flip a BP on/off without
    // losing its existence (a disabled BP still shows in the host's
    // BP pane, just doesn't halt). emfe_add_breakpoint puts a fresh
    // BP in `enabledBreakpoints`; emfe_remove_breakpoint erases from
    // both; emfe_get_breakpoints walks both for enumeration.
    std::unordered_set<uint16_t> enabledBreakpoints;
    std::unordered_set<uint16_t> disabledBreakpoints;

    // The "I'm stepping over a breakpoint" flag. emfe_step sets this
    // before ExecuteOne so the BP check at PC doesn't immediately
    // halt; ExecuteOne clears it on return. Without this, stepping
    // past a BP would be impossible — every emfe_step at a BP'd PC
    // would short-circuit to "BP hit, PC unchanged".
    bool inStepMode = false;

private:
    // ---- Instruction-fetch helpers ----------------------------------
    // Both advance PC; FetchWord uses little-endian byte order.
    uint8_t  FetchByte();
    uint16_t FetchWord();

    // ---- Register-code accessors (Zilog 3-bit encoding) -------------
    // 0=B 1=C 2=D 3=E 4=H 5=L 6=(HL) 7=A. Code 6 routes through
    // Memory->ReadByte(HL) / WriteByte(HL, val), which adds the
    // documented one-extra-memory-cycle to the instruction's T-state
    // count (callers add 3 T-states when reading or writing via code 6).
    uint8_t Reg8Read (int code);
    void    Reg8Write(int code, uint8_t val);

    // ---- 8-bit ALU helpers (all update F via SetF) ------------------
    // AluAdd / AluSub take the optional carry/borrow input (used by
    // ADC / SBC; pass false for ADD / SUB). They return the result
    // (caller writes to A as appropriate).
    // AluCp is SUB without writing the result back to A — and unlike
    // SUB, the undocumented Y / X flag bits come from the OPERAND
    // rather than the result (Zilog peculiarity, important for zexall).
    uint8_t AluAdd(uint8_t a, uint8_t b, bool carry);
    uint8_t AluSub(uint8_t a, uint8_t b, bool borrow);
    uint8_t AluAnd(uint8_t a, uint8_t b);
    uint8_t AluOr (uint8_t a, uint8_t b);
    uint8_t AluXor(uint8_t a, uint8_t b);
    void    AluCp (uint8_t a, uint8_t b);

    // ---- Stack helpers (Z80 little-endian, pre-decrement push) ------
    void     Push16(uint16_t val);
    uint16_t Pop16();

    // ---- Condition-code evaluator (Zilog 3-bit ccc) -----------------
    // 0 NZ  1 Z  2 NC  3 C  4 PO  5 PE  6 P  7 M
    bool CheckCond(int ccc) const;

    // ---- CB-prefix shift / rotate helpers ---------------------------
    // Each returns the post-op byte and sets F via SetShiftRotateFlags.
    // RotRL / RotRR consume the existing C flag as the rotated-in bit.
    uint8_t RotRLC  (uint8_t v);   // rotate left circular
    uint8_t RotRRC  (uint8_t v);   // rotate right circular
    uint8_t RotRL   (uint8_t v);   // rotate left through carry
    uint8_t RotRR   (uint8_t v);   // rotate right through carry
    uint8_t ShiftSLA(uint8_t v);   // shift left arithmetic
    uint8_t ShiftSRA(uint8_t v);   // shift right arithmetic (sign preserved)
    uint8_t ShiftSLL(uint8_t v);   // shift left logical (undocumented, bit 0 = 1)
    uint8_t ShiftSRL(uint8_t v);   // shift right logical

    // BIT b, v — test bit b of v, set flags only.
    //
    // `yx_source` supplies the byte from which the undocumented Y / X
    // bits are copied:
    //   BIT b, r              → yx_source = r (the operand itself)
    //   BIT b, (HL)           → yx_source = H (i.e. HL >> 8) — a
    //                           pragmatic approximation of the real
    //                           Z80's WZ/MEMPTR-high behaviour that is
    //                           good enough for zexall.
    //   BIT b, (IX+d) / (IY+d)→ yx_source = (effective_address >> 8)
    //
    // Phase 3.2 swapped the old single-argument signature for the dual
    // form; the rest of the flag rules are unchanged.
    void    OpBit(int b, uint8_t v, uint8_t yx_source);

    // Shared flag setter for the rotate / shift family above.
    void    SetShiftRotateFlags(uint8_t result, bool carry_out);

    // ---- Single-byte accumulator rotates (Phase 3.2) ----------------
    // Distinct from the CB-prefixed RLC/RRC/RL/RR versions:
    //   * S, Z, P/V are PRESERVED (the CB forms recompute them).
    //   * Y / X come from the post-rotate value of A.
    //   * H = 0, N = 0.
    //   * C = the bit shifted OUT of A.
    void OpRlca();   // 0x07 — rotate left circular
    void OpRrca();   // 0x0F — rotate right circular
    void OpRla();    // 0x17 — rotate left through carry
    void OpRra();    // 0x1F — rotate right through carry

    // ---- Misc accumulator / flag ops (Phase 3.2) --------------------
    // OpDaa: decimal-adjust A after a BCD ADD/SUB. The correction
    // depends on the post-op N (sub) / H (half-carry) / C flags.
    // OpCpl: A := ~A. Flags: H = 1, N = 1, Y/X from new A, others
    // preserved.
    // OpScf: C = 1, N = 0, H = 0. Y/X = bits 5/3 of A (pragmatic
    // "Q | A" approximation that suffices for zexall).
    // OpCcf: C = !C, N = 0, H = old C. Y/X = bits 5/3 of A.
    void OpDaa();    // 0x27
    void OpCpl();    // 0x2F
    void OpScf();    // 0x37
    void OpCcf();    // 0x3F

    // ---- 16-bit ADD helper (ADD HL,rr / ADD IX,rr / ADD IY,rr) ------
    // Computes target + addend, sets F per Zilog UM008011 §5:
    //   N := 0; C := carry from bit 15; H := half-carry from bit 11;
    //   Y / X copied from result high byte bits 5 / 3;
    //   S, Z, P/V preserved.
    uint16_t AddPair16(uint16_t target, uint16_t addend);

    // ---- DD / FD prefix dispatcher ----------------------------------
    // Called from ExecuteOne when opcode is 0xDD (IX) or 0xFD (IY).
    // `IXY` is a pointer to the index register being substituted for
    // HL by this prefix. Returns total T-states (including the 4 T-
    // states added by the prefix M1 fetch).
    int ExecuteWithIndex(uint16_t* IXY);

    // ---- 16-bit ADC HL,rr / SBC HL,rr helpers -----------------------
    // Both update HL in place and set full flags per Zilog UM008011:
    //   ADC: S/Z from result high byte sign + result==0,
    //        Y/X from result high byte bits 5/3,
    //        H from bit 11 carry, PV signed overflow,
    //        N=0, C from bit 15.
    //   SBC: same shape but N=1, H/C are borrow-style.
    // The OPERATIONS reference the existing C flag as carry-in.
    void AdcHl16(uint16_t addend);
    void SbcHl16(uint16_t addend);

    // ---- ED prefix dispatcher ---------------------------------------
    int ExecuteEdPrefix();

    // ---- Interrupt acceptance ---------------------------------------
    // Called by ExecuteOne between instructions when an NMI or a
    // maskable IRQ is pending. Return values are T-states consumed
    // by the acceptance sequence (push PC + jump-to-vector).
    int AcceptNmi();
    int AcceptIrq();
};
