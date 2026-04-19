/*
 * Z8000Timer.cpp - Simple Timer Peripheral Implementation
 *
 * Copyright (c) 2026 emfe Project
 * SPDX-License-Identifier: MIT
 */
#include "pch.h"
#include "Z8000Timer.h"

void Z8000Timer::Tick(int cycles) {
    if (!(m_cr & 0x01)) return;
    for (int i = 0; i < cycles; i++) {
        if (m_counter == 0) {
            m_overflow = true;
            if (m_cr & 0x02) {
                m_counter = m_reload;
            } else {
                m_cr &= ~0x01;
                return;
            }
        } else {
            m_counter--;
        }
    }
}

uint8_t Z8000Timer::Read(uint8_t offset) {
    switch (offset) {
    case 0: return static_cast<uint8_t>(m_reload & 0xFF);
    case 1: return static_cast<uint8_t>(m_reload >> 8);
    case 2: return static_cast<uint8_t>(m_counter & 0xFF);
    case 3: return static_cast<uint8_t>(m_counter >> 8);
    case 4: return m_cr;
    case 5: return m_overflow ? 0x01 : 0x00;
    default: return 0;
    }
}

void Z8000Timer::Write(uint8_t offset, uint8_t val) {
    switch (offset) {
    case 0: m_reload = (m_reload & 0xFF00) | val; break;
    case 1: m_reload = (m_reload & 0x00FF) | (static_cast<uint16_t>(val) << 8); break;
    case 2:
    case 3:
        break; // counter is read-only
    case 4:
        m_cr = val;
        if (val & 0x01) m_counter = m_reload; // starting: load from reload
        break;
    case 5: break; // SR read-only
    case 6: m_overflow = false; break; // ACK
    default: break;
    }
}

bool Z8000Timer::HasPendingIrq() const {
    return m_overflow && (m_cr & 0x04);
}

void Z8000Timer::Reset() {
    m_reload = 0;
    m_counter = 0;
    m_cr = 0;
    m_overflow = false;
}
