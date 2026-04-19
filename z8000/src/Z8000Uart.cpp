/*
 * Z8000Uart.cpp - Simple UART Peripheral Implementation
 *
 * Copyright (c) 2026 emfe Project
 * SPDX-License-Identifier: MIT
 */
#include "pch.h"
#include "Z8000Uart.h"

uint8_t Z8000Uart::Read(uint8_t offset) {
    switch (offset) {
    case 0: { // DR — pop RX FIFO
        std::lock_guard lock(m_mutex);
        if (m_rxCount == 0) return 0;
        uint8_t ch = m_rxFifo[m_rxTail];
        m_rxTail = (m_rxTail + 1) % FIFO_SIZE;
        m_rxCount--;
        return ch;
    }
    case 1: { // SR
        std::lock_guard lock(m_mutex);
        uint8_t sr = 0;
        if (m_rxCount > 0) sr |= 0x01;
        sr |= 0x02;                       // TX always empty (instant transmit)
        if (m_rxOverrun) sr |= 0x04;
        return sr;
    }
    case 2: // CR
        return m_cr;
    default:
        return 0;
    }
}

void Z8000Uart::Write(uint8_t offset, uint8_t val) {
    switch (offset) {
    case 0: // DR — TX
        if (TxCallback) TxCallback(val);
        break;
    case 1: { // SR — writing clears overrun
        std::lock_guard lock(m_mutex);
        m_rxOverrun = false;
        break;
    }
    case 2: // CR
        m_cr = val;
        break;
    default:
        break;
    }
}

void Z8000Uart::ReceiveChar(uint8_t ch) {
    std::lock_guard lock(m_mutex);
    if (m_rxCount >= FIFO_SIZE) {
        m_rxOverrun = true;
        return;
    }
    m_rxFifo[m_rxHead] = ch;
    m_rxHead = (m_rxHead + 1) % FIFO_SIZE;
    m_rxCount++;
}

bool Z8000Uart::HasPendingIrq() const {
    std::lock_guard lock(m_mutex);
    bool rxIrq = (m_cr & 0x01) && (m_rxCount > 0);
    bool txIrq = (m_cr & 0x02) != 0; // TX always empty
    return rxIrq || txIrq;
}

void Z8000Uart::Reset() {
    std::lock_guard lock(m_mutex);
    m_rxHead = m_rxTail = m_rxCount = 0;
    m_rxOverrun = false;
    m_cr = 0;
}
