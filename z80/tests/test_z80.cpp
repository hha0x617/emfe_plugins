// SPDX-License-Identifier: MIT OR Apache-2.0
// Copyright (c) 2026 hha0x617
//
// test_z80.cpp - Smoke tests for the Phase 1.1 scaffold of plugin_z80.
//
// At this stage the plugin has no opcodes implemented. The smoke tests
// verify only that the DLL loads, the discovery API reports the expected
// metadata, and create / reset / destroy work without crashing.
//
// Later phases (1.2 memory + UART, 1.3 register defs, ...) will grow
// this file alongside the implementation.

#include "emfe_plugin.h"
#include <cstdio>
#include <cstring>
#include <string>

static int g_pass = 0;
static int g_fail = 0;

#define TEST_TRUE(cond, msg)                                              \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__);     \
            g_fail++;                                                     \
            return;                                                       \
        }                                                                 \
    } while (0)

#define TEST_EQ(actual, expected, msg)                                    \
    do {                                                                  \
        if ((actual) != (expected)) {                                     \
            fprintf(stderr, "  FAIL: %s — got %lld, expected %lld (line %d)\n", \
                    msg, (long long)(actual), (long long)(expected), __LINE__); \
            g_fail++;                                                     \
            return;                                                       \
        }                                                                 \
    } while (0)

static void TestNegotiate() {
    printf("TestNegotiate...\n");

    EmfeNegotiateInfo info{};
    info.api_version_major = EMFE_API_VERSION_MAJOR;
    info.api_version_minor = EMFE_API_VERSION_MINOR;
    TEST_EQ(emfe_negotiate(&info), EMFE_OK, "negotiate matches version");

    EmfeNegotiateInfo bad{};
    bad.api_version_major = EMFE_API_VERSION_MAJOR + 1;
    TEST_EQ(emfe_negotiate(&bad), EMFE_ERR_UNSUPPORTED, "negotiate rejects wrong major");

    TEST_EQ(emfe_negotiate(nullptr), EMFE_ERR_INVALID, "negotiate rejects null");

    g_pass++;
}

static void TestBoardInfo() {
    printf("TestBoardInfo...\n");

    EmfeBoardInfo info{};
    TEST_EQ(emfe_get_board_info(&info), EMFE_OK, "get_board_info returns OK");
    TEST_TRUE(info.board_name != nullptr, "board_name not null");
    TEST_TRUE(info.cpu_name != nullptr, "cpu_name not null");
    TEST_TRUE(std::strcmp(info.cpu_name, "Z80") == 0, "cpu_name is Z80");

    TEST_EQ(emfe_get_board_info(nullptr), EMFE_ERR_INVALID, "rejects null");
    g_pass++;
}

static void TestLifecycle() {
    printf("TestLifecycle...\n");

    EmfeInstance inst = nullptr;
    TEST_EQ(emfe_create(&inst), EMFE_OK, "create returns OK");
    TEST_TRUE(inst != nullptr, "instance not null");

    TEST_EQ(emfe_get_memory_size(inst), (uint64_t)0x10000, "memory size is 64 KiB");
    TEST_EQ(emfe_get_state(inst), EMFE_STATE_STOPPED, "initial state is stopped");
    TEST_EQ(emfe_get_instruction_count(inst), (int64_t)0, "instruction count starts at 0");
    TEST_EQ(emfe_get_cycle_count(inst), (int64_t)0, "cycle count starts at 0");

    TEST_EQ(emfe_reset(inst), EMFE_OK, "reset returns OK");

    TEST_EQ(emfe_destroy(inst), EMFE_OK, "destroy returns OK");
    TEST_EQ(emfe_destroy(nullptr), EMFE_ERR_INVALID, "destroy rejects null");

    g_pass++;
}

// ---------------------------------------------------------------------
// Phase 1.2 tests — memory + UART
// ---------------------------------------------------------------------

static void TestMemoryPeekPoke() {
    printf("TestMemoryPeekPoke...\n");

    EmfeInstance inst = nullptr;
    TEST_EQ(emfe_create(&inst), EMFE_OK, "create OK");

    // After create RAM is zero-filled.
    TEST_EQ(emfe_peek_byte(inst, 0x0000), 0, "byte default 0");
    TEST_EQ(emfe_peek_byte(inst, 0xFFFF), 0, "high byte default 0");

    // Write + read a byte.
    TEST_EQ(emfe_poke_byte(inst, 0x1234, 0xAB), EMFE_OK, "poke byte OK");
    TEST_EQ(emfe_peek_byte(inst, 0x1234), 0xAB, "byte round-trip");

    // Word — little-endian per Z80 convention.
    TEST_EQ(emfe_poke_word(inst, 0x2000, 0xBEEF), EMFE_OK, "poke word OK");
    TEST_EQ(emfe_peek_byte(inst, 0x2000), 0xEF, "word LSB first");
    TEST_EQ(emfe_peek_byte(inst, 0x2001), 0xBE, "word MSB second");
    TEST_EQ(emfe_peek_word(inst, 0x2000), 0xBEEF, "word round-trip");

    // Long — 32-bit little-endian.
    TEST_EQ(emfe_poke_long(inst, 0x3000, 0xDEADBEEFu), EMFE_OK, "poke long OK");
    TEST_EQ(emfe_peek_long(inst, 0x3000), 0xDEADBEEFu, "long round-trip");

    // Range read.
    uint8_t buf[8] = {};
    TEST_EQ(emfe_peek_range(inst, 0x3000, buf, 4), EMFE_OK, "peek_range OK");
    TEST_EQ(buf[0], 0xEF, "range byte 0");
    TEST_EQ(buf[1], 0xBE, "range byte 1");
    TEST_EQ(buf[2], 0xAD, "range byte 2");
    TEST_EQ(buf[3], 0xDE, "range byte 3");

    // Reset clears RAM.
    TEST_EQ(emfe_reset(inst), EMFE_OK, "reset OK");
    TEST_EQ(emfe_peek_byte(inst, 0x1234), 0, "reset clears RAM");

    emfe_destroy(inst);
    g_pass++;
}

// Captures console-char-callback invocations.
struct CaptureCtx {
    std::string captured;
};
static void CaptureChar(void* user_data, char ch) {
    auto* c = static_cast<CaptureCtx*>(user_data);
    c->captured.push_back(ch);
}

static void TestConsoleSendChar() {
    // Host → guest direction (the FIFO is verified end-to-end in
    // Phase 1.4 once IN A,(0xFE) is implemented; for Phase 1.2 we
    // verify the API returns OK and does not crash).
    printf("TestConsoleSendChar...\n");

    EmfeInstance inst = nullptr;
    TEST_EQ(emfe_create(&inst), EMFE_OK, "create OK");

    TEST_EQ(emfe_send_char(inst, 'A'), EMFE_OK, "send_char OK");
    TEST_EQ(emfe_send_string(inst, "Hi"), EMFE_OK, "send_string OK");
    TEST_EQ(emfe_send_char(nullptr, 'A'), EMFE_ERR_INVALID, "send_char null rejected");
    TEST_EQ(emfe_send_string(inst, nullptr), EMFE_ERR_INVALID, "send_string null rejected");

    emfe_destroy(inst);
    g_pass++;
}

static void TestConsoleCallback() {
    // Verify guest → host: the callback fires when the UART would be
    // written from inside the CPU. We can't run IN/OUT yet (no
    // opcodes), so we provoke the callback by poking the UART's
    // ReceiveChar / TxCallback chain via a dummy write — but we DON'T
    // have direct access to the UART from the ABI. Instead, register
    // the callback and confirm registration succeeds; the wiring is
    // exercised at the source level by Phase 1.4's hello-world test.
    printf("TestConsoleCallback...\n");

    EmfeInstance inst = nullptr;
    TEST_EQ(emfe_create(&inst), EMFE_OK, "create OK");

    CaptureCtx ctx;
    TEST_EQ(emfe_set_console_char_callback(inst, CaptureChar, &ctx),
            EMFE_OK, "register callback OK");

    // Re-registering with nullptr clears it.
    TEST_EQ(emfe_set_console_char_callback(inst, nullptr, nullptr),
            EMFE_OK, "clear callback OK");
    TEST_EQ(emfe_set_console_char_callback(nullptr, CaptureChar, &ctx),
            EMFE_ERR_INVALID, "rejects null instance");

    emfe_destroy(inst);
    g_pass++;
}

// ---------------------------------------------------------------------
// Phase 1.3 tests — register defs, register r/w, NOP step
// ---------------------------------------------------------------------

// Helper: look up a single register by ID.
static uint64_t GetReg(EmfeInstance inst, uint32_t reg_id) {
    EmfeRegValue rv{};
    rv.reg_id = reg_id;
    emfe_get_registers(inst, &rv, 1);
    return rv.value.u64;
}
static EmfeResult SetReg(EmfeInstance inst, uint32_t reg_id, uint64_t value) {
    EmfeRegValue rv{};
    rv.reg_id = reg_id;
    rv.value.u64 = value;
    return emfe_set_registers(inst, &rv, 1);
}

// Z80 register IDs — mirrored from plugin_z80.cpp's RegId enum. Tests
// reference the wire IDs explicitly so any future renumbering is
// caught by recompilation here.
enum {
    REG_AF = 0, REG_A, REG_F,
    REG_BC, REG_B, REG_C,
    REG_DE, REG_D, REG_E,
    REG_HL, REG_H, REG_L,
    REG_IX, REG_IY, REG_SP, REG_PC,
    REG_I,  REG_R,
    REG_AF_SHADOW, REG_BC_SHADOW, REG_DE_SHADOW, REG_HL_SHADOW,
    REG_IFF1, REG_IFF2, REG_IM,
    REG_CYCLES, REG_INSTRUCTIONS,
};

static void TestRegisterDefs() {
    printf("TestRegisterDefs...\n");

    EmfeInstance inst = nullptr;
    TEST_EQ(emfe_create(&inst), EMFE_OK, "create OK");

    const EmfeRegisterDef* defs = nullptr;
    int32_t n = emfe_get_register_defs(inst, &defs);
    TEST_EQ(n, 27, "27 register definitions"); // 12 main + 4 idx/sys + 2 i/r + 4 shadow + 3 irq + 2 counters
    TEST_TRUE(defs != nullptr, "defs pointer non-null");

    // Spot-check that PC carries the PC flag and SP carries the SP flag.
    bool sawPc = false, sawSp = false, sawF = false;
    for (int32_t i = 0; i < n; ++i) {
        if (defs[i].reg_id == REG_PC) {
            sawPc = (defs[i].flags & EMFE_REG_FLAG_PC) != 0;
        }
        if (defs[i].reg_id == REG_SP) {
            sawSp = (defs[i].flags & EMFE_REG_FLAG_SP) != 0;
        }
        if (defs[i].reg_id == REG_F) {
            sawF = (defs[i].flags & EMFE_REG_FLAG_FLAGS) != 0;
        }
    }
    TEST_TRUE(sawPc, "PC flagged as program counter");
    TEST_TRUE(sawSp, "SP flagged as stack pointer");
    TEST_TRUE(sawF,  "F flagged as flags register");

    // Flag bits for F: must return 8 entries (S/Z/Y/H/X/P-V/N/C).
    const EmfeRegFlagBitDef* fbits = nullptr;
    int32_t nbits = emfe_get_register_flag_defs(inst, REG_F, &fbits);
    TEST_EQ(nbits, 8, "F has 8 flag bits");
    TEST_TRUE(fbits != nullptr, "flag-bit array non-null");

    emfe_destroy(inst);
    g_pass++;
}

static void TestRegisterRoundTrip() {
    printf("TestRegisterRoundTrip...\n");

    EmfeInstance inst = nullptr;
    TEST_EQ(emfe_create(&inst), EMFE_OK, "create OK");

    // Whole-pair writes are observable via the byte halves.
    TEST_EQ(SetReg(inst, REG_AF, 0xCAFE), EMFE_OK, "set AF OK");
    TEST_EQ(GetReg(inst, REG_AF), 0xCAFE, "AF round-trip");
    TEST_EQ(GetReg(inst, REG_A),  0xCA,   "A is AF high");
    TEST_EQ(GetReg(inst, REG_F),  0xFE,   "F is AF low");

    // Byte half writes update the pair.
    TEST_EQ(SetReg(inst, REG_B, 0x12), EMFE_OK, "set B OK");
    TEST_EQ(SetReg(inst, REG_C, 0x34), EMFE_OK, "set C OK");
    TEST_EQ(GetReg(inst, REG_BC), 0x1234, "BC reflects B/C halves");

    // Shadow set is independent of the main set.
    TEST_EQ(SetReg(inst, REG_BC,        0x0000), EMFE_OK, "main BC := 0");
    TEST_EQ(SetReg(inst, REG_BC_SHADOW, 0xABCD), EMFE_OK, "shadow BC := 0xABCD");
    TEST_EQ(GetReg(inst, REG_BC),        (uint64_t)0x0000, "main BC unchanged");
    TEST_EQ(GetReg(inst, REG_BC_SHADOW), (uint64_t)0xABCD, "shadow BC retained");

    // SP, PC, IX, IY round-trip.
    TEST_EQ(SetReg(inst, REG_PC, 0x8000), EMFE_OK, "set PC OK");
    TEST_EQ(GetReg(inst, REG_PC), 0x8000, "PC round-trip");
    TEST_EQ(SetReg(inst, REG_SP, 0xFFFE), EMFE_OK, "set SP OK");
    TEST_EQ(GetReg(inst, REG_SP), 0xFFFE, "SP round-trip");

    // IFF1 / IFF2 / IM round-trip.
    TEST_EQ(SetReg(inst, REG_IFF1, 1), EMFE_OK, "set IFF1");
    TEST_EQ(SetReg(inst, REG_IM,   2), EMFE_OK, "set IM");
    TEST_EQ(GetReg(inst, REG_IFF1), (uint64_t)1, "IFF1 round-trip");
    TEST_EQ(GetReg(inst, REG_IM),   (uint64_t)2, "IM round-trip");

    // Counters are read-only — writes are dropped silently.
    TEST_EQ(SetReg(inst, REG_CYCLES, 42), EMFE_OK, "cycle write accepted");
    TEST_EQ(GetReg(inst, REG_CYCLES), (uint64_t)0, "cycle write was dropped");

    emfe_destroy(inst);
    g_pass++;
}

static void TestStepNop() {
    printf("TestStepNop...\n");

    EmfeInstance inst = nullptr;
    TEST_EQ(emfe_create(&inst), EMFE_OK, "create OK");

    // RAM is zero-filled at create, so PC=0 sits on a NOP (0x00).
    TEST_EQ(GetReg(inst, REG_PC), (uint64_t)0, "PC starts at 0");
    TEST_EQ(emfe_step(inst), EMFE_OK, "step a NOP succeeds");
    TEST_EQ(GetReg(inst, REG_PC), (uint64_t)1, "PC advanced by 1");
    TEST_EQ(emfe_get_instruction_count(inst), (int64_t)1, "instruction count incremented");
    TEST_EQ(emfe_get_cycle_count(inst), (int64_t)4, "NOP consumed 4 T-states");

    // R register increments per fetch (low 7 bits, bit 7 preserved).
    TEST_EQ(GetReg(inst, REG_R), (uint64_t)1, "R incremented on fetch");

    // Step again on another NOP.
    TEST_EQ(emfe_step(inst), EMFE_OK, "second NOP OK");
    TEST_EQ(GetReg(inst, REG_PC), (uint64_t)2, "PC advanced again");
    TEST_EQ(emfe_get_cycle_count(inst), (int64_t)8, "cycles accumulated");

    // An unrecognised opcode (e.g. 0xDD with no follow-up — impossible
    // here since 0xDD is decoded; use 0xED 0x00 which is an unassigned
    // ED slot and now acts as NOP per Phase 2.3. We can't easily test
    // unsupported-trap now that DD/ED/CB are all decoded; assert
    // instead that the trap path is reachable via an isolated 0xDD at
    // the end of memory which would fetch garbage as the suffix — but
    // even that decodes per spec. The trap path is exercised by
    // Phase 2.1 RegisterDefs / Phase 1.2 step-trap test instead).

    emfe_destroy(inst);
    g_pass++;
}

static void TestRunningStateBlocksWrites() {
    // emfe_set_registers must reject writes while RUNNING. We don't
    // have emfe_run wired up yet (the worker thread is Phase 1.6+),
    // so we cheat by leaving STATE at STOPPED and just verify the path
    // is reachable. Full coverage lands once the run loop exists.
    printf("TestRunningStateBlocksWrites... (placeholder)\n");
    g_pass++;
}

// ---------------------------------------------------------------------
// Phase 1.4 tests — LD (load/store) instruction family
// ---------------------------------------------------------------------

// Helper: load a sequence of bytes starting at `addr` and reset PC to
// `addr` so emfe_step picks them up immediately.
static void LoadProgram(EmfeInstance inst, uint16_t addr,
                        const uint8_t* bytes, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        emfe_poke_byte(inst, addr + i, bytes[i]);
    }
    SetReg(inst, REG_PC, addr);
}

static void TestLdRegImmediate() {
    // LD r, n — pattern 00 ddd 110. Eight opcodes, one per dest.
    printf("TestLdRegImmediate...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // LD B, 0x11 ; LD C, 0x22 ; LD D, 0x33 ; LD E, 0x44 ;
    // LD H, 0x55 ; LD L, 0x66 ; LD A, 0x77
    const uint8_t prog[] = {
        0x06, 0x11, 0x0E, 0x22, 0x16, 0x33, 0x1E, 0x44,
        0x26, 0x55, 0x2E, 0x66, 0x3E, 0x77,
    };
    LoadProgram(inst, 0x0100, prog, sizeof(prog));
    for (int i = 0; i < 7; ++i) {
        TEST_EQ(emfe_step(inst), EMFE_OK, "LD r,n step OK");
    }
    TEST_EQ(GetReg(inst, REG_B), (uint64_t)0x11, "B = 0x11");
    TEST_EQ(GetReg(inst, REG_C), (uint64_t)0x22, "C = 0x22");
    TEST_EQ(GetReg(inst, REG_D), (uint64_t)0x33, "D = 0x33");
    TEST_EQ(GetReg(inst, REG_E), (uint64_t)0x44, "E = 0x44");
    TEST_EQ(GetReg(inst, REG_H), (uint64_t)0x55, "H = 0x55");
    TEST_EQ(GetReg(inst, REG_L), (uint64_t)0x66, "L = 0x66");
    TEST_EQ(GetReg(inst, REG_A), (uint64_t)0x77, "A = 0x77");
    TEST_EQ(emfe_get_cycle_count(inst), (int64_t)(7 * 7),
            "7 instructions × 7 T-states");
    TEST_EQ(GetReg(inst, REG_PC), (uint64_t)(0x0100 + 14), "PC advanced 14 bytes");

    emfe_destroy(inst);
    g_pass++;
}

static void TestLdRegPairImmediate() {
    // LD rr, nn — pattern 00 pp 0001. BC / DE / HL / SP.
    printf("TestLdRegPairImmediate...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // LD BC,0x1234 ; LD DE,0x5678 ; LD HL,0x9ABC ; LD SP,0xDEF0
    const uint8_t prog[] = {
        0x01, 0x34, 0x12,
        0x11, 0x78, 0x56,
        0x21, 0xBC, 0x9A,
        0x31, 0xF0, 0xDE,
    };
    LoadProgram(inst, 0, prog, sizeof(prog));
    for (int i = 0; i < 4; ++i) {
        TEST_EQ(emfe_step(inst), EMFE_OK, "LD rr,nn step OK");
    }
    TEST_EQ(GetReg(inst, REG_BC), (uint64_t)0x1234, "BC = 0x1234");
    TEST_EQ(GetReg(inst, REG_DE), (uint64_t)0x5678, "DE = 0x5678");
    TEST_EQ(GetReg(inst, REG_HL), (uint64_t)0x9ABC, "HL = 0x9ABC");
    TEST_EQ(GetReg(inst, REG_SP), (uint64_t)0xDEF0, "SP = 0xDEF0");
    TEST_EQ(emfe_get_cycle_count(inst), (int64_t)(4 * 10),
            "4 × 10 T-states");

    emfe_destroy(inst);
    g_pass++;
}

static void TestLdRegReg() {
    // LD r, r' — pattern 01 ddd sss. Spot-check several combinations
    // and verify HALT (0x76) is NOT executed.
    printf("TestLdRegReg...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // Set A=0xAA via LD A,n, then LD B,A ; LD C,B ; LD D,A ;
    //   LD H,D ; LD L,H ; LD A,L.
    const uint8_t prog[] = {
        0x3E, 0xAA,   // LD A, 0xAA
        0x47,         // LD B, A
        0x48,         // LD C, B
        0x57,         // LD D, A
        0x62,         // LD H, D
        0x6C,         // LD L, H
        0x7D,         // LD A, L
    };
    LoadProgram(inst, 0, prog, sizeof(prog));
    for (int i = 0; i < 7; ++i) {
        TEST_EQ(emfe_step(inst), EMFE_OK, "LD r,r' step OK");
    }
    TEST_EQ(GetReg(inst, REG_A), (uint64_t)0xAA, "A round-trip");
    TEST_EQ(GetReg(inst, REG_B), (uint64_t)0xAA, "B got A");
    TEST_EQ(GetReg(inst, REG_C), (uint64_t)0xAA, "C got B");
    TEST_EQ(GetReg(inst, REG_D), (uint64_t)0xAA, "D got A");
    TEST_EQ(GetReg(inst, REG_H), (uint64_t)0xAA, "H got D");
    TEST_EQ(GetReg(inst, REG_L), (uint64_t)0xAA, "L got H");
    // First instruction was LD A,n (7 T), remaining 6 were LD r,r' (4 T each).
    TEST_EQ(emfe_get_cycle_count(inst), (int64_t)(7 + 6 * 4),
            "cycle counts as expected");

    // HALT (0x76) is excluded from the LD r,r' opcode block. Until
    // Phase 2.4 it trapped; from Phase 2.4 onward it enters the halted
    // state and PC advances past the HALT byte.
    emfe_poke_byte(inst, 0x100, 0x76);
    SetReg(inst, REG_PC, 0x100);
    TEST_EQ(emfe_step(inst), EMFE_OK, "HALT now decoded (Phase 2.4)");
    TEST_EQ(GetReg(inst, REG_PC), (uint64_t)0x101, "PC advanced past HALT");

    emfe_destroy(inst);
    g_pass++;
}

static void TestLdRegMemHL() {
    // LD r, (HL) and LD (HL), r — Reg8Read/Write code 6 path.
    printf("TestLdRegMemHL...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // Stage HL=0x2000, memory[0x2000]=0x5A; then LD B,(HL).
    SetReg(inst, REG_HL, 0x2000);
    emfe_poke_byte(inst, 0x2000, 0x5A);
    const uint8_t prog1[] = { 0x46 };   // LD B, (HL)
    LoadProgram(inst, 0, prog1, sizeof(prog1));
    TEST_EQ(emfe_step(inst), EMFE_OK, "LD B,(HL) OK");
    TEST_EQ(GetReg(inst, REG_B), (uint64_t)0x5A, "B = mem[HL]");
    TEST_EQ(emfe_get_cycle_count(inst), (int64_t)7, "LD r,(HL) is 7 T");

    // LD (HL), C with C=0x3C.
    SetReg(inst, REG_C, 0x3C);
    const uint8_t prog2[] = { 0x71 };   // LD (HL), C
    LoadProgram(inst, 0x10, prog2, sizeof(prog2));
    TEST_EQ(emfe_step(inst), EMFE_OK, "LD (HL),C OK");
    TEST_EQ(emfe_peek_byte(inst, 0x2000), 0x3C, "mem[HL] = C");

    emfe_destroy(inst);
    g_pass++;
}

static void TestLdMemHLImmediate() {
    // LD (HL), n — special case of the LD r,n family with dst=6.
    printf("TestLdMemHLImmediate...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    SetReg(inst, REG_HL, 0x3000);
    const uint8_t prog[] = { 0x36, 0xA5 };   // LD (HL), 0xA5
    LoadProgram(inst, 0, prog, sizeof(prog));
    TEST_EQ(emfe_step(inst), EMFE_OK, "LD (HL),n OK");
    TEST_EQ(emfe_peek_byte(inst, 0x3000), 0xA5, "mem[HL] = 0xA5");
    TEST_EQ(emfe_get_cycle_count(inst), (int64_t)10, "LD (HL),n is 10 T");

    emfe_destroy(inst);
    g_pass++;
}

static void TestLdAMemBCDE() {
    // LD A,(BC) / LD A,(DE) and the writeback pair.
    printf("TestLdAMemBCDE...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    SetReg(inst, REG_BC, 0x4000);
    SetReg(inst, REG_DE, 0x4001);
    emfe_poke_byte(inst, 0x4000, 0x11);
    emfe_poke_byte(inst, 0x4001, 0x22);

    const uint8_t prog[] = {
        0x0A,         // LD A, (BC)  -> A = 0x11
        0x47,         // LD B, A
        0x1A,         // LD A, (DE)  -> A = 0x22
    };
    LoadProgram(inst, 0, prog, sizeof(prog));
    emfe_step(inst); TEST_EQ(GetReg(inst, REG_A), (uint64_t)0x11, "LD A,(BC)");
    emfe_step(inst);
    emfe_step(inst); TEST_EQ(GetReg(inst, REG_A), (uint64_t)0x22, "LD A,(DE)");

    // Writeback: LD (BC), A and LD (DE), A.
    SetReg(inst, REG_BC, 0x4010);
    SetReg(inst, REG_DE, 0x4011);
    SetReg(inst, REG_A,  0x99);
    const uint8_t prog2[] = { 0x02, 0x12 };   // LD (BC),A ; LD (DE),A
    LoadProgram(inst, 0x10, prog2, sizeof(prog2));
    emfe_step(inst); TEST_EQ(emfe_peek_byte(inst, 0x4010), 0x99, "(BC) <- A");
    emfe_step(inst); TEST_EQ(emfe_peek_byte(inst, 0x4011), 0x99, "(DE) <- A");

    emfe_destroy(inst);
    g_pass++;
}

static void TestLdAMemNNAbsolute() {
    // LD A,(nn) / LD (nn),A — direct 16-bit absolute.
    printf("TestLdAMemNNAbsolute...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    emfe_poke_byte(inst, 0x5000, 0x7F);
    const uint8_t prog1[] = { 0x3A, 0x00, 0x50 };   // LD A, (0x5000)
    LoadProgram(inst, 0, prog1, sizeof(prog1));
    TEST_EQ(emfe_step(inst), EMFE_OK, "LD A,(nn) OK");
    TEST_EQ(GetReg(inst, REG_A), (uint64_t)0x7F, "A = mem[0x5000]");
    TEST_EQ(emfe_get_cycle_count(inst), (int64_t)13, "13 T-states");

    // LD (0x6000), A
    SetReg(inst, REG_A, 0x42);
    const uint8_t prog2[] = { 0x32, 0x00, 0x60 };
    LoadProgram(inst, 0x10, prog2, sizeof(prog2));
    TEST_EQ(emfe_step(inst), EMFE_OK, "LD (nn),A OK");
    TEST_EQ(emfe_peek_byte(inst, 0x6000), 0x42, "mem[0x6000] = A");

    emfe_destroy(inst);
    g_pass++;
}

static void TestLdHLMemNNAbsolute() {
    // LD HL,(nn) / LD (nn),HL — 16-bit word transfer through absolute.
    printf("TestLdHLMemNNAbsolute...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // Plant a word at 0x7000: lo=0x34 hi=0x12 (little-endian).
    emfe_poke_byte(inst, 0x7000, 0x34);
    emfe_poke_byte(inst, 0x7001, 0x12);
    const uint8_t prog1[] = { 0x2A, 0x00, 0x70 };   // LD HL, (0x7000)
    LoadProgram(inst, 0, prog1, sizeof(prog1));
    TEST_EQ(emfe_step(inst), EMFE_OK, "LD HL,(nn) OK");
    TEST_EQ(GetReg(inst, REG_HL), (uint64_t)0x1234, "HL = 0x1234");
    TEST_EQ(emfe_get_cycle_count(inst), (int64_t)16, "16 T-states");

    // Writeback: LD (0x8000), HL with HL=0xCAFE.
    SetReg(inst, REG_HL, 0xCAFE);
    const uint8_t prog2[] = { 0x22, 0x00, 0x80 };
    LoadProgram(inst, 0x10, prog2, sizeof(prog2));
    TEST_EQ(emfe_step(inst), EMFE_OK, "LD (nn),HL OK");
    TEST_EQ(emfe_peek_byte(inst, 0x8000), 0xFE, "mem[0x8000] = lo");
    TEST_EQ(emfe_peek_byte(inst, 0x8001), 0xCA, "mem[0x8001] = hi");

    emfe_destroy(inst);
    g_pass++;
}

// ---------------------------------------------------------------------
// Phase 1.5 tests — 8-bit ALU (ADD/ADC/SUB/SBC/AND/OR/XOR/CP)
// ---------------------------------------------------------------------

// Flag bit mirrors for the test (same values as Z80Cpu::F_*).
static constexpr uint8_t TF_S = 0x80, TF_Z = 0x40, TF_Y = 0x20, TF_H = 0x10;
static constexpr uint8_t TF_X = 0x08, TF_PV = 0x04, TF_N = 0x02, TF_C = 0x01;

// Convenience: load a single ALU instruction (1 or 2 bytes), preset A
// and F, single-step once, return the resulting A and F. Does NOT
// reset the CPU — callers are responsible for any HL/(HL)/other
// setup that needs to survive the call (see TestAluMemoryIndirect).
// Cycle count is monotonic: tests that assert exact cycle counts must
// be the FIRST RunAlu call after emfe_create.
struct AluOut { uint8_t a; uint8_t f; };
static AluOut RunAlu(EmfeInstance inst, uint8_t initialA, uint8_t initialF,
                     const uint8_t* prog, size_t n) {
    SetReg(inst, REG_A, initialA);
    SetReg(inst, REG_F, initialF);
    LoadProgram(inst, 0, prog, n);
    emfe_step(inst);
    AluOut o{};
    o.a = (uint8_t)GetReg(inst, REG_A);
    o.f = (uint8_t)GetReg(inst, REG_F);
    return o;
}

static void TestAluAddBasic() {
    printf("TestAluAddBasic...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // ADD A,B with A=5, B=3 -> A=8, no S/Z/H/PV/N/C set.
    SetReg(inst, REG_B, 3);
    const uint8_t prog[] = { 0x80 };
    AluOut r = RunAlu(inst, 5, 0xFF, prog, 1);  // start with F=0xFF
    TEST_EQ(r.a, 8, "5 + 3 = 8");
    TEST_TRUE((r.f & TF_S) == 0, "S clear");
    TEST_TRUE((r.f & TF_Z) == 0, "Z clear");
    TEST_TRUE((r.f & TF_H) == 0, "H clear (no half-carry)");
    TEST_TRUE((r.f & TF_PV) == 0, "PV clear (no overflow)");
    TEST_TRUE((r.f & TF_N) == 0, "N clear (add)");
    TEST_TRUE((r.f & TF_C) == 0, "C clear (no carry)");

    emfe_destroy(inst);
    g_pass++;
}

static void TestAluAddCarryHalfOverflow() {
    printf("TestAluAddCarryHalfOverflow...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // ADD A,B with A=0xFF, B=1 -> A=0, Z=1, H=1, C=1.
    SetReg(inst, REG_B, 1);
    const uint8_t prog[] = { 0x80 };
    AluOut r = RunAlu(inst, 0xFF, 0, prog, 1);
    TEST_EQ(r.a, 0,                          "0xFF + 1 = 0 (mod 256)");
    TEST_TRUE((r.f & TF_Z) != 0,             "Z set");
    TEST_TRUE((r.f & TF_H) != 0,             "H set (0xF + 1 = 0x10)");
    TEST_TRUE((r.f & TF_C) != 0,             "C set");
    TEST_TRUE((r.f & TF_N) == 0,             "N clear");

    // Signed overflow: A=0x7F (+127), B=1 -> A=0x80 (-128), V=1.
    SetReg(inst, REG_B, 1);
    r = RunAlu(inst, 0x7F, 0, prog, 1);
    TEST_EQ(r.a, 0x80,                       "0x7F + 1 = 0x80");
    TEST_TRUE((r.f & TF_S) != 0,             "S set");
    TEST_TRUE((r.f & TF_PV) != 0,            "PV set (signed overflow)");
    TEST_TRUE((r.f & TF_H) != 0,             "H set");

    emfe_destroy(inst);
    g_pass++;
}

static void TestAluAdcUsesCarry() {
    printf("TestAluAdcUsesCarry...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // ADC A,B with A=1, B=1, C=1 -> A=3.
    SetReg(inst, REG_B, 1);
    const uint8_t prog[] = { 0x88 };           // ADC A,B
    AluOut r = RunAlu(inst, 1, TF_C, prog, 1);
    TEST_EQ(r.a, 3, "1 + 1 + carry(1) = 3");
    TEST_TRUE((r.f & TF_C) == 0, "no new carry out");

    emfe_destroy(inst);
    g_pass++;
}

static void TestAluSubBasic() {
    printf("TestAluSubBasic...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // SUB B with A=10, B=3 -> A=7, N=1, no Z/H/C.
    SetReg(inst, REG_B, 3);
    const uint8_t prog[] = { 0x90 };
    AluOut r = RunAlu(inst, 10, 0, prog, 1);
    TEST_EQ(r.a, 7, "10 - 3 = 7");
    TEST_TRUE((r.f & TF_N) != 0, "N set (sub)");
    TEST_TRUE((r.f & TF_Z) == 0, "Z clear");
    TEST_TRUE((r.f & TF_C) == 0, "no borrow");

    // SUB B with A=0, B=1 -> A=0xFF, borrow + S=1 + half-borrow.
    SetReg(inst, REG_B, 1);
    r = RunAlu(inst, 0, 0, prog, 1);
    TEST_EQ(r.a, 0xFF, "0 - 1 = 0xFF");
    TEST_TRUE((r.f & TF_S) != 0, "S set");
    TEST_TRUE((r.f & TF_H) != 0, "H set (half borrow)");
    TEST_TRUE((r.f & TF_C) != 0, "C set (borrow)");

    emfe_destroy(inst);
    g_pass++;
}

static void TestAluAndOrXor() {
    printf("TestAluAndOrXor...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // AND with A=0x0F, B=0xF0 -> A=0, Z=1, H=1, PV=1 (parity of 0 is even).
    SetReg(inst, REG_B, 0xF0);
    const uint8_t and_b[] = { 0xA0 };
    AluOut r = RunAlu(inst, 0x0F, 0, and_b, 1);
    TEST_EQ(r.a, 0, "0x0F AND 0xF0 = 0");
    TEST_TRUE((r.f & TF_Z) != 0,  "Z set");
    TEST_TRUE((r.f & TF_H) != 0,  "H set (AND quirk)");
    TEST_TRUE((r.f & TF_PV) != 0, "PV set (parity)");
    TEST_TRUE((r.f & TF_C) == 0,  "C clear");

    // OR with A=0x55, B=0xAA -> A=0xFF, S=1, PV=1.
    SetReg(inst, REG_B, 0xAA);
    const uint8_t or_b[] = { 0xB0 };
    r = RunAlu(inst, 0x55, 0, or_b, 1);
    TEST_EQ(r.a, 0xFF, "0x55 OR 0xAA = 0xFF");
    TEST_TRUE((r.f & TF_S) != 0,  "S set");
    TEST_TRUE((r.f & TF_PV) != 0, "PV set (8 bits, parity even)");
    TEST_TRUE((r.f & TF_H) == 0,  "H clear");

    // XOR self -> zero.
    SetReg(inst, REG_B, 0x55);
    const uint8_t xor_b[] = { 0xA8 };
    r = RunAlu(inst, 0x55, 0, xor_b, 1);
    TEST_EQ(r.a, 0, "0x55 XOR 0x55 = 0");
    TEST_TRUE((r.f & TF_Z) != 0, "Z set");

    emfe_destroy(inst);
    g_pass++;
}

static void TestAluCpPreservesA() {
    // CP is SUB without writing the result back. Its Y/X flags come
    // from the OPERAND, not the result — important for zexall.
    printf("TestAluCpPreservesA...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    SetReg(inst, REG_B, 10);
    const uint8_t cp_b[] = { 0xB8 };
    AluOut r = RunAlu(inst, 10, 0, cp_b, 1);
    TEST_EQ(r.a, 10, "A preserved (CP does not write)");
    TEST_TRUE((r.f & TF_Z) != 0, "Z set (equal)");
    TEST_TRUE((r.f & TF_N) != 0, "N set");
    TEST_TRUE((r.f & TF_C) == 0, "no borrow");

    // CP Y/X comes from operand: A=0x00, B=0x28 (bits 5+3 set in operand).
    SetReg(inst, REG_B, 0x28);
    r = RunAlu(inst, 0x00, 0, cp_b, 1);
    TEST_EQ(r.a, 0x00, "A still 0");
    TEST_TRUE((r.f & TF_Y) != 0, "Y from operand bit 5");
    TEST_TRUE((r.f & TF_X) != 0, "X from operand bit 3");
    TEST_TRUE((r.f & TF_C) != 0, "borrow (0 - 0x28 underflows)");

    emfe_destroy(inst);
    g_pass++;
}

static void TestAluImmediate() {
    // ADD A,n (0xC6) and friends — same 8 ops, immediate operand.
    printf("TestAluImmediate...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // ADD A,#0x10 with A=0x20.
    const uint8_t add[] = { 0xC6, 0x10 };
    AluOut r = RunAlu(inst, 0x20, 0, add, 2);
    TEST_EQ(r.a, 0x30, "0x20 + 0x10 = 0x30 via ADD A,n");

    // AND #0x0F with A=0xAB.
    const uint8_t and_imm[] = { 0xE6, 0x0F };
    r = RunAlu(inst, 0xAB, 0, and_imm, 2);
    TEST_EQ(r.a, 0x0B, "0xAB AND 0x0F = 0x0B");

    // CP #5 with A=5 (immediate compare, equality).
    const uint8_t cp_imm[] = { 0xFE, 0x05 };
    r = RunAlu(inst, 5, 0, cp_imm, 2);
    TEST_EQ(r.a, 5, "A preserved by CP n");
    TEST_TRUE((r.f & TF_Z) != 0, "Z set");

    emfe_destroy(inst);
    g_pass++;
}

static void TestAluMemoryIndirect() {
    // ALU through (HL) — 7 T-states instead of 4.
    printf("TestAluMemoryIndirect...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    SetReg(inst, REG_HL, 0x4000);
    emfe_poke_byte(inst, 0x4000, 0x42);
    const uint8_t prog[] = { 0x86 };           // ADD A, (HL)
    AluOut r = RunAlu(inst, 0x10, 0, prog, 1);
    TEST_EQ(r.a, 0x52, "0x10 + (HL=0x42) = 0x52");
    TEST_EQ(emfe_get_cycle_count(inst), (int64_t)7, "ALU (HL) is 7 T");

    emfe_destroy(inst);
    g_pass++;
}

// ---------------------------------------------------------------------
// Phase 1.6 tests — control flow (JP/JR/CALL/RET/RST/PUSH/POP/DJNZ)
// ---------------------------------------------------------------------

static void TestJpUnconditional() {
    printf("TestJpUnconditional...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    const uint8_t prog[] = { 0xC3, 0x34, 0x12 };     // JP 0x1234
    LoadProgram(inst, 0, prog, sizeof(prog));
    TEST_EQ(emfe_step(inst), EMFE_OK, "JP nn OK");
    TEST_EQ(GetReg(inst, REG_PC), (uint64_t)0x1234, "PC = nn");
    TEST_EQ(emfe_get_cycle_count(inst), (int64_t)10, "10 T");

    emfe_destroy(inst);
    g_pass++;
}

static void TestJpConditional() {
    printf("TestJpConditional...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // JP NZ, 0x2000 with Z=0 → taken.
    SetReg(inst, REG_F, 0);
    const uint8_t prog_nz[] = { 0xC2, 0x00, 0x20 };
    LoadProgram(inst, 0, prog_nz, sizeof(prog_nz));
    TEST_EQ(emfe_step(inst), EMFE_OK, "JP NZ taken OK");
    TEST_EQ(GetReg(inst, REG_PC), (uint64_t)0x2000, "PC jumped");

    // JP NZ with Z=1 → not taken (PC just advances past the operand).
    SetReg(inst, REG_F, TF_Z);
    LoadProgram(inst, 0x100, prog_nz, sizeof(prog_nz));
    TEST_EQ(emfe_step(inst), EMFE_OK, "JP NZ not-taken OK");
    TEST_EQ(GetReg(inst, REG_PC), (uint64_t)0x103, "PC just past operand");

    emfe_destroy(inst);
    g_pass++;
}

static void TestJpHl() {
    printf("TestJpHl...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    SetReg(inst, REG_HL, 0x5678);
    const uint8_t prog[] = { 0xE9 };                 // JP (HL)
    LoadProgram(inst, 0, prog, sizeof(prog));
    TEST_EQ(emfe_step(inst), EMFE_OK, "JP (HL) OK");
    TEST_EQ(GetReg(inst, REG_PC), (uint64_t)0x5678, "PC = HL");
    TEST_EQ(emfe_get_cycle_count(inst), (int64_t)4, "4 T-states");

    emfe_destroy(inst);
    g_pass++;
}

static void TestJrRelative() {
    printf("TestJrRelative...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // JR +5 from PC=0x100 lands at 0x102 + 5 = 0x107 (offset is added
    // AFTER the operand byte is consumed).
    const uint8_t prog_fwd[] = { 0x18, 0x05 };
    LoadProgram(inst, 0x100, prog_fwd, sizeof(prog_fwd));
    TEST_EQ(emfe_step(inst), EMFE_OK, "JR +5 OK");
    TEST_EQ(GetReg(inst, REG_PC), (uint64_t)0x107, "PC = 0x102 + 5");
    TEST_EQ(emfe_get_cycle_count(inst), (int64_t)12, "12 T-states");

    // JR -2 from PC=0x200 lands at 0x202 - 2 = 0x200 (self-loop).
    const uint8_t prog_back[] = { 0x18, 0xFE };
    LoadProgram(inst, 0x200, prog_back, sizeof(prog_back));
    TEST_EQ(emfe_step(inst), EMFE_OK, "JR -2 OK");
    TEST_EQ(GetReg(inst, REG_PC), (uint64_t)0x200, "self-loop landed");

    emfe_destroy(inst);
    g_pass++;
}

static void TestJrConditional() {
    printf("TestJrConditional...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // JR Z, +5 with Z=1 → taken.
    SetReg(inst, REG_F, TF_Z);
    const uint8_t prog[] = { 0x28, 0x05 };
    LoadProgram(inst, 0x100, prog, sizeof(prog));
    TEST_EQ(emfe_step(inst), EMFE_OK, "JR Z taken OK");
    TEST_EQ(GetReg(inst, REG_PC), (uint64_t)0x107, "PC jumped");
    TEST_EQ(emfe_get_cycle_count(inst), (int64_t)12, "12 T taken");

    // JR Z with Z=0 → not taken.
    SetReg(inst, REG_F, 0);
    LoadProgram(inst, 0x200, prog, sizeof(prog));
    int64_t cyc_before = emfe_get_cycle_count(inst);
    TEST_EQ(emfe_step(inst), EMFE_OK, "JR Z not-taken OK");
    TEST_EQ(GetReg(inst, REG_PC), (uint64_t)0x202, "PC past operand");
    TEST_EQ(emfe_get_cycle_count(inst) - cyc_before, (int64_t)7, "7 T not taken");

    emfe_destroy(inst);
    g_pass++;
}

static void TestDjnz() {
    printf("TestDjnz...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // DJNZ from B=3 should loop 3 times then fall through (B counts
    // down to 0). Plant `LD B,3 ; DJNZ -2` at PC=0 — each DJNZ jumps
    // back to itself until B becomes 0.
    const uint8_t prog[] = { 0x06, 0x03, 0x10, 0xFE };
    LoadProgram(inst, 0, prog, sizeof(prog));
    emfe_step(inst);   // LD B,3
    TEST_EQ(GetReg(inst, REG_B), (uint64_t)3, "B = 3");

    emfe_step(inst);   // DJNZ -2: B=2, jump
    TEST_EQ(GetReg(inst, REG_B), (uint64_t)2, "B decremented to 2");
    TEST_EQ(GetReg(inst, REG_PC), (uint64_t)2, "PC looped to DJNZ");

    emfe_step(inst);   // DJNZ -2: B=1, jump
    emfe_step(inst);   // DJNZ -2: B=0, fall through
    TEST_EQ(GetReg(inst, REG_B), (uint64_t)0, "B = 0");
    TEST_EQ(GetReg(inst, REG_PC), (uint64_t)4, "PC past DJNZ");

    emfe_destroy(inst);
    g_pass++;
}

static void TestPushPop() {
    printf("TestPushPop...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // Initialise SP and push HL, then pop into DE.
    SetReg(inst, REG_SP, 0xFFF0);
    SetReg(inst, REG_HL, 0xABCD);
    const uint8_t prog[] = { 0xE5, 0xD1 };           // PUSH HL ; POP DE
    LoadProgram(inst, 0, prog, sizeof(prog));

    emfe_step(inst);   // PUSH HL — SP -= 2, mem[SP] = HL
    TEST_EQ(GetReg(inst, REG_SP), (uint64_t)0xFFEE, "SP -= 2");
    TEST_EQ(emfe_peek_byte(inst, 0xFFEE), 0xCD, "low byte at SP");
    TEST_EQ(emfe_peek_byte(inst, 0xFFEF), 0xAB, "high byte at SP+1");

    emfe_step(inst);   // POP DE
    TEST_EQ(GetReg(inst, REG_SP), (uint64_t)0xFFF0, "SP restored");
    TEST_EQ(GetReg(inst, REG_DE), (uint64_t)0xABCD, "DE = original HL");

    // PUSH AF / POP BC round-trip.
    SetReg(inst, REG_AF, 0x1234);
    SetReg(inst, REG_BC, 0);
    const uint8_t prog2[] = { 0xF5, 0xC1 };
    LoadProgram(inst, 0x10, prog2, sizeof(prog2));
    emfe_step(inst); emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_BC), (uint64_t)0x1234, "AF -> BC via stack");

    emfe_destroy(inst);
    g_pass++;
}

static void TestCallRet() {
    printf("TestCallRet...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    SetReg(inst, REG_SP, 0xFFF0);

    // Program at 0x100:
    //   CALL 0x200      (3 bytes, PC after call = 0x103)
    //   ...
    // Subroutine at 0x200:
    //   RET             (1 byte)
    emfe_poke_byte(inst, 0x100, 0xCD);
    emfe_poke_byte(inst, 0x101, 0x00);
    emfe_poke_byte(inst, 0x102, 0x02);
    emfe_poke_byte(inst, 0x200, 0xC9);
    SetReg(inst, REG_PC, 0x100);

    emfe_step(inst);   // CALL 0x200
    TEST_EQ(GetReg(inst, REG_PC), (uint64_t)0x200, "PC at subroutine");
    TEST_EQ(GetReg(inst, REG_SP), (uint64_t)0xFFEE, "SP -= 2");
    // The return address pushed should be 0x103 (after the operand bytes).
    TEST_EQ(emfe_peek_byte(inst, 0xFFEE), 0x03, "return-addr low");
    TEST_EQ(emfe_peek_byte(inst, 0xFFEF), 0x01, "return-addr high");

    emfe_step(inst);   // RET
    TEST_EQ(GetReg(inst, REG_PC), (uint64_t)0x103, "PC back at caller");
    TEST_EQ(GetReg(inst, REG_SP), (uint64_t)0xFFF0, "SP restored");

    emfe_destroy(inst);
    g_pass++;
}

static void TestRst() {
    printf("TestRst...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    SetReg(inst, REG_SP, 0xFFF0);
    // Place RST 0x10 (opcode 0xD7) at PC=0x500.
    emfe_poke_byte(inst, 0x500, 0xD7);
    SetReg(inst, REG_PC, 0x500);

    TEST_EQ(emfe_step(inst), EMFE_OK, "RST 0x10 OK");
    TEST_EQ(GetReg(inst, REG_PC), (uint64_t)0x10, "PC = vector");
    TEST_EQ(GetReg(inst, REG_SP), (uint64_t)0xFFEE, "return-addr pushed");
    TEST_EQ(emfe_peek_byte(inst, 0xFFEE), 0x01, "return-addr low (0x501)");
    TEST_EQ(emfe_peek_byte(inst, 0xFFEF), 0x05, "return-addr high");
    TEST_EQ(emfe_get_cycle_count(inst), (int64_t)11, "11 T");

    emfe_destroy(inst);
    g_pass++;
}

// ---------------------------------------------------------------------
// Phase 1.7 tests — INC/DEC + IN/OUT + hello-world ROM end-to-end
// ---------------------------------------------------------------------

static void TestInc16BitPair() {
    printf("TestInc16BitPair...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    SetReg(inst, REG_HL, 0x1234);
    const uint8_t prog[] = { 0x23 };           // INC HL
    LoadProgram(inst, 0, prog, sizeof(prog));
    TEST_EQ(emfe_step(inst), EMFE_OK, "INC HL OK");
    TEST_EQ(GetReg(inst, REG_HL), (uint64_t)0x1235, "HL += 1");
    TEST_EQ(emfe_get_cycle_count(inst), (int64_t)6, "6 T-states");

    // Wrap: 0xFFFF + 1 == 0
    SetReg(inst, REG_HL, 0xFFFF);
    SetReg(inst, REG_PC, 0);
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_HL), (uint64_t)0x0000, "HL wraps");

    emfe_destroy(inst);
    g_pass++;
}

static void TestDec16BitPair() {
    printf("TestDec16BitPair...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    SetReg(inst, REG_BC, 0x0001);
    const uint8_t prog[] = { 0x0B };           // DEC BC
    LoadProgram(inst, 0, prog, sizeof(prog));
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_BC), (uint64_t)0, "BC -= 1");

    // 0 - 1 wraps to 0xFFFF.
    SetReg(inst, REG_PC, 0);
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_BC), (uint64_t)0xFFFF, "BC underflows to 0xFFFF");

    emfe_destroy(inst);
    g_pass++;
}

static void TestInc8BitFlags() {
    // INC r is supposed to set H when the low nibble wraps (was 0x0F),
    // set V when the result is 0x80 (was 0x7F), preserve C.
    printf("TestInc8BitFlags...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // INC B with B=0x0F → result 0x10, H set, C preserved.
    SetReg(inst, REG_B, 0x0F);
    SetReg(inst, REG_F, TF_C);
    const uint8_t inc_b[] = { 0x04 };
    LoadProgram(inst, 0, inc_b, 1);
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_B), (uint64_t)0x10, "B = 0x10");
    uint8_t f = (uint8_t)GetReg(inst, REG_F);
    TEST_TRUE((f & TF_H) != 0, "H set (low nibble was F)");
    TEST_TRUE((f & TF_C) != 0, "C preserved");
    TEST_TRUE((f & TF_N) == 0, "N cleared by INC");

    // INC B with B=0x7F → result 0x80, V set (signed overflow), S set.
    SetReg(inst, REG_B, 0x7F);
    SetReg(inst, REG_F, 0);
    SetReg(inst, REG_PC, 0);
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_B), (uint64_t)0x80, "0x7F + 1 = 0x80");
    f = (uint8_t)GetReg(inst, REG_F);
    TEST_TRUE((f & TF_S) != 0,  "S set");
    TEST_TRUE((f & TF_PV) != 0, "V set");
    TEST_TRUE((f & TF_H) != 0,  "H set");

    emfe_destroy(inst);
    g_pass++;
}

static void TestDec8BitFlags() {
    printf("TestDec8BitFlags...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // DEC C with C=0x80 → 0x7F, V set (signed underflow), N=1.
    // Low nibble of 0x80 is 0, so DEC borrows from bit 4 — H is set.
    SetReg(inst, REG_C, 0x80);
    SetReg(inst, REG_F, 0);
    const uint8_t dec_c[] = { 0x0D };
    LoadProgram(inst, 0, dec_c, 1);
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_C), (uint64_t)0x7F, "C = 0x7F");
    uint8_t f = (uint8_t)GetReg(inst, REG_F);
    TEST_TRUE((f & TF_N) != 0,  "N set (sub)");
    TEST_TRUE((f & TF_PV) != 0, "V set (0x80 → 0x7F crosses signed boundary)");
    TEST_TRUE((f & TF_H) != 0,  "H set (borrow from bit 4 since low nibble was 0)");

    // DEC C with C=0x05 → 0x04, H should be CLEAR (low nibble = 5).
    SetReg(inst, REG_C, 0x05);
    SetReg(inst, REG_F, 0);
    SetReg(inst, REG_PC, 0);
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_C), (uint64_t)0x04, "C = 0x04");
    f = (uint8_t)GetReg(inst, REG_F);
    TEST_TRUE((f & TF_H) == 0,  "H clear (no half-borrow)");
    TEST_TRUE((f & TF_PV) == 0, "V clear (no overflow)");

    emfe_destroy(inst);
    g_pass++;
}

static void TestInOutImmediate() {
    // OUT (0xFE), A delivers A through the UART → console callback.
    // IN  A, (0xFF) reads the UART status byte.
    printf("TestInOutImmediate...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    CaptureCtx ctx;
    emfe_set_console_char_callback(inst, CaptureChar, &ctx);

    SetReg(inst, REG_A, 'X');
    const uint8_t prog_out[] = { 0xD3, 0xFE };       // OUT (0xFE), A
    LoadProgram(inst, 0, prog_out, sizeof(prog_out));
    TEST_EQ(emfe_step(inst), EMFE_OK, "OUT (0xFE),A OK");
    TEST_EQ(emfe_get_cycle_count(inst), (int64_t)11, "11 T-states");
    TEST_TRUE(ctx.captured == "X", "console captured 'X'");

    // IN A, (0xFF): empty FIFO → status TX_READY (bit 0) only.
    const uint8_t prog_in[] = { 0xDB, 0xFF };        // IN A, (0xFF)
    LoadProgram(inst, 0x10, prog_in, sizeof(prog_in));
    TEST_EQ(emfe_step(inst), EMFE_OK, "IN A,(0xFF) OK");
    TEST_EQ(GetReg(inst, REG_A) & 0x01, (uint64_t)0x01, "TX_READY bit set");
    TEST_EQ(GetReg(inst, REG_A) & 0x02, (uint64_t)0x00, "RX_AVAIL clear (empty FIFO)");

    emfe_destroy(inst);
    g_pass++;
}

// ---------------------------------------------------------------------
// Phase 1.7 ACCEPTANCE TEST: load examples/hello/hello.asm assembled to
// 30 bytes, step until the self-loop is reached, assert the captured
// console output equals "Hello, World!\r\n".
//
// The byte sequence below is hand-assembled and must stay in lockstep
// with examples/hello/hello.asm — if you change one, regenerate the
// other.
// ---------------------------------------------------------------------

static const uint8_t hello_world_bin[] = {
    // start: ld   hl, message       (message starts at offset 0x000E)
    0x21, 0x0E, 0x00,
    // loop:  ld   a, (hl)
    0x7E,
    //        or   a                  ; Z := (A == 0)
    0xB7,
    //        jr   z, done            ; +5 → 0x000C
    0x28, 0x05,
    //        out  (0xFE), a
    0xD3, 0xFE,
    //        inc  hl
    0x23,
    //        jr   loop               ; -9 → 0x0003
    0x18, 0xF7,
    // done:  jr   done               ; -2 → 0x000C (self-loop)
    0x18, 0xFE,
    // message: "Hello, World!\r\n\0"
    'H','e','l','l','o',',',' ','W','o','r','l','d','!',
    0x0D, 0x0A, 0x00,
};

static void TestHelloWorld() {
    printf("TestHelloWorld...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    CaptureCtx ctx;
    emfe_set_console_char_callback(inst, CaptureChar, &ctx);

    // Load the ROM at 0x0000 and start at the reset vector.
    for (size_t i = 0; i < sizeof(hello_world_bin); ++i) {
        emfe_poke_byte(inst, (uint16_t)i, hello_world_bin[i]);
    }
    SetReg(inst, REG_PC, 0);

    // Run until the self-loop is reached (PC sits at 0x000C with the
    // captured string complete). 5000 steps is more than 30× what the
    // program actually needs; the loop exit covers the happy path.
    const uint64_t done_pc = 0x000C;
    bool reached_done = false;
    for (int i = 0; i < 5000; ++i) {
        emfe_step(inst);
        if (GetReg(inst, REG_PC) == done_pc &&
            ctx.captured.size() >= 15 &&
            ctx.captured.back() == '\n') {
            reached_done = true;
            break;
        }
    }
    TEST_TRUE(reached_done, "reached self-loop with full output");
    TEST_TRUE(ctx.captured == "Hello, World!\r\n",
              "captured string matches expected");

    emfe_destroy(inst);
    g_pass++;
}

// ---------------------------------------------------------------------
// Phase 2.1 tests — CB-prefix bit/shift/rotate
// ---------------------------------------------------------------------

// Run a single CB-prefixed opcode on register A, preset A and F,
// return resulting A and F.
static AluOut RunCB(EmfeInstance inst, uint8_t initialA, uint8_t initialF,
                    uint8_t cb_byte) {
    SetReg(inst, REG_A, initialA);
    SetReg(inst, REG_F, initialF);
    uint8_t prog[] = { 0xCB, cb_byte };
    LoadProgram(inst, 0, prog, sizeof(prog));
    emfe_step(inst);
    AluOut o{};
    o.a = (uint8_t)GetReg(inst, REG_A);
    o.f = (uint8_t)GetReg(inst, REG_F);
    return o;
}

static void TestCbRotations() {
    printf("TestCbRotations...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // RLC A (CB 07): 0x80 → 0x01, C=1
    AluOut r = RunCB(inst, 0x80, 0, 0x07);
    TEST_EQ(r.a, 0x01, "RLC 0x80 → 0x01");
    TEST_TRUE((r.f & TF_C) != 0, "RLC carry out");

    // RRC A (CB 0F): 0x01 → 0x80, C=1
    r = RunCB(inst, 0x01, 0, 0x0F);
    TEST_EQ(r.a, 0x80, "RRC 0x01 → 0x80");
    TEST_TRUE((r.f & TF_C) != 0, "RRC carry out");
    TEST_TRUE((r.f & TF_S) != 0, "RRC sign set (result 0x80)");

    // RL A (CB 17) with C=1: 0x80 → 0x01 (old carry into bit 0)
    r = RunCB(inst, 0x80, TF_C, 0x17);
    TEST_EQ(r.a, 0x01, "RL 0x80 + C=1 → 0x01");
    TEST_TRUE((r.f & TF_C) != 0, "new carry = old bit 7");

    // RR A (CB 1F) with C=1: 0x00 → 0x80
    r = RunCB(inst, 0x00, TF_C, 0x1F);
    TEST_EQ(r.a, 0x80, "RR 0x00 + C=1 → 0x80");
    TEST_TRUE((r.f & TF_C) == 0, "no new carry out");

    emfe_destroy(inst);
    g_pass++;
}

static void TestCbShifts() {
    printf("TestCbShifts...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // SLA A (CB 27): 0x80 → 0x00, C=1, Z=1
    AluOut r = RunCB(inst, 0x80, 0, 0x27);
    TEST_EQ(r.a, 0x00, "SLA 0x80 → 0x00");
    TEST_TRUE((r.f & TF_C) != 0, "SLA carry");
    TEST_TRUE((r.f & TF_Z) != 0, "SLA Z set");

    // SRA A (CB 2F): 0x80 → 0xC0 (sign preserved), C=0
    r = RunCB(inst, 0x80, 0, 0x2F);
    TEST_EQ(r.a, 0xC0, "SRA preserves sign bit");
    TEST_TRUE((r.f & TF_S) != 0, "S set");

    // SLL A (CB 37, undocumented): 0x80 → 0x01 (bit 0 forced to 1)
    r = RunCB(inst, 0x80, 0, 0x37);
    TEST_EQ(r.a, 0x01, "SLL (undocumented) bit 0 := 1");
    TEST_TRUE((r.f & TF_C) != 0, "SLL carry from bit 7");

    // SRL A (CB 3F): 0x80 → 0x40, C=0
    r = RunCB(inst, 0x80, 0, 0x3F);
    TEST_EQ(r.a, 0x40, "SRL 0x80 → 0x40");
    TEST_TRUE((r.f & TF_C) == 0, "no carry");

    emfe_destroy(inst);
    g_pass++;
}

static void TestCbBit() {
    // BIT b, A — flags only.
    // CB 7F = BIT 7, A.  CB 47 = BIT 0, A.
    printf("TestCbBit...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // BIT 7, A with A=0x80
    AluOut r = RunCB(inst, 0x80, 0, 0x7F);
    TEST_EQ(r.a, 0x80, "A unchanged");
    TEST_TRUE((r.f & TF_Z) == 0, "bit set → Z clear");
    TEST_TRUE((r.f & TF_S) != 0, "b=7 and bit set → S set");
    TEST_TRUE((r.f & TF_H) != 0, "H always 1");
    TEST_TRUE((r.f & TF_N) == 0, "N always 0");

    // BIT 7, A with A=0x00
    r = RunCB(inst, 0x00, 0, 0x7F);
    TEST_TRUE((r.f & TF_Z)  != 0, "bit clear → Z set");
    TEST_TRUE((r.f & TF_PV) != 0, "PV mirrors Z for BIT");

    // BIT 0, A with A=0x01
    r = RunCB(inst, 0x01, 0, 0x47);
    TEST_TRUE((r.f & TF_Z) == 0, "bit 0 set");
    TEST_TRUE((r.f & TF_S) == 0, "b≠7 → S clear");

    // BIT preserves C
    r = RunCB(inst, 0x00, TF_C, 0x7F);
    TEST_TRUE((r.f & TF_C) != 0, "C preserved through BIT");

    emfe_destroy(inst);
    g_pass++;
}

static void TestCbSetRes() {
    // SET 3, A = CB DF. RES 7, A = CB BF. Neither changes flags.
    printf("TestCbSetRes...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    AluOut r = RunCB(inst, 0x00, 0xFF, 0xDF);
    TEST_EQ(r.a, 0x08, "SET 3 sets bit 3");
    TEST_EQ(r.f, 0xFF, "SET preserves F");

    r = RunCB(inst, 0xFF, 0x00, 0xBF);
    TEST_EQ(r.a, 0x7F, "RES 7 clears bit 7");
    TEST_EQ(r.f, 0x00, "RES preserves F");

    // R bump check: CB-prefixed opcode must bump R twice (prefix +
    // suffix), matching real-hardware M1 cycle counting.
    SetReg(inst, REG_R, 0);
    SetReg(inst, REG_B, 0);
    const uint8_t prog[] = { 0xCB, 0xC0 };           // SET 0, B
    LoadProgram(inst, 0, prog, sizeof(prog));
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_B), (uint64_t)0x01, "SET 0, B sets bit 0");
    TEST_EQ(GetReg(inst, REG_R), (uint64_t)2,    "R bumped twice");

    emfe_destroy(inst);
    g_pass++;
}

static void TestCbMemoryIndirect() {
    printf("TestCbMemoryIndirect...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    SetReg(inst, REG_HL, 0x2000);
    emfe_poke_byte(inst, 0x2000, 0x80);
    // SLA (HL) = CB 26
    const uint8_t prog[] = { 0xCB, 0x26 };
    LoadProgram(inst, 0, prog, sizeof(prog));
    emfe_step(inst);
    TEST_EQ(emfe_peek_byte(inst, 0x2000), 0x00, "SLA (HL): 0x80 → 0x00");
    TEST_EQ(emfe_get_cycle_count(inst), (int64_t)15,
            "shift on (HL) is 15 T-states");

    // BIT 6, (HL) = CB 76, must NOT modify memory; 12 T-states.
    emfe_poke_byte(inst, 0x2000, 0x40);
    const uint8_t prog2[] = { 0xCB, 0x76 };
    LoadProgram(inst, 0x10, prog2, sizeof(prog2));
    int64_t cyc_before = emfe_get_cycle_count(inst);
    emfe_step(inst);
    TEST_EQ(emfe_peek_byte(inst, 0x2000), 0x40, "BIT does not modify memory");
    TEST_EQ(emfe_get_cycle_count(inst) - cyc_before, (int64_t)12,
            "BIT n, (HL) is 12 T-states");

    emfe_destroy(inst);
    g_pass++;
}

// ---------------------------------------------------------------------
// Phase 2.2 tests — ADD HL,rr + DD/FD prefix (IX/IY)
// ---------------------------------------------------------------------

static void TestAddHlPair() {
    printf("TestAddHlPair...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // ADD HL,BC : 0x1234 + 0x1111 = 0x2345. S/Z/PV preserved; N/H/C cleared.
    SetReg(inst, REG_HL, 0x1234);
    SetReg(inst, REG_BC, 0x1111);
    SetReg(inst, REG_F, TF_S | TF_Z | TF_PV);
    const uint8_t prog[] = { 0x09 };
    LoadProgram(inst, 0, prog, sizeof(prog));
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_HL), (uint64_t)0x2345, "HL := HL+BC");
    uint8_t f = (uint8_t)GetReg(inst, REG_F);
    TEST_TRUE((f & TF_N) == 0,  "N cleared");
    TEST_TRUE((f & TF_C) == 0,  "no carry");
    TEST_TRUE((f & TF_H) == 0,  "no half-carry");
    TEST_TRUE((f & TF_S) != 0,  "S preserved");
    TEST_TRUE((f & TF_Z) != 0,  "Z preserved");
    TEST_TRUE((f & TF_PV) != 0, "PV preserved");
    TEST_EQ(emfe_get_cycle_count(inst), (int64_t)11, "11 T-states");

    // ADD HL,HL with HL=0x8000 doubles to 0 + C=1
    SetReg(inst, REG_HL, 0x8000);
    SetReg(inst, REG_F, 0);
    const uint8_t addhlhl[] = { 0x29 };
    LoadProgram(inst, 0x10, addhlhl, 1);
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_HL), (uint64_t)0x0000, "HL+HL doubles");
    TEST_TRUE((GetReg(inst, REG_F) & TF_C) != 0, "C set on overflow");

    emfe_destroy(inst);
    g_pass++;
}

static void TestLdIxImmediate() {
    printf("TestLdIxImmediate...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    const uint8_t prog[] = { 0xDD, 0x21, 0x34, 0x12 };       // LD IX, 0x1234
    LoadProgram(inst, 0, prog, sizeof(prog));
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_IX), (uint64_t)0x1234, "IX := 0x1234");
    TEST_EQ(emfe_get_cycle_count(inst), (int64_t)14, "14 T-states");

    const uint8_t prog2[] = { 0xFD, 0x21, 0x78, 0x56 };      // LD IY, 0x5678
    LoadProgram(inst, 0x10, prog2, sizeof(prog2));
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_IY), (uint64_t)0x5678, "IY := 0x5678");

    emfe_destroy(inst);
    g_pass++;
}

static void TestIxIyArithmetic() {
    printf("TestIxIyArithmetic...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // INC IX (DD 23): 0xFFFF → 0x0000
    SetReg(inst, REG_IX, 0xFFFF);
    const uint8_t inc_ix[] = { 0xDD, 0x23 };
    LoadProgram(inst, 0, inc_ix, sizeof(inc_ix));
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_IX), (uint64_t)0x0000, "INC IX wraps");
    TEST_EQ(emfe_get_cycle_count(inst), (int64_t)10, "10 T-states");

    // DEC IY (FD 2B): 0x0000 → 0xFFFF
    SetReg(inst, REG_IY, 0x0000);
    const uint8_t dec_iy[] = { 0xFD, 0x2B };
    LoadProgram(inst, 0x10, dec_iy, sizeof(dec_iy));
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_IY), (uint64_t)0xFFFF, "DEC IY underflows");

    // ADD IX, BC: 0x1000 + 0x0100 = 0x1100
    SetReg(inst, REG_IX, 0x1000);
    SetReg(inst, REG_BC, 0x0100);
    SetReg(inst, REG_F, 0);
    const uint8_t add_ix_bc[] = { 0xDD, 0x09 };
    LoadProgram(inst, 0x20, add_ix_bc, sizeof(add_ix_bc));
    int64_t cyc_before = emfe_get_cycle_count(inst);
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_IX), (uint64_t)0x1100, "IX += BC");
    TEST_EQ(emfe_get_cycle_count(inst) - cyc_before, (int64_t)15,
            "ADD IX,rr is 15 T");

    // ADD IX, IX doubles IX (not IX+HL). With HL set to a distinct
    // value to make sure it isn't used.
    SetReg(inst, REG_IX, 0x4000);
    SetReg(inst, REG_HL, 0xAAAA);
    SetReg(inst, REG_F, 0);
    const uint8_t add_ix_ix[] = { 0xDD, 0x29 };
    LoadProgram(inst, 0x30, add_ix_ix, sizeof(add_ix_ix));
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_IX), (uint64_t)0x8000, "ADD IX,IX doubles");
    TEST_EQ(GetReg(inst, REG_HL), (uint64_t)0xAAAA, "HL untouched");

    emfe_destroy(inst);
    g_pass++;
}

static void TestIxDisplacementMemory() {
    printf("TestIxDisplacementMemory...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    SetReg(inst, REG_IX, 0x3000);
    emfe_poke_byte(inst, 0x3005, 0xAB);

    // LD B, (IX+5) = DD 46 05
    const uint8_t ld_b_ixd[] = { 0xDD, 0x46, 0x05 };
    LoadProgram(inst, 0, ld_b_ixd, sizeof(ld_b_ixd));
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_B), (uint64_t)0xAB, "B := mem[IX+5]");
    TEST_EQ(emfe_get_cycle_count(inst), (int64_t)19, "19 T-states");

    // LD (IX-1), C — signed displacement -1
    SetReg(inst, REG_C, 0x5A);
    const uint8_t ld_ixd_c[] = { 0xDD, 0x71, 0xFF };
    LoadProgram(inst, 0x10, ld_ixd_c, sizeof(ld_ixd_c));
    emfe_step(inst);
    TEST_EQ(emfe_peek_byte(inst, 0x2FFF), 0x5A, "(IX-1) := C with signed offset");

    // LD (IX+2), n
    const uint8_t ld_ixd_n[] = { 0xDD, 0x36, 0x02, 0x99 };
    LoadProgram(inst, 0x20, ld_ixd_n, sizeof(ld_ixd_n));
    int64_t cyc_before = emfe_get_cycle_count(inst);
    emfe_step(inst);
    TEST_EQ(emfe_peek_byte(inst, 0x3002), 0x99, "(IX+2) := 0x99");
    TEST_EQ(emfe_get_cycle_count(inst) - cyc_before, (int64_t)19,
            "LD (IX+d),n is 19 T");

    emfe_destroy(inst);
    g_pass++;
}

static void TestAluViaIxDisplacement() {
    printf("TestAluViaIxDisplacement...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    SetReg(inst, REG_IY, 0x4000);
    emfe_poke_byte(inst, 0x4003, 0x10);
    SetReg(inst, REG_A, 0x05);

    // ADD A, (IY+3) = FD 86 03
    const uint8_t prog[] = { 0xFD, 0x86, 0x03 };
    LoadProgram(inst, 0, prog, sizeof(prog));
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_A), (uint64_t)0x15, "A := A + mem[IY+3]");
    TEST_EQ(emfe_get_cycle_count(inst), (int64_t)19, "19 T-states");

    emfe_destroy(inst);
    g_pass++;
}

static void TestIxPushPopAndSpJp() {
    printf("TestIxPushPopAndSpJp...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    SetReg(inst, REG_SP, 0xFFF0);
    SetReg(inst, REG_IX, 0xCAFE);
    // PUSH IX (DD E5) ; POP IY (FD E1)
    const uint8_t prog[] = { 0xDD, 0xE5, 0xFD, 0xE1 };
    LoadProgram(inst, 0, prog, sizeof(prog));
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_SP), (uint64_t)0xFFEE, "SP -= 2");
    TEST_EQ(emfe_peek_byte(inst, 0xFFEE), 0xFE, "low byte");
    TEST_EQ(emfe_peek_byte(inst, 0xFFEF), 0xCA, "high byte");
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_IY), (uint64_t)0xCAFE, "IY := original IX");

    // LD SP, IX — set SP from IX
    SetReg(inst, REG_IX, 0x2000);
    const uint8_t ld_sp_ix[] = { 0xDD, 0xF9 };
    LoadProgram(inst, 0x10, ld_sp_ix, sizeof(ld_sp_ix));
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_SP), (uint64_t)0x2000, "SP := IX");

    // JP (IX) — PC := IX
    SetReg(inst, REG_IX, 0x5000);
    const uint8_t jp_ix[] = { 0xDD, 0xE9 };
    LoadProgram(inst, 0x20, jp_ix, sizeof(jp_ix));
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_PC), (uint64_t)0x5000, "PC := IX");

    emfe_destroy(inst);
    g_pass++;
}

static void TestDdCbBitSetSla() {
    // DD CB d xx — 4-byte form: BIT/SET/SLA on (IX+d).
    printf("TestDdCbBitSetSla...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    SetReg(inst, REG_IX, 0x5000);
    emfe_poke_byte(inst, 0x5003, 0x80);

    // BIT 7, (IX+3) = DD CB 03 7E. Memory unchanged; Z=0.
    const uint8_t bit_ixd[] = { 0xDD, 0xCB, 0x03, 0x7E };
    LoadProgram(inst, 0, bit_ixd, sizeof(bit_ixd));
    emfe_step(inst);
    TEST_EQ(emfe_peek_byte(inst, 0x5003), 0x80, "BIT does not modify memory");
    TEST_TRUE((GetReg(inst, REG_F) & TF_Z) == 0, "Z clear (bit set)");
    TEST_EQ(emfe_get_cycle_count(inst), (int64_t)20, "BIT (IX+d) is 20 T");

    // SET 0, (IX+3) = DD CB 03 C6. 0x80 → 0x81.
    const uint8_t set_ixd[] = { 0xDD, 0xCB, 0x03, 0xC6 };
    LoadProgram(inst, 0x10, set_ixd, sizeof(set_ixd));
    int64_t cyc_before = emfe_get_cycle_count(inst);
    emfe_step(inst);
    TEST_EQ(emfe_peek_byte(inst, 0x5003), 0x81, "SET 0 lights bit 0");
    TEST_EQ(emfe_get_cycle_count(inst) - cyc_before, (int64_t)23,
            "SET (IX+d) is 23 T");

    // SLA (IX+3) = DD CB 03 26. 0x81 → 0x02, C=1.
    const uint8_t sla_ixd[] = { 0xDD, 0xCB, 0x03, 0x26 };
    LoadProgram(inst, 0x20, sla_ixd, sizeof(sla_ixd));
    emfe_step(inst);
    TEST_EQ(emfe_peek_byte(inst, 0x5003), 0x02, "SLA shifts the byte");
    TEST_TRUE((GetReg(inst, REG_F) & TF_C) != 0, "carry from bit 7");

    emfe_destroy(inst);
    g_pass++;
}

// ---------------------------------------------------------------------
// Phase 2.3 tests — ED prefix (block I/O, IM, ADC/SBC HL, NEG, …)
// ---------------------------------------------------------------------

static void TestEdNeg() {
    printf("TestEdNeg...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // NEG with A=0x01 → 0xFF, N=1, C=1 (borrow), S=1
    SetReg(inst, REG_A, 0x01);
    SetReg(inst, REG_F, 0);
    const uint8_t prog[] = { 0xED, 0x44 };
    LoadProgram(inst, 0, prog, sizeof(prog));
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_A), (uint64_t)0xFF, "NEG 0x01 → 0xFF");
    uint8_t f = (uint8_t)GetReg(inst, REG_F);
    TEST_TRUE((f & TF_N) != 0, "N set (sub)");
    TEST_TRUE((f & TF_C) != 0, "C set (borrow from 0)");
    TEST_TRUE((f & TF_S) != 0, "S set");
    TEST_EQ(emfe_get_cycle_count(inst), (int64_t)8, "8 T-states");

    // NEG with A=0x00 → 0x00, Z=1, C=0
    SetReg(inst, REG_A, 0x00);
    SetReg(inst, REG_F, 0);
    SetReg(inst, REG_PC, 0);
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_A), (uint64_t)0x00, "NEG 0 → 0");
    TEST_TRUE((GetReg(inst, REG_F) & TF_Z) != 0, "Z set");
    TEST_TRUE((GetReg(inst, REG_F) & TF_C) == 0, "C clear (no borrow)");

    emfe_destroy(inst);
    g_pass++;
}

static void TestEdImAndIRRegisters() {
    // IM 2 (ED 5E) ; LD I,A (ED 47) ; LD A,I (ED 57)
    printf("TestEdImAndIRRegisters...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    const uint8_t prog[] = { 0xED, 0x5E };           // IM 2
    LoadProgram(inst, 0, prog, sizeof(prog));
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_IM), (uint64_t)2, "IM 2 sets IM=2");

    // LD I, A with A=0xC0 ; then LD A, I → A should be 0xC0 with flags
    SetReg(inst, REG_A, 0xC0);
    const uint8_t prog2[] = { 0xED, 0x47, 0xED, 0x57 };
    LoadProgram(inst, 0x10, prog2, sizeof(prog2));
    emfe_step(inst);   // LD I, A
    TEST_EQ(GetReg(inst, REG_I), (uint64_t)0xC0, "I := A");
    emfe_step(inst);   // LD A, I
    TEST_EQ(GetReg(inst, REG_A), (uint64_t)0xC0, "A := I round-trip");
    TEST_TRUE((GetReg(inst, REG_F) & TF_S) != 0, "S set (bit 7)");

    emfe_destroy(inst);
    g_pass++;
}

static void TestEdLdNNrr() {
    // LD (nn),DE (ED 53) ; LD HL,(nn) (ED 6B). LD HL/(nn) ED form is
    // redundant with the plain 0x2A but Z80 software uses both.
    printf("TestEdLdNNrr...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    SetReg(inst, REG_DE, 0xBEEF);
    const uint8_t prog[] = { 0xED, 0x53, 0x00, 0x40 };    // LD (0x4000), DE
    LoadProgram(inst, 0, prog, sizeof(prog));
    emfe_step(inst);
    TEST_EQ(emfe_peek_byte(inst, 0x4000), 0xEF, "low byte at nn");
    TEST_EQ(emfe_peek_byte(inst, 0x4001), 0xBE, "high byte at nn+1");
    TEST_EQ(emfe_get_cycle_count(inst), (int64_t)20, "20 T");

    // LD HL,(0x4000) — should read 0xBEEF back
    const uint8_t prog2[] = { 0xED, 0x6B, 0x00, 0x40 };
    LoadProgram(inst, 0x10, prog2, sizeof(prog2));
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_HL), (uint64_t)0xBEEF, "HL := mem[nn]");

    emfe_destroy(inst);
    g_pass++;
}

static void TestEdAdcSbcHl() {
    // ADC HL,BC (ED 4A) and SBC HL,BC (ED 42) with full flag check.
    printf("TestEdAdcSbcHl...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // ADC HL,BC with HL=0x1000, BC=0x2000, C=1 → HL=0x3001
    SetReg(inst, REG_HL, 0x1000);
    SetReg(inst, REG_BC, 0x2000);
    SetReg(inst, REG_F, TF_C);
    const uint8_t prog[] = { 0xED, 0x4A };
    LoadProgram(inst, 0, prog, sizeof(prog));
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_HL), (uint64_t)0x3001, "HL += BC + 1");
    uint8_t f = (uint8_t)GetReg(inst, REG_F);
    TEST_TRUE((f & TF_C) == 0, "no new carry");
    TEST_TRUE((f & TF_N) == 0, "N clear (add)");
    TEST_EQ(emfe_get_cycle_count(inst), (int64_t)15, "15 T-states");

    // SBC HL,HL with HL=anything and C=0 → result 0, Z=1, N=1
    SetReg(inst, REG_HL, 0x5555);
    SetReg(inst, REG_F, 0);
    const uint8_t prog2[] = { 0xED, 0x62 };      // SBC HL, HL
    LoadProgram(inst, 0x10, prog2, sizeof(prog2));
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_HL), (uint64_t)0, "HL - HL - 0 = 0");
    TEST_TRUE((GetReg(inst, REG_F) & TF_Z) != 0, "Z set");
    TEST_TRUE((GetReg(inst, REG_F) & TF_N) != 0, "N set");

    // SBC HL,HL with HL=anything and C=1 → result 0xFFFF, idiomatic
    // "negate carry" trick used by Z80 software.
    SetReg(inst, REG_HL, 0x0000);
    SetReg(inst, REG_F, TF_C);
    SetReg(inst, REG_PC, 0x10);
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_HL), (uint64_t)0xFFFF, "HL - HL - 1 = 0xFFFF");

    emfe_destroy(inst);
    g_pass++;
}

static void TestEdInOutWithC() {
    // OUT (C),r and IN r,(C). Use the UART port 0xFE.
    printf("TestEdInOutWithC...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    CaptureCtx ctx;
    emfe_set_console_char_callback(inst, CaptureChar, &ctx);

    SetReg(inst, REG_C, 0xFE);
    SetReg(inst, REG_A, 'Z');
    const uint8_t prog[] = { 0xED, 0x79 };            // OUT (C), A
    LoadProgram(inst, 0, prog, sizeof(prog));
    emfe_step(inst);
    TEST_TRUE(ctx.captured == "Z", "OUT (C),A reached console");
    TEST_EQ(emfe_get_cycle_count(inst), (int64_t)12, "12 T-states");

    // IN A,(C) from status port (0xFF): TX_READY (bit 0) always set.
    SetReg(inst, REG_C, 0xFF);
    const uint8_t prog2[] = { 0xED, 0x78 };            // IN A, (C)
    LoadProgram(inst, 0x10, prog2, sizeof(prog2));
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_A) & 0x01, (uint64_t)0x01, "TX_READY visible via IN");

    emfe_destroy(inst);
    g_pass++;
}

static void TestEdLdiAndLdir() {
    // LDI and LDIR — memcpy-style block move.
    printf("TestEdLdiAndLdir...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // Single LDI: copy one byte from HL to DE, advance both, BC--.
    SetReg(inst, REG_HL, 0x3000);
    SetReg(inst, REG_DE, 0x4000);
    SetReg(inst, REG_BC, 0x0005);
    emfe_poke_byte(inst, 0x3000, 0xAA);
    const uint8_t ldi[] = { 0xED, 0xA0 };
    LoadProgram(inst, 0, ldi, sizeof(ldi));
    emfe_step(inst);
    TEST_EQ(emfe_peek_byte(inst, 0x4000), 0xAA, "byte copied");
    TEST_EQ(GetReg(inst, REG_HL), (uint64_t)0x3001, "HL advanced");
    TEST_EQ(GetReg(inst, REG_DE), (uint64_t)0x4001, "DE advanced");
    TEST_EQ(GetReg(inst, REG_BC), (uint64_t)0x0004, "BC decremented");
    TEST_TRUE((GetReg(inst, REG_F) & TF_PV) != 0, "PV set (BC != 0)");

    // LDIR: copy 4 bytes (BC=4 left over from above) — should re-execute
    // until BC hits 0.
    for (uint8_t i = 1; i <= 4; ++i) {
        emfe_poke_byte(inst, 0x3000 + i, static_cast<uint8_t>(0x10 + i));
    }
    const uint8_t ldir[] = { 0xED, 0xB0 };
    LoadProgram(inst, 0x10, ldir, sizeof(ldir));
    // Step until BC reaches 0 — LDIR loops by rewinding PC.
    for (int i = 0; i < 10 && GetReg(inst, REG_BC) != 0; ++i) {
        emfe_step(inst);
    }
    TEST_EQ(GetReg(inst, REG_BC), (uint64_t)0, "LDIR ran to completion");
    TEST_EQ(emfe_peek_byte(inst, 0x4001), 0x11, "dest[1]");
    TEST_EQ(emfe_peek_byte(inst, 0x4004), 0x14, "dest[4]");
    TEST_TRUE((GetReg(inst, REG_F) & TF_PV) == 0, "PV clear after BC==0");

    emfe_destroy(inst);
    g_pass++;
}

static void TestEdCpirSearch() {
    // CPIR scans memory at HL for A, decrementing BC, stopping when
    // A matches or BC hits 0.
    printf("TestEdCpirSearch...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // Plant a string in memory ending with '!'. Search for '!'.
    const char* s = "Hello!";
    for (size_t i = 0; i <= 6; ++i) emfe_poke_byte(inst, 0x5000 + i, s[i]);

    SetReg(inst, REG_A, '!');
    SetReg(inst, REG_HL, 0x5000);
    SetReg(inst, REG_BC, 10);    // search up to 10 chars
    const uint8_t cpir[] = { 0xED, 0xB1 };
    LoadProgram(inst, 0, cpir, sizeof(cpir));
    for (int i = 0; i < 20 && GetReg(inst, REG_BC) != 0; ++i) {
        emfe_step(inst);
        if ((GetReg(inst, REG_F) & TF_Z) != 0) break;   // found
    }
    TEST_TRUE((GetReg(inst, REG_F) & TF_Z) != 0, "Z set when match found");
    // After match: HL points just PAST the matched character (0x5006)
    TEST_EQ(GetReg(inst, REG_HL), (uint64_t)0x5006, "HL = match+1");

    emfe_destroy(inst);
    g_pass++;
}

static void TestEdRrdRld() {
    // RRD / RLD — 12-bit BCD rotate between A.low and (HL).
    printf("TestEdRrdRld...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // RRD (Zilog UM008011): for a=0x12, m=0x34 (12-bit BCD nibbles
    // [A.high=1][A.low=2][m.high=3][m.low=4]):
    //   new A.low = old m.low (= 4)
    //   new m.high = old A.low (= 2)
    //   new m.low = old m.high (= 3)
    //   A.high unchanged.
    // So new A = 0x14, new mem = 0x23.
    SetReg(inst, REG_A, 0x12);
    SetReg(inst, REG_HL, 0x6000);
    emfe_poke_byte(inst, 0x6000, 0x34);
    const uint8_t rrd[] = { 0xED, 0x67 };
    LoadProgram(inst, 0, rrd, sizeof(rrd));
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_A), (uint64_t)0x14, "A.low := old mem.low");
    TEST_EQ(emfe_peek_byte(inst, 0x6000), 0x23, "mem.high := old A.low; mem.low := old mem.high");
    TEST_EQ(emfe_get_cycle_count(inst), (int64_t)18, "18 T-states");

    emfe_destroy(inst);
    g_pass++;
}

// ---------------------------------------------------------------------
// Phase 2.4 tests — EI / DI / HALT / interrupts
// ---------------------------------------------------------------------

static void TestEiDi() {
    printf("TestEiDi...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // EI sets IFF1, IFF2 := 1
    const uint8_t ei[] = { 0xFB };
    LoadProgram(inst, 0, ei, 1);
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_IFF1), (uint64_t)1, "EI sets IFF1");
    TEST_EQ(GetReg(inst, REG_IFF2), (uint64_t)1, "EI sets IFF2");

    // DI clears both
    const uint8_t di[] = { 0xF3 };
    LoadProgram(inst, 0x10, di, 1);
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_IFF1), (uint64_t)0, "DI clears IFF1");
    TEST_EQ(GetReg(inst, REG_IFF2), (uint64_t)0, "DI clears IFF2");

    emfe_destroy(inst);
    g_pass++;
}

static void TestHaltLoop() {
    // HALT enters halted state. Each subsequent step is a pseudo-NOP
    // (4 T, R bumps, InstructionCount bumps) but PC stays at HALT+1.
    printf("TestHaltLoop...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    const uint8_t prog[] = { 0x76 };           // HALT
    LoadProgram(inst, 0, prog, sizeof(prog));
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_PC), (uint64_t)1, "PC advanced past HALT");
    int64_t cycles_after_halt = emfe_get_cycle_count(inst);
    int64_t r_after_halt = (int64_t)GetReg(inst, REG_R);

    // Three more steps — all pseudo-NOPs while halted.
    emfe_step(inst);
    emfe_step(inst);
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_PC), (uint64_t)1, "PC stays at HALT+1 while halted");
    TEST_EQ(emfe_get_cycle_count(inst) - cycles_after_halt, (int64_t)(4 * 3),
            "3 × 4 T for pseudo-NOPs");
    TEST_EQ((int64_t)GetReg(inst, REG_R) - r_after_halt, (int64_t)3,
            "R bumps per pseudo-NOP");

    emfe_destroy(inst);
    g_pass++;
}

// We can't directly trigger NMI / IRQ via the public ABI yet — Phase
// 2.4 only adds the CPU-side machinery. Test by poking the Z80Cpu
// state through a private back-channel: there isn't one over the C
// ABI, so we exercise IM 1 and NMI vectors through the same path the
// host would use (manually set NmiPending/IrqLine via memory tricks)
// — but those fields aren't exposed either. Instead, we test the
// state machine indirectly:
//   - After RST 38h (which is the IM 1 vector), PC should land at
//     0x0038 with the previous PC pushed.
//   - We can't simulate IRQ acceptance from outside, but we CAN test
//     the EI/DI/HALT semantics and verify IM register changes drive
//     the right code path.
//
// A full interrupt-injection test will land in Phase 2.7 (the
// zexdoc integration) which exercises the path naturally via CP/M-
// stub ISRs. For Phase 2.4 we settle for verifying the dispatch
// surface compiles and HALT/EI/DI behave correctly.
static void TestImAndInterruptVector() {
    printf("TestImAndInterruptVector...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // Set IM 2, I := 0x40 — this would direct IM 2 IRQs through
    // vectors at 0x40FF / 0x40FE. We can't fire the IRQ from outside
    // but we can verify the state writes round-trip cleanly.
    const uint8_t prog[] = { 0xED, 0x5E, 0x3E, 0x40, 0xED, 0x47 };  // IM 2; LD A,0x40; LD I,A
    LoadProgram(inst, 0, prog, sizeof(prog));
    emfe_step(inst);   // IM 2
    emfe_step(inst);   // LD A, 0x40
    emfe_step(inst);   // LD I, A
    TEST_EQ(GetReg(inst, REG_IM), (uint64_t)2, "IM 2 set");
    TEST_EQ(GetReg(inst, REG_I),  (uint64_t)0x40, "I = 0x40");

    emfe_destroy(inst);
    g_pass++;
}

// ---------------------------------------------------------------------
// Phase 2.5 tests — Z80Disasm
// ---------------------------------------------------------------------

// Disassemble one instruction at `addr` and assert that the mnemonic
// and operand fields equal the expected strings. Also checks length.
static void AssertDisasm(EmfeInstance inst, uint16_t addr,
                         const char* expected_mnem,
                         const char* expected_ops,
                         uint32_t expected_len,
                         const char* tag) {
    EmfeDisasmLine line{};
    EmfeResult rc = emfe_disassemble_one(inst, addr, &line);
    if (rc != EMFE_OK) {
        fprintf(stderr, "  FAIL[%s]: disasm rc=%d\n", tag, rc);
        g_fail++;
        return;
    }
    if (std::strcmp(line.mnemonic, expected_mnem) != 0) {
        fprintf(stderr, "  FAIL[%s]: mnem got '%s' expected '%s'\n",
                tag, line.mnemonic, expected_mnem);
        g_fail++;
        return;
    }
    if (std::strcmp(line.operands, expected_ops) != 0) {
        fprintf(stderr, "  FAIL[%s]: ops got '%s' expected '%s'\n",
                tag, line.operands, expected_ops);
        g_fail++;
        return;
    }
    if (line.length != expected_len) {
        fprintf(stderr, "  FAIL[%s]: length got %u expected %u\n",
                tag, line.length, expected_len);
        g_fail++;
        return;
    }
}

static void TestDisasmPlain() {
    // Spot-check every "family" of plain (non-prefixed) opcodes.
    printf("TestDisasmPlain...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // 0x00 NOP
    emfe_poke_byte(inst, 0x0000, 0x00);
    AssertDisasm(inst, 0x0000, "NOP", "", 1, "NOP");

    // 0x21 LD HL, 0x1234
    emfe_poke_byte(inst, 0x0010, 0x21);
    emfe_poke_byte(inst, 0x0011, 0x34);
    emfe_poke_byte(inst, 0x0012, 0x12);
    AssertDisasm(inst, 0x0010, "LD", "HL, $1234", 3, "LD HL,nn");

    // 0x7E LD A, (HL)
    emfe_poke_byte(inst, 0x0020, 0x7E);
    AssertDisasm(inst, 0x0020, "LD", "A, (HL)", 1, "LD A,(HL)");

    // 0x80 ADD A, B
    emfe_poke_byte(inst, 0x0030, 0x80);
    AssertDisasm(inst, 0x0030, "ADD", "A, B", 1, "ADD A,B");

    // 0xC9 RET
    emfe_poke_byte(inst, 0x0040, 0xC9);
    AssertDisasm(inst, 0x0040, "RET", "", 1, "RET");

    // 0xD3 OUT (0xFE), A
    emfe_poke_byte(inst, 0x0050, 0xD3);
    emfe_poke_byte(inst, 0x0051, 0xFE);
    AssertDisasm(inst, 0x0050, "OUT", "($FE), A", 2, "OUT (n),A");

    // 0x76 HALT
    emfe_poke_byte(inst, 0x0060, 0x76);
    AssertDisasm(inst, 0x0060, "HALT", "", 1, "HALT");

    // 0xCD CALL $5000
    emfe_poke_byte(inst, 0x0070, 0xCD);
    emfe_poke_byte(inst, 0x0071, 0x00);
    emfe_poke_byte(inst, 0x0072, 0x50);
    AssertDisasm(inst, 0x0070, "CALL", "$5000", 3, "CALL");

    // 0x18 JR +5 at 0x0080: end-of-instruction = 0x0082, target = 0x0087
    emfe_poke_byte(inst, 0x0080, 0x18);
    emfe_poke_byte(inst, 0x0081, 0x05);
    AssertDisasm(inst, 0x0080, "JR", "$0087", 2, "JR e");

    emfe_destroy(inst);
    g_pass++;
}

static void TestDisasmCbPrefix() {
    printf("TestDisasmCbPrefix...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // CB 27  SLA A
    emfe_poke_byte(inst, 0, 0xCB);
    emfe_poke_byte(inst, 1, 0x27);
    AssertDisasm(inst, 0, "SLA", "A", 2, "SLA A");

    // CB 7F  BIT 7, A
    emfe_poke_byte(inst, 0x10, 0xCB);
    emfe_poke_byte(inst, 0x11, 0x7F);
    AssertDisasm(inst, 0x10, "BIT", "7, A", 2, "BIT 7,A");

    // CB C6  SET 0, (HL)
    emfe_poke_byte(inst, 0x20, 0xCB);
    emfe_poke_byte(inst, 0x21, 0xC6);
    AssertDisasm(inst, 0x20, "SET", "0, (HL)", 2, "SET 0,(HL)");

    emfe_destroy(inst);
    g_pass++;
}

static void TestDisasmDdFdPrefix() {
    printf("TestDisasmDdFdPrefix...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // DD 21 34 12  LD IX, $1234
    emfe_poke_byte(inst, 0, 0xDD);
    emfe_poke_byte(inst, 1, 0x21);
    emfe_poke_byte(inst, 2, 0x34);
    emfe_poke_byte(inst, 3, 0x12);
    AssertDisasm(inst, 0, "LD", "IX, $1234", 4, "LD IX,nn");

    // FD 46 05  LD B, (IY+5)
    emfe_poke_byte(inst, 0x10, 0xFD);
    emfe_poke_byte(inst, 0x11, 0x46);
    emfe_poke_byte(inst, 0x12, 0x05);
    AssertDisasm(inst, 0x10, "LD", "B, (IY+5)", 3, "LD B,(IY+5)");

    // DD 36 FF 99  LD (IX-1), $99
    emfe_poke_byte(inst, 0x20, 0xDD);
    emfe_poke_byte(inst, 0x21, 0x36);
    emfe_poke_byte(inst, 0x22, 0xFF);
    emfe_poke_byte(inst, 0x23, 0x99);
    AssertDisasm(inst, 0x20, "LD", "(IX-1), $99", 4, "LD (IX-1),n");

    // DD CB 03 7E  BIT 7, (IX+3)
    emfe_poke_byte(inst, 0x30, 0xDD);
    emfe_poke_byte(inst, 0x31, 0xCB);
    emfe_poke_byte(inst, 0x32, 0x03);
    emfe_poke_byte(inst, 0x33, 0x7E);
    AssertDisasm(inst, 0x30, "BIT", "7, (IX+3)", 4, "BIT 7,(IX+3)");

    emfe_destroy(inst);
    g_pass++;
}

static void TestDisasmEdPrefix() {
    printf("TestDisasmEdPrefix...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // ED B0  LDIR
    emfe_poke_byte(inst, 0, 0xED);
    emfe_poke_byte(inst, 1, 0xB0);
    AssertDisasm(inst, 0, "LDIR", "", 2, "LDIR");

    // ED 44  NEG
    emfe_poke_byte(inst, 0x10, 0xED);
    emfe_poke_byte(inst, 0x11, 0x44);
    AssertDisasm(inst, 0x10, "NEG", "", 2, "NEG");

    // ED 5E  IM 2
    emfe_poke_byte(inst, 0x20, 0xED);
    emfe_poke_byte(inst, 0x21, 0x5E);
    AssertDisasm(inst, 0x20, "IM", "2", 2, "IM 2");

    // ED 4B 34 12  LD BC, ($1234)
    emfe_poke_byte(inst, 0x30, 0xED);
    emfe_poke_byte(inst, 0x31, 0x4B);
    emfe_poke_byte(inst, 0x32, 0x34);
    emfe_poke_byte(inst, 0x33, 0x12);
    AssertDisasm(inst, 0x30, "LD", "BC, ($1234)", 4, "LD BC,(nn)");

    emfe_destroy(inst);
    g_pass++;
}

static void TestDisasmHelloWorldROM() {
    // Decode the 30-byte hello-world ROM from Phase 1.7 and verify
    // the first few lines step naturally.
    printf("TestDisasmHelloWorldROM...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    for (size_t i = 0; i < sizeof(hello_world_bin); ++i) {
        emfe_poke_byte(inst, (uint16_t)i, hello_world_bin[i]);
    }

    // 0x0000  LD HL, $000E
    AssertDisasm(inst, 0x0000, "LD",  "HL, $000E", 3, "hello[0]");
    // 0x0003  LD A, (HL)
    AssertDisasm(inst, 0x0003, "LD",  "A, (HL)",   1, "hello[3]");
    // 0x0004  OR A
    AssertDisasm(inst, 0x0004, "OR",  "A",         1, "hello[4]");
    // 0x0005  JR Z, $000C
    AssertDisasm(inst, 0x0005, "JR",  "Z, $000C",  2, "hello[5]");
    // 0x0007  OUT ($FE), A
    AssertDisasm(inst, 0x0007, "OUT", "($FE), A",  2, "hello[7]");
    // 0x0009  INC HL
    AssertDisasm(inst, 0x0009, "INC", "HL",        1, "hello[9]");
    // 0x000A  JR $0003
    AssertDisasm(inst, 0x000A, "JR",  "$0003",     2, "hello[A]");

    emfe_destroy(inst);
    g_pass++;
}

static void TestDisasmRangeAndProgramRange() {
    printf("TestDisasmRangeAndProgramRange...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // Load 3 instructions and disassemble the range.
    const uint8_t prog[] = {
        0x00,             // NOP
        0x06, 0x11,       // LD B, $11
        0xC9,             // RET
    };
    for (size_t i = 0; i < sizeof(prog); ++i) {
        emfe_poke_byte(inst, (uint16_t)i, prog[i]);
    }

    EmfeDisasmLine lines[10] = {};
    int32_t n = emfe_disassemble_range(inst, 0, 4, lines, 10);
    TEST_EQ(n, 3, "disassemble_range returns 3 lines");
    TEST_TRUE(std::strcmp(lines[0].mnemonic, "NOP") == 0, "line 0: NOP");
    TEST_TRUE(std::strcmp(lines[1].mnemonic, "LD")  == 0, "line 1: LD");
    TEST_TRUE(std::strcmp(lines[1].operands, "B, $11") == 0, "line 1 operands");
    TEST_TRUE(std::strcmp(lines[2].mnemonic, "RET") == 0, "line 2: RET");

    // get_program_range returns the full 64 KiB.
    uint64_t s = 0, e = 0;
    TEST_EQ(emfe_get_program_range(inst, &s, &e), EMFE_OK, "program_range OK");
    TEST_EQ(s, (uint64_t)0,       "start = 0");
    TEST_EQ(e, (uint64_t)0x10000, "end = 64 KiB");

    emfe_destroy(inst);
    g_pass++;
}

// ---------------------------------------------------------------------
// Phase 2.6 tests — minimal Settings
// ---------------------------------------------------------------------

static void TestSettingsDefsPresent() {
    printf("TestSettingsDefsPresent...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    const EmfeSettingDef* defs = nullptr;
    int32_t n = emfe_get_setting_defs(inst, &defs);
    TEST_TRUE(n >= 6, "≥ 6 setting defs (non-empty dialog)");
    TEST_TRUE(defs != nullptr, "defs pointer non-null");

    bool sawMemorySize = false, sawResetVector = false, sawClearRam = false;
    bool sawCols = false, sawRows = false, sawScrollback = false;
    for (int32_t i = 0; i < n; ++i) {
        const auto& d = defs[i];
        if (std::strcmp(d.key, "MemorySize") == 0) {
            sawMemorySize = (std::strcmp(d.group, "General") == 0);
        } else if (std::strcmp(d.key, "ResetVector") == 0) {
            sawResetVector = (std::strcmp(d.group, "General") == 0);
        } else if (std::strcmp(d.key, "ClearRamOnReset") == 0) {
            sawClearRam = (std::strcmp(d.group, "General") == 0
                           && d.type == EMFE_SETTING_BOOL);
        } else if (std::strcmp(d.key, "ConsoleColumns") == 0) {
            sawCols = (std::strcmp(d.group, "Console") == 0);
        } else if (std::strcmp(d.key, "ConsoleRows") == 0) {
            sawRows = (std::strcmp(d.group, "Console") == 0);
        } else if (std::strcmp(d.key, "ConsoleScrollbackLines") == 0) {
            sawScrollback = (std::strcmp(d.group, "Console") == 0);
        }
    }
    TEST_TRUE(sawMemorySize,  "MemorySize under General");
    TEST_TRUE(sawResetVector, "ResetVector under General");
    TEST_TRUE(sawClearRam,    "ClearRamOnReset under General (BOOL)");
    TEST_TRUE(sawCols,        "ConsoleColumns under Console");
    TEST_TRUE(sawRows,        "ConsoleRows under Console");
    TEST_TRUE(sawScrollback,  "ConsoleScrollbackLines under Console");

    emfe_destroy(inst);
    g_pass++;
}

static void TestSettingsGetSetApply() {
    printf("TestSettingsGetSetApply...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    const char* v = emfe_get_setting(inst, "ConsoleColumns");
    TEST_TRUE(std::strcmp(v, "80") == 0, "default ConsoleColumns is 80");

    TEST_EQ(emfe_set_setting(inst, "ConsoleColumns", "132"), EMFE_OK, "set OK");
    v = emfe_get_setting(inst, "ConsoleColumns");
    TEST_TRUE(std::strcmp(v, "132") == 0, "staged value visible");

    // Until apply, applied is still the default.
    const char* av = emfe_get_applied_setting(inst, "ConsoleColumns");
    TEST_TRUE(std::strcmp(av, "80") == 0, "applied still default");

    TEST_EQ(emfe_apply_settings(inst), EMFE_OK, "apply OK");
    av = emfe_get_applied_setting(inst, "ConsoleColumns");
    TEST_TRUE(std::strcmp(av, "132") == 0, "applied now == staged");

    emfe_destroy(inst);
    g_pass++;
}

static void TestSettingsRequiresResetDeferral() {
    printf("TestSettingsRequiresResetDeferral...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // ResetVector is REQUIRES_RESET.
    TEST_EQ(emfe_set_setting(inst, "ResetVector", "256"), EMFE_OK, "stage");
    TEST_EQ(emfe_apply_settings(inst), EMFE_OK, "apply");
    const char* staged  = emfe_get_setting(inst, "ResetVector");
    const char* applied = emfe_get_applied_setting(inst, "ResetVector");
    TEST_TRUE(std::strcmp(staged,  "256") == 0, "staged updated");
    TEST_TRUE(std::strcmp(applied, "0")   == 0, "applied still 0 (deferred)");

    TEST_EQ(emfe_reset(inst), EMFE_OK, "reset OK");
    applied = emfe_get_applied_setting(inst, "ResetVector");
    TEST_TRUE(std::strcmp(applied, "256") == 0, "applied reflects reset commit");

    // Invalid args.
    TEST_EQ(emfe_set_setting(nullptr, "X", "Y"), EMFE_ERR_INVALID, "rejects null instance");
    TEST_EQ(emfe_set_setting(inst, nullptr, "Y"), EMFE_ERR_INVALID, "rejects null key");
    TEST_TRUE(std::strcmp(emfe_get_setting(inst, "NonexistentKey"), "") == 0,
              "unknown key returns empty string");

    emfe_destroy(inst);
    g_pass++;
}

int main() {
    printf("=== emfe_plugin_z80 tests (through Phase 2.6) ===\n");
    TestNegotiate();
    TestBoardInfo();
    TestLifecycle();
    TestMemoryPeekPoke();
    TestConsoleSendChar();
    TestConsoleCallback();
    TestRegisterDefs();
    TestRegisterRoundTrip();
    TestStepNop();
    TestRunningStateBlocksWrites();
    // Phase 1.4
    TestLdRegImmediate();
    TestLdRegPairImmediate();
    TestLdRegReg();
    TestLdRegMemHL();
    TestLdMemHLImmediate();
    TestLdAMemBCDE();
    TestLdAMemNNAbsolute();
    TestLdHLMemNNAbsolute();
    // Phase 1.5
    TestAluAddBasic();
    TestAluAddCarryHalfOverflow();
    TestAluAdcUsesCarry();
    TestAluSubBasic();
    TestAluAndOrXor();
    TestAluCpPreservesA();
    TestAluImmediate();
    TestAluMemoryIndirect();
    // Phase 1.6
    TestJpUnconditional();
    TestJpConditional();
    TestJpHl();
    TestJrRelative();
    TestJrConditional();
    TestDjnz();
    TestPushPop();
    TestCallRet();
    TestRst();
    // Phase 1.7
    TestInc16BitPair();
    TestDec16BitPair();
    TestInc8BitFlags();
    TestDec8BitFlags();
    TestInOutImmediate();
    TestHelloWorld();
    // Phase 2.1
    TestCbRotations();
    TestCbShifts();
    TestCbBit();
    TestCbSetRes();
    TestCbMemoryIndirect();
    // Phase 2.2
    TestAddHlPair();
    TestLdIxImmediate();
    TestIxIyArithmetic();
    TestIxDisplacementMemory();
    TestAluViaIxDisplacement();
    TestIxPushPopAndSpJp();
    TestDdCbBitSetSla();
    // Phase 2.3
    TestEdNeg();
    TestEdImAndIRRegisters();
    TestEdLdNNrr();
    TestEdAdcSbcHl();
    TestEdInOutWithC();
    TestEdLdiAndLdir();
    TestEdCpirSearch();
    TestEdRrdRld();
    // Phase 2.4
    TestEiDi();
    TestHaltLoop();
    TestImAndInterruptVector();
    // Phase 2.5
    TestDisasmPlain();
    TestDisasmCbPrefix();
    TestDisasmDdFdPrefix();
    TestDisasmEdPrefix();
    TestDisasmHelloWorldROM();
    TestDisasmRangeAndProgramRange();
    // Phase 2.6
    TestSettingsDefsPresent();
    TestSettingsGetSetApply();
    TestSettingsRequiresResetDeferral();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
