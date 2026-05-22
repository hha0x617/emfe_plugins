/*
 * Z80Cpu.h - Zilog Z80 8-bit CPU core
 *
 * 8-bit data bus, 16-bit address bus (64 KiB flat).
 * 256-byte separate I/O space via IN / OUT.
 * Registers: AF / BC / DE / HL (with shadow AF' / BC' / DE' / HL'),
 *            IX / IY / SP / PC, I / R.
 * Interrupts: INT (modes 0 / 1 / 2) + NMI.
 *
 * SPDX-License-Identifier: MIT OR Apache-2.0
 * Copyright (c) 2026 hha0x617
 */
#pragma once
#include <cstdint>

class Z80Memory;

// Phase 1.1 — declaration-only scaffold.
// Register fields and the public execution surface are added in later
// Phase 1 PRs (1.3 registers, 1.4 data movement, ...).
class Z80Cpu {
public:
    // Main register file (8-bit halves of pairs are accessed via unions in
    // a later PR; for now keep the wide forms only).
    uint16_t AF = 0, BC = 0, DE = 0, HL = 0;
    uint16_t AF_ = 0, BC_ = 0, DE_ = 0, HL_ = 0;
    uint16_t IX = 0, IY = 0, SP = 0, PC = 0;
    uint8_t  I = 0, R = 0;

    // Interrupt state
    bool IFF1 = false, IFF2 = false;
    uint8_t IM = 0;          // interrupt mode 0 / 1 / 2
    bool Halted = false;

    // Counters
    uint64_t CycleCount = 0;
    uint64_t InstructionCount = 0;

    // Pending interrupt lines (set from outside, cleared on acceptance)
    bool NmiPending = false;
    bool IrqLine = false;

    Z80Memory* Memory = nullptr;

    // Reset to power-on state.
    void Reset();
};
