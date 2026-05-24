/*
 * Z80Disasm.h - Z80 disassembler
 *
 * Decodes a single instruction at a given address into mnemonic /
 * operand strings + the consumed byte length. Mirrors the same prefix
 * dispatch (plain / CB / DD-FD / ED / DD-CB / FD-CB) as Z80Cpu's
 * ExecuteOne, but produces text instead of executing.
 *
 * SPDX-License-Identifier: MIT OR Apache-2.0
 * Copyright (c) 2026 hha0x617
 */
#pragma once
#include <cstdint>
#include <string>

class Z80Memory;

class Z80Disasm {
public:
    struct Result {
        std::string mnemonic;       // "LD", "NOP", "JP", ...
        std::string operands;       // "A, (HL)" or "" if none
        std::string rawBytes;       // space-separated hex: "21 34 12"
        int         length;         // bytes consumed
    };

    // Decode the instruction starting at `addr`. Memory wraps 16-bit
    // on access; addresses near 0xFFFF will read across the boundary.
    static Result Decode(Z80Memory* mem, uint16_t addr);
};
