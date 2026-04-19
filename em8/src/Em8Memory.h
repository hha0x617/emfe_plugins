/*
 * Em8Memory.h - EM8 Memory Subsystem
 *
 * 64KB flat address space with memory-mapped I/O at $F000-$F03F.
 *   $F000-$F00F: UART
 *   $F010-$F01F: Timer
 *   $F020-$F02F: GPIO (stored in Data[])
 *   $F030-$F03F: Interrupt controller (stored in Data[])
 *
 * Copyright (c) 2026 emfe Project
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <cstdint>
#include <functional>
#include <array>
#include <atomic>
#include <unordered_set>
#include <mutex>

class Em8Uart;
class Em8Timer;

class Em8Memory {
public:
    std::array<uint8_t, 65536> Data{};

    Em8Uart* Uart = nullptr;
    Em8Timer* Timer = nullptr;

    // Watchpoint support
    std::unordered_set<uint16_t> ReadWatchpoints;
    std::unordered_set<uint16_t> WriteWatchpoints;
    std::atomic<bool> WatchpointHit{false};
    std::atomic<uint16_t> WatchpointHitAddress{0};
    std::mutex WatchpointMutex;

    uint8_t Read(uint16_t addr);
    void Write(uint16_t addr, uint8_t val);

    // Peek/poke bypass I/O side effects (for debugger)
    uint8_t Peek(uint16_t addr) const;
    void Poke(uint16_t addr, uint8_t val);
};
