// SPDX-License-Identifier: MIT OR Apache-2.0
// Copyright (c) 2026 hha0x617
//
// Z80Memory.cpp - 64 KiB flat memory + 256-byte I/O space dispatch.

#include "pch.h"
#include "Z80Memory.h"
#include "Z80Uart.h"
#include "Z80Timer.h"
#include "Z80DiskDevice.h"

void Z80Memory::Reset() {
    Ram.fill(0);
}

uint8_t Z80Memory::ReadByte(uint16_t addr) const {
    CheckWatchpoint(addr, /*isWrite*/false);
    return Ram[addr];
}

void Z80Memory::WriteByte(uint16_t addr, uint8_t val) {
    Ram[addr] = val;
    CheckWatchpoint(addr, /*isWrite*/true);
}

void Z80Memory::CheckWatchpoint(uint16_t addr, bool isWrite) const {
    for (const auto& wp : watchpoints) {
        if (!wp.enabled) continue;
        if (addr < wp.addr || addr >= wp.addr + wp.size) continue;
        // Direction filter: WP_READ fires only on reads, WP_WRITE
        // only on writes, WP_READWRITE on both.
        if (isWrite) {
            if (wp.kind == WP_READ) continue;
        } else {
            if (wp.kind == WP_WRITE) continue;
        }
        watchpointHit = true;
        return;     // one hit is enough; no point scanning further
    }
}

uint16_t Z80Memory::ReadWord(uint16_t addr) const {
    // Z80 is little-endian.
    uint16_t lo = Ram[addr];
    uint16_t hi = Ram[static_cast<uint16_t>(addr + 1)];
    return static_cast<uint16_t>(lo | (hi << 8));
}

void Z80Memory::WriteWord(uint16_t addr, uint16_t val) {
    Ram[addr] = static_cast<uint8_t>(val & 0xFF);
    Ram[static_cast<uint16_t>(addr + 1)] = static_cast<uint8_t>((val >> 8) & 0xFF);
}

uint8_t Z80Memory::ReadIo(uint8_t port) {
    // Console UART: ports 0xFE (data) and 0xFF (status). See docs/mmio.md.
    if (Uart && (port == 0xFE || port == 0xFF)) {
        return Uart->Read(port);
    }
    // Disk MMIO: ports 0xF0..0xF7. See docs/phase4_design.md §1.
    if (Disk && port >= 0xF0 && port <= 0xF7) {
        return Disk->ReadIo(port);
    }
    // Unmapped ports read 0xFF (the canonical "nothing here" bus value).
    return 0xFF;
}

void Z80Memory::WriteIo(uint8_t port, uint8_t val) {
    if (Uart && (port == 0xFE || port == 0xFF)) {
        Uart->Write(port, val);
        return;
    }
    if (Disk && port >= 0xF0 && port <= 0xF7) {
        Disk->WriteIo(port, val);
        return;
    }
    // Unmapped ports: drop the byte silently.
}
