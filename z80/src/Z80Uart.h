/*
 * Z80Uart.h - Console UART exposed via the Z80 I/O space
 *
 * Port allocation (final values fixed in docs/mmio.md during Phase 1.2):
 *   0xFE r/w : data
 *   0xFF r   : status (bit 0 = TX ready, bit 1 = RX available)
 *
 * SPDX-License-Identifier: MIT OR Apache-2.0
 * Copyright (c) 2026 hha0x617
 */
#pragma once
#include <cstdint>

// Phase 1.1 — declaration-only scaffold.
class Z80Uart {
public:
    void Reset();
};
