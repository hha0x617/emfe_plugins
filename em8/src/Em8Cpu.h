/*
 * Em8Cpu.h - EM8 Custom 8-bit CPU Core
 *
 * 8-bit data bus, 16-bit address bus (64KB).
 * Registers: A (accumulator), X/Y (index), SP (8-bit, stack $0100-$01FF),
 *            PC (16-bit), FL (flags: NV-B-IZC).
 * Vectors: NMI=$FFFA, Reset=$FFFC, IRQ=$FFFE (little-endian).
 *
 * Copyright (c) 2026 emfe Project
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <cstdint>
#include <string>
#include <functional>

class Em8Memory;

class Em8Cpu {
public:
    uint8_t A = 0, X = 0, Y = 0, SP = 0xFF;
    uint16_t PC = 0;
    uint8_t FL = 0x04; // I=1 on reset
    bool Halted = false;
    uint64_t CycleCount = 0;
    uint64_t InstructionCount = 0;

    // Pending interrupt flags (set from outside, cleared by CheckInterrupts)
    bool NmiPending = false;
    bool IrqLine = false; // Level-triggered

    // Shadow stack for call stack tracking
    static constexpr int MaxShadowStack = 256;
    uint16_t ShadowStack[MaxShadowStack]{};
    int ShadowStackTop = 0;

    Em8Memory* Memory = nullptr;

    // Execute one instruction. Returns cycles consumed.
    int ExecuteOne();

    // Check and handle pending IRQ/NMI. Returns cycles if interrupt taken, 0 otherwise.
    int CheckInterrupts();

    // Reset CPU state (reads reset vector from $FFFC)
    void Reset();

    // Disassemble one instruction at addr. Returns instruction length.
    struct DisasmResult {
        std::string mnemonic;
        std::string operands;
        std::string rawBytes;
        int length;
    };
    DisasmResult Disassemble(uint16_t addr) const;

    // Flag bits
    static constexpr uint8_t FLAG_C = 0x01;
    static constexpr uint8_t FLAG_Z = 0x02;
    static constexpr uint8_t FLAG_I = 0x04;
    static constexpr uint8_t FLAG_B = 0x10;
    static constexpr uint8_t FLAG_V = 0x40;
    static constexpr uint8_t FLAG_N = 0x80;

    bool GetFlag(uint8_t f) const { return (FL & f) != 0; }
    void SetFlag(uint8_t f, bool v) { if (v) FL |= f; else FL &= ~f; }

private:
    void SetNZ(uint8_t val);
    void Push8(uint8_t val);
    uint8_t Pull8();
    void Push16(uint16_t val);
    uint16_t Pull16();
    uint8_t ReadByte(uint16_t addr);
    void WriteByte(uint16_t addr, uint8_t val);
    uint8_t FetchByte();
    uint16_t FetchWord();

    void DoAdd(uint8_t val);
    void DoSub(uint8_t val);
    void DoCmp(uint8_t reg, uint8_t val);

    // Addressing mode helpers (return effective address)
    uint8_t AddrZpg();
    uint8_t AddrZpgX();
    uint16_t AddrAbs();
    uint16_t AddrAbsX();
    uint16_t AddrAbsY();
    uint16_t AddrInd(); // indirect via zero page pointer
};
