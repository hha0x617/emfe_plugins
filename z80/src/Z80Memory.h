/*
 * Z80Memory.h - 64 KiB flat memory + 256-byte I/O space
 *
 * SPDX-License-Identifier: MIT OR Apache-2.0
 * Copyright (c) 2026 hha0x617
 */
#pragma once
#include <cstdint>
#include <array>

// Phase 1.1 — declaration-only scaffold. Memory access methods are
// implemented in Phase 1.2 alongside the UART MMIO wiring.
class Z80Memory {
public:
    std::array<uint8_t, 0x10000> Ram{};

    void Reset();
};
