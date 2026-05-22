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
    // Cycle counts follow the Zilog UM008011 reference values.
    int ExecuteOne();

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
};
