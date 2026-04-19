/*
 * Em8Memory.cpp - EM8 Memory Subsystem Implementation
 *
 * Copyright (c) 2026 emfe Project
 * SPDX-License-Identifier: MIT
 */
#include "pch.h"
#include "Em8Memory.h"
#include "Em8Uart.h"
#include "Em8Timer.h"

uint8_t Em8Memory::Read(uint16_t addr)
{
    // Check read watchpoints
    if (!ReadWatchpoints.empty()) {
        std::lock_guard lock(WatchpointMutex);
        if (ReadWatchpoints.count(addr)) {
            WatchpointHit.store(true, std::memory_order_release);
            WatchpointHitAddress.store(addr, std::memory_order_release);
        }
    }

    // Memory-mapped I/O: $F000-$F03F
    if (addr >= 0xF000 && addr <= 0xF03F) {
        uint8_t offset = static_cast<uint8_t>(addr - 0xF000);

        if (offset <= 0x0F && Uart) {
            return Uart->Read(offset);
        }
        if (offset >= 0x10 && offset <= 0x1F && Timer) {
            return Timer->Read(static_cast<uint8_t>(offset - 0x10));
        }
        // $F020-$F02F: GPIO — just return Data[]
        // $F030-$F03F: Interrupt controller — just return Data[]
    }

    return Data[addr];
}

void Em8Memory::Write(uint16_t addr, uint8_t val)
{
    // Check write watchpoints
    if (!WriteWatchpoints.empty()) {
        std::lock_guard lock(WatchpointMutex);
        if (WriteWatchpoints.count(addr)) {
            WatchpointHit.store(true, std::memory_order_release);
            WatchpointHitAddress.store(addr, std::memory_order_release);
        }
    }

    // Memory-mapped I/O: $F000-$F03F
    if (addr >= 0xF000 && addr <= 0xF03F) {
        uint8_t offset = static_cast<uint8_t>(addr - 0xF000);

        if (offset <= 0x0F && Uart) {
            Uart->Write(offset, val);
            return;
        }
        if (offset >= 0x10 && offset <= 0x1F && Timer) {
            Timer->Write(static_cast<uint8_t>(offset - 0x10), val);
            return;
        }
        // $F020-$F02F: GPIO — fall through to Data[]
        // $F030-$F03F: Interrupt controller — fall through to Data[]
    }

    Data[addr] = val;
}

uint8_t Em8Memory::Peek(uint16_t addr) const
{
    return Data[addr];
}

void Em8Memory::Poke(uint16_t addr, uint8_t val)
{
    Data[addr] = val;
}
