// SPDX-License-Identifier: MIT OR Apache-2.0
// Copyright (c) 2026 hha0x617
//
// Z80DiskDevice.cpp - Sector-device disk MMIO. See header for the
// port map; see docs/phase4_design.md §1 for the full contract.

#include "pch.h"
#include "Z80DiskDevice.h"
#include "Z80Memory.h"

void Z80DiskDevice::SetImage(std::vector<uint8_t> bytes, bool writeProtect) {
    m_image        = std::move(bytes);
    m_writeProtect = writeProtect;
    // Mounting always clears the sticky error and resets the
    // addressing registers — a fresh mount should not inherit a
    // failed-command state from the previous image.
    m_lba         = 0;
    m_bufPtr      = 0;
    m_errorSticky = false;
}

void Z80DiskDevice::Reset() {
    m_lba         = 0;
    m_bufPtr      = 0;
    m_errorSticky = false;
}

uint8_t Z80DiskDevice::ReadIo(uint8_t port) {
    switch (port) {
    case 0xF0: return static_cast<uint8_t>(m_lba & 0xFF);
    case 0xF1: return static_cast<uint8_t>((m_lba >> 8) & 0xFF);
    case 0xF2: return static_cast<uint8_t>((m_lba >> 16) & 0xFF);
    case 0xF3: return static_cast<uint8_t>(m_bufPtr & 0xFF);
    case 0xF4: return static_cast<uint8_t>((m_bufPtr >> 8) & 0xFF);
    case 0xF5: return 0xFF;     // command is write-only; reads return bus noise
    case 0xF6: {
        uint8_t s = k_statReady;
        if (HasImage())         s |= k_statImageLoaded;
        if (m_writeProtect)     s |= k_statWriteProtect;
        if (m_errorSticky)      s |= k_statError;
        return s;
    }
    case 0xF7: return 0;
    default:   return 0xFF;
    }
}

void Z80DiskDevice::WriteIo(uint8_t port, uint8_t val) {
    switch (port) {
    case 0xF0: m_lba = (m_lba & 0xFFFF00u) |  val;                       break;
    case 0xF1: m_lba = (m_lba & 0xFF00FFu) | (static_cast<uint32_t>(val) << 8);  break;
    case 0xF2: m_lba = (m_lba & 0x00FFFFu) | (static_cast<uint32_t>(val) << 16); break;
    case 0xF3: m_bufPtr = static_cast<uint16_t>((m_bufPtr & 0xFF00) | val);                  break;
    case 0xF4: m_bufPtr = static_cast<uint16_t>((m_bufPtr & 0x00FF) | (static_cast<uint16_t>(val) << 8)); break;
    case 0xF5: ExecuteCommand(val); break;
    // 0xF6 status is read-only; 0xF7 reserved. Both drop writes.
    case 0xF6:
    case 0xF7:
    default:
        break;
    }
}

void Z80DiskDevice::ExecuteCommand(uint8_t cmd) {
    switch (cmd) {
    case k_cmdNop:
        // Writing NOP doubles as "clear sticky error" — the only way
        // for guest software to reset the ERROR bit short of remount.
        m_errorSticky = false;
        return;

    case k_cmdRead: {
        if (!HasImage()) { m_errorSticky = true; return; }
        uint32_t off = m_lba * k_sectorBytes;
        if (off + k_sectorBytes > m_image.size()) { m_errorSticky = true; return; }
        for (int i = 0; i < k_sectorBytes; ++i) {
            uint16_t addr = static_cast<uint16_t>((m_bufPtr + i) & 0xFFFF);
            Memory->Poke(addr, m_image[off + i]);
        }
        return;
    }

    case k_cmdWrite: {
        if (!HasImage())     { m_errorSticky = true; return; }
        if (m_writeProtect)  { m_errorSticky = true; return; }
        uint32_t off = m_lba * k_sectorBytes;
        if (off + k_sectorBytes > m_image.size()) { m_errorSticky = true; return; }
        for (int i = 0; i < k_sectorBytes; ++i) {
            uint16_t addr = static_cast<uint16_t>((m_bufPtr + i) & 0xFFFF);
            m_image[off + i] = Memory->Peek(addr);
        }
        return;
    }

    default:
        m_errorSticky = true;
        return;
    }
}
