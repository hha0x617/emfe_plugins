// SPDX-License-Identifier: MIT OR Apache-2.0
// Copyright (c) 2026 hha0x617
//
// Z80Cpu.cpp - Z80 CPU core.
//
// Phase 1.3: only NOP (0x00) is decoded. The opcode handler table
// grows in Phase 1.4 (data movement), 1.5 (arithmetic), 1.6 (control
// flow), and so on.

#include "pch.h"
#include "Z80Cpu.h"
#include "Z80Memory.h"

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

int Z80Cpu::ExecuteOne() {
    if (!Memory) return 0;

    uint8_t opcode = Memory->ReadByte(PC);

    // The R register's lower 7 bits increment on every instruction
    // fetch (per Zilog UM008011 §2.7.6 — bit 7 of R is preserved across
    // increments and copied into bit 7 of the value seen by `LD A,R`).
    R = static_cast<uint8_t>((R & 0x80) | ((R + 1) & 0x7F));

    switch (opcode) {
    case 0x00: // NOP — 4 T-states.
        PC = static_cast<uint16_t>(PC + 1);
        CycleCount += 4;
        InstructionCount += 1;
        return 4;
    default:
        // Unrecognised. Phase 1.3 understands only NOP; everything else
        // surfaces as an unsupported-step trap. PC is left untouched so
        // the host's disassembly pane still shows the offending byte.
        return 0;
    }
}
