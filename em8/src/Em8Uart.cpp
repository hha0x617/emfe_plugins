/*
 * Em8Uart.cpp - EM8 UART Peripheral Implementation
 *
 * Copyright (c) 2026 emfe Project
 * SPDX-License-Identifier: MIT
 */
#include "pch.h"
#include "Em8Uart.h"

uint8_t Em8Uart::Read(uint8_t offset)
{
    switch (offset) {
    case 0: { // UART_DR: pop RX FIFO
        std::lock_guard lock(m_mutex);
        if (m_rxCount == 0)
            return 0;
        uint8_t ch = m_rxFifo[m_rxTail];
        m_rxTail = (m_rxTail + 1) % FIFO_SIZE;
        m_rxCount--;
        return ch;
    }
    case 1: { // UART_SR
        std::lock_guard lock(m_mutex);
        uint8_t sr = 0;
        if (m_rxCount > 0)  sr |= 0x01; // RX ready
        sr |= 0x02;                       // TX empty (always ready)
        if (m_rxOverrun)    sr |= 0x04; // RX overrun
        return sr;
    }
    case 2: // UART_CR
        return m_cr;
    default:
        return 0;
    }
}

void Em8Uart::Write(uint8_t offset, uint8_t val)
{
    switch (offset) {
    case 0: // UART_DR: TX
        if (TxCallback) {
            TxCallback(val);
        }
        break;
    case 1: // UART_SR: writing clears overrun
    {
        std::lock_guard lock(m_mutex);
        m_rxOverrun = false;
        break;
    }
    case 2: // UART_CR
        m_cr = val;
        break;
    default:
        break;
    }
}

void Em8Uart::ReceiveChar(uint8_t ch)
{
    std::lock_guard lock(m_mutex);
    if (m_rxCount >= FIFO_SIZE) {
        m_rxOverrun = true;
        return;
    }
    m_rxFifo[m_rxHead] = ch;
    m_rxHead = (m_rxHead + 1) % FIFO_SIZE;
    m_rxCount++;
}

bool Em8Uart::HasPendingIrq() const
{
    // RX IRQ: RX ready && RX IRQ enable
    // TX IRQ: TX empty && TX IRQ enable (TX is always empty)
    bool rxIrq = (m_cr & 0x01) && (m_rxCount > 0);
    bool txIrq = (m_cr & 0x02) != 0; // TX always empty
    return rxIrq || txIrq;
}

void Em8Uart::Reset()
{
    std::lock_guard lock(m_mutex);
    m_rxHead = 0;
    m_rxTail = 0;
    m_rxCount = 0;
    m_rxOverrun = false;
    m_cr = 0;
}
