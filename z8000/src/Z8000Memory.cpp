/*
 * Z8000Memory.cpp - Z8000 Family Memory Subsystem Implementation
 *
 * Copyright (c) 2026 emfe Project
 * SPDX-License-Identifier: MIT
 */
#include "pch.h"
#include "Z8000Memory.h"

namespace {
    inline void CheckReadWp(Z8000Memory& m, uint16_t addr) {
        if (m.ReadWatchpoints.empty()) return;
        std::lock_guard lock(m.WatchpointMutex);
        if (m.ReadWatchpoints.count(addr)) {
            m.WatchpointHit.store(true, std::memory_order_release);
            m.WatchpointHitAddress.store(addr, std::memory_order_release);
        }
    }
    inline void CheckWriteWp(Z8000Memory& m, uint16_t addr) {
        if (m.WriteWatchpoints.empty()) return;
        std::lock_guard lock(m.WatchpointMutex);
        if (m.WriteWatchpoints.count(addr)) {
            m.WatchpointHit.store(true, std::memory_order_release);
            m.WatchpointHitAddress.store(addr, std::memory_order_release);
        }
    }
}

uint8_t Z8000Memory::ReadByte(uint16_t addr) {
    CheckReadWp(*this, addr);
    return Data[addr];
}

uint16_t Z8000Memory::ReadWord(uint16_t addr) {
    CheckReadWp(*this, addr);
    CheckReadWp(*this, static_cast<uint16_t>(addr + 1));
    return static_cast<uint16_t>(Data[addr]) << 8 |
           static_cast<uint16_t>(Data[static_cast<uint16_t>(addr + 1)]);
}

uint32_t Z8000Memory::ReadLong(uint16_t addr) {
    return static_cast<uint32_t>(ReadWord(addr)) << 16 |
           static_cast<uint32_t>(ReadWord(static_cast<uint16_t>(addr + 2)));
}

void Z8000Memory::WriteByte(uint16_t addr, uint8_t val) {
    CheckWriteWp(*this, addr);
    Data[addr] = val;
}

void Z8000Memory::WriteWord(uint16_t addr, uint16_t val) {
    CheckWriteWp(*this, addr);
    CheckWriteWp(*this, static_cast<uint16_t>(addr + 1));
    Data[addr] = static_cast<uint8_t>(val >> 8);
    Data[static_cast<uint16_t>(addr + 1)] = static_cast<uint8_t>(val & 0xFF);
}

void Z8000Memory::WriteLong(uint16_t addr, uint32_t val) {
    WriteWord(addr, static_cast<uint16_t>(val >> 16));
    WriteWord(static_cast<uint16_t>(addr + 2), static_cast<uint16_t>(val & 0xFFFF));
}
