// SPDX-License-Identifier: MIT OR Apache-2.0
// Copyright (c) 2026 hha0x617
//
// plugin_em8.cpp - EM8 custom 8-bit CPU plugin for emfe
// Wraps Em8Cpu + Em8Memory + Em8Uart + Em8Timer as a C ABI plugin DLL.

#include "emfe_plugin.h"
#include "Em8Cpu.h"
#include "Em8Memory.h"
#include "Em8Uart.h"
#include "Em8Timer.h"

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

// ============================================================================
// Register IDs
// ============================================================================

enum RegId : uint32_t {
    REG_A  = 0,    // Accumulator
    REG_X  = 1,    // X index register
    REG_Y  = 2,    // Y index register
    REG_SP = 3,    // Stack pointer
    REG_PC = 4,    // Program counter
    REG_FL = 5,    // Flags register (N Z C V I)
    // Counters (read-only, hidden)
    REG_CYCLES       = 6,
    REG_INSTRUCTIONS = 7,
    REG_COUNT
};

// ============================================================================
// Instance
// ============================================================================

struct EmfeInstanceData {
    // Core emulator components
    Em8Memory memory;
    Em8Cpu cpu;
    Em8Uart uart;
    Em8Timer timer;

    // State management
    std::atomic<EmfeState> state{EMFE_STATE_STOPPED};
    std::atomic<bool> stopRequested{false};
    std::thread workerThread;

    // Callbacks
    EmfeConsoleCharCallback consoleCharCb = nullptr;
    void* consoleCharUserData = nullptr;
    EmfeStateChangeCallback stateChangeCb = nullptr;
    void* stateChangeUserData = nullptr;
    EmfeDiagnosticCallback diagnosticCb = nullptr;
    void* diagnosticUserData = nullptr;

    // Breakpoints
    std::unordered_map<uint16_t, EmfeBreakpointInfo> breakpoints;
    std::unordered_set<uint16_t> enabledBreakpoints;
    std::vector<std::string> bpConditionStorage;

    // Watchpoints
    struct WatchpointEntry {
        uint16_t address;
        uint32_t size;
        EmfeWatchpointType type;
        bool enabled;
        std::string condition;
    };
    std::unordered_map<uint16_t, WatchpointEntry> watchpoints;
    std::mutex watchpointsMutex;
    // Watchpoint hit detection uses memory.WatchpointHit / memory.WatchpointHitAddress

    // Step out
    std::atomic<int32_t> stepOutTargetDepth{-1};

    // Register definitions (built once)
    std::vector<EmfeRegisterDef> regDefs;

    // Settings
    std::vector<EmfeSettingDef> settingDefs;
    std::string settingValueBuf;
    std::string appliedSettingValueBuf;
    std::unordered_map<std::string, std::string> settings;        // committed (after apply)
    std::unordered_map<std::string, std::string> stagedSettings;  // staged (before apply)
    std::unordered_map<std::string, std::string> appliedSettings; // in effect on hardware
    std::unordered_map<std::string, uint32_t> settingFlags;       // per-key EMFE_SETTING_FLAG_*

    // Disassembly string storage (valid until next call)
    struct DisasmStringStorage {
        std::string rawBytes;
        std::string mnemonic;
        std::string operands;
    };
    std::vector<DisasmStringStorage> disasmStorage;

    // Program range
    uint16_t programStartAddress = 0;
    uint16_t programEndAddress = 0;

    std::string lastError;

    void EmulationLoop();
    void NotifyStateChange(EmfeState newState, EmfeStopReason reason,
                           uint64_t addr = 0, const char* msg = nullptr);
    void BuildRegisterDefs();
    void BuildSettingDefs();
    void OutputChar(uint8_t ch);
    void UpdateIrqLine() {
        cpu.IrqLine = timer.HasPendingIrq() || uart.HasPendingIrq();
    }
};

// ============================================================================
// Register definition builder
// ============================================================================

void EmfeInstanceData::BuildRegisterDefs() {
    regDefs.clear();

    auto addReg = [&](uint32_t id, const char* name, const char* group,
                      EmfeRegType type, uint32_t bits, uint32_t flags) {
        regDefs.push_back({id, name, group, type, bits, flags});
    };

    addReg(REG_A,  "A",  "CPU", EMFE_REG_INT, 8,  EMFE_REG_FLAG_NONE);
    addReg(REG_X,  "X",  "CPU", EMFE_REG_INT, 8,  EMFE_REG_FLAG_NONE);
    addReg(REG_Y,  "Y",  "CPU", EMFE_REG_INT, 8,  EMFE_REG_FLAG_NONE);
    addReg(REG_SP, "SP", "CPU", EMFE_REG_INT, 8,  EMFE_REG_FLAG_SP);
    addReg(REG_PC, "PC", "CPU", EMFE_REG_INT, 16, EMFE_REG_FLAG_PC);
    addReg(REG_FL, "FL", "CPU", EMFE_REG_INT, 8,  EMFE_REG_FLAG_FLAGS);

    addReg(REG_CYCLES,       "Cycles",       "Counters", EMFE_REG_INT, 64,
           EMFE_REG_FLAG_READONLY | EMFE_REG_FLAG_HIDDEN);
    addReg(REG_INSTRUCTIONS, "Instructions", "Counters", EMFE_REG_INT, 64,
           EMFE_REG_FLAG_READONLY | EMFE_REG_FLAG_HIDDEN);
}

// ============================================================================
// Setting definitions builder
// ============================================================================

void EmfeInstanceData::BuildSettingDefs() {
    settingDefs.clear();
    const uint32_t R = EMFE_SETTING_FLAG_REQUIRES_RESET;

    auto add = [&](const char* key, const char* label, const char* group,
                   EmfeSettingType type, const char* defVal = "",
                   const char* constraints = nullptr,
                   uint32_t flags = 0) {
        settingDefs.push_back({ key, label, group, type, defVal, constraints,
                                nullptr, nullptr, flags });
        settingFlags[key] = flags;
        if (settings.find(key) == settings.end())
            settings[key] = defVal;
        if (stagedSettings.find(key) == stagedSettings.end())
            stagedSettings[key] = defVal;
        if (appliedSettings.find(key) == appliedSettings.end())
            appliedSettings[key] = defVal;
    };

    // General tab
    add("MemorySize", "Memory Size (KB)", "General", EMFE_SETTING_INT, "64", "1|64", R);
    add("Theme",      "Theme",            "General", EMFE_SETTING_COMBO, "Dark", "Dark|Light|System");

    // EM8 tab (board-specific)
    add("TimerReload", "Timer Reload Value", "EM8", EMFE_SETTING_INT, "4096", "1|65535", R);

    // Console tab
    add("ConsoleScrollbackLines", "Scrollback Lines", "Console", EMFE_SETTING_INT, "2000", "0|100000");
    add("ConsoleColumns",         "Columns",          "Console", EMFE_SETTING_INT, "80",   "40|320");
    add("ConsoleRows",            "Rows",             "Console", EMFE_SETTING_INT, "25",   "10|100");
}

// ============================================================================
// State notification
// ============================================================================

void EmfeInstanceData::NotifyStateChange(EmfeState newState, EmfeStopReason reason,
                                          uint64_t addr, const char* msg) {
    state.store(newState, std::memory_order_release);
    if (stateChangeCb) {
        EmfeStateInfo info{};
        info.state = newState;
        info.stop_reason = reason;
        info.stop_address = addr;
        info.stop_message = msg;
        stateChangeCb(stateChangeUserData, &info);
    }
}

// ============================================================================
// Console output helper
// ============================================================================

void EmfeInstanceData::OutputChar(uint8_t ch) {
    if (consoleCharCb)
        consoleCharCb(consoleCharUserData, static_cast<char>(ch));
}

// ============================================================================
// Condition expression evaluator
// Supports: A, X, Y, SP, PC, FL registers
//           $hex, 0xhex, decimal literals
//           ==, !=, <, >, <=, >=, &, &&, ||, ()
// ============================================================================

namespace {

struct CondParser {
    const char* p;
    const Em8Cpu& cpu;

    void skipWS() { while (*p == ' ' || *p == '\t') ++p; }

    bool tryChar(char c) { skipWS(); if (*p == c) { ++p; return true; } return false; }

    uint32_t parseNumber() {
        skipWS();
        if (*p == '$') { ++p; return static_cast<uint32_t>(strtoul(p, const_cast<char**>(&p), 16)); }
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { p += 2; return static_cast<uint32_t>(strtoul(p, const_cast<char**>(&p), 16)); }
        return static_cast<uint32_t>(strtoul(p, const_cast<char**>(&p), 10));
    }

    bool isWordChar(char c) {
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
    }

    bool tryReg(uint32_t& val) {
        skipWS();
        if ((p[0] == 'A' || p[0] == 'a') && !isWordChar(p[1])) {
            val = cpu.A; ++p; return true;
        }
        if ((p[0] == 'X' || p[0] == 'x') && !isWordChar(p[1])) {
            val = cpu.X; ++p; return true;
        }
        if ((p[0] == 'Y' || p[0] == 'y') && !isWordChar(p[1])) {
            val = cpu.Y; ++p; return true;
        }
        if (((p[0] == 'S' || p[0] == 's') && (p[1] == 'P' || p[1] == 'p')) && !isWordChar(p[2])) {
            val = cpu.SP; p += 2; return true;
        }
        if (((p[0] == 'P' || p[0] == 'p') && (p[1] == 'C' || p[1] == 'c')) && !isWordChar(p[2])) {
            val = cpu.PC; p += 2; return true;
        }
        if (((p[0] == 'F' || p[0] == 'f') && (p[1] == 'L' || p[1] == 'l')) && !isWordChar(p[2])) {
            val = cpu.FL; p += 2; return true;
        }
        return false;
    }

    uint32_t parsePrimary() {
        skipWS();
        uint32_t val;
        if (tryReg(val)) return val;
        if (*p == '(') { ++p; val = parseOr(); tryChar(')'); return val; }
        return parseNumber();
    }

    uint32_t parseBitAnd() {
        uint32_t left = parsePrimary();
        while (true) {
            skipWS();
            if (*p == '&' && p[1] != '&') { ++p; left &= parsePrimary(); }
            else break;
        }
        return left;
    }

    uint32_t parseCompare() {
        uint32_t left = parseBitAnd();
        skipWS();
        if (p[0] == '=' && p[1] == '=') { p += 2; return left == parseBitAnd() ? 1u : 0u; }
        if (p[0] == '!' && p[1] == '=') { p += 2; return left != parseBitAnd() ? 1u : 0u; }
        if (p[0] == '<' && p[1] == '=') { p += 2; return left <= parseBitAnd() ? 1u : 0u; }
        if (p[0] == '>' && p[1] == '=') { p += 2; return left >= parseBitAnd() ? 1u : 0u; }
        if (p[0] == '<') { ++p; return left < parseBitAnd() ? 1u : 0u; }
        if (p[0] == '>') { ++p; return left > parseBitAnd() ? 1u : 0u; }
        return left;
    }

    uint32_t parseAnd() {
        uint32_t left = parseCompare();
        while (true) {
            skipWS();
            if (p[0] == '&' && p[1] == '&') { p += 2; uint32_t right = parseCompare(); left = (left && right) ? 1u : 0u; }
            else break;
        }
        return left;
    }

    uint32_t parseOr() {
        uint32_t left = parseAnd();
        while (true) {
            skipWS();
            if (p[0] == '|' && p[1] == '|') { p += 2; uint32_t right = parseAnd(); left = (left || right) ? 1u : 0u; }
            else break;
        }
        return left;
    }

    bool evaluate() { return parseOr() != 0; }
};

bool EvaluateCondition(const Em8Cpu& cpu, const char* condition) {
    if (!condition || !*condition) return true;
    try {
        CondParser parser{condition, cpu};
        return parser.evaluate();
    } catch (...) {
        return true;
    }
}

} // anonymous namespace

// ============================================================================
// Emulation loop (runs on worker thread)
// ============================================================================

void EmfeInstanceData::EmulationLoop() {
    try {
        while (!stopRequested.load(std::memory_order_relaxed)) {
            // Check breakpoints (with condition evaluation)
            if (!enabledBreakpoints.empty() && enabledBreakpoints.contains(cpu.PC)) {
                auto bpIt = breakpoints.find(cpu.PC);
                if (bpIt == breakpoints.end() || !bpIt->second.condition ||
                    EvaluateCondition(cpu, bpIt->second.condition)) {
                    NotifyStateChange(EMFE_STATE_STOPPED, EMFE_STOP_REASON_BREAKPOINT,
                                      cpu.PC, "Breakpoint hit");
                    return;
                }
            }

            // Execute one instruction
            int cycles = cpu.ExecuteOne();
            timer.Tick(cycles);
            UpdateIrqLine();
            cpu.CheckInterrupts();

            if (cpu.Halted) {
                NotifyStateChange(EMFE_STATE_HALTED, EMFE_STOP_REASON_HALT,
                                  cpu.PC, "CPU halted");
                return;
            }

            // Check watchpoint hit (with condition evaluation)
            if (memory.WatchpointHit.exchange(false)) {
                uint16_t wpAddr = memory.WatchpointHitAddress.load();
                bool shouldStop = true;
                {
                    std::lock_guard<std::mutex> lock(watchpointsMutex);
                    auto wpIt = watchpoints.find(wpAddr);
                    if (wpIt != watchpoints.end() && !wpIt->second.condition.empty())
                        shouldStop = EvaluateCondition(cpu, wpIt->second.condition.c_str());
                }
                if (shouldStop) {
                    NotifyStateChange(EMFE_STATE_STOPPED, EMFE_STOP_REASON_WATCHPOINT,
                                      wpAddr, "Watchpoint hit");
                    return;
                }
            }

            // Step-out target: stop when shadow stack has unwound to or below target depth
            int32_t target = stepOutTargetDepth.load(std::memory_order_relaxed);
            if (target >= 0 && cpu.ShadowStackTop <= target) {
                stepOutTargetDepth.store(-1, std::memory_order_relaxed);
                NotifyStateChange(EMFE_STATE_STOPPED, EMFE_STOP_REASON_STEP,
                                  cpu.PC, "Step out");
                return;
            }
        }

        NotifyStateChange(EMFE_STATE_STOPPED, EMFE_STOP_REASON_USER,
                          cpu.PC, "Stopped by user");
    } catch (const std::exception& ex) {
        lastError = std::string("Emulation exception: ") + ex.what();
        NotifyStateChange(EMFE_STATE_HALTED, EMFE_STOP_REASON_EXCEPTION,
                          cpu.PC, lastError.c_str());
    } catch (...) {
        lastError = "Emulation: unknown exception";
        NotifyStateChange(EMFE_STATE_HALTED, EMFE_STOP_REASON_EXCEPTION,
                          cpu.PC, lastError.c_str());
    }
}

// ============================================================================
// Board info (static)
// ============================================================================

static EmfeBoardInfo s_boardInfo = {
    "EM8",
    "EM8",
    "Custom 8-bit CPU with UART, Timer, GPIO",
    "1.0.0",
    EMFE_CAP_LOAD_SREC | EMFE_CAP_LOAD_BINARY |
    EMFE_CAP_STEP_OVER | EMFE_CAP_STEP_OUT | EMFE_CAP_CALL_STACK |
    EMFE_CAP_WATCHPOINTS
};

// ============================================================================
// API Implementation
// ============================================================================

// ---------- Discovery & Lifecycle ----------

EmfeResult EMFE_CALL emfe_negotiate(const EmfeNegotiateInfo* info) {
    if (!info) return EMFE_ERR_INVALID;
    if (info->api_version_major != EMFE_API_VERSION_MAJOR) return EMFE_ERR_UNSUPPORTED;
    return EMFE_OK;
}

EmfeResult EMFE_CALL emfe_get_board_info(EmfeBoardInfo* out_info) {
    if (!out_info) return EMFE_ERR_INVALID;
    *out_info = s_boardInfo;
    return EMFE_OK;
}

EmfeResult EMFE_CALL emfe_create(EmfeInstance* out_instance) {
    if (!out_instance) return EMFE_ERR_INVALID;

    auto inst = new (std::nothrow) EmfeInstanceData();
    if (!inst) return EMFE_ERR_MEMORY;

    try {
        // Wire components together
        inst->cpu.Memory = &inst->memory;
        inst->memory.Uart = &inst->uart;
        inst->memory.Timer = &inst->timer;

        // UART TX callback -> console char callback
        inst->uart.TxCallback = [inst](uint8_t ch) {
            inst->OutputChar(ch);
        };

        // Timer IRQ is checked in CheckInterrupts via timer.HasPendingIrq()
        // Watchpoints are checked via memory.WatchpointHit atomic flag

        inst->cpu.Reset();
        inst->BuildRegisterDefs();
        inst->BuildSettingDefs();

        *out_instance = reinterpret_cast<EmfeInstance>(inst);
        return EMFE_OK;
    } catch (...) {
        delete inst;
        return EMFE_ERR_MEMORY;
    }
}

EmfeResult EMFE_CALL emfe_destroy(EmfeInstance instance) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);

    // Stop emulation if running
    inst->stopRequested.store(true, std::memory_order_release);
    if (inst->workerThread.joinable())
        inst->workerThread.join();

    delete inst;
    return EMFE_OK;
}

// ---------- Callbacks ----------

EmfeResult EMFE_CALL emfe_set_console_char_callback(
    EmfeInstance instance, EmfeConsoleCharCallback callback, void* user_data) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    inst->consoleCharCb = callback;
    inst->consoleCharUserData = user_data;
    return EMFE_OK;
}

EmfeResult EMFE_CALL emfe_set_state_change_callback(
    EmfeInstance instance, EmfeStateChangeCallback callback, void* user_data) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    inst->stateChangeCb = callback;
    inst->stateChangeUserData = user_data;
    return EMFE_OK;
}

EmfeResult EMFE_CALL emfe_set_diagnostic_callback(
    EmfeInstance instance, EmfeDiagnosticCallback callback, void* user_data) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    inst->diagnosticCb = callback;
    inst->diagnosticUserData = user_data;
    return EMFE_OK;
}

// ---------- Registers ----------

int32_t EMFE_CALL emfe_get_register_defs(EmfeInstance instance, const EmfeRegisterDef** out_defs) {
    if (!instance || !out_defs) return 0;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    *out_defs = inst->regDefs.data();
    return static_cast<int32_t>(inst->regDefs.size());
}

extern "C" int32_t EMFE_CALL emfe_get_register_flag_defs(
    EmfeInstance /*instance*/, uint32_t reg_id,
    const EmfeRegFlagBitDef** out_defs)
{
    if (!out_defs) return 0;
    *out_defs = nullptr;

    // em8 FL (8-bit) bit decomposition. Layout per Em8Cpu.h's FLAG_*
    // constants (matches a 6502-style status register):
    //   bit 0 : C (Carry)
    //   bit 1 : Z (Zero)
    //   bit 2 : I (Interrupt mask)
    //   bit 4 : B (Break)
    //   bit 6 : V (Overflow)
    //   bit 7 : N (Negative)
    // Bits 3 and 5 are reserved and not exposed.
    // Order is MSB-first (N V B I Z C) so the on-screen sequence
    // matches the way 6502 family status registers are usually written.
    static const EmfeRegFlagBitDef fl_bits[] = {
        { 7, "N" },
        { 6, "V" },
        { 4, "B" },
        { 2, "I" },
        { 1, "Z" },
        { 0, "C" },
    };

    if (reg_id == REG_FL) {
        *out_defs = fl_bits;
        return static_cast<int32_t>(sizeof(fl_bits) / sizeof(fl_bits[0]));
    }
    return 0;
}

EmfeResult EMFE_CALL emfe_get_registers(EmfeInstance instance, EmfeRegValue* values, int32_t count) {
    if (!instance || !values) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);

    for (int32_t i = 0; i < count; i++) {
        auto& v = values[i];
        v.value.u64 = 0;

        switch (v.reg_id) {
        case REG_A:  v.value.u64 = inst->cpu.A;  break;
        case REG_X:  v.value.u64 = inst->cpu.X;  break;
        case REG_Y:  v.value.u64 = inst->cpu.Y;  break;
        case REG_SP: v.value.u64 = inst->cpu.SP; break;
        case REG_PC: v.value.u64 = inst->cpu.PC; break;
        case REG_FL: v.value.u64 = inst->cpu.FL; break;
        case REG_CYCLES:       v.value.u64 = static_cast<uint64_t>(inst->cpu.CycleCount);       break;
        case REG_INSTRUCTIONS: v.value.u64 = static_cast<uint64_t>(inst->cpu.InstructionCount); break;
        default:
            return EMFE_ERR_INVALID;
        }
    }
    return EMFE_OK;
}

EmfeResult EMFE_CALL emfe_set_registers(EmfeInstance instance, const EmfeRegValue* values, int32_t count) {
    if (!instance || !values) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    if (inst->state.load() == EMFE_STATE_RUNNING) return EMFE_ERR_STATE;

    for (int32_t i = 0; i < count; i++) {
        const auto& v = values[i];
        switch (v.reg_id) {
        case REG_A:  inst->cpu.A  = static_cast<uint8_t>(v.value.u64); break;
        case REG_X:  inst->cpu.X  = static_cast<uint8_t>(v.value.u64); break;
        case REG_Y:  inst->cpu.Y  = static_cast<uint8_t>(v.value.u64); break;
        case REG_SP: inst->cpu.SP = static_cast<uint8_t>(v.value.u64); break;
        case REG_PC: inst->cpu.PC = static_cast<uint16_t>(v.value.u64); break;
        case REG_FL: inst->cpu.FL = static_cast<uint8_t>(v.value.u64); break;
        case REG_CYCLES:
        case REG_INSTRUCTIONS:
            break; // Read-only — ignore writes
        default:
            return EMFE_ERR_INVALID;
        }
    }
    return EMFE_OK;
}

// ---------- Memory ----------

uint8_t EMFE_CALL emfe_peek_byte(EmfeInstance instance, uint64_t address) {
    if (!instance) return 0;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    return inst->memory.Peek(static_cast<uint16_t>(address));
}

uint16_t EMFE_CALL emfe_peek_word(EmfeInstance instance, uint64_t address) {
    if (!instance) return 0;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    uint16_t addr = static_cast<uint16_t>(address);
    return static_cast<uint16_t>(inst->memory.Peek(addr)) |
           (static_cast<uint16_t>(inst->memory.Peek(addr + 1)) << 8);
}

uint32_t EMFE_CALL emfe_peek_long(EmfeInstance instance, uint64_t address) {
    if (!instance) return 0;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    uint16_t addr = static_cast<uint16_t>(address);
    return static_cast<uint32_t>(inst->memory.Peek(addr)) |
           (static_cast<uint32_t>(inst->memory.Peek(addr + 1)) << 8) |
           (static_cast<uint32_t>(inst->memory.Peek(addr + 2)) << 16) |
           (static_cast<uint32_t>(inst->memory.Peek(addr + 3)) << 24);
}

EmfeResult EMFE_CALL emfe_poke_byte(EmfeInstance instance, uint64_t address, uint8_t value) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    inst->memory.Poke(static_cast<uint16_t>(address), value);
    return EMFE_OK;
}

EmfeResult EMFE_CALL emfe_poke_word(EmfeInstance instance, uint64_t address, uint16_t value) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    uint16_t addr = static_cast<uint16_t>(address);
    inst->memory.Poke(addr,     static_cast<uint8_t>(value & 0xFF));
    inst->memory.Poke(addr + 1, static_cast<uint8_t>((value >> 8) & 0xFF));
    return EMFE_OK;
}

EmfeResult EMFE_CALL emfe_poke_long(EmfeInstance instance, uint64_t address, uint32_t value) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    uint16_t addr = static_cast<uint16_t>(address);
    inst->memory.Poke(addr,     static_cast<uint8_t>(value & 0xFF));
    inst->memory.Poke(addr + 1, static_cast<uint8_t>((value >> 8) & 0xFF));
    inst->memory.Poke(addr + 2, static_cast<uint8_t>((value >> 16) & 0xFF));
    inst->memory.Poke(addr + 3, static_cast<uint8_t>((value >> 24) & 0xFF));
    return EMFE_OK;
}

EmfeResult EMFE_CALL emfe_peek_range(EmfeInstance instance, uint64_t address,
                                      uint8_t* out_data, uint32_t length) {
    if (!instance || !out_data) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    uint16_t addr = static_cast<uint16_t>(address);
    for (uint32_t i = 0; i < length; i++)
        out_data[i] = inst->memory.Peek(static_cast<uint16_t>(addr + i));
    return EMFE_OK;
}

uint64_t EMFE_CALL emfe_get_memory_size(EmfeInstance instance) {
    if (!instance) return 0;
    return 65536; // 16-bit address space
}

// ---------- Disassembly ----------

EmfeResult EMFE_CALL emfe_disassemble_one(EmfeInstance instance, uint64_t address,
                                           EmfeDisasmLine* out_line) {
    if (!instance || !out_line) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);

    auto result = inst->cpu.Disassemble(static_cast<uint16_t>(address));

    // Store strings (valid until next call)
    inst->disasmStorage.resize(1);
    inst->disasmStorage[0].rawBytes = result.rawBytes;
    inst->disasmStorage[0].mnemonic = result.mnemonic;
    inst->disasmStorage[0].operands = result.operands;

    out_line->address = address;
    out_line->raw_bytes = inst->disasmStorage[0].rawBytes.c_str();
    out_line->mnemonic = inst->disasmStorage[0].mnemonic.c_str();
    out_line->operands = inst->disasmStorage[0].operands.c_str();
    out_line->length = result.length;
    return EMFE_OK;
}

int32_t EMFE_CALL emfe_disassemble_range(EmfeInstance instance, uint64_t start_address,
                                          uint64_t end_address, EmfeDisasmLine* out_lines,
                                          int32_t max_lines) {
    if (!instance || !out_lines || max_lines <= 0) return 0;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);

    uint16_t addr = static_cast<uint16_t>(start_address);
    uint16_t end = static_cast<uint16_t>(end_address);
    int32_t count = 0;

    inst->disasmStorage.clear();
    inst->disasmStorage.reserve(max_lines);

    // First pass: collect all disassembly results
    while (addr < end && count < max_lines) {
        auto result = inst->cpu.Disassemble(addr);
        inst->disasmStorage.push_back({result.rawBytes, result.mnemonic, result.operands});
        out_lines[count].address = addr;
        out_lines[count].length = result.length;
        addr += static_cast<uint16_t>(result.length);
        count++;
    }

    // Second pass: set string pointers (safe — no more reallocation)
    for (int32_t i = 0; i < count; i++) {
        out_lines[i].raw_bytes = inst->disasmStorage[i].rawBytes.c_str();
        out_lines[i].mnemonic = inst->disasmStorage[i].mnemonic.c_str();
        out_lines[i].operands = inst->disasmStorage[i].operands.c_str();
    }
    return count;
}

EmfeResult EMFE_CALL emfe_get_program_range(EmfeInstance instance, uint64_t* out_start, uint64_t* out_end) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    if (out_start) *out_start = inst->programStartAddress;
    if (out_end)   *out_end   = inst->programEndAddress;
    return EMFE_OK;
}

// ---------- Execution ----------

EmfeResult EMFE_CALL emfe_step(EmfeInstance instance) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    if (inst->state.load() == EMFE_STATE_RUNNING) return EMFE_ERR_STATE;

    inst->state.store(EMFE_STATE_STEPPING);
    int cycles = inst->cpu.ExecuteOne();
    inst->timer.Tick(cycles);
    inst->UpdateIrqLine();
    inst->cpu.CheckInterrupts();
    inst->state.store(inst->cpu.Halted ? EMFE_STATE_HALTED : EMFE_STATE_STOPPED);

    inst->NotifyStateChange(inst->state.load(), EMFE_STOP_REASON_STEP,
                            inst->cpu.PC, nullptr);
    return EMFE_OK;
}

EmfeResult EMFE_CALL emfe_step_over(EmfeInstance instance) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    if (inst->state.load() == EMFE_STATE_RUNNING) return EMFE_ERR_STATE;

    // Disassemble current instruction to check if it's JSR
    auto line = inst->cpu.Disassemble(inst->cpu.PC);
    bool isCall = (line.mnemonic == "JSR");

    if (!isCall) {
        // Not a call — just step
        return emfe_step(instance);
    }

    // Wait for any pending previous worker thread to finish
    if (inst->workerThread.joinable())
        inst->workerThread.join();

    // Set temporary breakpoint at next instruction
    uint16_t nextPC = inst->cpu.PC + static_cast<uint16_t>(line.length);
    bool hadBP = inst->enabledBreakpoints.contains(nextPC);
    if (!hadBP)
        inst->enabledBreakpoints.insert(nextPC);

    // Run
    inst->stopRequested.store(false);
    inst->state.store(EMFE_STATE_RUNNING);

    inst->workerThread = std::thread([inst, nextPC, hadBP]() {
        inst->EmulationLoop();
        if (!hadBP)
            inst->enabledBreakpoints.erase(nextPC);
    });

    return EMFE_OK;
}

EmfeResult EMFE_CALL emfe_step_out(EmfeInstance instance) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    if (inst->state.load() == EMFE_STATE_RUNNING) return EMFE_ERR_STATE;

    if (inst->cpu.ShadowStackTop > 0) {
        if (inst->workerThread.joinable())
            inst->workerThread.join();

        int32_t targetDepth = inst->cpu.ShadowStackTop - 1;
        inst->stepOutTargetDepth.store(targetDepth, std::memory_order_relaxed);
        inst->stopRequested.store(false);
        inst->state.store(EMFE_STATE_RUNNING);

        inst->workerThread = std::thread([inst]() {
            inst->EmulationLoop();
            inst->stepOutTargetDepth.store(-1, std::memory_order_relaxed);
        });
    }

    return EMFE_OK;
}

EmfeResult EMFE_CALL emfe_run(EmfeInstance instance) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    if (inst->state.load() == EMFE_STATE_RUNNING) return EMFE_ERR_STATE;

    // Wait for previous thread to finish
    if (inst->workerThread.joinable())
        inst->workerThread.join();

    // If PC is sitting on a breakpoint, execute one instruction first
    // to avoid immediately re-triggering the same breakpoint.
    if (!inst->enabledBreakpoints.empty() && inst->enabledBreakpoints.contains(inst->cpu.PC)) {
        int cycles = inst->cpu.ExecuteOne();
        inst->timer.Tick(cycles);
        inst->cpu.CheckInterrupts();
        if (inst->cpu.Halted) {
            inst->NotifyStateChange(EMFE_STATE_HALTED, EMFE_STOP_REASON_HALT,
                                    inst->cpu.PC, "CPU halted");
            return EMFE_OK;
        }
    }

    inst->stopRequested.store(false);
    inst->state.store(EMFE_STATE_RUNNING);

    inst->workerThread = std::thread([inst]() {
        inst->EmulationLoop();
    });

    return EMFE_OK;
}

EmfeResult EMFE_CALL emfe_stop(EmfeInstance instance) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    inst->stopRequested.store(true, std::memory_order_release);

    if (inst->workerThread.joinable())
        inst->workerThread.join();

    return EMFE_OK;
}

EmfeResult EMFE_CALL emfe_reset(EmfeInstance instance) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    if (inst->state.load() == EMFE_STATE_RUNNING) return EMFE_ERR_STATE;

    // Flush deferred REQUIRES_RESET settings now that it's safe to rebuild devices.
    inst->appliedSettings = inst->settings;

    inst->cpu.Reset();
    inst->uart.Reset();
    inst->timer.Reset();
    inst->state.store(EMFE_STATE_STOPPED);
    inst->NotifyStateChange(EMFE_STATE_STOPPED, EMFE_STOP_REASON_NONE,
                            inst->cpu.PC, "Reset");
    return EMFE_OK;
}

EmfeState EMFE_CALL emfe_get_state(EmfeInstance instance) {
    if (!instance) return EMFE_STATE_STOPPED;
    return reinterpret_cast<EmfeInstanceData*>(instance)->state.load();
}

int64_t EMFE_CALL emfe_get_instruction_count(EmfeInstance instance) {
    if (!instance) return 0;
    return reinterpret_cast<EmfeInstanceData*>(instance)->cpu.InstructionCount;
}

int64_t EMFE_CALL emfe_get_cycle_count(EmfeInstance instance) {
    if (!instance) return 0;
    return reinterpret_cast<EmfeInstanceData*>(instance)->cpu.CycleCount;
}

// ---------- Breakpoints ----------

EmfeResult EMFE_CALL emfe_add_breakpoint(EmfeInstance instance, uint64_t address) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    uint16_t addr = static_cast<uint16_t>(address);

    EmfeBreakpointInfo bp{};
    bp.address = address;
    bp.enabled = true;
    bp.condition = nullptr;
    inst->breakpoints[addr] = bp;
    inst->enabledBreakpoints.insert(addr);
    return EMFE_OK;
}

EmfeResult EMFE_CALL emfe_remove_breakpoint(EmfeInstance instance, uint64_t address) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    uint16_t addr = static_cast<uint16_t>(address);
    inst->breakpoints.erase(addr);
    inst->enabledBreakpoints.erase(addr);
    return EMFE_OK;
}

EmfeResult EMFE_CALL emfe_enable_breakpoint(EmfeInstance instance, uint64_t address, bool enabled) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    uint16_t addr = static_cast<uint16_t>(address);

    auto it = inst->breakpoints.find(addr);
    if (it == inst->breakpoints.end()) return EMFE_ERR_NOTFOUND;

    it->second.enabled = enabled;
    if (enabled)
        inst->enabledBreakpoints.insert(addr);
    else
        inst->enabledBreakpoints.erase(addr);
    return EMFE_OK;
}

EmfeResult EMFE_CALL emfe_set_breakpoint_condition(EmfeInstance instance, uint64_t address,
                                                    const char* condition) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    uint16_t addr = static_cast<uint16_t>(address);

    auto it = inst->breakpoints.find(addr);
    if (it == inst->breakpoints.end()) return EMFE_ERR_NOTFOUND;

    if (condition) {
        inst->bpConditionStorage.push_back(condition);
        it->second.condition = inst->bpConditionStorage.back().c_str();
    } else {
        it->second.condition = nullptr;
    }
    return EMFE_OK;
}

EmfeResult EMFE_CALL emfe_clear_breakpoints(EmfeInstance instance) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    inst->breakpoints.clear();
    inst->enabledBreakpoints.clear();
    inst->bpConditionStorage.clear();
    return EMFE_OK;
}

int32_t EMFE_CALL emfe_get_breakpoints(EmfeInstance instance, EmfeBreakpointInfo* out_breakpoints,
                                        int32_t max_count) {
    if (!instance || !out_breakpoints || max_count <= 0) return 0;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);

    int32_t i = 0;
    for (auto& [addr, bp] : inst->breakpoints) {
        if (i >= max_count) break;
        out_breakpoints[i] = bp;
        i++;
    }
    return i;
}

// ---------- Watchpoints ----------

EmfeResult EMFE_CALL emfe_add_watchpoint(EmfeInstance instance, uint64_t address,
                                          EmfeWatchpointSize size, EmfeWatchpointType type) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    uint16_t addr = static_cast<uint16_t>(address);
    std::lock_guard<std::mutex> lock(inst->watchpointsMutex);
    inst->watchpoints[addr] = {addr, static_cast<uint32_t>(size), type, true, ""};
    return EMFE_OK;
}

EmfeResult EMFE_CALL emfe_remove_watchpoint(EmfeInstance instance, uint64_t address) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    uint16_t addr = static_cast<uint16_t>(address);
    std::lock_guard<std::mutex> lock(inst->watchpointsMutex);
    inst->watchpoints.erase(addr);
    return EMFE_OK;
}

EmfeResult EMFE_CALL emfe_enable_watchpoint(EmfeInstance instance, uint64_t address, bool enabled) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    uint16_t addr = static_cast<uint16_t>(address);
    std::lock_guard<std::mutex> lock(inst->watchpointsMutex);
    auto it = inst->watchpoints.find(addr);
    if (it == inst->watchpoints.end()) return EMFE_ERR_NOTFOUND;
    it->second.enabled = enabled;
    return EMFE_OK;
}

EmfeResult EMFE_CALL emfe_set_watchpoint_condition(EmfeInstance instance, uint64_t address,
                                                    const char* condition) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    uint16_t addr = static_cast<uint16_t>(address);
    std::lock_guard<std::mutex> lock(inst->watchpointsMutex);
    auto it = inst->watchpoints.find(addr);
    if (it == inst->watchpoints.end()) return EMFE_ERR_NOTFOUND;
    it->second.condition = condition ? condition : "";
    return EMFE_OK;
}

EmfeResult EMFE_CALL emfe_clear_watchpoints(EmfeInstance instance) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    std::lock_guard<std::mutex> lock(inst->watchpointsMutex);
    inst->watchpoints.clear();
    return EMFE_OK;
}

int32_t EMFE_CALL emfe_get_watchpoints(EmfeInstance instance, EmfeWatchpointInfo* out_watchpoints,
                                        int32_t max_count) {
    if (!instance || !out_watchpoints || max_count <= 0) return 0;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    std::lock_guard<std::mutex> lock(inst->watchpointsMutex);
    int32_t i = 0;
    for (auto& [addr, wp] : inst->watchpoints) {
        if (i >= max_count) break;
        out_watchpoints[i].address = wp.address;
        out_watchpoints[i].size = static_cast<EmfeWatchpointSize>(wp.size);
        out_watchpoints[i].type = wp.type;
        out_watchpoints[i].enabled = wp.enabled;
        out_watchpoints[i].condition = wp.condition.empty() ? nullptr : wp.condition.c_str();
        i++;
    }
    return i;
}

// ---------- Call Stack ----------

int32_t EMFE_CALL emfe_get_call_stack(EmfeInstance instance, EmfeCallStackEntry* out_entries,
                                       int32_t max_count) {
    if (!instance || !out_entries || max_count <= 0) return 0;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);

    int32_t count = (std::min)(static_cast<int32_t>(inst->cpu.ShadowStackTop), max_count);
    for (int32_t i = 0; i < count; i++) {
        uint16_t retAddr = inst->cpu.ShadowStack[count - 1 - i];
        out_entries[i].call_pc = 0;
        out_entries[i].target_pc = 0;
        out_entries[i].return_pc = retAddr;
        out_entries[i].frame_pointer = 0;
        out_entries[i].kind = EMFE_CALL_KIND_CALL;
        out_entries[i].label = nullptr;
    }
    return count;
}

// ---------- Framebuffer (unsupported) ----------

EmfeResult EMFE_CALL emfe_get_framebuffer_info(EmfeInstance instance, EmfeFramebufferInfo* out_info) {
    (void)instance; (void)out_info;
    return EMFE_ERR_UNSUPPORTED;
}

uint32_t EMFE_CALL emfe_get_palette_entry(EmfeInstance instance, uint32_t index) {
    (void)instance; (void)index;
    return 0;
}

int32_t EMFE_CALL emfe_get_palette(EmfeInstance instance, uint32_t* out_colors, int32_t max_count) {
    (void)instance; (void)out_colors; (void)max_count;
    return 0;
}

// ---------- Input events (unsupported) ----------

EmfeResult EMFE_CALL emfe_push_key(EmfeInstance instance, uint32_t scancode, bool pressed) {
    (void)instance; (void)scancode; (void)pressed;
    return EMFE_ERR_UNSUPPORTED;
}

EmfeResult EMFE_CALL emfe_push_mouse_move(EmfeInstance instance, int32_t dx, int32_t dy) {
    (void)instance; (void)dx; (void)dy;
    return EMFE_ERR_UNSUPPORTED;
}

EmfeResult EMFE_CALL emfe_push_mouse_absolute(EmfeInstance instance, int32_t x, int32_t y) {
    (void)instance; (void)x; (void)y;
    return EMFE_ERR_UNSUPPORTED;
}

EmfeResult EMFE_CALL emfe_push_mouse_button(EmfeInstance instance, int32_t button, bool pressed) {
    (void)instance; (void)button; (void)pressed;
    return EMFE_ERR_UNSUPPORTED;
}

// ---------- File Loading ----------

EmfeResult EMFE_CALL emfe_load_elf(EmfeInstance instance, const char* file_path) {
    (void)instance; (void)file_path;
    return EMFE_ERR_UNSUPPORTED; // 8-bit CPU doesn't use ELF
}

EmfeResult EMFE_CALL emfe_load_binary(EmfeInstance instance, const char* file_path,
                                       uint64_t load_address) {
    if (!instance || !file_path) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    if (inst->state.load() == EMFE_STATE_RUNNING) return EMFE_ERR_STATE;

    try {
        std::ifstream file(file_path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            inst->lastError = std::string("Cannot open file: ") + file_path;
            return EMFE_ERR_IO;
        }

        auto size = file.tellg();
        file.seekg(0, std::ios::beg);

        std::vector<uint8_t> data(static_cast<size_t>(size));
        if (!file.read(reinterpret_cast<char*>(data.data()), size)) {
            inst->lastError = std::string("Failed to read file: ") + file_path;
            return EMFE_ERR_IO;
        }

        uint16_t addr = static_cast<uint16_t>(load_address);
        for (size_t i = 0; i < data.size() && (addr + i) < 0x10000; i++)
            inst->memory.Poke(static_cast<uint16_t>(addr + i), data[i]);

        inst->programStartAddress = addr;
        inst->programEndAddress = static_cast<uint16_t>(addr + data.size());

        // Read reset vector from $FFFC-$FFFD if the image covers that area
        uint16_t resetVec = static_cast<uint16_t>(
            inst->memory.Peek(0xFFFC) | (inst->memory.Peek(0xFFFD) << 8));
        if (resetVec != 0)
            inst->cpu.PC = resetVec;
        else
            inst->cpu.PC = addr;
        inst->cpu.SP = 0xFF;
        inst->lastError.clear();
        return EMFE_OK;
    } catch (const std::exception& ex) {
        inst->lastError = ex.what();
        return EMFE_ERR_IO;
    }
}

EmfeResult EMFE_CALL emfe_load_srec(EmfeInstance instance, const char* file_path) {
    if (!instance || !file_path) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    if (inst->state.load() == EMFE_STATE_RUNNING) return EMFE_ERR_STATE;

    try {
        std::ifstream file(file_path);
        if (!file.is_open()) {
            inst->lastError = std::string("Cannot open file: ") + file_path;
            return EMFE_ERR_IO;
        }

        uint16_t minAddr = 0xFFFF;
        uint16_t maxAddr = 0;
        uint16_t entryPoint = 0;
        bool hasEntry = false;

        auto parseHexByte = [](const char* p) -> uint8_t {
            auto nibble = [](char c) -> uint8_t {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'A' && c <= 'F') return 10 + c - 'A';
                if (c >= 'a' && c <= 'f') return 10 + c - 'a';
                return 0;
            };
            return static_cast<uint8_t>((nibble(p[0]) << 4) | nibble(p[1]));
        };

        std::string line;
        while (std::getline(file, line)) {
            if (line.size() < 4 || line[0] != 'S') continue;

            char recType = line[1];
            const char* p = line.c_str() + 2;

            uint8_t byteCount = parseHexByte(p);
            p += 2;

            if (recType == '0') {
                // S0: Header record — skip
                continue;
            } else if (recType == '1') {
                // S1: Data record with 16-bit address
                uint16_t addr = static_cast<uint16_t>((parseHexByte(p) << 8) | parseHexByte(p + 2));
                p += 4;
                int dataCount = byteCount - 3; // subtract address (2) + checksum (1)
                for (int i = 0; i < dataCount; i++) {
                    uint8_t dataByte = parseHexByte(p);
                    p += 2;
                    inst->memory.Poke(addr, dataByte);
                    if (addr < minAddr) minAddr = addr;
                    if (addr >= maxAddr) maxAddr = addr + 1;
                    addr++;
                }
            } else if (recType == '9') {
                // S9: Start address (16-bit)
                entryPoint = static_cast<uint16_t>((parseHexByte(p) << 8) | parseHexByte(p + 2));
                hasEntry = true;
            }
            // S2/S3/S7/S8: 24-bit and 32-bit variants — ignore for 8-bit CPU
        }

        if (hasEntry)
            inst->cpu.PC = entryPoint;
        else if (minAddr <= maxAddr)
            inst->cpu.PC = minAddr;

        inst->cpu.SP = 0xFF;
        inst->programStartAddress = minAddr;
        inst->programEndAddress = maxAddr;
        inst->lastError.clear();
        return EMFE_OK;
    } catch (const std::exception& ex) {
        inst->lastError = ex.what();
        return EMFE_ERR_IO;
    }
}

const char* EMFE_CALL emfe_get_last_error(EmfeInstance instance) {
    if (!instance) return "Invalid instance";
    return reinterpret_cast<EmfeInstanceData*>(instance)->lastError.c_str();
}

// ---------- Console I/O ----------

EmfeResult EMFE_CALL emfe_send_char(EmfeInstance instance, char ch) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    inst->uart.ReceiveChar(static_cast<uint8_t>(ch));
    return EMFE_OK;
}

EmfeResult EMFE_CALL emfe_send_string(EmfeInstance instance, const char* str) {
    if (!instance || !str) return EMFE_ERR_INVALID;
    while (*str)
        emfe_send_char(instance, *str++);
    return EMFE_OK;
}

// ---------- Settings ----------

// Plugin-managed data directory
static std::string& GetDataDirString() {
    static std::string dataDir;
    return dataDir;
}

static std::filesystem::path GetSettingsPath() {
    // Settings persistence is host-driven: emfe_WinUI3Cpp / emfe_CsWPF call
    // emfe_set_data_dir on plugin load with a per-host per-plugin path under
    // %LOCALAPPDATA%\<app>\<plugin-stem>.  The Windows LOCALAPPDATA fallback
    // (the plugin inventing its own top-level directory) was removed because
    // it polluted the user profile and bypassed the per-host layout.  An
    // empty path makes load/save no-op gracefully; the XDG-style Linux
    // fallback remains because it lives under $HOME/.config and is
    // user-namespaced.
    auto& dir = GetDataDirString();
    if (dir.empty()) {
#ifdef _WIN32
        return {};
#else
        const char* home = std::getenv("HOME");
        if (home) dir = std::string(home) + "/.config/emfe_plugin_em8";
#endif
    }
    if (dir.empty()) return {};
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return std::filesystem::path(dir) / "appsettings.json";
}

EmfeResult EMFE_CALL emfe_set_data_dir(const char* path) {
    if (!path) return EMFE_ERR_INVALID;
    try {
        GetDataDirString() = path;
        return EMFE_OK;
    } catch (...) {
        return EMFE_ERR_MEMORY;
    }
}

int32_t EMFE_CALL emfe_get_setting_defs(EmfeInstance instance, const EmfeSettingDef** out_defs) {
    if (!instance || !out_defs) return 0;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    *out_defs = inst->settingDefs.data();
    return static_cast<int32_t>(inst->settingDefs.size());
}

const char* EMFE_CALL emfe_get_setting(EmfeInstance instance, const char* key) {
    if (!instance || !key) return "";
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    auto it = inst->stagedSettings.find(key);
    if (it != inst->stagedSettings.end()) {
        inst->settingValueBuf = it->second;
        return inst->settingValueBuf.c_str();
    }
    return "";
}

EmfeResult EMFE_CALL emfe_set_setting(EmfeInstance instance, const char* key, const char* value) {
    if (!instance || !key || !value) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    inst->stagedSettings[key] = value;
    return EMFE_OK;
}

EmfeResult EMFE_CALL emfe_apply_settings(EmfeInstance instance) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    inst->settings = inst->stagedSettings;
    // Only hot-swap-safe settings update appliedSettings immediately;
    // REQUIRES_RESET settings wait until emfe_reset.
    for (auto& [key, val] : inst->stagedSettings) {
        auto fit = inst->settingFlags.find(key);
        uint32_t flags = (fit != inst->settingFlags.end()) ? fit->second : 0;
        if (!(flags & EMFE_SETTING_FLAG_REQUIRES_RESET)) {
            inst->appliedSettings[key] = val;
        }
    }
    return EMFE_OK;
}

const char* EMFE_CALL emfe_get_applied_setting(EmfeInstance instance, const char* key) {
    if (!instance || !key) return "";
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    auto it = inst->appliedSettings.find(key);
    if (it != inst->appliedSettings.end()) {
        inst->appliedSettingValueBuf = it->second;
        return inst->appliedSettingValueBuf.c_str();
    }
    return "";
}

// LIST type — EM8 has no list settings
int32_t EMFE_CALL emfe_get_list_item_defs(EmfeInstance instance, const char* list_key,
                                           const EmfeListItemDef** out_defs) {
    (void)instance; (void)list_key; (void)out_defs;
    return 0;
}

int32_t EMFE_CALL emfe_get_list_item_count(EmfeInstance instance, const char* list_key) {
    (void)instance; (void)list_key;
    return 0;
}

const char* EMFE_CALL emfe_get_list_item_field(EmfeInstance instance, const char* list_key,
                                                int32_t item_index, const char* field_key) {
    (void)instance; (void)list_key; (void)item_index; (void)field_key;
    return "";
}

EmfeResult EMFE_CALL emfe_set_list_item_field(EmfeInstance instance, const char* list_key,
                                               int32_t item_index, const char* field_key,
                                               const char* value) {
    (void)instance; (void)list_key; (void)item_index; (void)field_key; (void)value;
    return EMFE_ERR_UNSUPPORTED;
}

int32_t EMFE_CALL emfe_add_list_item(EmfeInstance instance, const char* list_key) {
    (void)instance; (void)list_key;
    return -1;
}

EmfeResult EMFE_CALL emfe_remove_list_item(EmfeInstance instance, const char* list_key,
                                            int32_t item_index) {
    (void)instance; (void)list_key; (void)item_index;
    return EMFE_ERR_UNSUPPORTED;
}

EmfeResult EMFE_CALL emfe_save_settings(EmfeInstance instance) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    try {
        auto path = GetSettingsPath();
        // No data dir configured — skip persistence silently.
        if (path.empty()) return EMFE_OK;
        std::ofstream ofs(path);
        if (!ofs.is_open()) {
            inst->lastError = "Failed to open settings file for writing";
            return EMFE_ERR_IO;
        }
        ofs << "{\n";
        bool first = true;
        for (auto& [key, val] : inst->settings) {
            if (!first) ofs << ",\n";
            ofs << "  \"" << key << "\": \"" << val << "\"";
            first = false;
        }
        ofs << "\n}\n";
        return EMFE_OK;
    } catch (const std::exception& ex) {
        inst->lastError = ex.what();
        return EMFE_ERR_IO;
    }
}

EmfeResult EMFE_CALL emfe_load_settings(EmfeInstance instance) {
    if (!instance) return EMFE_ERR_INVALID;
    auto inst = reinterpret_cast<EmfeInstanceData*>(instance);
    try {
        auto path = GetSettingsPath();
        if (path.empty() || !std::filesystem::exists(path)) return EMFE_OK;

        std::ifstream ifs(path);
        if (!ifs.is_open()) return EMFE_OK;

        std::string content((std::istreambuf_iterator<char>(ifs)),
                             std::istreambuf_iterator<char>());

        for (auto& [key, val] : inst->settings) {
            auto keyStr = "\"" + key + "\"";
            auto pos = content.find(keyStr);
            if (pos != std::string::npos) {
                auto colon = content.find(':', pos + keyStr.size());
                if (colon != std::string::npos) {
                    auto q1 = content.find('"', colon + 1);
                    if (q1 != std::string::npos) {
                        auto q2 = content.find('"', q1 + 1);
                        if (q2 != std::string::npos) {
                            val = content.substr(q1 + 1, q2 - q1 - 1);
                        }
                    }
                }
            }
        }
        inst->stagedSettings = inst->settings;
        inst->appliedSettings = inst->settings;
        return EMFE_OK;
    } catch (const std::exception& ex) {
        inst->lastError = ex.what();
        return EMFE_ERR_IO;
    }
}

// ---------- String Utilities ----------

void EMFE_CALL emfe_release_string(const char* str) {
    (void)str; // All strings are plugin-owned, no dynamic allocation to free
}

// ============================================================================
// DLL entry point
// ============================================================================

#ifdef _WIN32
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    (void)hModule;
    (void)lpReserved;
    switch (reason) {
    case DLL_PROCESS_ATTACH:
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
#endif
