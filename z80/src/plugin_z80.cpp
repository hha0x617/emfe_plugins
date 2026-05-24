// SPDX-License-Identifier: MIT OR Apache-2.0
// Copyright (c) 2026 hha0x617
//
// plugin_z80.cpp - Zilog Z80 8-bit CPU plugin for emfe.
//
// Phase 1.1 — scaffold only. This file exports every required emfe ABI
// function so the DLL is loadable by the host, but the implementations
// are stubs that return safe defaults. Real behaviour is implemented in
// later Phase 1 PRs (1.2 memory + UART, 1.3 registers, ...).
//
// The structure mirrors em8/src/plugin_em8.cpp so the family stays
// consistent.

#include "emfe_plugin.h"
#include "Z80Cpu.h"
#include "Z80Disasm.h"
#include "Z80Memory.h"
#include "Z80Uart.h"
#include "Z80Timer.h"
#include "Z80DiskDevice.h"

#include <fstream>

#include <cstdint>
#include <cstring>
#include <atomic>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

// ============================================================================
// Register IDs (host displays in declaration order, grouped by `group`)
// ============================================================================

enum RegId : uint32_t {
    // Primary pairs and their 8-bit halves.
    REG_AF = 0, REG_A, REG_F,
    REG_BC,     REG_B, REG_C,
    REG_DE,     REG_D, REG_E,
    REG_HL,     REG_H, REG_L,
    // Index, stack, program counter.
    REG_IX, REG_IY, REG_SP, REG_PC,
    // Interrupt / refresh.
    REG_I, REG_R,
    // Shadow set (16-bit only — halves are rarely useful interactively).
    REG_AF_SHADOW, REG_BC_SHADOW, REG_DE_SHADOW, REG_HL_SHADOW,
    // Interrupt enable flip-flops + mode.
    REG_IFF1, REG_IFF2, REG_IM,
    // Counters (hidden / read-only).
    REG_CYCLES, REG_INSTRUCTIONS,
    REG_COUNT
};

// ============================================================================
// Board info (static — lives for the DLL's lifetime)
// ============================================================================

static EmfeBoardInfo s_board_info = {
    "Z80",                                          // board_name
    "Z80",                                          // cpu_name
    "Zilog Z80 8-bit CPU plugin (scaffold)",        // description
    "0.1.0",                                        // version
};

// ============================================================================
// Instance
// ============================================================================

struct EmfeInstanceData {
    Z80Cpu cpu;
    Z80Memory memory;
    Z80Uart uart;
    Z80Timer timer;
    Z80DiskDevice disk;
    std::atomic<EmfeState> state{EMFE_STATE_STOPPED};

    // Phase 6.1: emfe_run lifecycle. The worker thread polls
    // stopRequested in a tight ExecuteOne loop; emfe_stop sets the
    // flag and joins. Both fields are also touched in emfe_destroy
    // (stop the worker BEFORE deleting any device state) so a host
    // that drops an instance mid-run doesn't UAF its CPU thread.
    std::atomic<bool> stopRequested{false};
    std::thread       runThread;

    // Console char callback (set by the host) — fired whenever the
    // guest writes a byte to the UART data port. Stored under a mutex
    // so the host can swap it while the worker thread is running.
    EmfeConsoleCharCallback consoleCharCb = nullptr;
    void* consoleCharUserData = nullptr;
    std::mutex consoleCbMutex;

    // Register definitions, built once at create() and handed back via
    // emfe_get_register_defs. Strings live in static storage (literals)
    // so the vector can be copied / reset freely.
    std::vector<EmfeRegisterDef> regDefs;

    // Disassembly result buffers — owned per-instance. Overwritten by
    // each emfe_disassemble_one / emfe_disassemble_range call (the
    // ABI contract says the strings are valid only until the next
    // call into the same instance's disasm functions).
    std::string disasmMnemonic;
    std::string disasmOperands;
    std::string disasmRawBytes;
    std::vector<EmfeDisasmLine> disasmRangeLines;
    std::vector<std::string>    disasmRangeStringPool;

    // Settings (Phase 2.6 — minimal placeholder set so the host's
    // Settings dialog renders non-empty and z80 has family parity with
    // em8 / z8000 / mc68030 / mc6809. Full settings UI is Phase 6.)
    // - settingDefs    : defs handed back via emfe_get_setting_defs
    // - stagedSettings : edits accumulate here until emfe_apply_settings
    // - settings       : "current" working copy (applied + REQUIRES_RESET
    //                    staged values combined)
    // - appliedSettings: only the non-REQUIRES_RESET keys reach this
    //                    map on emfe_apply_settings; the REQUIRES_RESET
    //                    keys move on the next emfe_reset.
    // - settingFlags   : per-key flag bitmask, mirrors what is in defs
    // - settingValueBuf / appliedSettingValueBuf: returned char-pointer
    //                    storage for emfe_get_setting / get_applied_setting
    std::vector<EmfeSettingDef> settingDefs;
    std::unordered_map<std::string, std::string> stagedSettings;
    std::unordered_map<std::string, std::string> settings;
    std::unordered_map<std::string, std::string> appliedSettings;
    std::unordered_map<std::string, uint32_t>    settingFlags;
    std::string settingValueBuf;
    std::string appliedSettingValueBuf;
};

static inline EmfeInstanceData* to_inst(EmfeInstance i) {
    return reinterpret_cast<EmfeInstanceData*>(i);
}

// ============================================================================
// Discovery & lifecycle
// ============================================================================

extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_negotiate(const EmfeNegotiateInfo* info) {
    if (!info) return EMFE_ERR_INVALID;
    if (info->api_version_major != EMFE_API_VERSION_MAJOR) return EMFE_ERR_UNSUPPORTED;
    return EMFE_OK;
}

extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_get_board_info(EmfeBoardInfo* out_info) {
    if (!out_info) return EMFE_ERR_INVALID;
    *out_info = s_board_info;
    return EMFE_OK;
}

extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_create(EmfeInstance* out_instance) {
    if (!out_instance) return EMFE_ERR_INVALID;
    auto* inst = new (std::nothrow) EmfeInstanceData();
    if (!inst) return EMFE_ERR_MEMORY;
    inst->cpu.Memory = &inst->memory;
    inst->memory.Uart = &inst->uart;
    inst->memory.Timer = &inst->timer;
    inst->memory.Disk = &inst->disk;
    inst->disk.Memory = &inst->memory;
    // Wire UART TX → console char callback. Captures `inst` so we can
    // grab the latest callback under the mutex when each byte fires.
    inst->uart.TxCallback = [inst](uint8_t ch) {
        EmfeConsoleCharCallback cb = nullptr;
        void* user = nullptr;
        {
            std::lock_guard lock(inst->consoleCbMutex);
            cb = inst->consoleCharCb;
            user = inst->consoleCharUserData;
        }
        if (cb) cb(user, static_cast<char>(ch));
    };
    inst->cpu.Reset();
    inst->memory.Reset();
    inst->uart.Reset();
    inst->timer.Reset();
    inst->disk.Reset();

    // Build the register definition table. Order here drives the host's
    // display order; group strings cluster registers under collapsible
    // headings.
    auto add = [inst](uint32_t id, const char* name, const char* group,
                      uint32_t bits, uint32_t flags) {
        inst->regDefs.push_back({id, name, group, EMFE_REG_INT, bits, flags});
    };
    add(REG_AF, "AF", "Main", 16, EMFE_REG_FLAG_NONE);
    add(REG_A,  "A",  "Main",  8, EMFE_REG_FLAG_NONE);
    add(REG_F,  "F",  "Main",  8, EMFE_REG_FLAG_FLAGS);
    add(REG_BC, "BC", "Main", 16, EMFE_REG_FLAG_NONE);
    add(REG_B,  "B",  "Main",  8, EMFE_REG_FLAG_NONE);
    add(REG_C,  "C",  "Main",  8, EMFE_REG_FLAG_NONE);
    add(REG_DE, "DE", "Main", 16, EMFE_REG_FLAG_NONE);
    add(REG_D,  "D",  "Main",  8, EMFE_REG_FLAG_NONE);
    add(REG_E,  "E",  "Main",  8, EMFE_REG_FLAG_NONE);
    add(REG_HL, "HL", "Main", 16, EMFE_REG_FLAG_NONE);
    add(REG_H,  "H",  "Main",  8, EMFE_REG_FLAG_NONE);
    add(REG_L,  "L",  "Main",  8, EMFE_REG_FLAG_NONE);
    add(REG_IX, "IX", "Index", 16, EMFE_REG_FLAG_NONE);
    add(REG_IY, "IY", "Index", 16, EMFE_REG_FLAG_NONE);
    add(REG_SP, "SP", "System", 16, EMFE_REG_FLAG_SP);
    add(REG_PC, "PC", "System", 16, EMFE_REG_FLAG_PC);
    add(REG_I,  "I",  "System",  8, EMFE_REG_FLAG_NONE);
    add(REG_R,  "R",  "System",  8, EMFE_REG_FLAG_NONE);
    add(REG_AF_SHADOW, "AF'", "Shadow", 16, EMFE_REG_FLAG_NONE);
    add(REG_BC_SHADOW, "BC'", "Shadow", 16, EMFE_REG_FLAG_NONE);
    add(REG_DE_SHADOW, "DE'", "Shadow", 16, EMFE_REG_FLAG_NONE);
    add(REG_HL_SHADOW, "HL'", "Shadow", 16, EMFE_REG_FLAG_NONE);
    add(REG_IFF1, "IFF1", "Interrupt", 8, EMFE_REG_FLAG_NONE);
    add(REG_IFF2, "IFF2", "Interrupt", 8, EMFE_REG_FLAG_NONE);
    add(REG_IM,   "IM",   "Interrupt", 8, EMFE_REG_FLAG_NONE);
    add(REG_CYCLES,       "Cycles",       "Counters", 64,
        EMFE_REG_FLAG_READONLY | EMFE_REG_FLAG_HIDDEN);
    add(REG_INSTRUCTIONS, "Instructions", "Counters", 64,
        EMFE_REG_FLAG_READONLY | EMFE_REG_FLAG_HIDDEN);

    // Minimal Settings (Phase 2.6). Strings are literals so the def
    // table stays valid for the instance's lifetime. RR = requires
    // reset to take effect on hardware.
    const uint32_t RR = EMFE_SETTING_FLAG_REQUIRES_RESET;
    auto addSetting = [inst](const char* key, const char* label,
                             const char* group, EmfeSettingType type,
                             const char* defVal,
                             const char* constraints,
                             uint32_t flags) {
        inst->settingDefs.push_back({
            key, label, group, type, defVal, constraints,
            nullptr, nullptr, flags
        });
        inst->settingFlags[key] = flags;
        inst->stagedSettings [key] = defVal;
        inst->settings        [key] = defVal;
        inst->appliedSettings [key] = defVal;
    };

    // General tab — emulator-wide knobs. Z80 RAM is always 64 KiB; the
    // entry is informational (constraint locks the range to a single
    // value) and exists so the dialog shows what the user is running.
    addSetting("MemorySize", "Memory Size (KiB)", "General",
               EMFE_SETTING_INT, "64", "64|64", RR);
    addSetting("ResetVector", "Reset Vector (PC on reset)", "General",
               EMFE_SETTING_INT, "0", "0|65535", RR);
    addSetting("ClearRamOnReset", "Clear RAM on reset", "General",
               EMFE_SETTING_BOOL, "true", nullptr, 0);

    // Console tab — UART display preferences. Match em8's keys so
    // host code that consumes these settings can do so uniformly
    // across plugins.
    addSetting("ConsoleColumns", "Columns", "Console",
               EMFE_SETTING_INT, "80", "40|320", 0);
    addSetting("ConsoleRows", "Rows", "Console",
               EMFE_SETTING_INT, "25", "10|100", 0);
    addSetting("ConsoleScrollbackLines", "Scrollback Lines", "Console",
               EMFE_SETTING_INT, "2000", "0|100000", 0);

    // Disk tab — Phase 4.4. The path is REQUIRES_RESET because the
    // image is read off disk inside emfe_reset, where the disk MMIO
    // is already cleared and the device is in a known state.
    //
    //   DiskImagePath          empty = unmount; otherwise host path
    //   DiskImageWriteProtect  controls the WRITE_PROTECT status bit
    //   BootFromDisk           Phase 4.5 will flip the cold-boot path;
    //                          stored here so the Settings UI can
    //                          carry the flag forward.
    addSetting("DiskImagePath", "Disk Image", "Disk",
               EMFE_SETTING_FILE, "", nullptr, RR);
    addSetting("DiskImageWriteProtect", "Write Protect", "Disk",
               EMFE_SETTING_BOOL, "false", nullptr, RR);
    addSetting("BootFromDisk", "Boot from Disk", "Disk",
               EMFE_SETTING_BOOL, "false", nullptr, RR);

    *out_instance = reinterpret_cast<EmfeInstance>(inst);
    return EMFE_OK;
}

extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_destroy(EmfeInstance instance) {
    if (!instance) return EMFE_ERR_INVALID;
    auto* inst = to_inst(instance);
    // Phase 6.1: stop the worker thread BEFORE tearing the instance
    // down. Worker reads/writes inst->cpu / inst->memory; if delete
    // runs while the thread is still ExecuteOne'ing we'd UAF.
    inst->stopRequested.store(true, std::memory_order_release);
    if (inst->runThread.joinable()) {
        inst->runThread.join();
    }
    delete inst;
    return EMFE_OK;
}

// ============================================================================
// Callbacks (accepted but not yet fired)
// ============================================================================

extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_set_console_char_callback(
        EmfeInstance instance, EmfeConsoleCharCallback cb, void* user) {
    if (!instance) return EMFE_ERR_INVALID;
    auto* inst = to_inst(instance);
    std::lock_guard lock(inst->consoleCbMutex);
    inst->consoleCharCb = cb;
    inst->consoleCharUserData = user;
    return EMFE_OK;
}

extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_set_state_change_callback(
        EmfeInstance instance, EmfeStateChangeCallback /*cb*/, void* /*user*/) {
    return instance ? EMFE_OK : EMFE_ERR_INVALID;
}

extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_set_diagnostic_callback(
        EmfeInstance instance, EmfeDiagnosticCallback /*cb*/, void* /*user*/) {
    return instance ? EMFE_OK : EMFE_ERR_INVALID;
}

// ============================================================================
// Registers — Phase 1.3 fills these in
// ============================================================================

extern "C" EMFE_EXPORT int32_t EMFE_CALL emfe_get_register_defs(
        EmfeInstance instance, const EmfeRegisterDef** out_defs) {
    if (!instance || !out_defs) return 0;
    auto* inst = to_inst(instance);
    *out_defs = inst->regDefs.data();
    return static_cast<int32_t>(inst->regDefs.size());
}

extern "C" EMFE_EXPORT int32_t EMFE_CALL emfe_get_register_flag_defs(
        EmfeInstance /*instance*/, uint32_t reg_id,
        const EmfeRegFlagBitDef** out_defs) {
    if (!out_defs) return 0;
    *out_defs = nullptr;

    // Z80 F register layout per Zilog UM008011 §2.4:
    //   bit 7 S   bit 6 Z   bit 5 Y*  bit 4 H
    //   bit 3 X*  bit 2 P/V bit 1 N   bit 0 C
    // The Y and X bits are undocumented copies of bits 5 and 3 of the
    // last ALU result; they're exposed in the bit decomposition so the
    // host's checkbox UI matches what zexall checks. Order is MSB-first
    // so the displayed string reads like a status byte literal.
    static const EmfeRegFlagBitDef f_bits[] = {
        { 7, "S" },
        { 6, "Z" },
        { 5, "Y" },
        { 4, "H" },
        { 3, "X" },
        { 2, "P/V" },
        { 1, "N" },
        { 0, "C" },
    };
    if (reg_id == REG_F) {
        *out_defs = f_bits;
        return static_cast<int32_t>(sizeof(f_bits) / sizeof(f_bits[0]));
    }
    return 0;
}

extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_get_registers(
        EmfeInstance instance, EmfeRegValue* values, int32_t count) {
    if (!instance || !values) return EMFE_ERR_INVALID;
    auto& cpu = to_inst(instance)->cpu;
    for (int32_t i = 0; i < count; ++i) {
        auto& v = values[i];
        v.value.u64 = 0;
        switch (v.reg_id) {
        case REG_AF: v.value.u64 = cpu.AF; break;
        case REG_A:  v.value.u64 = cpu.A(); break;
        case REG_F:  v.value.u64 = cpu.F(); break;
        case REG_BC: v.value.u64 = cpu.BC; break;
        case REG_B:  v.value.u64 = cpu.B(); break;
        case REG_C:  v.value.u64 = cpu.C(); break;
        case REG_DE: v.value.u64 = cpu.DE; break;
        case REG_D:  v.value.u64 = cpu.D(); break;
        case REG_E:  v.value.u64 = cpu.E(); break;
        case REG_HL: v.value.u64 = cpu.HL; break;
        case REG_H:  v.value.u64 = cpu.H(); break;
        case REG_L:  v.value.u64 = cpu.L(); break;
        case REG_IX: v.value.u64 = cpu.IX; break;
        case REG_IY: v.value.u64 = cpu.IY; break;
        case REG_SP: v.value.u64 = cpu.SP; break;
        case REG_PC: v.value.u64 = cpu.PC; break;
        case REG_I:  v.value.u64 = cpu.I;  break;
        case REG_R:  v.value.u64 = cpu.R;  break;
        case REG_AF_SHADOW: v.value.u64 = cpu.AF_; break;
        case REG_BC_SHADOW: v.value.u64 = cpu.BC_; break;
        case REG_DE_SHADOW: v.value.u64 = cpu.DE_; break;
        case REG_HL_SHADOW: v.value.u64 = cpu.HL_; break;
        case REG_IFF1: v.value.u64 = cpu.IFF1 ? 1u : 0u; break;
        case REG_IFF2: v.value.u64 = cpu.IFF2 ? 1u : 0u; break;
        case REG_IM:   v.value.u64 = cpu.IM; break;
        case REG_CYCLES:       v.value.u64 = cpu.CycleCount; break;
        case REG_INSTRUCTIONS: v.value.u64 = static_cast<uint64_t>(cpu.InstructionCount); break;
        default: return EMFE_ERR_INVALID;
        }
    }
    return EMFE_OK;
}

extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_set_registers(
        EmfeInstance instance, const EmfeRegValue* values, int32_t count) {
    if (!instance || !values) return EMFE_ERR_INVALID;
    auto* inst = to_inst(instance);
    if (inst->state.load() == EMFE_STATE_RUNNING) return EMFE_ERR_STATE;
    auto& cpu = inst->cpu;
    for (int32_t i = 0; i < count; ++i) {
        const auto& v = values[i];
        switch (v.reg_id) {
        case REG_AF: cpu.AF = static_cast<uint16_t>(v.value.u64); break;
        case REG_A:  cpu.SetA(static_cast<uint8_t>(v.value.u64)); break;
        case REG_F:  cpu.SetF(static_cast<uint8_t>(v.value.u64)); break;
        case REG_BC: cpu.BC = static_cast<uint16_t>(v.value.u64); break;
        case REG_B:  cpu.SetB(static_cast<uint8_t>(v.value.u64)); break;
        case REG_C:  cpu.SetC(static_cast<uint8_t>(v.value.u64)); break;
        case REG_DE: cpu.DE = static_cast<uint16_t>(v.value.u64); break;
        case REG_D:  cpu.SetD(static_cast<uint8_t>(v.value.u64)); break;
        case REG_E:  cpu.SetE(static_cast<uint8_t>(v.value.u64)); break;
        case REG_HL: cpu.HL = static_cast<uint16_t>(v.value.u64); break;
        case REG_H:  cpu.SetH(static_cast<uint8_t>(v.value.u64)); break;
        case REG_L:  cpu.SetL(static_cast<uint8_t>(v.value.u64)); break;
        case REG_IX: cpu.IX = static_cast<uint16_t>(v.value.u64); break;
        case REG_IY: cpu.IY = static_cast<uint16_t>(v.value.u64); break;
        case REG_SP: cpu.SP = static_cast<uint16_t>(v.value.u64); break;
        case REG_PC: cpu.PC = static_cast<uint16_t>(v.value.u64); break;
        case REG_I:  cpu.I  = static_cast<uint8_t>(v.value.u64); break;
        case REG_R:  cpu.R  = static_cast<uint8_t>(v.value.u64); break;
        case REG_AF_SHADOW: cpu.AF_ = static_cast<uint16_t>(v.value.u64); break;
        case REG_BC_SHADOW: cpu.BC_ = static_cast<uint16_t>(v.value.u64); break;
        case REG_DE_SHADOW: cpu.DE_ = static_cast<uint16_t>(v.value.u64); break;
        case REG_HL_SHADOW: cpu.HL_ = static_cast<uint16_t>(v.value.u64); break;
        case REG_IFF1: cpu.IFF1 = (v.value.u64 != 0); break;
        case REG_IFF2: cpu.IFF2 = (v.value.u64 != 0); break;
        case REG_IM:   cpu.IM   = static_cast<uint8_t>(v.value.u64 & 0x03); break;
        case REG_CYCLES:
        case REG_INSTRUCTIONS:
            break; // read-only, silently dropped
        default: return EMFE_ERR_INVALID;
        }
    }
    return EMFE_OK;
}

// ============================================================================
// Memory — Phase 1.2 fills these in
// ============================================================================

extern "C" EMFE_EXPORT uint8_t EMFE_CALL emfe_peek_byte(EmfeInstance instance, uint64_t address) {
    if (!instance) return 0;
    return to_inst(instance)->memory.Peek(static_cast<uint16_t>(address & 0xFFFF));
}

extern "C" EMFE_EXPORT uint16_t EMFE_CALL emfe_peek_word(EmfeInstance instance, uint64_t address) {
    if (!instance) return 0;
    auto& mem = to_inst(instance)->memory;
    uint16_t a = static_cast<uint16_t>(address & 0xFFFF);
    // Z80 is little-endian; the word at addr is (mem[addr] | (mem[addr+1] << 8)).
    uint16_t lo = mem.Peek(a);
    uint16_t hi = mem.Peek(static_cast<uint16_t>(a + 1));
    return static_cast<uint16_t>(lo | (hi << 8));
}

extern "C" EMFE_EXPORT uint32_t EMFE_CALL emfe_peek_long(EmfeInstance instance, uint64_t address) {
    if (!instance) return 0;
    auto& mem = to_inst(instance)->memory;
    uint16_t a = static_cast<uint16_t>(address & 0xFFFF);
    uint32_t b0 = mem.Peek(a);
    uint32_t b1 = mem.Peek(static_cast<uint16_t>(a + 1));
    uint32_t b2 = mem.Peek(static_cast<uint16_t>(a + 2));
    uint32_t b3 = mem.Peek(static_cast<uint16_t>(a + 3));
    return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
}

extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_poke_byte(EmfeInstance instance, uint64_t address, uint8_t value) {
    if (!instance) return EMFE_ERR_INVALID;
    to_inst(instance)->memory.Poke(static_cast<uint16_t>(address & 0xFFFF), value);
    return EMFE_OK;
}

extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_poke_word(EmfeInstance instance, uint64_t address, uint16_t value) {
    if (!instance) return EMFE_ERR_INVALID;
    auto& mem = to_inst(instance)->memory;
    uint16_t a = static_cast<uint16_t>(address & 0xFFFF);
    mem.Poke(a, static_cast<uint8_t>(value & 0xFF));
    mem.Poke(static_cast<uint16_t>(a + 1), static_cast<uint8_t>((value >> 8) & 0xFF));
    return EMFE_OK;
}

extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_poke_long(EmfeInstance instance, uint64_t address, uint32_t value) {
    if (!instance) return EMFE_ERR_INVALID;
    auto& mem = to_inst(instance)->memory;
    uint16_t a = static_cast<uint16_t>(address & 0xFFFF);
    mem.Poke(a,                                static_cast<uint8_t>(value & 0xFF));
    mem.Poke(static_cast<uint16_t>(a + 1),     static_cast<uint8_t>((value >> 8) & 0xFF));
    mem.Poke(static_cast<uint16_t>(a + 2),     static_cast<uint8_t>((value >> 16) & 0xFF));
    mem.Poke(static_cast<uint16_t>(a + 3),     static_cast<uint8_t>((value >> 24) & 0xFF));
    return EMFE_OK;
}

extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_peek_range(
        EmfeInstance instance, uint64_t address, uint8_t* out_data, uint32_t size) {
    if (!instance || !out_data) return EMFE_ERR_INVALID;
    auto& mem = to_inst(instance)->memory;
    for (uint32_t i = 0; i < size; ++i) {
        out_data[i] = mem.Peek(static_cast<uint16_t>((address + i) & 0xFFFF));
    }
    return EMFE_OK;
}

extern "C" EMFE_EXPORT uint64_t EMFE_CALL emfe_get_memory_size(EmfeInstance) {
    return 0x10000; // 64 KiB flat
}

// ============================================================================
// Disassembly — Phase 2.5 fills these in
// ============================================================================

extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_disassemble_one(
        EmfeInstance instance, uint64_t address, EmfeDisasmLine* out_line) {
    if (!instance || !out_line) return EMFE_ERR_INVALID;
    auto* inst = to_inst(instance);
    auto res = Z80Disasm::Decode(&inst->memory, static_cast<uint16_t>(address & 0xFFFF));
    inst->disasmMnemonic = std::move(res.mnemonic);
    inst->disasmOperands = std::move(res.operands);
    inst->disasmRawBytes = std::move(res.rawBytes);
    out_line->address   = address & 0xFFFF;
    out_line->raw_bytes = inst->disasmRawBytes.c_str();
    out_line->mnemonic  = inst->disasmMnemonic.c_str();
    out_line->operands  = inst->disasmOperands.c_str();
    out_line->length    = static_cast<uint32_t>(res.length);
    return EMFE_OK;
}

extern "C" EMFE_EXPORT int32_t EMFE_CALL emfe_disassemble_range(
        EmfeInstance instance, uint64_t start_address, uint64_t end_address,
        EmfeDisasmLine* out_lines, int32_t max_lines) {
    if (!instance || !out_lines || max_lines <= 0) return 0;
    auto* inst = to_inst(instance);

    // Re-build the per-call string pool so the c_str() pointers we
    // hand back stay valid until the NEXT range call.
    inst->disasmRangeLines.clear();
    inst->disasmRangeStringPool.clear();
    // Reserve generously: 3 strings per line (mnemonic / operands / raw).
    inst->disasmRangeStringPool.reserve(static_cast<size_t>(max_lines) * 3);

    uint16_t addr = static_cast<uint16_t>(start_address & 0xFFFF);
    uint16_t end  = static_cast<uint16_t>(end_address & 0xFFFF);
    int32_t count = 0;

    while (count < max_lines) {
        // End-exclusive semantics: include lines whose START address
        // is < end_address. A line starting AT end_address is treated
        // as outside the range. end < start signals "no upper bound,
        // cap only via max_lines" (wraparound case).
        if (end_address >= start_address && addr >= end) break;

        auto res = Z80Disasm::Decode(&inst->memory, addr);
        // Store the three strings into the pool so their lifetimes
        // outlive this loop iteration.
        inst->disasmRangeStringPool.push_back(std::move(res.mnemonic));
        const char* mnem_p = inst->disasmRangeStringPool.back().c_str();
        inst->disasmRangeStringPool.push_back(std::move(res.operands));
        const char* ops_p  = inst->disasmRangeStringPool.back().c_str();
        inst->disasmRangeStringPool.push_back(std::move(res.rawBytes));
        const char* raw_p  = inst->disasmRangeStringPool.back().c_str();

        EmfeDisasmLine line{};
        line.address   = addr;
        line.raw_bytes = raw_p;
        line.mnemonic  = mnem_p;
        line.operands  = ops_p;
        line.length    = static_cast<uint32_t>(res.length);
        out_lines[count++] = line;

        // Defensive: avoid infinite loop if Decode reports length 0.
        if (res.length <= 0) break;
        addr = static_cast<uint16_t>(addr + res.length);
    }

    // String-pool pointers might be invalidated if the underlying
    // vector reallocates during push_back. Reserve up-front above
    // guarantees no reallocation. As a paranoia step, rebuild the
    // c_str pointers from the now-final vector contents.
    for (int32_t i = 0; i < count; ++i) {
        out_lines[i].mnemonic  = inst->disasmRangeStringPool[i * 3 + 0].c_str();
        out_lines[i].operands  = inst->disasmRangeStringPool[i * 3 + 1].c_str();
        out_lines[i].raw_bytes = inst->disasmRangeStringPool[i * 3 + 2].c_str();
    }

    return count;
}

extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_get_program_range(
        EmfeInstance instance, uint64_t* out_start, uint64_t* out_end) {
    // Z80 has flat 64 KiB memory and no "loaded program" concept yet —
    // the host can use [0x0000, 0x10000) as the whole disassemblable
    // range until file-load APIs land in a later phase.
    if (!instance || !out_start || !out_end) return EMFE_ERR_INVALID;
    *out_start = 0x0000;
    *out_end   = 0x10000;
    return EMFE_OK;
}

// ============================================================================
// Execution — Phase 1.3 onwards
// ============================================================================

extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_step(EmfeInstance instance) {
    if (!instance) return EMFE_ERR_INVALID;
    auto* inst = to_inst(instance);
    if (inst->state.load() == EMFE_STATE_RUNNING) return EMFE_ERR_STATE;
    int cycles = inst->cpu.ExecuteOne();
    return cycles > 0 ? EMFE_OK : EMFE_ERR_UNSUPPORTED;
}

extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_step_over(EmfeInstance instance) {
    return instance ? EMFE_ERR_UNSUPPORTED : EMFE_ERR_INVALID;
}

extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_step_out(EmfeInstance instance) {
    return instance ? EMFE_ERR_UNSUPPORTED : EMFE_ERR_INVALID;
}

// Phase 6.1: emfe_run + emfe_stop worker-thread lifecycle.
//
// emfe_run spawns a worker thread that loops Z80Cpu::ExecuteOne
// until: (a) stopRequested is set (emfe_stop / emfe_destroy), or
// (b) ExecuteOne returns 0 (UNSUPPORTED opcode — the guest hit
// something we don't implement). The thread updates state to
// STOPPED on exit so the host can poll emfe_get_state to detect
// auto-halt.
//
// Calling emfe_run while already running returns EMFE_ERR_STATE.
// emfe_stop / emfe_destroy are idempotent on an already-stopped
// instance (stopRequested set unconditionally; join is no-op if
// the thread isn't joinable).

extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_run(EmfeInstance instance) {
    if (!instance) return EMFE_ERR_INVALID;
    auto* inst = to_inst(instance);

    EmfeState expected = EMFE_STATE_STOPPED;
    if (!inst->state.compare_exchange_strong(expected, EMFE_STATE_RUNNING)) {
        return EMFE_ERR_STATE;     // already running (or some other state)
    }

    // Clean up any previous worker — emfe_stop on the prior run may
    // have set state→STOPPED but never joined (it does, but defensive).
    if (inst->runThread.joinable()) {
        inst->runThread.join();
    }
    inst->stopRequested.store(false, std::memory_order_release);

    inst->runThread = std::thread([inst]() {
        while (!inst->stopRequested.load(std::memory_order_acquire)) {
            int cycles = inst->cpu.ExecuteOne();
            if (cycles <= 0) break;      // UNSUPPORTED or future-sentinel
        }
        inst->state.store(EMFE_STATE_STOPPED, std::memory_order_release);
    });
    return EMFE_OK;
}

extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_stop(EmfeInstance instance) {
    if (!instance) return EMFE_ERR_INVALID;
    auto* inst = to_inst(instance);
    inst->stopRequested.store(true, std::memory_order_release);
    if (inst->runThread.joinable()) {
        inst->runThread.join();
    }
    // The worker stores STOPPED itself on exit; do it here too for
    // the idempotent "stop an already-stopped instance" case.
    inst->state.store(EMFE_STATE_STOPPED, std::memory_order_release);
    return EMFE_OK;
}

// Phase 4.4: load the disk image referenced by the DiskImagePath /
// DiskImageWriteProtect settings (if any) into the Z80DiskDevice.
// Called from inside emfe_reset AFTER the REQUIRES_RESET settings
// have moved into appliedSettings and AFTER inst->disk.Reset() has
// cleared the device's transient state.
//
// Empty DiskImagePath means "Settings doesn't manage the mount" —
// leave the disk device alone (so hosts / tests that mounted via
// the vendor-prefixed emfe_z80_mount_disk_image hook keep their
// image). Non-empty path replaces; missing / unreadable file
// explicitly unmounts so the user notices a typo.
static void ApplyDiskImageOnReset(EmfeInstanceData* inst) {
    auto path_it = inst->appliedSettings.find("DiskImagePath");
    auto wp_it   = inst->appliedSettings.find("DiskImageWriteProtect");
    const std::string& path = (path_it != inst->appliedSettings.end()) ? path_it->second : std::string();
    bool writeProtect = false;
    if (wp_it != inst->appliedSettings.end()) {
        const std::string& v = wp_it->second;
        writeProtect = (v == "true" || v == "1");
    }

    if (path.empty()) return;     // Settings disengaged — preserve any vendor mount

    std::ifstream f(path, std::ios::binary);
    if (!f) {
        inst->disk.SetImage({}, false);   // file missing → unmount (loud failure)
        return;
    }
    f.seekg(0, std::ios::end);
    auto end = f.tellg();
    if (end <= 0) {
        inst->disk.SetImage({}, false);
        return;
    }
    auto size = static_cast<size_t>(end);
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(size);
    f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    if (!f) {
        inst->disk.SetImage({}, false);
        return;
    }
    inst->disk.SetImage(std::move(bytes), writeProtect);
}

// Phase 4.5: if BootFromDisk is enabled AND a disk image is mounted,
// pre-load the system tracks from the image into the canonical RAM
// addresses and start execution at the cold-boot vector (0x0000).
//
// This is the host-side equivalent of "the boot stub already ran" —
// the plugin lays down the boot stub at 0x0000, the CCP / BDOS at
// 0xE400 / 0xEC00, and the CBIOS at 0xFA00 in one shot, so that on
// the very first instruction execution the JP 0xFA00 in the boot
// stub lands in real CBIOS code. The layout matches what build.ps1
// writes to a fresh SSSD-256K image.
//
// No-op if BootFromDisk is false or no image is mounted — the cold
// boot then behaves like Phase 1.7 / Phase 3.4 (bare-metal at PC=0
// running whatever's already in RAM).
static void ApplyBootFromDiskOnReset(EmfeInstanceData* inst) {
    auto boot_it = inst->appliedSettings.find("BootFromDisk");
    bool bootFromDisk = false;
    if (boot_it != inst->appliedSettings.end()) {
        const std::string& v = boot_it->second;
        bootFromDisk = (v == "true" || v == "1");
    }
    if (!bootFromDisk) return;
    if (!inst->disk.HasImage()) return;

    // Sector size is fixed at 128 bytes (CP/M 2.2 native record).
    // System layout written by build.ps1:
    //   LBA 0       boot stub @ RAM 0x0000  (3 bytes of JP + padding)
    //   LBA 1..44   CCP + BDOS @ RAM 0xE400 (44 sectors = 5632 bytes —
    //                                        the CODE portion of the
    //                                        5683-byte assembled blob)
    //   LBA 45      NOT LOADED — contains Calkins' CPM22.Z80 internal
    //                            fake BIOS jump table (17 × 'JP 0'
    //                            placeholders). Real CBIOS overlays
    //                            this region in RAM, so loading the
    //                            fake table would overwrite us.
    //   LBA 46..57  CBIOS      @ RAM 0xFA00 (12 sectors = 1536 bytes —
    //                                        exactly fills 0xFA00..0xFFFF)
    // The trick: CPM22.Z80's BDOS internally CALLs "CONOUT", "READ" etc.
    // at addresses 0xFA0C, 0xFA27 etc — which are inside the fake BIOS
    // table at 0xFA00. By placing our REAL CBIOS at the same 0xFA00
    // base address, those calls resolve to OUR routines. The fake bytes
    // at LBA 45 in the disk image are never loaded into RAM.
    auto copyRange = [inst](uint32_t srcLba, uint32_t count, uint16_t dstAddr) {
        for (uint32_t s = 0; s < count; ++s) {
            uint32_t srcOff = (srcLba + s) * 128u;
            for (int b = 0; b < 128; ++b) {
                uint8_t v = inst->disk.PeekImageByte(srcOff + b);
                inst->memory.Poke(static_cast<uint16_t>(dstAddr + s * 128 + b), v);
            }
        }
    };
    copyRange(0,   1,  0x0000);   // boot stub
    copyRange(1,  44,  0xE400);   // CCP + BDOS (code+data, NOT fake table)
    copyRange(46, 12,  0xFA00);   // real CBIOS, overlaying fake-table addrs

    inst->cpu.PC = 0x0000;
}

extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_reset(EmfeInstance instance) {
    if (!instance) return EMFE_ERR_INVALID;
    auto* inst = to_inst(instance);
    // Reset is also the point at which REQUIRES_RESET settings commit
    // from staging into the applied-to-hardware map. Non-REQUIRES_RESET
    // keys were already committed on emfe_apply_settings.
    for (const auto& [key, val] : inst->stagedSettings) {
        auto fit = inst->settingFlags.find(key);
        uint32_t flags = (fit != inst->settingFlags.end()) ? fit->second : 0;
        if (flags & EMFE_SETTING_FLAG_REQUIRES_RESET) {
            inst->appliedSettings[key] = val;
        }
    }
    inst->cpu.Reset();
    inst->memory.Reset();
    inst->uart.Reset();
    inst->timer.Reset();
    inst->disk.Reset();
    // The disk image lives at a host-side file path that we mount
    // after the device transient state has been cleared — that way
    // the freshly-loaded image is the first thing the guest sees.
    ApplyDiskImageOnReset(inst);
    // Once the image is mounted (or not), decide whether we should
    // pre-stage system tracks into RAM and start at the cold-boot
    // vector. This is what makes CP/M actually boot.
    ApplyBootFromDiskOnReset(inst);
    return EMFE_OK;
}

extern "C" EMFE_EXPORT EmfeState EMFE_CALL emfe_get_state(EmfeInstance instance) {
    return instance ? to_inst(instance)->state.load() : EMFE_STATE_STOPPED;
}

extern "C" EMFE_EXPORT int64_t EMFE_CALL emfe_get_instruction_count(EmfeInstance instance) {
    return instance ? (int64_t)to_inst(instance)->cpu.InstructionCount : 0;
}

extern "C" EMFE_EXPORT int64_t EMFE_CALL emfe_get_cycle_count(EmfeInstance instance) {
    return instance ? (int64_t)to_inst(instance)->cpu.CycleCount : 0;
}

// ============================================================================
// Breakpoints — Phase 6 fills these in
// ============================================================================

extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_add_breakpoint(EmfeInstance, uint64_t)     { return EMFE_ERR_UNSUPPORTED; }
extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_remove_breakpoint(EmfeInstance, uint64_t)  { return EMFE_ERR_UNSUPPORTED; }
extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_enable_breakpoint(EmfeInstance, uint64_t, bool) { return EMFE_ERR_UNSUPPORTED; }
extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_set_breakpoint_condition(EmfeInstance, uint64_t, const char*) { return EMFE_ERR_UNSUPPORTED; }
extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_clear_breakpoints(EmfeInstance)            { return EMFE_OK; }

extern "C" EMFE_EXPORT int32_t EMFE_CALL emfe_get_breakpoints(
        EmfeInstance, EmfeBreakpointInfo* /*out*/, int32_t /*max*/) {
    return 0;
}

// ============================================================================
// Watchpoints — Phase 6 fills these in
// ============================================================================

extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_add_watchpoint(EmfeInstance, uint64_t, EmfeWatchpointSize, EmfeWatchpointType) { return EMFE_ERR_UNSUPPORTED; }
extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_remove_watchpoint(EmfeInstance, uint64_t)  { return EMFE_ERR_UNSUPPORTED; }
extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_enable_watchpoint(EmfeInstance, uint64_t, bool) { return EMFE_ERR_UNSUPPORTED; }
extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_set_watchpoint_condition(EmfeInstance, uint64_t, const char*) { return EMFE_ERR_UNSUPPORTED; }
extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_clear_watchpoints(EmfeInstance)            { return EMFE_OK; }

extern "C" EMFE_EXPORT int32_t EMFE_CALL emfe_get_watchpoints(
        EmfeInstance, EmfeWatchpointInfo* /*out*/, int32_t /*max*/) {
    return 0;
}

// ============================================================================
// Call stack — Phase 6
// ============================================================================

extern "C" EMFE_EXPORT int32_t EMFE_CALL emfe_get_call_stack(
        EmfeInstance, EmfeCallStackEntry* /*out*/, int32_t /*max*/) {
    return 0;
}

// ============================================================================
// Framebuffer / input — not applicable for Z80 (text-only)
// ============================================================================

extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_get_framebuffer_info(
        EmfeInstance, EmfeFramebufferInfo* /*out*/) {
    return EMFE_ERR_UNSUPPORTED;
}

extern "C" EMFE_EXPORT uint32_t EMFE_CALL emfe_get_palette_entry(EmfeInstance, uint32_t) { return 0; }
extern "C" EMFE_EXPORT int32_t  EMFE_CALL emfe_get_palette(EmfeInstance, uint32_t* /*out*/, int32_t /*max*/) { return 0; }

extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_push_key(EmfeInstance, uint32_t, bool)       { return EMFE_ERR_UNSUPPORTED; }
extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_push_mouse_move(EmfeInstance, int32_t, int32_t)     { return EMFE_ERR_UNSUPPORTED; }
extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_push_mouse_absolute(EmfeInstance, int32_t, int32_t) { return EMFE_ERR_UNSUPPORTED; }
extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_push_mouse_button(EmfeInstance, int32_t, bool)      { return EMFE_ERR_UNSUPPORTED; }

// ============================================================================
// File loading — Phase 1.4 onwards (binary at least)
// ============================================================================

extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_load_elf(EmfeInstance, const char*)                          { return EMFE_ERR_UNSUPPORTED; }
extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_load_binary(EmfeInstance, const char*, uint64_t /*address*/) { return EMFE_ERR_UNSUPPORTED; }
extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_load_srec(EmfeInstance, const char*)                         { return EMFE_ERR_UNSUPPORTED; }

extern "C" EMFE_EXPORT const char* EMFE_CALL emfe_get_last_error(EmfeInstance) {
    return "";
}

// ============================================================================
// Console I/O — Phase 1.2 wires these into the UART
// ============================================================================

extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_send_char(EmfeInstance instance, char ch) {
    if (!instance) return EMFE_ERR_INVALID;
    to_inst(instance)->uart.ReceiveChar(static_cast<uint8_t>(ch));
    return EMFE_OK;
}

extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_send_string(EmfeInstance instance, const char* str) {
    if (!instance || !str) return EMFE_ERR_INVALID;
    auto& uart = to_inst(instance)->uart;
    for (const char* p = str; *p; ++p) {
        uart.ReceiveChar(static_cast<uint8_t>(*p));
    }
    return EMFE_OK;
}

// ============================================================================
// Settings — Phase 6 fills these in
// ============================================================================

extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_set_data_dir(const char*) { return EMFE_OK; }

extern "C" EMFE_EXPORT int32_t EMFE_CALL emfe_get_setting_defs(
        EmfeInstance instance, const EmfeSettingDef** out_defs) {
    if (!instance || !out_defs) return 0;
    auto* inst = to_inst(instance);
    *out_defs = inst->settingDefs.data();
    return static_cast<int32_t>(inst->settingDefs.size());
}

extern "C" EMFE_EXPORT const char* EMFE_CALL emfe_get_setting(
        EmfeInstance instance, const char* key) {
    if (!instance || !key) return "";
    auto* inst = to_inst(instance);
    auto it = inst->stagedSettings.find(key);
    if (it == inst->stagedSettings.end()) return "";
    inst->settingValueBuf = it->second;
    return inst->settingValueBuf.c_str();
}

extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_set_setting(
        EmfeInstance instance, const char* key, const char* value) {
    if (!instance || !key || !value) return EMFE_ERR_INVALID;
    auto* inst = to_inst(instance);
    inst->stagedSettings[key] = value;
    return EMFE_OK;
}

extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_apply_settings(EmfeInstance instance) {
    if (!instance) return EMFE_ERR_INVALID;
    auto* inst = to_inst(instance);
    // Bring `settings` (working copy) fully up to staged. For the
    // applied-to-hardware map, only non-REQUIRES_RESET keys move now;
    // REQUIRES_RESET keys wait for emfe_reset.
    inst->settings = inst->stagedSettings;
    for (const auto& [key, val] : inst->stagedSettings) {
        auto fit = inst->settingFlags.find(key);
        uint32_t flags = (fit != inst->settingFlags.end()) ? fit->second : 0;
        if (!(flags & EMFE_SETTING_FLAG_REQUIRES_RESET)) {
            inst->appliedSettings[key] = val;
        }
    }
    return EMFE_OK;
}

extern "C" EMFE_EXPORT const char* EMFE_CALL emfe_get_applied_setting(
        EmfeInstance instance, const char* key) {
    if (!instance || !key) return "";
    auto* inst = to_inst(instance);
    auto it = inst->appliedSettings.find(key);
    if (it == inst->appliedSettings.end()) return "";
    inst->appliedSettingValueBuf = it->second;
    return inst->appliedSettingValueBuf.c_str();
}

extern "C" EMFE_EXPORT int32_t EMFE_CALL emfe_get_list_item_defs(
        EmfeInstance, const char* /*list_key*/, const EmfeListItemDef** out_defs) {
    if (out_defs) *out_defs = nullptr;
    return 0;
}

extern "C" EMFE_EXPORT int32_t     EMFE_CALL emfe_get_list_item_count(EmfeInstance, const char*)              { return 0; }
extern "C" EMFE_EXPORT const char* EMFE_CALL emfe_get_list_item_field(EmfeInstance, const char*, int32_t, const char*) { return nullptr; }
extern "C" EMFE_EXPORT EmfeResult  EMFE_CALL emfe_set_list_item_field(EmfeInstance, const char*, int32_t, const char*, const char*) { return EMFE_ERR_UNSUPPORTED; }
extern "C" EMFE_EXPORT int32_t     EMFE_CALL emfe_add_list_item(EmfeInstance, const char*)                    { return -1; }
extern "C" EMFE_EXPORT EmfeResult  EMFE_CALL emfe_remove_list_item(EmfeInstance, const char*, int32_t)        { return EMFE_ERR_UNSUPPORTED; }
extern "C" EMFE_EXPORT EmfeResult  EMFE_CALL emfe_save_settings(EmfeInstance)                                 { return EMFE_OK; }
extern "C" EMFE_EXPORT EmfeResult  EMFE_CALL emfe_load_settings(EmfeInstance)                                 { return EMFE_OK; }

// ============================================================================
// String utility
// ============================================================================

extern "C" EMFE_EXPORT void EMFE_CALL emfe_release_string(const char* /*str*/) {
    // No allocator handoff yet — strings returned from this plugin are
    // either nullptr or pointers into static storage.
}

// ============================================================================
// Phase 4.1: vendor-prefixed test/host disk-mount API
// ============================================================================
//
// The Settings-based mount path (Phase 4.4) reads a file from the
// host file system and feeds it through this same SetImage hook. For
// Phase 4.1 (CPU MMIO) we just expose a vendor-prefixed entry point so
// the unit tests can mount an in-memory image. Real host integrations
// (emfe_CsWPF / emfe_WinUI3Cpp) get their wiring in 4.4.
//
// `bytes` may be NULL when `size_bytes == 0` (unmount). On success
// the disk device is updated and any sticky-error state is cleared.

extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_z80_mount_disk_image(
    EmfeInstance instance,
    const uint8_t* bytes,
    uint32_t size_bytes,
    int32_t write_protect)
{
    if (!instance) return EMFE_ERR_INVALID;
    auto* inst = to_inst(instance);
    std::vector<uint8_t> v;
    if (size_bytes > 0) {
        if (!bytes) return EMFE_ERR_INVALID;
        v.assign(bytes, bytes + size_bytes);
    }
    inst->disk.SetImage(std::move(v), write_protect != 0);
    return EMFE_OK;
}
