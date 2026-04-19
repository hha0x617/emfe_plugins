/*
 * Em8Timer.h - EM8 Timer Peripheral
 *
 * Registers at base+offset ($F010):
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

class Em8Timer {
public:
    // Tick the timer by the given number of CPU cycles
    void Tick(int cycles);

    uint8_t Read(uint8_t offset);
    void Write(uint8_t offset, uint8_t val);

    // Returns true if timer has a pending IRQ (overflow && IRQ enable)
    bool HasPendingIrq() const;

    void Reset();

private:
    uint16_t m_reload = 0;    // Reload value (TMR_HI:TMR_LO)
    uint16_t m_counter = 0;   // Current counter
    uint8_t m_cr = 0;         // Control register
    bool m_overflow = false;   // Overflow flag
};
