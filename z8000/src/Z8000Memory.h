/*
 * Z8000Memory.h - Z8000 Family Memory Subsystem
 *
 * Phase 1: 64KB flat address space (Z8002/Z8004, non-segmented).
 * Phase 2+: will widen to 23-bit addressing for Z8001/Z8003 segmented mode.
 *
 * Z8000 is big-endian: words are stored with high byte at the lower address.
 * Word accesses must be aligned to even addresses.
 *
 * Peripherals live in the separate I/O port space (see Z8000Cpu IN/OUT
 * instructions), not in memory. This keeps RAM uncluttered and matches
 * typical Z8000 system designs (e.g., Olivetti M20).
 *
 * Copyright (c) 2026 emfe Project
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <cstdint>
#include <array>
#include <atomic>
#include <unordered_set>
#include <mutex>

class Z8000Memory {
public:
    std::array<uint8_t, 65536> Data{};

    // Watchpoint support (byte granularity)
    std::unordered_set<uint16_t> ReadWatchpoints;
    std::unordered_set<uint16_t> WriteWatchpoints;
    std::atomic<bool> WatchpointHit{false};
    std::atomic<uint16_t> WatchpointHitAddress{0};
    std::mutex WatchpointMutex;

    // CPU-visible access (fires watchpoints).
    uint8_t  ReadByte(uint16_t addr);
    uint16_t ReadWord(uint16_t addr);   // big-endian; addr should be even
    uint32_t ReadLong(uint16_t addr);   // big-endian; addr should be even

    void WriteByte(uint16_t addr, uint8_t val);
    void WriteWord(uint16_t addr, uint16_t val);
    void WriteLong(uint16_t addr, uint32_t val);

    // Debugger access (no watchpoints, no side effects).
    uint8_t  PeekByte(uint16_t addr) const { return Data[addr]; }
    uint16_t PeekWord(uint16_t addr) const {
        return static_cast<uint16_t>(Data[addr]) << 8 |
               static_cast<uint16_t>(Data[static_cast<uint16_t>(addr + 1)]);
    }
    uint32_t PeekLong(uint16_t addr) const {
        return static_cast<uint32_t>(PeekWord(addr)) << 16 |
               static_cast<uint32_t>(PeekWord(static_cast<uint16_t>(addr + 2)));
    }
    void PokeByte(uint16_t addr, uint8_t val) { Data[addr] = val; }
    void PokeWord(uint16_t addr, uint16_t val) {
        Data[addr] = static_cast<uint8_t>(val >> 8);
        Data[static_cast<uint16_t>(addr + 1)] = static_cast<uint8_t>(val & 0xFF);
    }
    void PokeLong(uint16_t addr, uint32_t val) {
        PokeWord(addr, static_cast<uint16_t>(val >> 16));
        PokeWord(static_cast<uint16_t>(addr + 2), static_cast<uint16_t>(val & 0xFFFF));
    }
};
