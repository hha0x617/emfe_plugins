/*
 * Z8000Cpu.h - Zilog Z8000 Family CPU Core
 *
 * Covers Z8001 / Z8002 / Z8003 / Z8004. Variant is selected at construction
 * via SetVariant(). Differences between variants:
 *   - Segmented bit (Z8001/Z8003): 23-bit addressing, 32-bit PC, segmented
 *     addressing modes, long-offset data formats.
 *   - Abort/VM bit (Z8003/Z8004): memory-access instructions are restartable
 *     on external abort (for demand-paged virtual memory).
 *
 * Phase 1 implements Z8002 mode only (non-segmented, no abort). The variant
 * flags are present but Segmented=true / AbortCapable=true are stubs.
 *
 * Register file:
 *   R0..R15  (16 bit)   - primary word registers
 *   RH0..RH7 / RL0..RL7 - byte views of R0..R7 (high/low halves)
 *   RR0,RR2,..RR14      - 32-bit pairs (R(n):R(n+1))
 *   RQ0,RQ4,RQ8,RQ12    - 64-bit quads (R(n)..R(n+3))
 *   R14/R15             - stack pointer (R15 in non-seg, RR14 in seg mode).
 *                         Current mode's SP lives in R[14]/R[15]; the other
 *                         mode's SP is held in ShadowR14/ShadowR15.
 *
 * Special registers:
 *   PC        - program counter (16-bit in non-seg, seg#+offset in seg mode)
 *   FCW       - flag and control word (see FCW_* constants)
 *   PSAP      - program status area pointer (exception vector base)
 *   REFRESH   - DRAM refresh counter (not modelled cycle-accurately)
 *
 * Copyright (c) 2026 emfe Project
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <cstdint>
#include <string>
#include <functional>

class Z8000Memory;

class Z8000Cpu {
public:
    enum Variant : uint8_t {
        VARIANT_Z8001 = 0,  // Segmented
        VARIANT_Z8002 = 1,  // Non-segmented  (Phase 1 target)
        VARIANT_Z8003 = 2,  // Segmented + virtual memory (abort)
        VARIANT_Z8004 = 3,  // Non-segmented + virtual memory (abort)
    };

    // --- Register file ---
    uint16_t R[16] = {};
    // Shadow stack pointer for the inactive mode (normal or system).
    // On S/N transition these are swapped with R[14]/R[15].
    uint16_t ShadowR14 = 0;
    uint16_t ShadowR15 = 0;

    // --- Special / control registers ---
    uint16_t PC = 0;
    uint16_t PCSegment = 0;    // Z8001/Z8003 only (high byte of segmented PC)
    uint16_t FCW = 0;
    uint16_t PSAPOffset = 0;   // Program Status Area base
    uint16_t PSAPSegment = 0;  // segmented variants only
    uint16_t Refresh = 0;

    // --- State ---
    bool Halted = false;
    uint64_t CycleCount = 0;
    uint64_t InstructionCount = 0;

    // --- Interrupts (level-sensitive) ---
    bool NmiPending = false;
    bool NviLine    = false;
    bool ViLine     = false;
    uint8_t ViVector = 0;  // vector identifier presented by device on VI ack

    // --- Shadow stack for call-stack tracking (debugger) ---
    static constexpr int MaxShadowStack = 256;
    uint16_t ShadowStack[MaxShadowStack]{};
    int ShadowStackTop = 0;

    // --- Variant configuration ---
    Variant m_variant = VARIANT_Z8002;
    bool m_segmented = false;    // derived from variant
    bool m_abortCapable = false; // derived from variant

    void SetVariant(Variant v) {
        m_variant = v;
        m_segmented    = (v == VARIANT_Z8001 || v == VARIANT_Z8003);
        m_abortCapable = (v == VARIANT_Z8003 || v == VARIANT_Z8004);
    }

    // --- Subsystem wiring ---
    Z8000Memory* Memory = nullptr;
    // I/O callbacks (normal and special I/O). Plugin wires these to UART/Timer.
    std::function<uint8_t(uint16_t)>  ReadIoByte;
    std::function<uint16_t(uint16_t)> ReadIoWord;
    std::function<void(uint16_t, uint8_t)>  WriteIoByte;
    std::function<void(uint16_t, uint16_t)> WriteIoWord;

    // --- FCW bits ---
    static constexpr uint16_t FCW_SEG  = 0x4000;  // Segmented mode (Z8001/Z8003)
    static constexpr uint16_t FCW_SN   = 0x2000;  // 1=System, 0=Normal
    static constexpr uint16_t FCW_EPA  = 0x1000;  // Extended Processor Available
    static constexpr uint16_t FCW_VIE  = 0x0800;  // Vectored Interrupt Enable
    static constexpr uint16_t FCW_NVIE = 0x0400;  // Non-Vectored Interrupt Enable
    static constexpr uint16_t FCW_C    = 0x0080;  // Carry
    static constexpr uint16_t FCW_Z    = 0x0040;  // Zero
    static constexpr uint16_t FCW_S    = 0x0020;  // Sign
    static constexpr uint16_t FCW_PV   = 0x0010;  // Parity / Overflow (mode-dep.)
    static constexpr uint16_t FCW_DA   = 0x0008;  // Decimal Adjust (op-type flag)
    static constexpr uint16_t FCW_H    = 0x0004;  // Half-Carry

    bool GetFlag(uint16_t f) const { return (FCW & f) != 0; }
    void SetFlag(uint16_t f, bool v) { if (v) FCW |= f; else FCW &= ~f; }

    bool IsSystem() const { return (FCW & FCW_SN) != 0; }

    // --- Byte register access (RH0, RL0, RH1, RL1, ..., RH7, RL7) ---
    // Field encoding in instructions: 0=RH0, 1=RH1, ..., 7=RH7,
    //                                  8=RL0, 9=RL1, ..., 15=RL7
    uint8_t GetByteReg(int n) const {
        if (n < 8) return static_cast<uint8_t>(R[n] >> 8);       // RHn = high byte
        else       return static_cast<uint8_t>(R[n - 8] & 0xFF); // RLn = low byte
    }
    void SetByteReg(int n, uint8_t val) {
        if (n < 8) R[n] = static_cast<uint16_t>((R[n] & 0x00FF) | (uint16_t(val) << 8));
        else       R[n - 8] = static_cast<uint16_t>((R[n - 8] & 0xFF00) | val);
    }

    // --- Long (32-bit) register access: RR0, RR2, ..., RR14 ---
    uint32_t GetLongReg(int n) const {
        return (static_cast<uint32_t>(R[n & 0xE]) << 16) |
                static_cast<uint32_t>(R[(n & 0xE) + 1]);
    }
    void SetLongReg(int n, uint32_t val) {
        R[n & 0xE]       = static_cast<uint16_t>(val >> 16);
        R[(n & 0xE) + 1] = static_cast<uint16_t>(val & 0xFFFF);
    }

    // --- Quad (64-bit) register access: RQ0, RQ4, RQ8, RQ12 ---
    uint64_t GetQuadReg(int n) const {
        return (static_cast<uint64_t>(GetLongReg(n & 0xC)) << 32) |
                static_cast<uint64_t>(GetLongReg((n & 0xC) + 2));
    }
    void SetQuadReg(int n, uint64_t val) {
        SetLongReg(n & 0xC,       static_cast<uint32_t>(val >> 32));
        SetLongReg((n & 0xC) + 2, static_cast<uint32_t>(val & 0xFFFFFFFFu));
    }

    // --- Stack pointer helpers ---
    // Non-segmented: SP = R15. Segmented: SP = RR14 (R14=seg, R15=offset).
    // All Phase 1 code uses R[15] directly.
    uint16_t GetSP() const { return R[15]; }
    void SetSP(uint16_t val) { R[15] = val; }

    // Exchange R[14]/R[15] with ShadowR14/ShadowR15 when S/N changes.
    void SwapStackBanks();

    // --- Lifecycle ---
    void Reset();

    // Execute one instruction. Returns cycles consumed (0 if halted and no
    // interrupt was taken).
    int ExecuteOne();

    // Handle highest-priority pending interrupt. Returns cycles consumed
    // (0 if no interrupt was taken).
    int CheckInterrupts();

    // --- Disassembly (implemented in Z8000Disasm.cpp) ---
    struct DisasmResult {
        std::string mnemonic;
        std::string operands;
        std::string rawBytes;
        int length;
    };
    DisasmResult Disassemble(uint16_t addr) const;

    // --- Helpers (used by instruction handlers) ---
    uint16_t FetchWord();   // fetches at PC, advances PC by 2
    void     Push16(uint16_t val);
    uint16_t Pop16();
    void     Push32(uint32_t val);
    uint32_t Pop32();

    // Flag setters for common ALU results
    void UpdateFlagsByte(uint8_t result);
    void UpdateFlagsWord(uint16_t result);
    void UpdateFlagsLong(uint32_t result);

    // Arithmetic primitives with flag updates
    uint8_t  DoAddByte(uint8_t a, uint8_t b, bool withCarry);
    uint16_t DoAddWord(uint16_t a, uint16_t b, bool withCarry);
    uint8_t  DoSubByte(uint8_t a, uint8_t b, bool withCarry);
    uint16_t DoSubWord(uint16_t a, uint16_t b, bool withCarry);

    // Trap/exception entry (vector offset from PSAP).
    // identifier is the optional "reason code" stored on the stack.
    void TakeException(uint16_t vectorOffset, bool pushIdentifier, uint16_t identifier);
};
