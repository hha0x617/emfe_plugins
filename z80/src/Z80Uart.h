/*
 * Z80Uart.h - Console UART for the Z80 I/O space
 *
 * Port allocation (see docs/mmio.md):
 *   0xFE r/w : data port
 *               IN  pops the next byte from the RX FIFO (0 if empty)
 *               OUT transmits the byte to the host via TxCallback
 *   0xFF r   : status byte
 *               bit 0 TX_READY  (always 1 — TX is non-blocking)
 *               bit 1 RX_AVAIL  (FIFO has at least one byte)
 *               bit 2 RX_OVERRUN (sticky — set when ReceiveChar drops)
 *   0xFF w   : ignored (reserved)
 *
 * The RX FIFO is 8 bytes deep, populated by the host through
 * ReceiveChar (which the plugin's emfe_send_char / emfe_send_string
 * ABI ultimately funnels into).
 *
 * SPDX-License-Identifier: MIT OR Apache-2.0
 * Copyright (c) 2026 hha0x617
 */
#pragma once
#include <cstdint>
#include <functional>
#include <mutex>

class Z80Uart {
public:
    static constexpr int FIFO_SIZE = 8;

    // Called whenever the guest writes to port 0xFE (TX). The host
    // wires this to its EmfeConsoleCharCallback.
    std::function<void(uint8_t)> TxCallback;

    // Called by the CPU core's IN / OUT dispatch via Z80Memory.
    uint8_t Read (uint8_t port);
    void    Write(uint8_t port, uint8_t val);

    // Called from the host when there is a new RX byte for the guest.
    void ReceiveChar(uint8_t ch);

    void Reset();

private:
    uint8_t  m_rxFifo[FIFO_SIZE]{};
    int      m_rxHead = 0;
    int      m_rxTail = 0;
    int      m_rxCount = 0;
    bool     m_rxOverrun = false;
    std::mutex m_mutex;
};
