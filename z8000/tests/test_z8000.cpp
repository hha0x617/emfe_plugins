// test_z8000.cpp - Basic test harness for emfe_plugin_z8000
// Tests the C ABI plugin interface without requiring Google Test.
//
// Phase 1 limitations: only NOP ($8D07) is implemented. Tests that
// exercise real instructions will be expanded along with task #5.

#include "emfe_plugin.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <thread>
#include <mutex>
#include <string>

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
            fprintf(stderr, "  FAIL: %s - expected %lld, got %lld (line %d)\n", \
                    msg, (long long)(expected), (long long)(actual), __LINE__); \
            g_testsFailed++;                                                \
            return;                                                         \
        }                                                                   \
    } while (0)

// Register IDs (must match RegId enum in plugin_z8000.cpp)
static constexpr uint32_t REG_R0  = 0;
static constexpr uint32_t REG_R15 = 15;
static constexpr uint32_t REG_PC  = 16;
static constexpr uint32_t REG_FCW = 17;

// ============================================================================
// Negotiate
// ============================================================================

static void TestNegotiate() {
    printf("TestNegotiate...\n");

    EmfeNegotiateInfo info{};
    info.api_version_major = EMFE_API_VERSION_MAJOR;
    info.api_version_minor = EMFE_API_VERSION_MINOR;

    EmfeResult r = emfe_negotiate(&info);
    TEST_ASSERT(r == EMFE_OK, "negotiate should succeed");

    EmfeNegotiateInfo bad{};
    bad.api_version_major = 99;
    r = emfe_negotiate(&bad);
    TEST_ASSERT(r == EMFE_ERR_UNSUPPORTED, "wrong major should fail");

    r = emfe_negotiate(nullptr);
    TEST_ASSERT(r == EMFE_ERR_INVALID, "null should fail");

    printf("  PASS\n");
    g_testsPassed++;
}

// ============================================================================
// Board info
// ============================================================================

static void TestBoardInfo() {
    printf("TestBoardInfo...\n");

    EmfeBoardInfo info{};
    EmfeResult r = emfe_get_board_info(&info);
    TEST_ASSERT(r == EMFE_OK, "get_board_info should succeed");
    TEST_ASSERT(strcmp(info.board_name, "Z8000") == 0, "board_name should be Z8000");
    TEST_ASSERT(info.cpu_name != nullptr, "cpu_name should not be null");
    TEST_ASSERT(info.version != nullptr, "version should not be null");

    printf("  PASS\n");
    g_testsPassed++;
}

// ============================================================================
// Create / destroy
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
// Register defs
// ============================================================================

static void TestRegisterDefs() {
    printf("TestRegisterDefs...\n");

    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    const EmfeRegisterDef* defs = nullptr;
    int32_t count = emfe_get_register_defs(inst, &defs);
    TEST_ASSERT(count >= 18, "should have at least 16 GPRs + PC + FCW");
    TEST_ASSERT(defs != nullptr, "defs should not be null");

    // R0 at index 0
    TEST_ASSERT(strcmp(defs[0].name, "R0") == 0, "first reg should be R0");
    TEST_ASSERT(defs[0].bit_width == 16, "R0 should be 16-bit");

    // R15 has SP flag
    bool foundSP = false;
    bool foundPC = false;
    for (int i = 0; i < count; i++) {
        if (defs[i].flags & EMFE_REG_FLAG_SP) {
            foundSP = true;
            TEST_ASSERT(strcmp(defs[i].name, "R15") == 0, "SP flag should be on R15");
        }
        if (defs[i].flags & EMFE_REG_FLAG_PC) {
            foundPC = true;
            TEST_ASSERT(strcmp(defs[i].name, "PC") == 0, "PC flag reg should be named PC");
            TEST_ASSERT(defs[i].bit_width == 16, "PC should be 16-bit");
        }
    }
    TEST_ASSERT(foundSP, "should have SP flag on R15");
    TEST_ASSERT(foundPC, "should have a PC register");

    emfe_destroy(inst);
    printf("  PASS\n");
    g_testsPassed++;
}

// ============================================================================
// Memory (big-endian)
// ============================================================================

static void TestMemory() {
    printf("TestMemory...\n");

    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    emfe_poke_byte(inst, 0x1000, 0xAB);
    TEST_ASSERT_EQ(emfe_peek_byte(inst, 0x1000), 0xAB, "peek_byte");

    // Big-endian word: 0x1234 -> [0x12][0x34]
    emfe_poke_word(inst, 0x1010, 0x1234);
    TEST_ASSERT_EQ(emfe_peek_byte(inst, 0x1010), 0x12, "word hi byte first (big-endian)");
    TEST_ASSERT_EQ(emfe_peek_byte(inst, 0x1011), 0x34, "word lo byte second");
    TEST_ASSERT_EQ(emfe_peek_word(inst, 0x1010), 0x1234, "peek_word roundtrip");

    emfe_poke_long(inst, 0x1020, 0xDEADBEEF);
    TEST_ASSERT_EQ(emfe_peek_byte(inst, 0x1020), 0xDE, "long byte0=DE");
    TEST_ASSERT_EQ(emfe_peek_byte(inst, 0x1023), 0xEF, "long byte3=EF");
    TEST_ASSERT_EQ(emfe_peek_long(inst, 0x1020), 0xDEADBEEF, "peek_long roundtrip");

    TEST_ASSERT_EQ(emfe_get_memory_size(inst), 65536, "64KB memory");

    emfe_destroy(inst);
    printf("  PASS\n");
    g_testsPassed++;
}

// ============================================================================
// Step: NOP advances PC by 2
// ============================================================================

static void TestStepNop() {
    printf("TestStepNop...\n");

    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // NOP encoding: $8D07 (big-endian in memory: [0x8D][0x07])
    emfe_poke_word(inst, 0x0100, 0x8D07);
    emfe_poke_word(inst, 0x0102, 0x8D07);

    EmfeRegValue pcReg{};
    pcReg.reg_id = REG_PC;
    pcReg.value.u64 = 0x0100;
    emfe_set_registers(inst, &pcReg, 1);

    EmfeResult r = emfe_step(inst);
    TEST_ASSERT(r == EMFE_OK, "step NOP should succeed");

    emfe_get_registers(inst, &pcReg, 1);
    TEST_ASSERT_EQ(pcReg.value.u64, 0x0102, "PC should advance by 2");

    int64_t icount = emfe_get_instruction_count(inst);
    TEST_ASSERT_EQ(icount, 1, "one instruction executed");

    emfe_destroy(inst);
    printf("  PASS\n");
    g_testsPassed++;
}

// ============================================================================
// Reset: R0..R15 cleared, PC loaded from PSA vector if present
// ============================================================================

static void TestReset() {
    printf("TestReset...\n");

    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    EmfeRegValue regs[2];
    regs[0].reg_id = REG_R0;  regs[0].value.u64 = 0xABCD;
    regs[1].reg_id = REG_PC;  regs[1].value.u64 = 0x4000;
    emfe_set_registers(inst, regs, 2);

    EmfeResult r = emfe_reset(inst);
    TEST_ASSERT(r == EMFE_OK, "reset should succeed");

    EmfeRegValue r0{};
    r0.reg_id = REG_R0;
    emfe_get_registers(inst, &r0, 1);
    TEST_ASSERT_EQ(r0.value.u64, 0, "R0 should be 0 after reset");

    // FCW should have SN (system) bit set
    EmfeRegValue fcw{};
    fcw.reg_id = REG_FCW;
    emfe_get_registers(inst, &fcw, 1);
    TEST_ASSERT((fcw.value.u64 & 0x2000) != 0, "FCW.SN should be set on reset");

    emfe_destroy(inst);
    printf("  PASS\n");
    g_testsPassed++;
}

// ============================================================================
// Breakpoints
// ============================================================================

static void TestBreakpoints() {
    printf("TestBreakpoints...\n");

    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    EmfeResult r = emfe_add_breakpoint(inst, 0x0200);
    TEST_ASSERT(r == EMFE_OK, "add breakpoint");

    EmfeBreakpointInfo bps[10];
    int32_t count = emfe_get_breakpoints(inst, bps, 10);
    TEST_ASSERT_EQ(count, 1, "1 breakpoint");
    TEST_ASSERT_EQ(bps[0].address, 0x0200, "breakpoint address");

    r = emfe_enable_breakpoint(inst, 0x0200, false);
    TEST_ASSERT(r == EMFE_OK, "disable breakpoint");
    emfe_get_breakpoints(inst, bps, 10);
    TEST_ASSERT(!bps[0].enabled, "disabled");

    r = emfe_remove_breakpoint(inst, 0x0200);
    TEST_ASSERT(r == EMFE_OK, "remove breakpoint");
    count = emfe_get_breakpoints(inst, bps, 10);
    TEST_ASSERT_EQ(count, 0, "0 breakpoints after remove");

    emfe_destroy(inst);
    printf("  PASS\n");
    g_testsPassed++;
}

// ============================================================================
// Breakpoint stops Run
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

    // Five NOPs starting at $0100
    for (int i = 0; i < 5; i++)
        emfe_poke_word(inst, static_cast<uint64_t>(0x0100 + i * 2), 0x8D07);

    EmfeRegValue pcReg{};
    pcReg.reg_id = REG_PC;
    pcReg.value.u64 = 0x0100;
    emfe_set_registers(inst, &pcReg, 1);

    // Break at third NOP
    emfe_add_breakpoint(inst, 0x0104);
    emfe_run(inst);

    auto start = std::chrono::steady_clock::now();
    while (!g_stateCallbackFired) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(5)) break;
    }

    TEST_ASSERT(g_stateCallbackFired, "state callback should fire");
    TEST_ASSERT(g_lastStopReason == EMFE_STOP_REASON_BREAKPOINT, "BP stop reason");

    emfe_destroy(inst);
    printf("  PASS\n");
    g_testsPassed++;
}

// ============================================================================
// Disassembly (NOP recognised, others as DW)
// ============================================================================

static void TestDisassembly() {
    printf("TestDisassembly...\n");

    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    emfe_poke_word(inst, 0x0100, 0x8D07);  // NOP
    emfe_poke_word(inst, 0x0102, 0xABCD);  // unknown -> DW $ABCD

    EmfeDisasmLine line{};
    EmfeResult r = emfe_disassemble_one(inst, 0x0100, &line);
    TEST_ASSERT(r == EMFE_OK, "disassemble NOP");
    TEST_ASSERT(strcmp(line.mnemonic, "NOP") == 0, "mnemonic NOP");
    TEST_ASSERT(line.length == 2, "NOP length 2");

    r = emfe_disassemble_one(inst, 0x0102, &line);
    TEST_ASSERT(r == EMFE_OK, "disassemble unknown");
    TEST_ASSERT(strcmp(line.mnemonic, "DW") == 0, "unknown -> DW");

    emfe_destroy(inst);
    printf("  PASS\n");
    g_testsPassed++;
}

// ============================================================================
// Settings: CpuVariant combo
// ============================================================================

static void TestSettings() {
    printf("TestSettings...\n");

    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    const EmfeSettingDef* defs = nullptr;
    int32_t count = emfe_get_setting_defs(inst, &defs);
    TEST_ASSERT(count >= 1, "at least 1 setting");

    const char* variant = emfe_get_setting(inst, "CpuVariant");
    TEST_ASSERT(variant != nullptr, "CpuVariant not null");
    TEST_ASSERT(strcmp(variant, "Z8002") == 0, "default variant Z8002");

    EmfeResult r = emfe_set_setting(inst, "CpuVariant", "Z8001");
    TEST_ASSERT(r == EMFE_OK, "set CpuVariant");
    r = emfe_apply_settings(inst);
    TEST_ASSERT(r == EMFE_OK, "apply settings");

    variant = emfe_get_setting(inst, "CpuVariant");
    TEST_ASSERT(strcmp(variant, "Z8001") == 0, "variant changed to Z8001");

    emfe_destroy(inst);
    printf("  PASS\n");
    g_testsPassed++;
}

// ============================================================================
// Send character to UART
// ============================================================================

static void TestSendChar() {
    printf("TestSendChar...\n");

    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    EmfeResult r = emfe_send_char(inst, 'Z');
    TEST_ASSERT(r == EMFE_OK, "send_char");

    r = emfe_send_string(inst, "Hello");
    TEST_ASSERT(r == EMFE_OK, "send_string");

    emfe_destroy(inst);
    printf("  PASS\n");
    g_testsPassed++;
}

// ============================================================================
// LDB Rbd,#imm8 + LD Rd,#imm16
// ============================================================================

static void TestImmediateLoads() {
    printf("TestImmediateLoads...\n");

    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // LDB RL0,#$42  -> $C842
    emfe_poke_word(inst, 0x0100, 0xC842);
    // LD  R1,#$1234 -> $2101 $1234
    emfe_poke_word(inst, 0x0102, 0x2101);
    emfe_poke_word(inst, 0x0104, 0x1234);

    EmfeRegValue pc{}; pc.reg_id = REG_PC; pc.value.u64 = 0x0100;
    emfe_set_registers(inst, &pc, 1);

    emfe_step(inst); // LDB RL0,#$42
    EmfeRegValue r0{}; r0.reg_id = REG_R0;
    emfe_get_registers(inst, &r0, 1);
    TEST_ASSERT_EQ(r0.value.u64 & 0xFF, 0x42, "RL0 = $42");

    emfe_step(inst); // LD R1,#$1234
    EmfeRegValue r1{}; r1.reg_id = REG_R0 + 1;
    emfe_get_registers(inst, &r1, 1);
    TEST_ASSERT_EQ(r1.value.u64, 0x1234, "R1 = $1234");

    emfe_get_registers(inst, &pc, 1);
    TEST_ASSERT_EQ(pc.value.u64, 0x0106, "PC after LD imm16");

    emfe_destroy(inst);
    printf("  PASS\n");
    g_testsPassed++;
}

// ============================================================================
// ADD Rd,Rs + CP Rd,Rs (flag updates)
// ============================================================================

static void TestAddCp() {
    printf("TestAddCp...\n");

    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // LD R1,#$0003   ; $2101 $0003
    // LD R2,#$0004   ; $2102 $0004
    // ADD R1,R2      ; $8121  (src=R2, dst=R1)
    // CP  R1,R2      ; $8B21
    emfe_poke_word(inst, 0x0100, 0x2101);
    emfe_poke_word(inst, 0x0102, 0x0003);
    emfe_poke_word(inst, 0x0104, 0x2102);
    emfe_poke_word(inst, 0x0106, 0x0004);
    emfe_poke_word(inst, 0x0108, 0x8121);
    emfe_poke_word(inst, 0x010A, 0x8B21);

    EmfeRegValue pc{}; pc.reg_id = REG_PC; pc.value.u64 = 0x0100;
    emfe_set_registers(inst, &pc, 1);

    emfe_step(inst); emfe_step(inst); // loads
    emfe_step(inst);                  // ADD R1,R2
    EmfeRegValue r1{}; r1.reg_id = REG_R0 + 1;
    emfe_get_registers(inst, &r1, 1);
    TEST_ASSERT_EQ(r1.value.u64, 7, "R1 = 3+4 = 7");

    emfe_step(inst); // CP R1,R2 ; compare 7 vs 4 -> Z=0, C=0, S=0
    EmfeRegValue fcw{}; fcw.reg_id = REG_FCW;
    emfe_get_registers(inst, &fcw, 1);
    TEST_ASSERT((fcw.value.u64 & 0x0040) == 0, "Z=0 after CP 7,4");
    TEST_ASSERT((fcw.value.u64 & 0x0080) == 0, "C=0 after CP 7,4");

    emfe_destroy(inst);
    printf("  PASS\n");
    g_testsPassed++;
}

// ============================================================================
// JR NE, backward loop (simple counter down to 0)
// ============================================================================

static void TestJrLoop() {
    printf("TestJrLoop...\n");

    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // LD  R1,#$0003        ; 2101 0003
    // L:
    //   SUB R1,R2  (R2=0? No — use CP with 0 not possible without LD)
    // Simpler loop: decrement using SUB R1,R2 where R2 was loaded as 1.
    //   LD R2,#$0001       ; 2102 0001
    //   SUB R1,R2          ; 8321
    //   JR NE,-2 (back to SUB)   ; EE FE  (cc=NE=$E, disp=-2: 0xFE)
    //   HALT                ; 7A00
    emfe_poke_word(inst, 0x0100, 0x2101);  // LD R1,#3
    emfe_poke_word(inst, 0x0102, 0x0003);
    emfe_poke_word(inst, 0x0104, 0x2102);  // LD R2,#1
    emfe_poke_word(inst, 0x0106, 0x0001);
    emfe_poke_word(inst, 0x0108, 0x8321);  // SUB R1,R2
    // JR at $010A: after fetch PC=$010C, target = $010C + 2*disp.
    // To go back to SUB at $0108 we need disp = -2 = $FE.
    emfe_poke_word(inst, 0x010A, 0xEEFE);  // JR NE,$0108
    emfe_poke_word(inst, 0x010C, 0x7A00);  // HALT

    EmfeRegValue pc{}; pc.reg_id = REG_PC; pc.value.u64 = 0x0100;
    emfe_set_registers(inst, &pc, 1);

    // Run until halted (with timeout)
    g_stateCallbackFired = false;
    g_lastStopReason = EMFE_STOP_REASON_NONE;
    emfe_set_state_change_callback(inst, TestStateCallback, nullptr);
    emfe_run(inst);

    auto start = std::chrono::steady_clock::now();
    while (!g_stateCallbackFired) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(5)) break;
    }

    TEST_ASSERT(g_lastStopReason == EMFE_STOP_REASON_HALT, "loop ends with HALT");

    EmfeRegValue r1{}; r1.reg_id = REG_R0 + 1;
    emfe_get_registers(inst, &r1, 1);
    TEST_ASSERT_EQ(r1.value.u64, 0, "R1 decremented to 0");

    emfe_destroy(inst);
    printf("  PASS\n");
    g_testsPassed++;
}

// ============================================================================
// Phase 1b: memory load/store via indirect register
// ============================================================================

static void TestMemoryIndirect() {
    printf("TestMemoryIndirect...\n");

    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // Program:
    //   LD  R1,#$2000    ; 2101 2000   - pointer
    //   LD  R2,#$ABCD    ; 2102 ABCD   - value to store
    //   LD  @R1,R2       ; 2F12        - store word to [R1]
    //   LD  R3,@R1       ; 2113        - load word from [R1]
    //   HALT             ; 7A00
    emfe_poke_word(inst, 0x0100, 0x2101);
    emfe_poke_word(inst, 0x0102, 0x2000);
    emfe_poke_word(inst, 0x0104, 0x2102);
    emfe_poke_word(inst, 0x0106, 0xABCD);
    emfe_poke_word(inst, 0x0108, 0x2F12);  // LD @R1,R2
    emfe_poke_word(inst, 0x010A, 0x2113);  // LD R3,@R1
    emfe_poke_word(inst, 0x010C, 0x7A00);

    EmfeRegValue pc{}; pc.reg_id = REG_PC; pc.value.u64 = 0x0100;
    emfe_set_registers(inst, &pc, 1);

    for (int i = 0; i < 4; i++) emfe_step(inst);  // load pointer + value, store, load

    TEST_ASSERT_EQ(emfe_peek_word(inst, 0x2000), 0xABCD, "@$2000 should hold $ABCD");

    EmfeRegValue r3{}; r3.reg_id = REG_R0 + 3;
    emfe_get_registers(inst, &r3, 1);
    TEST_ASSERT_EQ(r3.value.u64, 0xABCD, "R3 loaded from memory");

    emfe_destroy(inst);
    printf("  PASS\n");
    g_testsPassed++;
}

// ============================================================================
// Phase 1b: CALL + RET (simple subroutine)
// ============================================================================

static void TestCallRet() {
    printf("TestCallRet...\n");

    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // Main:
    //   LD  R15,#$F000       ; set SP
    //   LD  R1,#$0005
    //   CALL sub             ; $5F00 $0200
    //   HALT
    // sub (at $0200):
    //   ADD R1,R1            ; $8111
    //   RET                  ; $9E08 (cc=T always)
    emfe_poke_word(inst, 0x0100, 0x210F);  // LD R15,#$F000
    emfe_poke_word(inst, 0x0102, 0xF000);
    emfe_poke_word(inst, 0x0104, 0x2101);  // LD R1,#$0005
    emfe_poke_word(inst, 0x0106, 0x0005);
    emfe_poke_word(inst, 0x0108, 0x5F00);  // CALL
    emfe_poke_word(inst, 0x010A, 0x0200);
    emfe_poke_word(inst, 0x010C, 0x7A00);  // HALT

    emfe_poke_word(inst, 0x0200, 0x8111);  // ADD R1,R1 -> R1=10
    emfe_poke_word(inst, 0x0202, 0x9E08);  // RET always

    EmfeRegValue pc{}; pc.reg_id = REG_PC; pc.value.u64 = 0x0100;
    emfe_set_registers(inst, &pc, 1);

    g_stateCallbackFired = false;
    g_lastStopReason = EMFE_STOP_REASON_NONE;
    emfe_set_state_change_callback(inst, TestStateCallback, nullptr);
    emfe_run(inst);

    auto start = std::chrono::steady_clock::now();
    while (!g_stateCallbackFired) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(5)) break;
    }
    TEST_ASSERT(g_lastStopReason == EMFE_STOP_REASON_HALT, "program halted");

    EmfeRegValue r1{}; r1.reg_id = REG_R0 + 1;
    emfe_get_registers(inst, &r1, 1);
    TEST_ASSERT_EQ(r1.value.u64, 10, "R1 = 5+5 after subroutine");

    emfe_destroy(inst);
    printf("  PASS\n");
    g_testsPassed++;
}

// ============================================================================
// Phase 1b: PUSH / POP
// ============================================================================

static void TestPushPop() {
    printf("TestPushPop...\n");

    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // Setup:
    //   LD R15,#$F000  (SP)
    //   LD R1,#$1111
    //   LD R2,#$2222
    //   PUSH @R15,R1   ; $13F1
    //   PUSH @R15,R2   ; $13F2
    //   POP  R3,@R15   ; $17F3
    //   POP  R4,@R15   ; $17F4
    //   HALT
    emfe_poke_word(inst, 0x0100, 0x210F);
    emfe_poke_word(inst, 0x0102, 0xF000);
    emfe_poke_word(inst, 0x0104, 0x2101);
    emfe_poke_word(inst, 0x0106, 0x1111);
    emfe_poke_word(inst, 0x0108, 0x2102);
    emfe_poke_word(inst, 0x010A, 0x2222);
    emfe_poke_word(inst, 0x010C, 0x13F1);
    emfe_poke_word(inst, 0x010E, 0x13F2);
    emfe_poke_word(inst, 0x0110, 0x17F3);
    emfe_poke_word(inst, 0x0112, 0x17F4);
    emfe_poke_word(inst, 0x0114, 0x7A00);

    EmfeRegValue pc{}; pc.reg_id = REG_PC; pc.value.u64 = 0x0100;
    emfe_set_registers(inst, &pc, 1);

    for (int i = 0; i < 7; i++) emfe_step(inst); // 7 instructions before HALT

    EmfeRegValue r3{}, r4{};
    r3.reg_id = REG_R0 + 3; r4.reg_id = REG_R0 + 4;
    emfe_get_registers(inst, &r3, 1);
    emfe_get_registers(inst, &r4, 1);
    // LIFO order: last-pushed ($2222) pops first into R3.
    TEST_ASSERT_EQ(r3.value.u64, 0x2222, "R3 = $2222 (last pushed)");
    TEST_ASSERT_EQ(r4.value.u64, 0x1111, "R4 = $1111 (first pushed)");

    emfe_destroy(inst);
    printf("  PASS\n");
    g_testsPassed++;
}

// ============================================================================
// Phase 1b: UART output via OUTB to port $FE00
// ============================================================================

static std::atomic<int> g_uartTxCount{0};
static char g_uartLastChar = 0;

static void EMFE_CALL UartTxCallback(void* /*user_data*/, char ch) {
    g_uartLastChar = ch;
    g_uartTxCount++;
}

static void TestUartOutput() {
    printf("TestUartOutput...\n");

    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    g_uartTxCount = 0;
    g_uartLastChar = 0;
    emfe_set_console_char_callback(inst, UartTxCallback, nullptr);

    // Program:
    //   LDB RL0,#'Z'     ; $C85A  (RL0 = byte reg 8)
    //   OUTB #$FE00,RL0  ; $3A86 $FE00  (sub-op=6=OUT, s=8=RL0, hi=$3A=byte)
    //   HALT
    emfe_poke_word(inst, 0x0100, 0xC85A);
    emfe_poke_word(inst, 0x0102, 0x3A86);
    emfe_poke_word(inst, 0x0104, 0xFE00);
    emfe_poke_word(inst, 0x0106, 0x7A00);

    EmfeRegValue pc{}; pc.reg_id = REG_PC; pc.value.u64 = 0x0100;
    emfe_set_registers(inst, &pc, 1);

    emfe_step(inst);  // LDB
    emfe_step(inst);  // OUTB -> should fire TxCallback

    TEST_ASSERT(g_uartTxCount > 0, "UART TX callback fired");
    TEST_ASSERT_EQ(g_uartLastChar, 'Z', "UART output 'Z'");

    emfe_destroy(inst);
    printf("  PASS\n");
    g_testsPassed++;
}

// ============================================================================
// Example: load examples/hello/hello.bin and verify full UART output.
// Only runs when EMFE_Z8000_EXAMPLES_DIR env var is set (skipped otherwise).
// ============================================================================

static std::string g_uartBuf;
static std::mutex g_uartMutex;

static void EMFE_CALL BufferingUartCallback(void* /*user_data*/, char ch) {
    std::lock_guard<std::mutex> lock(g_uartMutex);
    g_uartBuf.push_back(ch);
}

static void TestHelloExample() {
    printf("TestHelloExample...\n");

    const char* dir = std::getenv("EMFE_Z8000_EXAMPLES_DIR");
    if (!dir) {
        printf("  SKIP (EMFE_Z8000_EXAMPLES_DIR not set)\n");
        return;
    }

    std::string path = std::string(dir) + "/hello/hello.bin";

    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    g_uartBuf.clear();
    emfe_set_console_char_callback(inst, BufferingUartCallback, nullptr);

    EmfeResult r = emfe_load_binary(inst, path.c_str(), 0);
    TEST_ASSERT(r == EMFE_OK, "load_binary hello.bin");

    g_stateCallbackFired = false;
    g_lastStopReason = EMFE_STOP_REASON_NONE;
    emfe_set_state_change_callback(inst, TestStateCallback, nullptr);
    emfe_run(inst);

    auto start = std::chrono::steady_clock::now();
    while (!g_stateCallbackFired) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(5)) break;
    }
    TEST_ASSERT(g_lastStopReason == EMFE_STOP_REASON_HALT, "hello halts");

    {
        std::lock_guard<std::mutex> lock(g_uartMutex);
        if (g_uartBuf != "Hello, Z8000!\r\n") {
            fprintf(stderr, "  FAIL: UART buffer mismatch, got \"");
            for (char c : g_uartBuf) {
                if (c >= 0x20 && c < 0x7F) fputc(c, stderr);
                else fprintf(stderr, "\\x%02X", static_cast<uint8_t>(c));
            }
            fprintf(stderr, "\"\n");
            g_testsFailed++;
            emfe_destroy(inst);
            return;
        }
    }

    emfe_destroy(inst);
    printf("  PASS\n");
    g_testsPassed++;
}

// ============================================================================
// Example: echo.bin - polling echo. Verify banner is emitted on startup, then
// inject a char and verify it is echoed back.
// ============================================================================

static void TestEchoExample() {
    printf("TestEchoExample...\n");

    const char* dir = std::getenv("EMFE_Z8000_EXAMPLES_DIR");
    if (!dir) {
        printf("  SKIP (EMFE_Z8000_EXAMPLES_DIR not set)\n");
        return;
    }

    std::string path = std::string(dir) + "/echo/echo.bin";

    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    g_uartBuf.clear();
    emfe_set_console_char_callback(inst, BufferingUartCallback, nullptr);

    EmfeResult r = emfe_load_binary(inst, path.c_str(), 0);
    TEST_ASSERT(r == EMFE_OK, "load_binary echo.bin");

    emfe_run(inst);

    // Wait for banner (polls g_uartBuf size).
    auto start = std::chrono::steady_clock::now();
    while (true) {
        {
            std::lock_guard<std::mutex> lock(g_uartMutex);
            if (g_uartBuf.size() >= 50) break;
        }
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(5)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::string banner;
    {
        std::lock_guard<std::mutex> lock(g_uartMutex);
        banner = g_uartBuf;
        g_uartBuf.clear();
    }
    TEST_ASSERT(banner.find("Z8000 Echo") != std::string::npos, "banner emitted");

    // Inject 'A' and verify it is echoed.
    emfe_send_char(inst, 'A');
    auto inj = std::chrono::steady_clock::now();
    while (true) {
        {
            std::lock_guard<std::mutex> lock(g_uartMutex);
            if (!g_uartBuf.empty()) break;
        }
        if (std::chrono::steady_clock::now() - inj > std::chrono::seconds(2)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::string echoed;
    {
        std::lock_guard<std::mutex> lock(g_uartMutex);
        echoed = g_uartBuf;
    }
    TEST_ASSERT(echoed.find('A') != std::string::npos, "'A' echoed");

    emfe_stop(inst);
    emfe_destroy(inst);
    printf("  PASS\n");
    g_testsPassed++;
}

// ============================================================================
// Diagnostic: load fib.bin, set BP after CALR print_u16, verify R8 progresses.
// ============================================================================

static std::atomic<int> g_bpHitCount{0};
static void EMFE_CALL CountingStateCallback(void* /*u*/, const EmfeStateInfo* info) {
    g_stateCallbackFired = true;
    g_lastStopReason = info->stop_reason;
    if (info->stop_reason == EMFE_STOP_REASON_BREAKPOINT) g_bpHitCount++;
}

static void TestFibonacciBinRegisterTrace() {
    printf("TestFibonacciBinRegisterTrace...\n");
    const char* dir = std::getenv("EMFE_Z8000_EXAMPLES_DIR");
    if (!dir) { printf("  SKIP\n"); return; }

    std::string path = std::string(dir) + "/fibonacci/fibonacci.bin";

    EmfeInstance inst = nullptr;
    emfe_create(&inst);
    g_uartBuf.clear();
    emfe_set_console_char_callback(inst, BufferingUartCallback, nullptr);
    emfe_load_binary(inst, path.c_str(), 0);

    // fib_loop is at $01EE, CALR print_u16 is at $01F0, next is $01F2.
    // Break at $01F2 and verify R8 progresses: 0, 1, 1, 2, 3, ...
    emfe_add_breakpoint(inst, 0x01F2);

    g_bpHitCount = 0;
    emfe_set_state_change_callback(inst, CountingStateCallback, nullptr);

    uint16_t expected[] = {0, 1, 1, 2, 3, 5};
    for (int iter = 0; iter < 6; iter++) {
        g_stateCallbackFired = false;
        emfe_run(inst);

        auto start = std::chrono::steady_clock::now();
        while (!g_stateCallbackFired) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (std::chrono::steady_clock::now() - start > std::chrono::seconds(5)) break;
        }
        TEST_ASSERT(g_lastStopReason == EMFE_STOP_REASON_BREAKPOINT, "BP hit");

        EmfeRegValue r8{}; r8.reg_id = REG_R0 + 8;
        emfe_get_registers(inst, &r8, 1);
        fprintf(stderr, "  iter %d: R8=%llu (expected %u)\n",
                iter, (unsigned long long)r8.value.u64, expected[iter]);
        if (r8.value.u64 != expected[iter]) {
            g_testsFailed++;
            emfe_destroy(inst);
            return;
        }
    }

    emfe_destroy(inst);
    printf("  PASS\n");
    g_testsPassed++;
}

// ============================================================================
// Diagnostic: Fibonacci arithmetic only (no print, no subroutine)
// ============================================================================

static void TestFibonacciArithmetic() {
    printf("TestFibonacciArithmetic...\n");

    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // Program: compute 3 fib iterations; final R8 should be fib(3)=2.
    //
    // LD R1,#1            2101 0001
    // LD R8,#0            2108 0000
    // LD R9,#1            2109 0001
    // LD R10,#3           210A 0003
    // loop:
    //   LD R11,R8         A18B
    //   ADD R11,R9        819B
    //   LD R8,R9          A198
    //   LD R9,R11         A1B9
    //   SUB R10,R1        831A
    //   JR NE,loop        EEFA  (disp -6 after fetch at PC+2)
    // HALT                7A00
    uint16_t addr = 0x0100;
    auto w = [&](uint16_t v) { emfe_poke_word(inst, addr, v); addr += 2; };
    w(0x2101); w(0x0001);
    w(0x2108); w(0x0000);
    w(0x2109); w(0x0001);
    w(0x210A); w(0x0003);
    uint16_t loop_addr = addr;
    w(0xA18B);
    w(0x819B);
    w(0xA198);
    w(0xA1B9);
    w(0x831A);
    uint16_t jr_addr = addr;
    int16_t disp = (static_cast<int16_t>(loop_addr) - (static_cast<int16_t>(jr_addr) + 2)) / 2;
    w(static_cast<uint16_t>(0xEE00 | (disp & 0xFF)));
    w(0x7A00);

    EmfeRegValue pc{}; pc.reg_id = REG_PC; pc.value.u64 = 0x0100;
    emfe_set_registers(inst, &pc, 1);

    g_stateCallbackFired = false;
    g_lastStopReason = EMFE_STOP_REASON_NONE;
    emfe_set_state_change_callback(inst, TestStateCallback, nullptr);
    emfe_run(inst);

    auto start = std::chrono::steady_clock::now();
    while (!g_stateCallbackFired) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(5)) break;
    }
    TEST_ASSERT(g_lastStopReason == EMFE_STOP_REASON_HALT, "arithmetic halts");

    EmfeRegValue r8{}; r8.reg_id = REG_R0 + 8;
    EmfeRegValue r9{}; r9.reg_id = REG_R0 + 9;
    emfe_get_registers(inst, &r8, 1);
    emfe_get_registers(inst, &r9, 1);
    // After 3 iters of (tmp=a+b; a=b; b=tmp): starting (0,1),
    // iter1: tmp=1, a=1, b=1
    // iter2: tmp=2, a=1, b=2
    // iter3: tmp=3, a=2, b=3
    TEST_ASSERT_EQ(r8.value.u64, 2, "R8 should be fib(2)=2");
    TEST_ASSERT_EQ(r9.value.u64, 3, "R9 should be fib(3)=3");

    emfe_destroy(inst);
    printf("  PASS\n");
    g_testsPassed++;
}

// ============================================================================
// Example: fibonacci.bin - computes fib(0..10) and prints decimal.
// ============================================================================

static void TestFibonacciExample() {
    printf("TestFibonacciExample...\n");

    const char* dir = std::getenv("EMFE_Z8000_EXAMPLES_DIR");
    if (!dir) {
        printf("  SKIP (EMFE_Z8000_EXAMPLES_DIR not set)\n");
        return;
    }

    std::string path = std::string(dir) + "/fibonacci/fibonacci.bin";

    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    g_uartBuf.clear();
    emfe_set_console_char_callback(inst, BufferingUartCallback, nullptr);

    EmfeResult r = emfe_load_binary(inst, path.c_str(), 0);
    TEST_ASSERT(r == EMFE_OK, "load_binary fibonacci.bin");

    g_stateCallbackFired = false;
    g_lastStopReason = EMFE_STOP_REASON_NONE;
    emfe_set_state_change_callback(inst, TestStateCallback, nullptr);
    emfe_run(inst);

    auto start = std::chrono::steady_clock::now();
    while (!g_stateCallbackFired) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(10)) break;
    }
    TEST_ASSERT(g_lastStopReason == EMFE_STOP_REASON_HALT, "fibonacci halts");

    std::string output;
    {
        std::lock_guard<std::mutex> lock(g_uartMutex);
        output = g_uartBuf;
    }
    // Expected to contain key fibonacci terms 0, 1, 1, 2, 3, 5, 8, 13, 21, 34, 55.
    bool ok = output.find("Fibonacci:") != std::string::npos
           && output.find("55") != std::string::npos
           && output.find("HALT") != std::string::npos;
    if (!ok) {
        fprintf(stderr, "  FAIL: fibonacci output unexpected: \"");
        for (char c : output) {
            if (c >= 0x20 && c < 0x7F) fputc(c, stderr);
            else fprintf(stderr, "\\x%02X", static_cast<uint8_t>(c));
        }
        fprintf(stderr, "\"\n");
        g_testsFailed++;
        emfe_destroy(inst);
        return;
    }

    emfe_destroy(inst);
    printf("  PASS\n");
    g_testsPassed++;
}

// ============================================================================
// Unsupported (ELF on Phase 1)
// ============================================================================

static void TestUnsupported() {
    printf("TestUnsupported...\n");

    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    EmfeResult r = emfe_load_elf(inst, "dummy.elf");
    TEST_ASSERT(r == EMFE_ERR_UNSUPPORTED, "load_elf unsupported in Phase 1");

    EmfeFramebufferInfo fb{};
    r = emfe_get_framebuffer_info(inst, &fb);
    TEST_ASSERT(r == EMFE_ERR_UNSUPPORTED, "framebuffer unsupported");

    r = emfe_push_key(inst, 0, true);
    TEST_ASSERT(r == EMFE_ERR_UNSUPPORTED, "push_key unsupported");

    emfe_destroy(inst);
    printf("  PASS\n");
    g_testsPassed++;
}

// ============================================================================
// Main
// ============================================================================

int main() {
    printf("=== emfe_plugin_z8000 Test Suite ===\n\n");

    TestNegotiate();
    TestBoardInfo();
    TestCreateDestroy();
    TestRegisterDefs();
    TestMemory();
    TestStepNop();
    TestReset();
    TestBreakpoints();
    TestBreakpointStopsRun();
    TestDisassembly();
    TestSettings();
    TestSendChar();
    TestImmediateLoads();
    TestAddCp();
    TestJrLoop();
    TestMemoryIndirect();
    TestCallRet();
    TestPushPop();
    TestUartOutput();
    TestFibonacciArithmetic();
    TestFibonacciBinRegisterTrace();
    TestHelloExample();
    TestEchoExample();
    TestFibonacciExample();
    TestUnsupported();

    printf("\n=== Results: %d passed, %d failed ===\n", g_testsPassed, g_testsFailed);
    return g_testsFailed > 0 ? 1 : 0;
}
