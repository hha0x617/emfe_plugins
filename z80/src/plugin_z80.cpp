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
#include "Z80Memory.h"
#include "Z80Uart.h"
#include "Z80Timer.h"

#include <cstdint>
#include <cstring>
#include <atomic>
#include <mutex>
#include <new>
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
    std::atomic<EmfeState> state{EMFE_STATE_STOPPED};

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

    *out_instance = reinterpret_cast<EmfeInstance>(inst);
    return EMFE_OK;
}

extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_destroy(EmfeInstance instance) {
    if (!instance) return EMFE_ERR_INVALID;
    delete to_inst(instance);
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
        EmfeInstance, uint64_t, EmfeDisasmLine* /*out*/) {
    return EMFE_ERR_UNSUPPORTED;
}

extern "C" EMFE_EXPORT int32_t EMFE_CALL emfe_disassemble_range(
        EmfeInstance, uint64_t, uint64_t, EmfeDisasmLine* /*out*/, int32_t /*max*/) {
    return 0;
}

extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_get_program_range(
        EmfeInstance, uint64_t* /*start*/, uint64_t* /*end*/) {
    return EMFE_ERR_UNSUPPORTED;
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

extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_run(EmfeInstance instance) {
    return instance ? EMFE_ERR_UNSUPPORTED : EMFE_ERR_INVALID;
}

extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_stop(EmfeInstance instance) {
    return instance ? EMFE_OK : EMFE_ERR_INVALID;
}

extern "C" EMFE_EXPORT EmfeResult EMFE_CALL emfe_reset(EmfeInstance instance) {
    if (!instance) return EMFE_ERR_INVALID;
    auto* inst = to_inst(instance);
    inst->cpu.Reset();
    inst->memory.Reset();
    inst->uart.Reset();
    inst->timer.Reset();
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
        EmfeInstance, const EmfeSettingDef** out_defs) {
    if (out_defs) *out_defs = nullptr;
    return 0;
}

extern "C" EMFE_EXPORT const char* EMFE_CALL emfe_get_setting(EmfeInstance, const char*)              { return nullptr; }
extern "C" EMFE_EXPORT EmfeResult  EMFE_CALL emfe_set_setting(EmfeInstance, const char*, const char*) { return EMFE_ERR_UNSUPPORTED; }
extern "C" EMFE_EXPORT EmfeResult  EMFE_CALL emfe_apply_settings(EmfeInstance)                        { return EMFE_OK; }
extern "C" EMFE_EXPORT const char* EMFE_CALL emfe_get_applied_setting(EmfeInstance, const char*)      { return nullptr; }

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
