// test_em8.cpp - Basic test harness for emfe_plugin_em8
// Tests the C ABI plugin interface without requiring Google Test.

#include "emfe_plugin.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <thread>

static int g_testsPassed = 0;
static int g_testsFailed = 0;

#define TEST_ASSERT(cond, msg)                                              \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__);       \
            g_testsFailed++;                                                \
            return;                                                         \
        }                                                                   \
    } while (0)

#define TEST_ASSERT_EQ(actual, expected, msg)                               \
    do {                                                                    \
        if ((actual) != (expected)) {                                       \
            fprintf(stderr, "  FAIL: %s — expected %lld, got %lld (line %d)\n", \
                    msg, (long long)(expected), (long long)(actual), __LINE__); \
            g_testsFailed++;                                                \
            return;                                                         \
        }                                                                   \
    } while (0)

// ============================================================================
// Test: Negotiate
// ============================================================================

static void TestNegotiate() {
    printf("TestNegotiate...\n");

    EmfeNegotiateInfo info{};
    info.api_version_major = EMFE_API_VERSION_MAJOR;
    info.api_version_minor = EMFE_API_VERSION_MINOR;
    info.flags = 0;

    EmfeResult r = emfe_negotiate(&info);
    TEST_ASSERT(r == EMFE_OK, "negotiate should succeed");

    // Wrong major version
    EmfeNegotiateInfo bad{};
    bad.api_version_major = 99;
    r = emfe_negotiate(&bad);
    TEST_ASSERT(r == EMFE_ERR_UNSUPPORTED, "negotiate with wrong major should fail");

    // Null
    r = emfe_negotiate(nullptr);
    TEST_ASSERT(r == EMFE_ERR_INVALID, "negotiate with null should fail");

    printf("  PASS\n");
    g_testsPassed++;
}

// ============================================================================
// Test: Board Info
// ============================================================================

static void TestBoardInfo() {
    printf("TestBoardInfo...\n");

    EmfeBoardInfo info{};
    EmfeResult r = emfe_get_board_info(&info);
    TEST_ASSERT(r == EMFE_OK, "get_board_info should succeed");
    TEST_ASSERT(strcmp(info.board_name, "EM8") == 0, "board_name should be EM8");
    TEST_ASSERT(strcmp(info.cpu_name, "EM8") == 0, "cpu_name should be EM8");
    TEST_ASSERT(info.version != nullptr, "version should not be null");

    printf("  PASS\n");
    g_testsPassed++;
}

// ============================================================================
// Test: Create and Destroy
// ============================================================================

static void TestCreateDestroy() {
    printf("TestCreateDestroy...\n");

    EmfeInstance inst = nullptr;
    EmfeResult r = emfe_create(&inst);
    TEST_ASSERT(r == EMFE_OK, "create should succeed");
    TEST_ASSERT(inst != nullptr, "instance should not be null");

    r = emfe_destroy(inst);
    TEST_ASSERT(r == EMFE_OK, "destroy should succeed");

    printf("  PASS\n");
    g_testsPassed++;
}

// ============================================================================
// Test: Register Defs
// ============================================================================

static void TestRegisterDefs() {
    printf("TestRegisterDefs...\n");

    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    const EmfeRegisterDef* defs = nullptr;
    int32_t count = emfe_get_register_defs(inst, &defs);
    TEST_ASSERT(count == 8, "should have 8 register defs");
    TEST_ASSERT(defs != nullptr, "defs should not be null");

    // Check A register
    TEST_ASSERT(strcmp(defs[0].name, "A") == 0, "first reg should be A");
    TEST_ASSERT(defs[0].bit_width == 8, "A should be 8-bit");

    // Check PC register
    bool foundPC = false;
    for (int i = 0; i < count; i++) {
        if (defs[i].flags & EMFE_REG_FLAG_PC) {
            foundPC = true;
            TEST_ASSERT(strcmp(defs[i].name, "PC") == 0, "PC flag reg should be named PC");
            TEST_ASSERT(defs[i].bit_width == 16, "PC should be 16-bit");
        }
    }
    TEST_ASSERT(foundPC, "should have a PC register");

    emfe_destroy(inst);
    printf("  PASS\n");
    g_testsPassed++;
}

// ============================================================================
// Test: Step and Verify Registers
// Program: LDA #$42 (opcode $A0 $42), STA $F000 (opcode $AD $00 $F0), HLT ($9A)
// ============================================================================

static void TestStepAndRegisters() {
    printf("TestStepAndRegisters...\n");

    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // Write a small program at address $0200
    // LDA #$42  -> opcode $A0 $42
    // STA $F000 -> opcode $AD $00 $F0
    // HLT       -> opcode $9A
    emfe_poke_byte(inst, 0x0200, 0xA0); // LDA #imm
    emfe_poke_byte(inst, 0x0201, 0x42); // #$42
    emfe_poke_byte(inst, 0x0202, 0xAD); // STA abs
    emfe_poke_byte(inst, 0x0203, 0x00); // $F000 low
    emfe_poke_byte(inst, 0x0204, 0xF0); // $F000 high
    emfe_poke_byte(inst, 0x0205, 0x9A); // HLT

    // Set PC to $0200
    EmfeRegValue pcReg{};
    pcReg.reg_id = 4; // REG_PC
    pcReg.value.u64 = 0x0200;
    emfe_set_registers(inst, &pcReg, 1);

    // Step: execute LDA #$42
    EmfeResult r = emfe_step(inst);
    TEST_ASSERT(r == EMFE_OK, "step should succeed");

    // Read A register
    EmfeRegValue aReg{};
    aReg.reg_id = 0; // REG_A
    r = emfe_get_registers(inst, &aReg, 1);
    TEST_ASSERT(r == EMFE_OK, "get_registers should succeed");
    TEST_ASSERT_EQ(aReg.value.u64, 0x42, "A should be $42 after LDA #$42");

    // Read PC — should have advanced
    pcReg.reg_id = 4;
    emfe_get_registers(inst, &pcReg, 1);
    TEST_ASSERT_EQ(pcReg.value.u64, 0x0202, "PC should be $0202 after LDA #$42");

    // Step: execute STA $F000
    r = emfe_step(inst);
    TEST_ASSERT(r == EMFE_OK, "step STA should succeed");

    // $F000 is UART DR — write goes to TX, read pops RX FIFO.
    // Verify PC advanced correctly instead.
    pcReg.reg_id = 4;
    emfe_get_registers(inst, &pcReg, 1);
    TEST_ASSERT_EQ(pcReg.value.u64, 0x0205, "PC should be $0205 after STA $F000");

    // Step: execute HLT
    r = emfe_step(inst);
    TEST_ASSERT(r == EMFE_OK, "step HLT should succeed");

    EmfeState state = emfe_get_state(inst);
    TEST_ASSERT(state == EMFE_STATE_HALTED, "state should be HALTED after HLT");

    emfe_destroy(inst);
    printf("  PASS\n");
    g_testsPassed++;
}

// ============================================================================
// Test: Memory peek/poke
// ============================================================================

static void TestMemory() {
    printf("TestMemory...\n");

    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // Byte
    emfe_poke_byte(inst, 0x1000, 0xAB);
    TEST_ASSERT_EQ(emfe_peek_byte(inst, 0x1000), 0xAB, "peek_byte should match poke");

    // Word (little-endian)
    emfe_poke_word(inst, 0x1010, 0x1234);
    TEST_ASSERT_EQ(emfe_peek_word(inst, 0x1010), 0x1234, "peek_word should match poke");

    // Long (little-endian)
    emfe_poke_long(inst, 0x1020, 0xDEADBEEF);
    TEST_ASSERT_EQ(emfe_peek_long(inst, 0x1020), 0xDEADBEEF, "peek_long should match poke");

    // Range
    uint8_t data[4];
    emfe_peek_range(inst, 0x1020, data, 4);
    TEST_ASSERT_EQ(data[0], 0xEF, "range byte 0 (low byte of DEADBEEF)");
    TEST_ASSERT_EQ(data[1], 0xBE, "range byte 1");
    TEST_ASSERT_EQ(data[2], 0xAD, "range byte 2");
    TEST_ASSERT_EQ(data[3], 0xDE, "range byte 3 (high byte)");

    // Memory size
    TEST_ASSERT_EQ(emfe_get_memory_size(inst), 65536, "memory size should be 64K");

    emfe_destroy(inst);
    printf("  PASS\n");
    g_testsPassed++;
}

// ============================================================================
// Test: UART TX callback
// ============================================================================

static std::atomic<int> g_txCharCount{0};
static char g_lastTxChar = 0;

static void EMFE_CALL TestTxCallback(void* /*user_data*/, char ch) {
    g_lastTxChar = ch;
    g_txCharCount++;
}

static void TestUartTxCallback() {
    printf("TestUartTxCallback...\n");

    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    g_txCharCount = 0;
    g_lastTxChar = 0;
    emfe_set_console_char_callback(inst, TestTxCallback, nullptr);

    // Write a program that writes 'H' to UART TX register ($F010)
    // LDA #$48 ($A0 $48)
    // STA $F000 ($AD $00 $F0)
    // HLT ($9A)
    emfe_poke_byte(inst, 0x0300, 0xA0);
    emfe_poke_byte(inst, 0x0301, 0x48); // 'H'
    emfe_poke_byte(inst, 0x0302, 0xAD);
    emfe_poke_byte(inst, 0x0303, 0x00); // UART DR low byte
    emfe_poke_byte(inst, 0x0304, 0xF0); // UART DR high byte
    emfe_poke_byte(inst, 0x0305, 0x9A); // HLT

    EmfeRegValue pcReg{};
    pcReg.reg_id = 4; // REG_PC
    pcReg.value.u64 = 0x0300;
    emfe_set_registers(inst, &pcReg, 1);

    // Step through all three instructions
    emfe_step(inst); // LDA #$48
    emfe_step(inst); // STA $F010 — should trigger UART TX
    emfe_step(inst); // HLT

    TEST_ASSERT(g_txCharCount > 0, "UART TX callback should have fired");
    TEST_ASSERT_EQ(g_lastTxChar, 'H', "UART should have transmitted 'H'");

    emfe_destroy(inst);
    printf("  PASS\n");
    g_testsPassed++;
}

// ============================================================================
// Test: Breakpoints
// ============================================================================

static void TestBreakpoints() {
    printf("TestBreakpoints...\n");

    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // Add breakpoint
    EmfeResult r = emfe_add_breakpoint(inst, 0x0202);
    TEST_ASSERT(r == EMFE_OK, "add breakpoint should succeed");

    // Get breakpoints
    EmfeBreakpointInfo bps[10];
    int32_t count = emfe_get_breakpoints(inst, bps, 10);
    TEST_ASSERT_EQ(count, 1, "should have 1 breakpoint");
    TEST_ASSERT_EQ(bps[0].address, 0x0202, "breakpoint address");
    TEST_ASSERT(bps[0].enabled, "breakpoint should be enabled");

    // Disable breakpoint
    r = emfe_enable_breakpoint(inst, 0x0202, false);
    TEST_ASSERT(r == EMFE_OK, "disable breakpoint should succeed");

    count = emfe_get_breakpoints(inst, bps, 10);
    TEST_ASSERT(!bps[0].enabled, "breakpoint should be disabled");

    // Remove breakpoint
    r = emfe_remove_breakpoint(inst, 0x0202);
    TEST_ASSERT(r == EMFE_OK, "remove breakpoint should succeed");

    count = emfe_get_breakpoints(inst, bps, 10);
    TEST_ASSERT_EQ(count, 0, "should have 0 breakpoints after remove");

    // Clear breakpoints
    emfe_add_breakpoint(inst, 0x0100);
    emfe_add_breakpoint(inst, 0x0200);
    r = emfe_clear_breakpoints(inst);
    TEST_ASSERT(r == EMFE_OK, "clear breakpoints should succeed");
    count = emfe_get_breakpoints(inst, bps, 10);
    TEST_ASSERT_EQ(count, 0, "should have 0 breakpoints after clear");

    emfe_destroy(inst);
    printf("  PASS\n");
    g_testsPassed++;
}

// ============================================================================
// Test: Breakpoint triggers stop during Run
// ============================================================================

static std::atomic<bool> g_stateCallbackFired{false};
static EmfeStopReason g_lastStopReason = EMFE_STOP_REASON_NONE;

static void EMFE_CALL TestStateCallback(void* /*user_data*/, const EmfeStateInfo* info) {
    g_stateCallbackFired = true;
    g_lastStopReason = info->stop_reason;
}

static void TestBreakpointStopsRun() {
    printf("TestBreakpointStopsRun...\n");

    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    g_stateCallbackFired = false;
    g_lastStopReason = EMFE_STOP_REASON_NONE;
    emfe_set_state_change_callback(inst, TestStateCallback, nullptr);

    // Write a loop: NOP ($98), NOP ($98), JMP $0200 ($60 $00 $02)
    emfe_poke_byte(inst, 0x0200, 0x98); // NOP
    emfe_poke_byte(inst, 0x0201, 0x98); // NOP
    emfe_poke_byte(inst, 0x0202, 0x60); // JMP abs
    emfe_poke_byte(inst, 0x0203, 0x00); // $0200 low
    emfe_poke_byte(inst, 0x0204, 0x02); // $0200 high

    EmfeRegValue pcReg{};
    pcReg.reg_id = 4;
    pcReg.value.u64 = 0x0200;
    emfe_set_registers(inst, &pcReg, 1);

    // Set breakpoint at $0201 (second NOP)
    emfe_add_breakpoint(inst, 0x0201);

    // Run — should stop at breakpoint
    emfe_run(inst);

    // Wait for state callback (with timeout)
    auto start = std::chrono::steady_clock::now();
    while (!g_stateCallbackFired) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > std::chrono::seconds(5)) break;
    }

    TEST_ASSERT(g_stateCallbackFired, "state callback should have fired");
    TEST_ASSERT(g_lastStopReason == EMFE_STOP_REASON_BREAKPOINT,
                "stop reason should be BREAKPOINT");

    EmfeState state = emfe_get_state(inst);
    TEST_ASSERT(state == EMFE_STATE_STOPPED, "state should be STOPPED after breakpoint");

    emfe_destroy(inst);
    printf("  PASS\n");
    g_testsPassed++;
}

// ============================================================================
// Test: Disassembly
// ============================================================================

static void TestDisassembly() {
    printf("TestDisassembly...\n");

    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // Write NOP at $0100
    emfe_poke_byte(inst, 0x0100, 0x98); // NOP

    EmfeDisasmLine line{};
    EmfeResult r = emfe_disassemble_one(inst, 0x0100, &line);
    TEST_ASSERT(r == EMFE_OK, "disassemble_one should succeed");
    TEST_ASSERT(line.length > 0, "instruction length should be > 0");
    TEST_ASSERT(line.mnemonic != nullptr, "mnemonic should not be null");
    TEST_ASSERT(line.raw_bytes != nullptr, "raw_bytes should not be null");

    emfe_destroy(inst);
    printf("  PASS\n");
    g_testsPassed++;
}

// ============================================================================
// Test: Reset
// ============================================================================

static void TestReset() {
    printf("TestReset...\n");

    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // Modify registers
    EmfeRegValue regs[2];
    regs[0].reg_id = 0; // A
    regs[0].value.u64 = 0xFF;
    regs[1].reg_id = 4; // PC
    regs[1].value.u64 = 0x1234;
    emfe_set_registers(inst, regs, 2);

    // Reset
    EmfeResult r = emfe_reset(inst);
    TEST_ASSERT(r == EMFE_OK, "reset should succeed");

    // Check registers are cleared
    EmfeRegValue aReg{};
    aReg.reg_id = 0;
    emfe_get_registers(inst, &aReg, 1);
    TEST_ASSERT_EQ(aReg.value.u64, 0, "A should be 0 after reset");

    emfe_destroy(inst);
    printf("  PASS\n");
    g_testsPassed++;
}

// ============================================================================
// Test: Unsupported APIs
// ============================================================================

static void TestUnsupported() {
    printf("TestUnsupported...\n");

    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // ELF loading
    EmfeResult r = emfe_load_elf(inst, "dummy.elf");
    TEST_ASSERT(r == EMFE_ERR_UNSUPPORTED, "load_elf should return UNSUPPORTED");

    // Framebuffer
    EmfeFramebufferInfo fbInfo{};
    r = emfe_get_framebuffer_info(inst, &fbInfo);
    TEST_ASSERT(r == EMFE_ERR_UNSUPPORTED, "framebuffer should return UNSUPPORTED");

    // Input
    r = emfe_push_key(inst, 0, true);
    TEST_ASSERT(r == EMFE_ERR_UNSUPPORTED, "push_key should return UNSUPPORTED");

    emfe_destroy(inst);
    printf("  PASS\n");
    g_testsPassed++;
}

// ============================================================================
// Test: Settings
// ============================================================================

static void TestSettings() {
    printf("TestSettings...\n");

    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    const EmfeSettingDef* defs = nullptr;
    int32_t count = emfe_get_setting_defs(inst, &defs);
    TEST_ASSERT(count >= 1, "should have at least 1 setting");

    // Get default theme
    const char* theme = emfe_get_setting(inst, "Theme");
    TEST_ASSERT(theme != nullptr, "theme should not be null");
    TEST_ASSERT(strcmp(theme, "Dark") == 0, "default theme should be Dark");

    // Set theme
    EmfeResult r = emfe_set_setting(inst, "Theme", "Light");
    TEST_ASSERT(r == EMFE_OK, "set_setting should succeed");

    r = emfe_apply_settings(inst);
    TEST_ASSERT(r == EMFE_OK, "apply_settings should succeed");

    theme = emfe_get_setting(inst, "Theme");
    TEST_ASSERT(strcmp(theme, "Light") == 0, "theme should be Light after apply");

    emfe_destroy(inst);
    printf("  PASS\n");
    g_testsPassed++;
}

// ============================================================================
// Test: Send char to UART
// ============================================================================

static void TestSendChar() {
    printf("TestSendChar...\n");

    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    EmfeResult r = emfe_send_char(inst, 'Z');
    TEST_ASSERT(r == EMFE_OK, "send_char should succeed");

    r = emfe_send_string(inst, "Hello");
    TEST_ASSERT(r == EMFE_OK, "send_string should succeed");

    emfe_destroy(inst);
    printf("  PASS\n");
    g_testsPassed++;
}

// ============================================================================
// Main
// ============================================================================

int main() {
    printf("=== emfe_plugin_em8 Test Suite ===\n\n");

    TestNegotiate();
    TestBoardInfo();
    TestCreateDestroy();
    TestRegisterDefs();
    TestStepAndRegisters();
    TestMemory();
    TestUartTxCallback();
    TestBreakpoints();
    TestBreakpointStopsRun();
    TestDisassembly();
    TestReset();
    TestUnsupported();
    TestSettings();
    TestSendChar();

    printf("\n=== Results: %d passed, %d failed ===\n",
           g_testsPassed, g_testsFailed);

    return g_testsFailed > 0 ? 1 : 0;
}
