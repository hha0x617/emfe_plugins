/*
 * Z80Memory.h - 64 KiB flat memory + 256-byte separate I/O space
 *
 * The Z80 has two address spaces:
 *   - 64 KiB main memory addressed by `LD` / `ST` and friends
 *   - 256-byte I/O space addressed by `IN` / `OUT`
 *
 * Main memory is a flat std::array<uint8_t, 0x10000>. I/O space reads
 * and writes are dispatched to the attached peripherals (Uart, Timer,
 * disk in later phases) based on the port number. Ports without a
 * mapped peripheral read as 0xFF and writes are dropped.
 *
 * See docs/mmio.md for the canonical port allocation.
 *
 * SPDX-License-Identifier: MIT OR Apache-2.0
 * Copyright (c) 2026 hha0x617
 */
#pragma once
#include <cstdint>
#include <array>
#include <vector>

class Z80Uart;
class Z80Timer;
class Z80DiskDevice;

class Z80Memory {
public:
    std::array<uint8_t, 0x10000> Ram{};

    Z80Uart*       Uart  = nullptr;
    Z80Timer*      Timer = nullptr;
    Z80DiskDevice* Disk  = nullptr;

    // ---- Phase 6.3 watchpoint state ---------------------------------
    // A watchpoint covers a [addr, addr+size) byte range. ReadByte /
    // WriteByte walk the vector and set `watchpointHit` if any WP
    // matches (with the access direction matching the WP's type).
    // ExecuteOne in Z80Cpu observes the flag after each instruction
    // and returns the -1 sentinel (same path as Phase 6.2's BP hit)
    // so the run-loop auto-halts.
    enum WpKind {
        WP_READ      = 0,
        WP_WRITE     = 1,
        WP_READWRITE = 2,
    };
    struct Watchpoint {
        uint16_t addr;
        uint8_t  size;       // 1 / 2 / 4 bytes of coverage
        uint8_t  kind;       // WpKind
        bool     enabled;
    };
    std::vector<Watchpoint> watchpoints;
    mutable bool watchpointHit = false;   // mutable so const ReadByte sets it

    // Clears `watchpointHit` and returns the previous value. Called
    // by Z80Cpu::ExecuteOne after instruction dispatch.
    bool TakeWatchpointHit() {
        bool h = watchpointHit;
        watchpointHit = false;
        return h;
    }

    void Reset();

    // ---- Main memory access (called from the CPU core) ---------------
    // ReadByte / WriteByte are the canonical bus operations; Read/Write
    // Word are little-endian helpers used by 16-bit Z80 instructions.
    uint8_t  ReadByte (uint16_t addr) const;
    void     WriteByte(uint16_t addr, uint8_t val);
    uint16_t ReadWord (uint16_t addr) const;
    void     WriteWord(uint16_t addr, uint16_t val);

    // Peek / poke skip any side-effect machinery (debugger use).
    // IMPORTANT: peek/poke do NOT fire watchpoints — the host's
    // debugger pane reads memory to render it; those reads must not
    // pretend the guest accessed the byte.
    uint8_t  Peek(uint16_t addr) const { return Ram[addr]; }
    void     Poke(uint16_t addr, uint8_t val) { Ram[addr] = val; }

    // ---- I/O space access (IN / OUT) ---------------------------------
    // The Z80 uses the high byte of BC as the high byte of the I/O
    // address for `IN A,(C)` / `OUT (C),A`, but the bottom 8 bits are
    // the actual port number in every real-world use we care about.
    // Take an 8-bit port and let callers decide whether to mask.
    uint8_t  ReadIo (uint8_t port);
    void     WriteIo(uint8_t port, uint8_t val);

private:
    // Walk the WP vector; if any enabled WP matches (`addr` is within
    // [wp.addr, wp.addr+wp.size) AND the direction matches wp.kind),
    // set `watchpointHit`. mutable for use from const ReadByte.
    void CheckWatchpoint(uint16_t addr, bool isWrite) const;
};
