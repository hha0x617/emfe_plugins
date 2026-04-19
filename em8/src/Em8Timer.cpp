/*
 * Em8Timer.cpp - EM8 Timer Peripheral Implementation
 *
 * Copyright (c) 2026 emfe Project
 * SPDX-License-Identifier: MIT
 */
#include "pch.h"
#include "Em8Timer.h"

void Em8Timer::Tick(int cycles)
{
    if (!(m_cr & 0x01)) // Not enabled
        return;

    for (int i = 0; i < cycles; i++) {
        if (m_counter == 0) {
            m_overflow = true;
            if (m_cr & 0x02) { // Auto-reload
                m_counter = m_reload;
            } else {
                m_cr &= ~0x01; // Disable on overflow if no auto-reload
                return;
            }
        } else {
            m_counter--;
        }
    }
}

uint8_t Em8Timer::Read(uint8_t offset)
{
    switch (offset) {
    case 0: // TMR_LO
        return static_cast<uint8_t>(m_reload & 0xFF);
    case 1: // TMR_HI
        return static_cast<uint8_t>(m_reload >> 8);
    case 2: // TMR_CTR_LO (read-only)
        return static_cast<uint8_t>(m_counter & 0xFF);
    case 3: // TMR_CTR_HI (read-only)
        return static_cast<uint8_t>(m_counter >> 8);
    case 4: // TMR_CR
        return m_cr;
    case 5: // TMR_SR
        return m_overflow ? 0x01 : 0x00;
    default:
        return 0;
    }
}

void Em8Timer::Write(uint8_t offset, uint8_t val)
{
    switch (offset) {
    case 0: // TMR_LO
        m_reload = (m_reload & 0xFF00) | val;
        break;
    case 1: // TMR_HI
        m_reload = (m_reload & 0x00FF) | (static_cast<uint16_t>(val) << 8);
        break;
    case 2: // TMR_CTR_LO (read-only, ignore writes)
        break;
    case 3: // TMR_CTR_HI (read-only, ignore writes)
        break;
    case 4: // TMR_CR
        m_cr = val;
        if (val & 0x01) {
            // Starting the timer: load counter from reload value
            m_counter = m_reload;
        }
        break;
    case 5: // TMR_SR (read-only, ignore writes)
        break;
    case 6: // TMR_ACK: write to clear overflow
        m_overflow = false;
        break;
    default:
        break;
    }
}

bool Em8Timer::HasPendingIrq() const
{
    return m_overflow && (m_cr & 0x04); // Overflow && IRQ enable
}

void Em8Timer::Reset()
{
    m_reload = 0;
    m_counter = 0;
    m_cr = 0;
    m_overflow = false;
}
