/*
 * Z8000Uart.h - Simple UART Peripheral (Phase 1 stand-in for Z8530 SCC)
 *
 * Z8000 I/O port space. Base port $FE00:
 *   +0 UART_DR:  read = pop RX FIFO, write = TX (calls TxCallback)
 *   +1 UART_SR:  bit0 = RX ready, bit1 = TX empty, bit2 = RX overrun
 *   +2 UART_CR:  bit0 = RX IRQ enable, bit1 = TX IRQ enable
 *
 * Phase 2 will replace this with a proper Z8530 SCC implementation.
 *
 * Copyright (c) 2026 emfe Project
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <cstdint>
#include <functional>
#include <mutex>

class Z8000Uart {
public:
    static constexpr int FIFO_SIZE = 16;

    // TX callback: called when guest writes to UART_DR
    std::function<void(uint8_t)> TxCallback;

    uint8_t Read(uint8_t offset);
    void    Write(uint8_t offset, uint8_t val);

    // Called from host to push a character into the RX FIFO
    void ReceiveChar(uint8_t ch);

    bool HasPendingIrq() const;

    void Reset();

private:
    uint8_t m_rxFifo[FIFO_SIZE]{};
    int m_rxHead = 0;
    int m_rxTail = 0;
    int m_rxCount = 0;
    bool m_rxOverrun = false;

    uint8_t m_cr = 0;
    mutable std::mutex m_mutex;
};
