// SPDX-License-Identifier: MIT OR Apache-2.0
// Copyright (c) 2026 hha0x617
//
// Z80Cpu.cpp - Z80 CPU core. Phase 1.1 scaffold only.

#include "pch.h"
#include "Z80Cpu.h"

void Z80Cpu::Reset() {
    // Z80 power-on / RESET state per Zilog UM008011 §2.4:
    //   PC = 0x0000, I = 0, R = 0, IFF1 = IFF2 = 0, IM = 0.
    // Other registers are undefined on real hardware; we clear them for
    // determinism.
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
