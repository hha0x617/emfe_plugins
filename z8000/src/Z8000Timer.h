/*
 * Z8000Timer.h - Simple Timer Peripheral (Phase 1 stand-in for Z8036 CIO / Z8430 CTC)
 *
 * Z8000 I/O port space. Base port $FE10:
 *   +0 TMR_LO:     reload value low byte
 *   +1 TMR_HI:     reload value high byte
 *   +2 TMR_CTR_LO: current counter low byte (read-only)
 *   +3 TMR_CTR_HI: current counter high byte (read-only)
 *   +4 TMR_CR:     bit0 = enable, bit1 = auto-reload, bit2 = IRQ enable
 *   +5 TMR_SR:     bit0 = overflow flag
 *   +6 TMR_ACK:    write any value to clear overflow
 *
 * Copyright (c) 2026 emfe Project
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <cstdint>

class Z8000Timer {
public:
    void Tick(int cycles);

    uint8_t Read(uint8_t offset);
    void    Write(uint8_t offset, uint8_t val);

    bool HasPendingIrq() const;

    void Reset();

private:
    uint16_t m_reload = 0;
    uint16_t m_counter = 0;
    uint8_t m_cr = 0;
    bool m_overflow = false;
};
