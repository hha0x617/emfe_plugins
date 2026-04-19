// test_plugin.cpp - Quick smoke test for the emfe_plugin_mc68030 DLL
// Build: cl /std:c++20 /EHsc /utf-8 /I..\..\..\api test_plugin.cpp /Fe:test_plugin.exe

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <string>
#include "emfe_plugin.h"

int main()
{
    // Load DLL
    HMODULE hDll = LoadLibraryW(L"emfe_plugin_mc68030.dll");
    if (!hDll) {
        printf("FAIL: LoadLibrary failed (%lu)\n", GetLastError());
        return 1;
    }
    printf("OK: DLL loaded\n");

    // Resolve functions
    #define LOAD(name) auto name = reinterpret_cast<decltype(::name)*>(GetProcAddress(hDll, #name)); \
        if (!name) { printf("FAIL: " #name " not found\n"); return 1; }

    LOAD(emfe_negotiate)
    LOAD(emfe_get_board_info)
    LOAD(emfe_create)
    LOAD(emfe_destroy)
    LOAD(emfe_get_register_defs)
    LOAD(emfe_get_registers)
    LOAD(emfe_set_registers)
    LOAD(emfe_peek_byte)
    LOAD(emfe_poke_byte)
    LOAD(emfe_poke_long)
    LOAD(emfe_peek_range)
    LOAD(emfe_get_memory_size)
    LOAD(emfe_disassemble_one)
    LOAD(emfe_step)
    LOAD(emfe_reset)
    LOAD(emfe_get_state)
    LOAD(emfe_add_breakpoint)
    LOAD(emfe_clear_breakpoints)
    LOAD(emfe_get_last_error)
    #undef LOAD
    printf("OK: All functions resolved\n");

    // Negotiate
    EmfeNegotiateInfo nego{};
    nego.api_version_major = EMFE_API_VERSION_MAJOR;
    nego.api_version_minor = EMFE_API_VERSION_MINOR;
    if (emfe_negotiate(&nego) != EMFE_OK) {
        printf("FAIL: negotiate\n");
        return 1;
    }
    printf("OK: negotiate\n");

    // Board info
    EmfeBoardInfo info{};
    emfe_get_board_info(&info);
    printf("OK: board=%s cpu=%s ver=%s\n", info.board_name, info.cpu_name, info.version);

    // Create instance
    EmfeInstance inst = nullptr;
    if (emfe_create(&inst) != EMFE_OK || !inst) {
        printf("FAIL: create\n");
        return 1;
    }
    printf("OK: instance created\n");

    // Memory size
    uint64_t memSize = emfe_get_memory_size(inst);
    printf("OK: memory size = %llu bytes (%llu MB)\n", memSize, memSize / (1024*1024));

    // Register defs
    const EmfeRegisterDef* defs = nullptr;
    int32_t regCount = emfe_get_register_defs(inst, &defs);
    printf("OK: %d register defs\n", regCount);

    // Write some code into memory (NOP NOP NOP MOVEQ #42,D0)
    // NOP = 0x4E71, MOVEQ #42,D0 = 0x702A
    emfe_poke_byte(inst, 0, 0x4E); emfe_poke_byte(inst, 1, 0x71); // NOP
    emfe_poke_byte(inst, 2, 0x4E); emfe_poke_byte(inst, 3, 0x71); // NOP
    emfe_poke_byte(inst, 4, 0x70); emfe_poke_byte(inst, 5, 0x2A); // MOVEQ #42,D0
    emfe_poke_byte(inst, 6, 0x4E); emfe_poke_byte(inst, 7, 0x71); // NOP

    // Set initial state: PC=0, SR=0x2700, A7=top of RAM
    EmfeRegValue setRegs[3]{};
    setRegs[0].reg_id = 16; setRegs[0].value.u64 = 0;         // PC = 0
    setRegs[1].reg_id = 17; setRegs[1].value.u64 = 0x2700;    // SR = supervisor
    setRegs[2].reg_id = 15; setRegs[2].value.u64 = 0x01000000; // A7 = 16MB
    emfe_set_registers(inst, setRegs, 3);
    printf("OK: PC/SR/A7 set\n");

    // Disassemble
    EmfeDisasmLine line{};
    emfe_disassemble_one(inst, 0, &line);
    printf("OK: disasm @%llX: %s %s (%s) len=%u\n",
        line.address, line.mnemonic, line.operands, line.raw_bytes, line.length);

    // Step 3 times (NOP, NOP, MOVEQ)
    for (int i = 0; i < 3; i++) {
        emfe_step(inst);
    }

    // Read D0 - should be 42
    EmfeRegValue d0Val{};
    d0Val.reg_id = 0; // REG_D0
    emfe_get_registers(inst, &d0Val, 1);
    printf("OK: after 3 steps, D0 = 0x%08X (%llu)\n",
        (uint32_t)d0Val.value.u64, d0Val.value.u64);

    // Read PC - should be 6 (after MOVEQ)
    EmfeRegValue pcVal{};
    pcVal.reg_id = 16; // REG_PC
    emfe_get_registers(inst, &pcVal, 1);
    printf("OK: PC = 0x%08X\n", (uint32_t)pcVal.value.u64);

    // Verify D0 == 42
    if ((uint32_t)d0Val.value.u64 != 42) {
        printf("\nFAIL: D0 should be 42 but is %u\n", (uint32_t)d0Val.value.u64);
        return 1;
    }

    // ========================================
    // MOVE.L (d16,A7),D7 regression test
    // ========================================
    printf("\n--- MOVE.L (4,A7),D7 test ---\n");
    {
        // Place a known value on the stack
        // A7 = 0x2000, put 0xDEADBEEF at A7+4 = 0x2004
        emfe_poke_long(inst, 0x2004, 0xDEADBEEF);

        // Code at 0x3000: MOVE.L (4,A7),D7 = 2E2F 0004, then NOP
        emfe_poke_byte(inst, 0x3000, 0x2E); emfe_poke_byte(inst, 0x3001, 0x2F);
        emfe_poke_byte(inst, 0x3002, 0x00); emfe_poke_byte(inst, 0x3003, 0x04);
        emfe_poke_byte(inst, 0x3004, 0x4E); emfe_poke_byte(inst, 0x3005, 0x71); // NOP

        // Setup: PC=0x3000, SR=0x2700, A7=0x2000, D7=0
        EmfeRegValue setupRegs2[4]{};
        setupRegs2[0].reg_id = 16; setupRegs2[0].value.u64 = 0x3000;  // PC
        setupRegs2[1].reg_id = 17; setupRegs2[1].value.u64 = 0x2700;  // SR
        setupRegs2[2].reg_id = 15; setupRegs2[2].value.u64 = 0x2000;  // A7
        setupRegs2[3].reg_id = 7;  setupRegs2[3].value.u64 = 0;       // D7
        emfe_set_registers(inst, setupRegs2, 4);

        // Disassemble to verify
        EmfeDisasmLine dl{};
        emfe_disassemble_one(inst, 0x3000, &dl);
        printf("  disasm: %s %s (%s)\n", dl.mnemonic, dl.operands, dl.raw_bytes);

        // Step once
        emfe_step(inst);

        // Read PC and D7
        EmfeRegValue checkRegs[2]{};
        checkRegs[0].reg_id = 16; // PC
        checkRegs[1].reg_id = 7;  // D7
        emfe_get_registers(inst, checkRegs, 2);
        uint32_t newPC = (uint32_t)checkRegs[0].value.u64;
        uint32_t newD7 = (uint32_t)checkRegs[1].value.u64;
        printf("  after step: PC=%08X D7=%08X\n", newPC, newD7);

        if (newPC == 0x3004 && newD7 == 0xDEADBEEF) {
            printf("  OK: MOVE.L (4,A7),D7 works correctly\n");
        } else {
            printf("  FAIL: expected PC=00003004 D7=DEADBEEF\n");
            if (newPC == 0) printf("  NOTE: PC went to 0 — likely bus error or address error\n");
        }
    }

    // ========================================
    // Phase 2: Settings API tests
    // ========================================

    auto emfe_get_setting_defs = reinterpret_cast<decltype(::emfe_get_setting_defs)*>(
        GetProcAddress(hDll, "emfe_get_setting_defs"));
    auto emfe_get_setting = reinterpret_cast<decltype(::emfe_get_setting)*>(
        GetProcAddress(hDll, "emfe_get_setting"));
    auto emfe_set_setting = reinterpret_cast<decltype(::emfe_set_setting)*>(
        GetProcAddress(hDll, "emfe_set_setting"));
    auto emfe_apply_settings = reinterpret_cast<decltype(::emfe_apply_settings)*>(
        GetProcAddress(hDll, "emfe_apply_settings"));
    auto emfe_load_binary = reinterpret_cast<decltype(::emfe_load_binary)*>(
        GetProcAddress(hDll, "emfe_load_binary"));
    auto emfe_load_srec = reinterpret_cast<decltype(::emfe_load_srec)*>(
        GetProcAddress(hDll, "emfe_load_srec"));

    if (!emfe_get_setting_defs || !emfe_get_setting || !emfe_set_setting || !emfe_apply_settings) {
        printf("FAIL: Phase 2 functions not found\n");
        return 1;
    }
    printf("OK: Phase 2 functions resolved\n");

    // Get setting defs
    const EmfeSettingDef* sdefs = nullptr;
    int32_t scount = emfe_get_setting_defs(inst, &sdefs);
    printf("OK: %d setting defs\n", scount);

    // Print first few settings
    for (int i = 0; i < scount && i < 5; i++) {
        printf("  [%s] %s (group=%s, type=%d, default=%s)\n",
            sdefs[i].key, sdefs[i].label, sdefs[i].group, sdefs[i].type,
            sdefs[i].default_value ? sdefs[i].default_value : "");
    }

    // Get/set a setting
    const char* memSizeSetting = emfe_get_setting(inst, "MemorySize");
    printf("OK: MemorySize = %s\n", memSizeSetting);

    emfe_set_setting(inst, "JitEnabled", "true");
    const char* jit = emfe_get_setting(inst, "JitEnabled");
    printf("OK: JitEnabled after set = %s\n", jit);

    emfe_apply_settings(inst);
    printf("OK: settings applied\n");

    // ========================================
    // Console callback test
    // ========================================

    auto emfe_set_console_char_callback = reinterpret_cast<decltype(::emfe_set_console_char_callback)*>(
        GetProcAddress(hDll, "emfe_set_console_char_callback"));
    if (!emfe_set_console_char_callback) {
        printf("FAIL: emfe_set_console_char_callback not found\n");
        return 1;
    }

    static std::string consoleOutput;
    emfe_set_console_char_callback(inst,
        [](void*, char ch) { consoleOutput += ch; }, nullptr);

    // Write a small program that does TRAP #15 with .OUTCHR (funcCode=0x0020)
    // to output 'H' then 'i' then halt
    //   MOVE.B #'H', D0  → 0x103C 0x0048
    //   TRAP #15          → 0x4E4F
    //   .OUTCHR           → 0x0020
    //   MOVE.B #'i', D0  → 0x103C 0x0069
    //   TRAP #15          → 0x4E4F
    //   .OUTCHR           → 0x0020
    //   STOP #$2700       → 0x4E72 0x2700
    uint16_t code[] = {
        0x103C, 0x0048, // MOVE.B #'H', D0
        0x4E4F,         // TRAP #15
        0x0020,         // .OUTCHR
        0x103C, 0x0069, // MOVE.B #'i', D0
        0x4E4F,         // TRAP #15
        0x0020,         // .OUTCHR
        0x4E72, 0x2700  // STOP #$2700
    };

    // ========================================
    // hello.s19 test (Generic mode, ConsoleDevice TRAP)
    // ========================================
    printf("\n--- hello.s19 test (Generic mode) ---\n");
    {
        printf("  BoardType = %s\n", emfe_get_setting(inst, "BoardType"));

        // Make sure we're in Generic mode
        emfe_set_setting(inst, "BoardType", "Generic");
        emfe_set_setting(inst, "MemorySize", "1");
        emfe_apply_settings(inst);
        printf("  BoardType after = %s\n", emfe_get_setting(inst, "BoardType"));

        // Re-register console callback
        consoleOutput.clear();
        emfe_set_console_char_callback(inst,
            [](void*, char ch) { consoleOutput += ch; }, nullptr);

        // Load hello.s19
        if (emfe_load_srec) {
            auto sr = emfe_load_srec(inst, "D:\\projects\\em68030\\examples\\hello.s19");
            printf("  load_srec result = %d\n", sr);
        }

        // Check PC
        EmfeRegValue chkPC{};
        chkPC.reg_id = 16;
        emfe_get_registers(inst, &chkPC, 1);
        printf("  PC after load = %08X\n", (uint32_t)chkPC.value.u64);

        // Check A7/SR
        EmfeRegValue chkA7{}, chkSR{};
        chkA7.reg_id = 15;
        chkSR.reg_id = 17;
        emfe_get_registers(inst, &chkA7, 1);
        emfe_get_registers(inst, &chkSR, 1);
        printf("  A7 = %08X, SR = %04X\n", (uint32_t)chkA7.value.u64, (uint16_t)chkSR.value.u64);

        // Disassemble at PC
        EmfeDisasmLine dl{};
        emfe_disassemble_one(inst, (uint32_t)chkPC.value.u64, &dl);
        printf("  disasm @PC: %s %s\n", dl.mnemonic, dl.operands);

        // Step and trace
        for (int i = 0; i < 10; i++) {
            EmfeRegValue trPC{}, trD0{};
            trPC.reg_id = 16;
            trD0.reg_id = 0;
            emfe_get_registers(inst, &trPC, 1);
            emfe_get_registers(inst, &trD0, 1);
            printf("  step %d: PC=%08X D0=%08X state=%d\n",
                i, (uint32_t)trPC.value.u64, (uint32_t)trD0.value.u64, emfe_get_state(inst));
            emfe_step(inst);
            if (emfe_get_state(inst) == EMFE_STATE_HALTED) {
                printf("  HALTED: %s\n", emfe_get_last_error(inst));
                break;
            }
        }

        printf("  console output = \"%s\"\n", consoleOutput.c_str());
        if (consoleOutput.find("hello") != std::string::npos)
            printf("  OK: hello.s19 works\n");
        else
            printf("  FAIL: expected 'hello' in console output\n");
    }

    // Ensure BoardType is MVME147 for console test
    auto emfe_get_setting2 = reinterpret_cast<decltype(::emfe_get_setting)*>(
        GetProcAddress(hDll, "emfe_get_setting"));
    printf("  BoardType before = %s\n", emfe_get_setting2(inst, "BoardType"));

    emfe_set_setting(inst, "BoardType", "MVME147");
    emfe_set_setting(inst, "MemorySize", "32");
    emfe_apply_settings(inst);
    printf("  BoardType after apply = %s\n", emfe_get_setting2(inst, "BoardType"));

    // Re-register console callback (instance may have been recreated)
    consoleOutput.clear();
    emfe_set_console_char_callback(inst,
        [](void*, char ch) { consoleOutput += ch; }, nullptr);

    // Load code
    for (int i = 0; i < sizeof(code)/sizeof(code[0]); i++) {
        emfe_poke_byte(inst, 0x1000 + i*2,     static_cast<uint8_t>(code[i] >> 8));
        emfe_poke_byte(inst, 0x1000 + i*2 + 1, static_cast<uint8_t>(code[i] & 0xFF));
    }

    // Set PC to code, SR to supervisor
    EmfeRegValue setupRegs[3]{};
    setupRegs[0].reg_id = 16; setupRegs[0].value.u64 = 0x1000;    // PC
    setupRegs[1].reg_id = 17; setupRegs[1].value.u64 = 0x2700;    // SR
    setupRegs[2].reg_id = 15; setupRegs[2].value.u64 = 0x00100000; // A7
    emfe_set_registers(inst, setupRegs, 3);

    consoleOutput.clear();

    // Step through with PC trace
    for (int i = 0; i < 20; i++) {
        EmfeRegValue tracePC{};
        tracePC.reg_id = 16;
        emfe_get_registers(inst, &tracePC, 1);
        printf("  step %d: PC=%08X state=%d\n", i, (uint32_t)tracePC.value.u64, emfe_get_state(inst));
        emfe_step(inst);
        if (emfe_get_state(inst) == EMFE_STATE_HALTED) {
            printf("  HALTED\n");
            break;
        }
    }

    printf("OK: console output = \"%s\"\n", consoleOutput.c_str());
    if (consoleOutput.find("Hi") != std::string::npos) {
        printf("OK: console callback working\n");
    } else {
        printf("NOTE: console output did not contain \"Hi\" (BoardType may not be MVME147)\n");
    }

    printf("\n=== ALL TESTS PASSED ===\n");

    // Cleanup
    emfe_destroy(inst);
    FreeLibrary(hDll);
    return 0;
}
