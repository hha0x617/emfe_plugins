/*
 * Z80DiskDevice.h - Sector-device disk MMIO (Phase 4.1)
 *
 * Implements the minimal "sector device" specified in
 * docs/phase4_design.md §1. Exposes 8 I/O ports (0xF0 - 0xF7):
 *
 *   0xF0 r/w  LBA byte 0 (bits 0..7)
 *   0xF1 r/w  LBA byte 1 (bits 8..15)
 *   0xF2 r/w  LBA byte 2 (bits 16..23)
 *   0xF3 r/w  Sector buffer pointer low (guest RAM)
 *   0xF4 r/w  Sector buffer pointer high
 *   0xF5 w    Command register (READ=0x01, WRITE=0x02, NOP=0x00)
 *   0xF6 r    Status register (READY/ERROR/WRITE_PROTECT/IMAGE_LOADED)
 *   0xF7 r    Reserved — reads 0 in Phase 4
 *
 * Sector size is fixed at 128 bytes (CP/M 2.2 native record). The
 * CBIOS handles deblocking for any larger logical sector size.
 *
 * SPDX-License-Identifier: MIT OR Apache-2.0
 * Copyright (c) 2026 hha0x617
 */
#pragma once
#include <cstdint>
#include <vector>

class Z80Memory;

class Z80DiskDevice {
public:
    static constexpr int      k_sectorBytes = 128;
    static constexpr uint8_t  k_cmdNop      = 0x00;
    static constexpr uint8_t  k_cmdRead     = 0x01;
    static constexpr uint8_t  k_cmdWrite    = 0x02;

    // Status bits — match docs/phase4_design.md §1.4.
    static constexpr uint8_t  k_statReady         = 0x01;
    static constexpr uint8_t  k_statError         = 0x02;
    static constexpr uint8_t  k_statWriteProtect  = 0x04;
    static constexpr uint8_t  k_statImageLoaded   = 0x08;

    // Wired by the plugin during construction.
    Z80Memory* Memory = nullptr;

    // ---- Host-side mount API -----------------------------------------
    // SetImage replaces the currently-mounted image (if any) with the
    // supplied bytes. Empty `bytes` unmounts. Size must be a multiple
    // of k_sectorBytes for sane behaviour; the device does not enforce
    // that here, but reads past the end set the ERROR bit.
    void SetImage(std::vector<uint8_t> bytes, bool writeProtect);

    // True if an image is currently mounted (size > 0).
    bool HasImage() const { return !m_image.empty(); }
    // Current image size in bytes (0 if unmounted).
    uint32_t ImageSizeBytes() const { return static_cast<uint32_t>(m_image.size()); }
    // True if the mounted image is read-only.
    bool IsWriteProtected() const { return m_writeProtect; }

    // Read a single byte from the mounted image at the given offset
    // for verification in tests. Returns 0 if out of range.
    uint8_t PeekImageByte(uint32_t offset) const {
        if (offset >= m_image.size()) return 0;
        return m_image[offset];
    }
    // Write a single byte into the mounted image for test setup.
    // No-op if out of range. Bypasses write-protect since this is a
    // host-side hook, not a guest-side MMIO path.
    void PokeImageByte(uint32_t offset, uint8_t value) {
        if (offset < m_image.size()) m_image[offset] = value;
    }

    // ---- Reset wiring (called from Z80Memory::Reset) -----------------
    // Clears LBA, buffer pointer, and error sticky bit. The mounted
    // image is preserved (Settings reload is a separate operation).
    void Reset();

    // ---- I/O port surface --------------------------------------------
    uint8_t  ReadIo (uint8_t port);
    void     WriteIo(uint8_t port, uint8_t val);

private:
    std::vector<uint8_t> m_image;
    bool     m_writeProtect = false;

    uint32_t m_lba           = 0;   // 24-bit value, top byte ignored
    uint16_t m_bufPtr        = 0;
    bool     m_errorSticky   = false;

    void ExecuteCommand(uint8_t cmd);
};
