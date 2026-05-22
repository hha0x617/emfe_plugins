// SPDX-License-Identifier: MIT OR Apache-2.0
// Copyright (c) 2026 hha0x617
//
// Z80Uart.cpp - Console UART implementation.

#include "pch.h"
#include "Z80Uart.h"

// Status bits (see docs/mmio.md)
namespace {
    constexpr uint8_t STATUS_TX_READY  = 0x01;
    constexpr uint8_t STATUS_RX_AVAIL  = 0x02;
    constexpr uint8_t STATUS_RX_OVERRUN = 0x04;
}

uint8_t Z80Uart::Read(uint8_t port) {
    switch (port) {
    case 0xFE: { // data: pop one RX byte
        std::lock_guard lock(m_mutex);
        if (m_rxCount == 0) {
            return 0;
        }
        uint8_t ch = m_rxFifo[m_rxTail];
        m_rxTail = (m_rxTail + 1) % FIFO_SIZE;
        m_rxCount--;
        return ch;
    }
    case 0xFF: { // status
        std::lock_guard lock(m_mutex);
        uint8_t sr = STATUS_TX_READY;
        if (m_rxCount > 0)   sr |= STATUS_RX_AVAIL;
        if (m_rxOverrun)     sr |= STATUS_RX_OVERRUN;
        return sr;
    }
    default:
        return 0xFF;
    }
}

void Z80Uart::Write(uint8_t port, uint8_t val) {
    switch (port) {
    case 0xFE: // data: transmit to host
        if (TxCallback) {
            TxCallback(val);
        }
        break;
    case 0xFF: // status: ignored / reserved
        break;
    default:
        break;
    }
}

void Z80Uart::ReceiveChar(uint8_t ch) {
    std::lock_guard lock(m_mutex);
    if (m_rxCount >= FIFO_SIZE) {
        m_rxOverrun = true;
        return;
    }
    m_rxFifo[m_rxHead] = ch;
    m_rxHead = (m_rxHead + 1) % FIFO_SIZE;
    m_rxCount++;
}

void Z80Uart::Reset() {
    std::lock_guard lock(m_mutex);
    m_rxHead = 0;
    m_rxTail = 0;
    m_rxCount = 0;
    m_rxOverrun = false;
}
