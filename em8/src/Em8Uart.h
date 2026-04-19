/*
 * Em8Uart.h - EM8 UART Peripheral
 *
 * Registers at base+offset ($F000):
 *   +0 UART_DR:  read = pop RX FIFO, write = TX (call TxCallback)
 *   +1 UART_SR:  bit0 = RX ready, bit1 = TX empty (always 1), bit2 = RX overrun
 *   +2 UART_CR:  bit0 = RX IRQ enable, bit1 = TX IRQ enable
 *
 * RX FIFO: circular buffer of 8 bytes.
 *
 * Copyright (c) 2026 emfe Project
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <cstdint>
#include <functional>
#include <mutex>

class Em8Uart {
public:
    static constexpr int FIFO_SIZE = 8;

    // TX callback: called when guest writes to UART_DR
    std::function<void(uint8_t)> TxCallback;

    uint8_t Read(uint8_t offset);
    void Write(uint8_t offset, uint8_t val);

    // Called from host to push a character into the RX FIFO
    void ReceiveChar(uint8_t ch);

    // Returns true if UART has a pending IRQ
    bool HasPendingIrq() const;

    void Reset();

private:
    uint8_t m_rxFifo[FIFO_SIZE]{};
    int m_rxHead = 0;
    int m_rxTail = 0;
    int m_rxCount = 0;
    bool m_rxOverrun = false;

    uint8_t m_cr = 0; // Control register
    std::mutex m_mutex;
};
