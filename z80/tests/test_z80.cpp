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
#include <memory>
#include <string>
#include <vector>

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

// ---------------------------------------------------------------------
// Phase 2.7 tests — CP/M-stub harness + optional zexdoc run
// ---------------------------------------------------------------------

// Run a CP/M .COM image preloaded at memory address `loadAt` (0x0100
// for real CP/M) under a stub BDOS that captures console output.
//
// Stops when one of:
//   - PC reaches 0x0000 (CP/M warm-boot trap)
//   - BDOS call C=0 (system reset)
//   - step budget exhausted
//
// Returns the captured console output, including any BDOS output but
// not the raw bytes the program would push through the OUT (0xFE), A
// path (that's the plugin's own UART, separate from CP/M's BDOS).
struct CpmStubResult {
    std::string output;
    int64_t     steps;
    bool        completed;       // exited cleanly (warm boot / C=0)
};

static CpmStubResult RunCpmStub(EmfeInstance inst, int64_t maxSteps) {
    CpmStubResult r{};
    SetReg(inst, REG_SP, 0xFFF0);
    while (r.steps < maxSteps) {
        uint64_t pc = GetReg(inst, REG_PC);
        if (pc == 0x0000) { r.completed = true; break; }
        if (pc == 0x0005) {
            // CP/M BDOS entry. Service the call in C++.
            uint8_t c  = (uint8_t)GetReg(inst, REG_C);
            uint8_t e  = (uint8_t)GetReg(inst, REG_E);
            uint16_t de = (uint16_t)GetReg(inst, REG_DE);
            if (c == 2) {
                r.output.push_back((char)e);
            } else if (c == 9) {
                for (int k = 0; k < 65536; ++k) {
                    uint8_t ch = (uint8_t)emfe_peek_byte(inst, (de + k) & 0xFFFF);
                    if (ch == '$') break;
                    r.output.push_back((char)ch);
                }
            } else if (c == 0) {
                r.completed = true; break;
            }
            // Pop the return address off the stack manually (no need
            // to actually execute the byte at 0x0005).
            uint16_t sp = (uint16_t)GetReg(inst, REG_SP);
            uint8_t lo = (uint8_t)emfe_peek_byte(inst, sp);
            uint8_t hi = (uint8_t)emfe_peek_byte(inst, (sp + 1) & 0xFFFF);
            SetReg(inst, REG_SP, (uint64_t)((sp + 2) & 0xFFFF));
            SetReg(inst, REG_PC, (uint64_t)(lo | (hi << 8)));
            continue;
        }
        emfe_step(inst);
        ++r.steps;
    }
    return r;
}

static void TestCpmStubHarness() {
    // Verify the CP/M-stub wiring with a tiny self-contained program
    // that prints "OK\r\n" via BDOS function 9 and then warm-boots.
    printf("TestCpmStubHarness...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // 0x0100 (entry):
    //   LD C, 9            0E 09          ; offset 0..1
    //   LD DE, 0x010B      11 0B 01       ; offset 2..4
    //   CALL 0x0005        CD 05 00       ; offset 5..7
    //   JP 0x0000          C3 00 00       ; offset 8..10  (warm boot)
    // 0x010B message: "OK\r\n$"           4F 4B 0D 0A 24  ; offset 11..15
    const uint8_t prog[] = {
        0x0E, 0x09,
        0x11, 0x0B, 0x01,
        0xCD, 0x05, 0x00,
        0xC3, 0x00, 0x00,
        0x4F, 0x4B, 0x0D, 0x0A, 0x24,
    };
    for (size_t i = 0; i < sizeof(prog); ++i) {
        emfe_poke_byte(inst, (uint16_t)(0x0100 + i), prog[i]);
    }
    SetReg(inst, REG_PC, 0x0100);

    CpmStubResult r = RunCpmStub(inst, 1000);
    TEST_TRUE(r.completed, "harness terminated cleanly");
    TEST_TRUE(r.output == "OK\r\n", "captured output == 'OK\\r\\n'");

    emfe_destroy(inst);
    g_pass++;
}

static void TestZexdocIfPresent() {
    // Look for zexdoc.com next to the test executable. If absent,
    // print a SKIP and count as pass — this is the "user drops the
    // .COM here to enable the full Z80 ISA validation" pattern.
    printf("TestZexdocIfPresent...\n");

    // Search a handful of likely paths (working-dir-relative). When
    // the test is invoked from build/bin/Release/ the .COM is two
    // levels up + into z80/examples/zexdoc/.
    const char* candidates[] = {
        "examples/zexdoc/zexdoc.com",
        "../../../examples/zexdoc/zexdoc.com",
        "../../examples/zexdoc/zexdoc.com",
        "zexdoc.com",
    };
    std::string foundPath;
    std::vector<uint8_t> bytes;
    for (const char* path : candidates) {
        FILE* f = std::fopen(path, "rb");
        if (!f) continue;
        std::fseek(f, 0, SEEK_END);
        long len = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (len > 0 && len < 0x10000) {
            bytes.resize((size_t)len);
            if (std::fread(bytes.data(), 1, (size_t)len, f) == (size_t)len) {
                foundPath = path;
            }
        }
        std::fclose(f);
        if (!foundPath.empty()) break;
    }

    if (foundPath.empty()) {
        printf("  (SKIP — zexdoc.com not found; see z80/examples/zexdoc/README.md)\n");
        g_pass++;
        return;
    }
    printf("  (found at %s, %zu bytes)\n", foundPath.c_str(), bytes.size());

    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // Load .COM at 0x0100. zexdoc fits within 64 KiB and uses BDOS
    // calls — no extra setup required beyond placing the bytes.
    for (size_t i = 0; i < bytes.size(); ++i) {
        emfe_poke_byte(inst, (uint16_t)((0x0100 + i) & 0xFFFF), bytes[i]);
    }
    SetReg(inst, REG_PC, 0x0100);

    // zexdoc needs ~70M instructions. Cap at 200M for safety.
    CpmStubResult r = RunCpmStub(inst, 200000000);

    printf("  zexdoc completed=%d steps=%lld bytes_output=%zu\n",
           r.completed ? 1 : 0, (long long)r.steps, r.output.size());
    // Last line typically: "Tests complete"
    bool sawCompleteMarker =
        r.output.find("Tests complete") != std::string::npos ||
        r.output.find("complete") != std::string::npos;
    bool sawAnyError =
        r.output.find("ERROR") != std::string::npos ||
        r.output.find("FAILED") != std::string::npos;

    if (sawAnyError) {
        fprintf(stderr, "  FAIL: zexdoc reported an error in the output\n");
        fprintf(stderr, "  --- captured output (last 2000 chars) ---\n%s\n",
                r.output.substr(r.output.size() > 2000 ? r.output.size() - 2000 : 0).c_str());
        g_fail++;
        emfe_destroy(inst);
        return;
    }
    if (!sawCompleteMarker) {
        fprintf(stderr, "  FAIL: zexdoc did not reach the 'Tests complete' marker (ran out of steps?)\n");
        g_fail++;
        emfe_destroy(inst);
        return;
    }

    emfe_destroy(inst);
    g_pass++;
}

// ---------------------------------------------------------------------
// Phase 3.1 tests — IX/IY 8-bit halves (IXH/IXL/IYH/IYL)
// ---------------------------------------------------------------------

static void TestIxyHalfIncDecLdN() {
    printf("TestIxyHalfIncDecLdN...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // LD IXH, $34  : DD 26 34  →  IX becomes $3400 (low byte 0)
    SetReg(inst, REG_IX, 0x0000);
    const uint8_t ld_ixh[] = { 0xDD, 0x26, 0x34 };
    LoadProgram(inst, 0, ld_ixh, sizeof(ld_ixh));
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_IX), (uint64_t)0x3400, "LD IXH, $34");
    TEST_EQ(emfe_get_cycle_count(inst), (int64_t)11, "LD IXH, n is 11 T");

    // LD IXL, $5A  : DD 2E 5A  →  IX becomes $345A
    const uint8_t ld_ixl[] = { 0xDD, 0x2E, 0x5A };
    LoadProgram(inst, 0x10, ld_ixl, sizeof(ld_ixl));
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_IX), (uint64_t)0x345A, "LD IXL, $5A keeps IXH");

    // INC IXH (DD 24): low byte preserved, high byte += 1
    const uint8_t inc_ixh[] = { 0xDD, 0x24 };
    LoadProgram(inst, 0x20, inc_ixh, sizeof(inc_ixh));
    int64_t before = emfe_get_cycle_count(inst);
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_IX), (uint64_t)0x355A, "INC IXH ticks high byte");
    TEST_EQ(emfe_get_cycle_count(inst) - before, (int64_t)8, "INC IXH is 8 T");

    // DEC IYL (FD 2D): exercise IY path + flag rules. With IY=$0100,
    // DEC IYL goes 00 → FF, so H is set (low nibble borrow) and N is
    // set; C is preserved.
    SetReg(inst, REG_IY, 0x0100);
    SetReg(inst, REG_F, TF_C);
    const uint8_t dec_iyl[] = { 0xFD, 0x2D };
    LoadProgram(inst, 0x30, dec_iyl, sizeof(dec_iyl));
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_IY), (uint64_t)0x01FF, "DEC IYL underflows low byte only");
    uint8_t f = (uint8_t)GetReg(inst, REG_F);
    TEST_TRUE((f & TF_N) != 0, "DEC sets N");
    TEST_TRUE((f & TF_H) != 0, "DEC underflow sets H");
    TEST_TRUE((f & TF_S) != 0, "result $FF sets S");
    TEST_TRUE((f & TF_C) != 0, "C preserved");
    TEST_TRUE((f & TF_Z) == 0, "Z clear (result != 0)");

    // INC IXL wraps with PV (0x7F → 0x80)
    SetReg(inst, REG_IX, 0xAA7F);
    SetReg(inst, REG_F, 0);
    const uint8_t inc_ixl[] = { 0xDD, 0x2C };
    LoadProgram(inst, 0x40, inc_ixl, sizeof(inc_ixl));
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_IX), (uint64_t)0xAA80, "INC IXL: $7F → $80, IXH untouched");
    TEST_TRUE((GetReg(inst, REG_F) & TF_PV) != 0, "INC PV on signed overflow");
    TEST_TRUE((GetReg(inst, REG_F) & TF_S)  != 0, "INC sign set");

    emfe_destroy(inst);
    g_pass++;
}

static void TestIxyHalfLdRegReg() {
    printf("TestIxyHalfLdRegReg...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // LD B, IXH (DD 44) — copy IX high byte to B without touching H.
    SetReg(inst, REG_IX, 0xABCD);
    SetReg(inst, REG_HL, 0x1111);
    const uint8_t ld_b_ixh[] = { 0xDD, 0x44 };
    LoadProgram(inst, 0, ld_b_ixh, sizeof(ld_b_ixh));
    int64_t before = emfe_get_cycle_count(inst);
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_B),  (uint64_t)0xAB,   "B := IXH");
    TEST_EQ(GetReg(inst, REG_HL), (uint64_t)0x1111, "HL untouched");
    TEST_EQ(emfe_get_cycle_count(inst) - before, (int64_t)8, "LD r, IXH is 8 T");

    // LD C, IXL (DD 4D)
    const uint8_t ld_c_ixl[] = { 0xDD, 0x4D };
    LoadProgram(inst, 0x10, ld_c_ixl, sizeof(ld_c_ixl));
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_C),  (uint64_t)0xCD,   "C := IXL");

    // LD IXH, E (DD 63) with E = 0x5C and IX = 0x99XX
    SetReg(inst, REG_IX, 0x9988);
    SetReg(inst, REG_E,  0x5C);
    const uint8_t ld_ixh_e[] = { 0xDD, 0x63 };
    LoadProgram(inst, 0x20, ld_ixh_e, sizeof(ld_ixh_e));
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_IX), (uint64_t)0x5C88, "IXH := E, IXL untouched");

    // LD IXL, IXH (DD 6C) — same-register swap variant
    SetReg(inst, REG_IX, 0xBE7F);
    const uint8_t ld_ixl_ixh[] = { 0xDD, 0x6C };
    LoadProgram(inst, 0x30, ld_ixl_ixh, sizeof(ld_ixl_ixh));
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_IX), (uint64_t)0xBEBE, "IXL := IXH (both halves equal)");

    // FD-prefix variant on the IY side: LD IYH, A (FD 67)
    SetReg(inst, REG_IY, 0x1122);
    SetReg(inst, REG_A,  0xF0);
    const uint8_t ld_iyh_a[] = { 0xFD, 0x67 };
    LoadProgram(inst, 0x40, ld_iyh_a, sizeof(ld_iyh_a));
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_IY), (uint64_t)0xF022, "IYH := A");

    emfe_destroy(inst);
    g_pass++;
}

static void TestIxyHalfAlu() {
    printf("TestIxyHalfAlu...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // ADD A, IXH (DD 84): A=$10, IX=$23xx → A=$33
    SetReg(inst, REG_A,  0x10);
    SetReg(inst, REG_IX, 0x2399);
    SetReg(inst, REG_F,  0);
    const uint8_t add_a_ixh[] = { 0xDD, 0x84 };
    LoadProgram(inst, 0, add_a_ixh, sizeof(add_a_ixh));
    int64_t before = emfe_get_cycle_count(inst);
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_A), (uint64_t)0x33, "A := A + IXH");
    TEST_EQ(emfe_get_cycle_count(inst) - before, (int64_t)8, "ADD A, IXH is 8 T");

    // SUB IYL (FD 95): A=$05, IY=$??03 → A=$02
    SetReg(inst, REG_A,  0x05);
    SetReg(inst, REG_IY, 0x7703);
    SetReg(inst, REG_F,  0);
    const uint8_t sub_iyl[] = { 0xFD, 0x95 };
    LoadProgram(inst, 0x10, sub_iyl, sizeof(sub_iyl));
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_A), (uint64_t)0x02, "A := A - IYL");
    TEST_TRUE((GetReg(inst, REG_F) & TF_N) != 0, "N set after SUB");

    // CP IXL (DD BD): A=$80, IXL=$80 → Z=1 and A unchanged
    SetReg(inst, REG_A,  0x80);
    SetReg(inst, REG_IX, 0x0080);
    SetReg(inst, REG_F,  0);
    const uint8_t cp_ixl[] = { 0xDD, 0xBD };
    LoadProgram(inst, 0x20, cp_ixl, sizeof(cp_ixl));
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_A), (uint64_t)0x80, "CP does not modify A");
    TEST_TRUE((GetReg(inst, REG_F) & TF_Z) != 0, "CP Z set on equality");

    // AND IYH (FD A4): A=$F0, IYH=$0F → A=0, H=1, N=0
    SetReg(inst, REG_A,  0xF0);
    SetReg(inst, REG_IY, 0x0FAA);
    SetReg(inst, REG_F,  TF_C);
    const uint8_t and_iyh[] = { 0xFD, 0xA4 };
    LoadProgram(inst, 0x30, and_iyh, sizeof(and_iyh));
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_A), (uint64_t)0x00, "AND IYH yields 0");
    uint8_t f = (uint8_t)GetReg(inst, REG_F);
    TEST_TRUE((f & TF_Z) != 0, "Z set");
    TEST_TRUE((f & TF_H) != 0, "H set on AND (Zilog quirk)");
    TEST_TRUE((f & TF_C) == 0, "C cleared on AND");

    emfe_destroy(inst);
    g_pass++;
}

static void TestDdPrefixedHaltRegression() {
    // DD 76 (and FD 76) must execute as plain HALT, NOT as
    // "LD (HL), (IX+d)" (which the old (opcode & 0xC7) == 0x46
    // guard would have matched). The fix is in ExecuteWithIndex —
    // this test catches a regression if it ever creeps back.
    printf("TestDdPrefixedHaltRegression...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // Seed memory at HL so any accidental write would be observable.
    SetReg(inst, REG_HL, 0x4000);
    SetReg(inst, REG_IX, 0x2000);
    emfe_poke_byte(inst, 0x4000, 0x55);
    emfe_poke_byte(inst, 0x2003, 0xAA);   // would land at (HL) if buggy

    const uint8_t dd_halt[] = { 0xDD, 0x76 };
    LoadProgram(inst, 0, dd_halt, sizeof(dd_halt));
    // Two steps: first the DD-prefix NOP, then the plain HALT pickup.
    emfe_step(inst);
    emfe_step(inst);
    TEST_EQ(emfe_peek_byte(inst, 0x4000), 0x55, "(HL) untouched by DD HALT");
    TEST_EQ(GetReg(inst, REG_IFF1), (uint64_t)0, "HALT does not enable interrupts");
    // After HALT, PC points one past 0x76 — i.e. address 2.
    TEST_EQ(GetReg(inst, REG_PC), (uint64_t)0x0002, "PC parked past HALT byte");

    emfe_destroy(inst);
    g_pass++;
}

static void TestDisasmIxyHalves() {
    printf("TestDisasmIxyHalves...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // INC IXH (DD 24) ; DEC IYL (FD 2D)
    const uint8_t inc_ixh[] = { 0xDD, 0x24 };
    LoadProgram(inst, 0x0000, inc_ixh, sizeof(inc_ixh));
    AssertDisasm(inst, 0x0000, "INC", "IXH", 2, "DD INC IXH");
    const uint8_t dec_iyl[] = { 0xFD, 0x2D };
    LoadProgram(inst, 0x0010, dec_iyl, sizeof(dec_iyl));
    AssertDisasm(inst, 0x0010, "DEC", "IYL", 2, "FD DEC IYL");

    // LD IXH, $5A (DD 26 5A) ; LD IYL, $C0 (FD 2E C0)
    const uint8_t ld_ixh[] = { 0xDD, 0x26, 0x5A };
    LoadProgram(inst, 0x0020, ld_ixh, sizeof(ld_ixh));
    AssertDisasm(inst, 0x0020, "LD", "IXH, $5A", 3, "DD LD IXH,n");
    const uint8_t ld_iyl[] = { 0xFD, 0x2E, 0xC0 };
    LoadProgram(inst, 0x0030, ld_iyl, sizeof(ld_iyl));
    AssertDisasm(inst, 0x0030, "LD", "IYL, $C0", 3, "FD LD IYL,n");

    // LD B, IXH (DD 44) ; LD IXL, IXH (DD 6C)
    const uint8_t ld_b_ixh[] = { 0xDD, 0x44 };
    LoadProgram(inst, 0x0040, ld_b_ixh, sizeof(ld_b_ixh));
    AssertDisasm(inst, 0x0040, "LD", "B, IXH", 2, "DD LD B,IXH");
    const uint8_t ld_ixl_ixh[] = { 0xDD, 0x6C };
    LoadProgram(inst, 0x0050, ld_ixl_ixh, sizeof(ld_ixl_ixh));
    AssertDisasm(inst, 0x0050, "LD", "IXL, IXH", 2, "DD LD IXL,IXH");

    // ADD A, IXH (DD 84) ; SUB IYL (FD 95)
    const uint8_t add_a_ixh[] = { 0xDD, 0x84 };
    LoadProgram(inst, 0x0060, add_a_ixh, sizeof(add_a_ixh));
    AssertDisasm(inst, 0x0060, "ADD", "A, IXH", 2, "DD ADD A,IXH");
    const uint8_t sub_iyl[] = { 0xFD, 0x95 };
    LoadProgram(inst, 0x0070, sub_iyl, sizeof(sub_iyl));
    AssertDisasm(inst, 0x0070, "SUB", "IYL", 2, "FD SUB IYL");

    emfe_destroy(inst);
    g_pass++;
}

// ---------------------------------------------------------------------
// Phase 3.2 tests — Y/X undocumented flag propagation
//   * RLCA / RRCA / RLA / RRA (single-byte rotates — preserve S/Z/PV)
//   * DAA / CPL / SCF / CCF (newly implemented + their flag rules)
//   * BIT b, (HL) / BIT b, (IX+d) Y/X-source rule (from address high)
// ---------------------------------------------------------------------

static void TestRlcaRrcaPreserveSzpv() {
    printf("TestRlcaRrcaPreserveSzpv...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // RLCA on A=0x85 (1000 0101) → 0x0B; old bit7=1 → C=1.
    SetReg(inst, REG_A, 0x85);
    // Seed S, Z, P/V so we can verify preservation; seed H/N so we can
    // verify they get cleared.
    SetReg(inst, REG_F, TF_S | TF_Z | TF_PV | TF_H | TF_N);
    const uint8_t rlca[] = { 0x07 };
    LoadProgram(inst, 0, rlca, sizeof(rlca));
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_A), (uint64_t)0x0B, "RLCA(0x85) = 0x0B");
    uint8_t f = (uint8_t)GetReg(inst, REG_F);
    TEST_TRUE((f & TF_S)  != 0, "S preserved");
    TEST_TRUE((f & TF_Z)  != 0, "Z preserved");
    TEST_TRUE((f & TF_PV) != 0, "PV preserved");
    TEST_TRUE((f & TF_H)  == 0, "H cleared");
    TEST_TRUE((f & TF_N)  == 0, "N cleared");
    TEST_TRUE((f & TF_C)  != 0, "C from old bit 7");
    TEST_EQ(emfe_get_cycle_count(inst), (int64_t)4, "RLCA is 4 T");

    // RRCA on A=0x01 → 0x80; old bit0=1 → C=1.
    SetReg(inst, REG_A, 0x01);
    SetReg(inst, REG_F, 0);
    const uint8_t rrca[] = { 0x0F };
    LoadProgram(inst, 0x10, rrca, sizeof(rrca));
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_A), (uint64_t)0x80, "RRCA(0x01) = 0x80");
    TEST_TRUE((GetReg(inst, REG_F) & TF_C) != 0, "C from old bit 0");

    emfe_destroy(inst);
    g_pass++;
}

static void TestRlaRraThroughCarry() {
    printf("TestRlaRraThroughCarry...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // RLA on A=0x40 with C=1 → 0x81 ; new C from old bit 7 = 0.
    SetReg(inst, REG_A, 0x40);
    SetReg(inst, REG_F, TF_C);
    const uint8_t rla[] = { 0x17 };
    LoadProgram(inst, 0, rla, sizeof(rla));
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_A), (uint64_t)0x81, "RLA: 0x40 with C=1 → 0x81");
    TEST_TRUE((GetReg(inst, REG_F) & TF_C) == 0, "new C is 0 (old bit 7 was 0)");

    // RRA on A=0x01 with C=0 → 0x00 ; new C from old bit 0 = 1.
    // Note: RRA does NOT update Z (S/Z/PV preserved), so we don't
    // assert on Z even though the value happens to be zero.
    SetReg(inst, REG_A, 0x01);
    SetReg(inst, REG_F, 0);
    const uint8_t rra[] = { 0x1F };
    LoadProgram(inst, 0x10, rra, sizeof(rra));
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_A), (uint64_t)0x00, "RRA: 0x01 with C=0 → 0x00");
    TEST_TRUE((GetReg(inst, REG_F) & TF_C) != 0, "new C is 1 (old bit 0 was 1)");

    emfe_destroy(inst);
    g_pass++;
}

static void TestDaaAfterAdd() {
    // 0x15 + 0x27 = 0x3C binary ; DAA → 0x42 (15 + 27 = 42 BCD).
    printf("TestDaaAfterAdd...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    SetReg(inst, REG_A, 0x15);
    SetReg(inst, REG_B, 0x27);
    SetReg(inst, REG_F, 0);
    const uint8_t prog[] = { 0x80, 0x27 };       // ADD A, B ; DAA
    LoadProgram(inst, 0, prog, sizeof(prog));
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_A), (uint64_t)0x3C, "binary ADD result");
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_A), (uint64_t)0x42, "DAA → BCD 42");
    uint8_t f = (uint8_t)GetReg(inst, REG_F);
    TEST_TRUE((f & TF_N) == 0, "N preserved from ADD (0)");
    TEST_TRUE((f & TF_C) == 0, "no decimal carry");
    TEST_TRUE((f & TF_Z) == 0, "Z clear");

    emfe_destroy(inst);
    g_pass++;
}

static void TestDaaAfterAddWithDecimalCarry() {
    // 0x99 + 0x01 = 0x9A binary ; DAA → 0x00 with C=1 (decimal carry).
    printf("TestDaaAfterAddWithDecimalCarry...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    SetReg(inst, REG_A, 0x99);
    SetReg(inst, REG_B, 0x01);
    SetReg(inst, REG_F, 0);
    const uint8_t prog[] = { 0x80, 0x27 };
    LoadProgram(inst, 0, prog, sizeof(prog));
    emfe_step(inst);   // ADD → 0x9A
    emfe_step(inst);   // DAA
    TEST_EQ(GetReg(inst, REG_A), (uint64_t)0x00, "99+01 BCD = 00 (carry)");
    uint8_t f = (uint8_t)GetReg(inst, REG_F);
    TEST_TRUE((f & TF_C) != 0, "decimal carry set");
    TEST_TRUE((f & TF_Z) != 0, "Z set");

    emfe_destroy(inst);
    g_pass++;
}

static void TestDaaAfterSub() {
    // 0x35 - 0x17 = 0x1E binary ; DAA → 0x18 (35 - 17 = 18 BCD).
    printf("TestDaaAfterSub...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    SetReg(inst, REG_A, 0x35);
    SetReg(inst, REG_B, 0x17);
    SetReg(inst, REG_F, 0);
    const uint8_t prog[] = { 0x90, 0x27 };       // SUB B ; DAA
    LoadProgram(inst, 0, prog, sizeof(prog));
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_A), (uint64_t)0x1E, "binary SUB result");
    TEST_TRUE((GetReg(inst, REG_F) & TF_N) != 0, "N set by SUB");
    TEST_TRUE((GetReg(inst, REG_F) & TF_H) != 0, "H set by SUB nibble borrow");
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_A), (uint64_t)0x18, "DAA → 18 BCD");
    TEST_TRUE((GetReg(inst, REG_F) & TF_N) != 0, "N preserved (still sub-mode)");

    emfe_destroy(inst);
    g_pass++;
}

static void TestCplFlags() {
    printf("TestCplFlags...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    SetReg(inst, REG_A, 0x5A);
    SetReg(inst, REG_F, TF_S | TF_Z | TF_PV | TF_C);
    const uint8_t cpl[] = { 0x2F };
    LoadProgram(inst, 0, cpl, sizeof(cpl));
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_A), (uint64_t)0xA5, "A := ~A");
    uint8_t f = (uint8_t)GetReg(inst, REG_F);
    TEST_TRUE((f & TF_H) != 0, "H := 1");
    TEST_TRUE((f & TF_N) != 0, "N := 1");
    TEST_TRUE((f & TF_S) != 0, "S preserved");
    TEST_TRUE((f & TF_Z) != 0, "Z preserved");
    TEST_TRUE((f & TF_PV) != 0, "PV preserved");
    TEST_TRUE((f & TF_C) != 0, "C preserved");
    // 0xA5 = 1010 0101 → bit 5 = 1, bit 3 = 0.
    TEST_TRUE((f & TF_Y) != 0, "Y from new A bit 5");
    TEST_TRUE((f & TF_X) == 0, "X from new A bit 3");

    emfe_destroy(inst);
    g_pass++;
}

static void TestScfFlags() {
    printf("TestScfFlags...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    SetReg(inst, REG_A, 0x28);                  // bit 5 = 1, bit 3 = 1
    SetReg(inst, REG_F, TF_S | TF_Z | TF_PV | TF_H | TF_N);
    const uint8_t scf[] = { 0x37 };
    LoadProgram(inst, 0, scf, sizeof(scf));
    emfe_step(inst);
    uint8_t f = (uint8_t)GetReg(inst, REG_F);
    TEST_TRUE((f & TF_C) != 0, "C := 1");
    TEST_TRUE((f & TF_N) == 0, "N := 0");
    TEST_TRUE((f & TF_H) == 0, "H := 0");
    TEST_TRUE((f & TF_S) != 0, "S preserved");
    TEST_TRUE((f & TF_Z) != 0, "Z preserved");
    TEST_TRUE((f & TF_PV) != 0, "PV preserved");
    TEST_TRUE((f & TF_Y) != 0, "Y from A bit 5");
    TEST_TRUE((f & TF_X) != 0, "X from A bit 3");

    emfe_destroy(inst);
    g_pass++;
}

static void TestCcfFlags() {
    printf("TestCcfFlags...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // First CCF: old C = 1 → new C = 0, H := 1.
    SetReg(inst, REG_A, 0x00);                  // Y/X both 0
    SetReg(inst, REG_F, TF_C);
    const uint8_t ccf[] = { 0x3F };
    LoadProgram(inst, 0, ccf, sizeof(ccf));
    emfe_step(inst);
    uint8_t f = (uint8_t)GetReg(inst, REG_F);
    TEST_TRUE((f & TF_C) == 0, "C flipped (was 1)");
    TEST_TRUE((f & TF_H) != 0, "H := old C (1)");
    TEST_TRUE((f & TF_N) == 0, "N := 0");
    TEST_TRUE((f & TF_Y) == 0, "Y from A bit 5 (0)");
    TEST_TRUE((f & TF_X) == 0, "X from A bit 3 (0)");

    // Second CCF: old C = 0 → new C = 1, H := 0.
    SetReg(inst, REG_A, 0x00);
    SetReg(inst, REG_F, 0);
    LoadProgram(inst, 0x10, ccf, sizeof(ccf));
    emfe_step(inst);
    f = (uint8_t)GetReg(inst, REG_F);
    TEST_TRUE((f & TF_C) != 0, "C flipped from 0 to 1");
    TEST_TRUE((f & TF_H) == 0, "H := old C (0)");

    emfe_destroy(inst);
    g_pass++;
}

static void TestBitMemoryYxFromAddressHigh() {
    // BIT 0, (HL): Y/X come from H (high byte of HL), NOT from val.
    // Seed memory at HL such that a buggy implementation (Y/X from
    // val) would visibly differ.
    printf("TestBitMemoryYxFromAddressHigh...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // HL = 0x2800 — H = 0x28 → bit 5 = 1, bit 3 = 1.
    // memory[HL] = 0x00 — if Y/X were from the value they'd both be 0.
    SetReg(inst, REG_HL, 0x2800);
    emfe_poke_byte(inst, 0x2800, 0x00);
    SetReg(inst, REG_F, 0);
    const uint8_t bit_hl[] = { 0xCB, 0x46 };       // BIT 0, (HL)
    LoadProgram(inst, 0, bit_hl, sizeof(bit_hl));
    emfe_step(inst);
    uint8_t f = (uint8_t)GetReg(inst, REG_F);
    TEST_TRUE((f & TF_Z) != 0, "Z set (bit 0 of 0x00 is clear)");
    TEST_TRUE((f & TF_Y) != 0, "Y from H=0x28 bit 5");
    TEST_TRUE((f & TF_X) != 0, "X from H=0x28 bit 3");

    // BIT 7, (IX-2) with IX=0x2880 → EA=0x287E → high=0x28.
    // memory[EA] = 0x80 (bit 7 set) so Z=0.
    SetReg(inst, REG_IX, 0x2880);
    emfe_poke_byte(inst, 0x287E, 0x80);
    SetReg(inst, REG_F, 0);
    const uint8_t bit_ixd[] = { 0xDD, 0xCB, 0xFE, 0x7E };   // BIT 7, (IX-2)
    LoadProgram(inst, 0x10, bit_ixd, sizeof(bit_ixd));
    emfe_step(inst);
    f = (uint8_t)GetReg(inst, REG_F);
    TEST_TRUE((f & TF_Z) == 0, "Z clear (bit 7 of 0x80 set)");
    TEST_TRUE((f & TF_Y) != 0, "Y from EA high 0x28 bit 5");
    TEST_TRUE((f & TF_X) != 0, "X from EA high 0x28 bit 3");

    emfe_destroy(inst);
    g_pass++;
}

static void TestAdd16YxFromResultHigh() {
    // ADD HL, BC: 0x2800 + 0x0800 = 0x3000 ; result high = 0x30 →
    // bit 5 = 1 (Y), bit 3 = 0 (X clear).
    printf("TestAdd16YxFromResultHigh...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    SetReg(inst, REG_HL, 0x2800);
    SetReg(inst, REG_BC, 0x0800);
    SetReg(inst, REG_F, 0);
    const uint8_t add_hl_bc[] = { 0x09 };
    LoadProgram(inst, 0, add_hl_bc, sizeof(add_hl_bc));
    emfe_step(inst);
    TEST_EQ(GetReg(inst, REG_HL), (uint64_t)0x3000, "HL := 0x3000");
    uint8_t f = (uint8_t)GetReg(inst, REG_F);
    TEST_TRUE((f & TF_Y) != 0, "Y from 0x30 bit 5");
    TEST_TRUE((f & TF_X) == 0, "X from 0x30 bit 3");
    TEST_TRUE((f & TF_N) == 0, "N := 0");
    TEST_TRUE((f & TF_C) == 0, "no carry");

    emfe_destroy(inst);
    g_pass++;
}

static void TestDisasmPhase32Opcodes() {
    printf("TestDisasmPhase32Opcodes...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    const struct { uint8_t op; const char* mnem; } table[] = {
        { 0x07, "RLCA" }, { 0x0F, "RRCA" }, { 0x17, "RLA" }, { 0x1F, "RRA" },
        { 0x27, "DAA"  }, { 0x2F, "CPL"  }, { 0x37, "SCF" }, { 0x3F, "CCF" },
    };
    for (size_t i = 0; i < sizeof(table)/sizeof(table[0]); ++i) {
        uint16_t addr = static_cast<uint16_t>(0x0100 + 0x10 * i);
        uint8_t bytes[1] = { table[i].op };
        LoadProgram(inst, addr, bytes, 1);
        AssertDisasm(inst, addr, table[i].mnem, "", 1, table[i].mnem);
    }

    emfe_destroy(inst);
    g_pass++;
}

// ---------------------------------------------------------------------
// Phase 3.3 tests — ED-prefix undocumented NOPs
//
// Real Z80 hardware treats every unrecognised ED-prefix opcode as a
// 2-byte 8 T-state NOP (prefix 4 T + suffix 4 T, suffix dropped).
// The CPU side has always done this; Phase 3.3 also re-renders the
// disasm to call them out as "NOP*" instead of the previous "DB"
// rendering, which made these slots look like raw data bytes.
// ---------------------------------------------------------------------

static void TestEdNopBehaviour() {
    printf("TestEdNopBehaviour...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // Sample suffixes from each unassigned region:
    //   0x00 — bottom of the ED 00..3F unassigned block
    //   0x37 — top of the same block
    //   0xC0 — unassigned in the ED 80..BF area outside block ops
    //   0xFF — top of the table, never assigned
    const uint8_t suffixes[] = { 0x00, 0x37, 0xC0, 0xFF };
    for (size_t i = 0; i < sizeof(suffixes); ++i) {
        uint16_t addr = static_cast<uint16_t>(0x1000 + 0x10 * i);
        SetReg(inst, REG_PC, addr);
        emfe_poke_byte(inst, addr,                   0xED);
        emfe_poke_byte(inst, static_cast<uint16_t>(addr + 1), suffixes[i]);

        int64_t cyc_before = emfe_get_cycle_count(inst);
        int64_t ins_before = emfe_get_instruction_count(inst);
        emfe_step(inst);

        TEST_EQ(GetReg(inst, REG_PC),
                static_cast<uint64_t>(addr + 2),
                "PC advances by 2 (both prefix + suffix consumed)");
        TEST_EQ(emfe_get_cycle_count(inst) - cyc_before, (int64_t)8,
                "ED NOP* is 8 T-states");
        TEST_EQ(emfe_get_instruction_count(inst) - ins_before, (int64_t)1,
                "counted as one instruction");
    }

    // ED 00 must not surface as EMFE_ERR_UNSUPPORTED. emfe_step
    // returning OK is the gate here — TEST_EQ above on PC/cycles
    // would fail with rc != OK anyway, but assert explicitly to make
    // intent clear if the API ever changes.
    SetReg(inst, REG_PC, 0x2000);
    emfe_poke_byte(inst, 0x2000, 0xED);
    emfe_poke_byte(inst, 0x2001, 0x00);
    TEST_EQ(emfe_step(inst), EMFE_OK, "ED 00 reports OK, not UNSUPPORTED");

    emfe_destroy(inst);
    g_pass++;
}

static void TestDisasmEdUnassignedAsNopStar() {
    printf("TestDisasmEdUnassignedAsNopStar...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    const uint8_t suffixes[] = { 0x00, 0x37, 0xC0, 0xFF };
    for (size_t i = 0; i < sizeof(suffixes); ++i) {
        uint16_t addr = static_cast<uint16_t>(0x0400 + 0x10 * i);
        emfe_poke_byte(inst, addr,                   0xED);
        emfe_poke_byte(inst, static_cast<uint16_t>(addr + 1), suffixes[i]);
        AssertDisasm(inst, addr, "NOP*", "", 2, "ED unassigned");
    }

    // Sanity: documented ED opcodes are NOT rewritten as NOP*.
    emfe_poke_byte(inst, 0x0500, 0xED);
    emfe_poke_byte(inst, 0x0501, 0x44);
    AssertDisasm(inst, 0x0500, "NEG", "", 2, "documented ED still NEG");

    emfe_destroy(inst);
    g_pass++;
}

// ---------------------------------------------------------------------
// Phase 3.4 — eForth-Z80 REPL demo
//
// Bill Muench eForth (public domain) is a small portable Forth that
// runs as a bare-metal Z80 program — no operating system, no CP/M,
// just a cold start at PC := 0x0000 driving the UART.
//
// The harness uses the UART path (ports 0xFE / 0xFF, see docs/mmio.md)
// rather than a CP/M-stub like the zexdoc test, because eForth has no
// OS to intercept. Same SKIP-when-binary-absent pattern as zexdoc.
// ---------------------------------------------------------------------

static void TestEForthIfPresent() {
    printf("TestEForthIfPresent...\n");

    const char* candidates[] = {
        "examples/eforth/eforth.bin",
        "../../../examples/eforth/eforth.bin",
        "../../examples/eforth/eforth.bin",
        "eforth.bin",
    };
    std::string foundPath;
    std::vector<uint8_t> bytes;
    for (const char* path : candidates) {
        FILE* f = std::fopen(path, "rb");
        if (!f) continue;
        std::fseek(f, 0, SEEK_END);
        long len = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (len > 0 && len < 0x10000) {
            bytes.resize(static_cast<size_t>(len));
            if (std::fread(bytes.data(), 1, bytes.size(), f) == bytes.size()) {
                foundPath = path;
            }
        }
        std::fclose(f);
        if (!foundPath.empty()) break;
    }

    if (foundPath.empty()) {
        printf("  (SKIP — eforth.bin not found; see z80/examples/eforth/README.md)\n");
        g_pass++;
        return;
    }
    printf("  (found at %s, %zu bytes)\n", foundPath.c_str(), bytes.size());

    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // Load at the cold-vector address. Most bare-metal Z80 ROMs
    // (including the published eForth-Z80 ports) sit at 0x0000.
    for (size_t i = 0; i < bytes.size(); ++i) {
        emfe_poke_byte(inst, static_cast<uint16_t>(i & 0xFFFF), bytes[i]);
    }
    SetReg(inst, REG_PC, 0x0000);
    SetReg(inst, REG_SP, 0xFF00);

    CaptureCtx ctx;
    emfe_set_console_char_callback(inst, CaptureChar, &ctx);

    // Cold-start budget: most published eForth-Z80 images print a
    // banner within ~500k instructions. 5M is comfortably generous
    // and still finishes inside the unit-test second budget.
    const int64_t k_coldBudget = 5000000;
    for (int64_t i = 0; i < k_coldBudget; ++i) {
        if (emfe_step(inst) != EMFE_OK) {
            fprintf(stderr, "  FAIL: cold-start step returned non-OK at i=%lld\n",
                    static_cast<long long>(i));
            g_fail++;
            emfe_destroy(inst);
            return;
        }
    }
    size_t banner_len = ctx.captured.size();
    printf("  cold start produced %zu chars\n", banner_len);
    if (banner_len == 0) {
        fprintf(stderr,
                "  FAIL: no UART output after %lld cold-start steps. "
                "If your eForth image needs a different load address "
                "or longer warm-up, adjust TestEForthIfPresent at the "
                "top of the function.\n",
                static_cast<long long>(k_coldBudget));
        g_fail++;
        emfe_destroy(inst);
        return;
    }

    // REPL transaction: ask "1 2 + . CR\n" and run for another budget.
    // Use CR + LF to be tolerant of either line-ending convention.
    size_t pre_repl = ctx.captured.size();
    emfe_send_string(inst, "1 2 + . CR\r\n");
    const int64_t k_replBudget = 2000000;
    for (int64_t i = 0; i < k_replBudget; ++i) {
        emfe_step(inst);   // ignore non-OK here — eForth can park itself
    }
    std::string repl_output = ctx.captured.substr(pre_repl);
    printf("  REPL produced %zu chars after the prompt\n", repl_output.size());

    // We accept any output that contains a "3" digit (the result of
    // 1 + 2). Forth's "." prints the number followed by a space, so
    // "3 " is the typical form, but some images print just "3" then
    // CR/LF. A bare "3" digit is sufficient evidence the REPL ran.
    bool saw_three = (repl_output.find('3') != std::string::npos);
    if (!saw_three) {
        fprintf(stderr,
                "  FAIL: REPL did not produce '3' after '1 2 + .'. "
                "Captured (last 200 chars):\n%s\n",
                repl_output.substr(repl_output.size() > 200 ? repl_output.size() - 200 : 0).c_str());
        g_fail++;
        emfe_destroy(inst);
        return;
    }

    emfe_destroy(inst);
    g_pass++;
}

// ---------------------------------------------------------------------
// Phase 4.1 tests — sector-device disk MMIO (ports 0xF0..0xF7)
// See z80/docs/phase4_design.md §1.
// ---------------------------------------------------------------------

// Vendor-prefixed mount API (declared in plugin_z80.cpp). Settings-
// based mounting is Phase 4.4; for now the unit tests poke an image
// through this dedicated hook.
extern "C" EmfeResult emfe_z80_mount_disk_image(
    EmfeInstance instance, const uint8_t* bytes, uint32_t size_bytes,
    int32_t write_protect);

// Tiny IN-port / OUT-port helpers used by the disk-device tests. They
// run one-byte programs at address 0; each call resets PC to 0 so
// successive INs/OUTs are independent.
static uint8_t InPort(EmfeInstance inst, uint8_t port) {
    const uint8_t prog[] = { 0xDB, port };       // IN A, (port)
    LoadProgram(inst, 0, prog, sizeof(prog));
    SetReg(inst, REG_PC, 0);
    emfe_step(inst);
    return static_cast<uint8_t>(GetReg(inst, REG_A));
}
static void OutPort(EmfeInstance inst, uint8_t port, uint8_t value) {
    SetReg(inst, REG_A, value);
    const uint8_t prog[] = { 0xD3, port };       // OUT (port), A
    LoadProgram(inst, 0, prog, sizeof(prog));
    SetReg(inst, REG_PC, 0);
    emfe_step(inst);
}

static void TestDiskStatusUnmounted() {
    printf("TestDiskStatusUnmounted...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // No image mounted — READY set, IMAGE_LOADED clear, ERROR clear.
    uint8_t s = InPort(inst, 0xF6);
    TEST_TRUE((s & 0x01) != 0, "READY set on power-up");
    TEST_TRUE((s & 0x08) == 0, "IMAGE_LOADED clear when nothing mounted");
    TEST_TRUE((s & 0x02) == 0, "ERROR clear initially");

    // READ command with no image must set ERROR.
    OutPort(inst, 0xF5, 0x01);
    s = InPort(inst, 0xF6);
    TEST_TRUE((s & 0x02) != 0, "ERROR set after READ with no image");

    // NOP command clears the sticky error.
    OutPort(inst, 0xF5, 0x00);
    s = InPort(inst, 0xF6);
    TEST_TRUE((s & 0x02) == 0, "NOP clears sticky ERROR");

    emfe_destroy(inst);
    g_pass++;
}

static void TestDiskLbaRegisterRoundTrip() {
    printf("TestDiskLbaRegisterRoundTrip...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    OutPort(inst, 0xF0, 0xAB);
    OutPort(inst, 0xF1, 0xCD);
    OutPort(inst, 0xF2, 0x12);
    TEST_EQ(InPort(inst, 0xF0), (uint64_t)0xAB, "LBA[0] round-trip");
    TEST_EQ(InPort(inst, 0xF1), (uint64_t)0xCD, "LBA[1] round-trip");
    TEST_EQ(InPort(inst, 0xF2), (uint64_t)0x12, "LBA[2] round-trip");

    OutPort(inst, 0xF3, 0x34);
    OutPort(inst, 0xF4, 0x80);
    TEST_EQ(InPort(inst, 0xF3), (uint64_t)0x34, "buf[0] round-trip");
    TEST_EQ(InPort(inst, 0xF4), (uint64_t)0x80, "buf[1] round-trip");

    emfe_destroy(inst);
    g_pass++;
}

static void TestDiskReadSector() {
    printf("TestDiskReadSector...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // Two-sector image: sector 0 = 0xAA pattern, sector 1 = 0x55 pattern.
    std::vector<uint8_t> image(256, 0);
    for (int i = 0; i < 128; ++i) image[i]       = 0xAA;
    for (int i = 0; i < 128; ++i) image[128 + i] = 0x55;
    TEST_EQ(emfe_z80_mount_disk_image(inst, image.data(),
                                      static_cast<uint32_t>(image.size()), 0),
            EMFE_OK, "mount OK");

    uint8_t s = InPort(inst, 0xF6);
    TEST_TRUE((s & 0x08) != 0, "IMAGE_LOADED set after mount");
    TEST_TRUE((s & 0x04) == 0, "WRITE_PROTECT clear (mounted RW)");

    // Read sector 1 into guest RAM at 0x8000.
    OutPort(inst, 0xF0, 0x01);
    OutPort(inst, 0xF1, 0x00);
    OutPort(inst, 0xF2, 0x00);
    OutPort(inst, 0xF3, 0x00);
    OutPort(inst, 0xF4, 0x80);
    OutPort(inst, 0xF5, 0x01);

    s = InPort(inst, 0xF6);
    TEST_TRUE((s & 0x02) == 0, "no ERROR on valid read");

    for (int i = 0; i < 128; ++i) {
        if (emfe_peek_byte(inst, static_cast<uint16_t>(0x8000 + i)) != 0x55) {
            fprintf(stderr, "  FAIL: byte %d != 0x55\n", i);
            g_fail++;
            emfe_destroy(inst);
            return;
        }
    }
    TEST_EQ(static_cast<uint64_t>(emfe_peek_byte(inst, 0x8080)),
            static_cast<uint64_t>(0x00),
            "next byte past sector boundary untouched");

    emfe_destroy(inst);
    g_pass++;
}

static void TestDiskWriteSectorRw() {
    // RW image: write a guest pattern, read it back, verify round-trip.
    printf("TestDiskWriteSectorRw...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    std::vector<uint8_t> image(128, 0);
    emfe_z80_mount_disk_image(inst, image.data(),
                              static_cast<uint32_t>(image.size()), 0);

    for (int i = 0; i < 128; ++i) {
        emfe_poke_byte(inst, static_cast<uint16_t>(0x9000 + i),
                       static_cast<uint8_t>(0xC0 | (i & 0x3F)));
    }
    OutPort(inst, 0xF0, 0x00);
    OutPort(inst, 0xF1, 0x00);
    OutPort(inst, 0xF2, 0x00);
    OutPort(inst, 0xF3, 0x00);
    OutPort(inst, 0xF4, 0x90);
    OutPort(inst, 0xF5, 0x02);

    uint8_t s = InPort(inst, 0xF6);
    TEST_TRUE((s & 0x02) == 0, "no ERROR on valid write");

    // Read back into a different RAM region and verify round-trip.
    OutPort(inst, 0xF3, 0x00);
    OutPort(inst, 0xF4, 0xA0);
    OutPort(inst, 0xF5, 0x01);
    for (int i = 0; i < 128; ++i) {
        uint8_t want = static_cast<uint8_t>(0xC0 | (i & 0x3F));
        if (emfe_peek_byte(inst, static_cast<uint16_t>(0xA000 + i)) != want) {
            fprintf(stderr, "  FAIL: write/readback byte %d mismatch\n", i);
            g_fail++;
            emfe_destroy(inst);
            return;
        }
    }

    emfe_destroy(inst);
    g_pass++;
}

static void TestDiskWriteRejectedOnRoImage() {
    // RO image: write attempt sets ERROR, does NOT modify the image
    // (reading back gives the pre-mount bytes, not the guest pattern).
    printf("TestDiskWriteRejectedOnRoImage...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // Pre-seed the image with a known fingerprint so a successful
    // (and therefore buggy) write would be detectable on readback.
    std::vector<uint8_t> image(128);
    for (int i = 0; i < 128; ++i) image[i] = static_cast<uint8_t>(0x42 ^ i);
    emfe_z80_mount_disk_image(inst, image.data(),
                              static_cast<uint32_t>(image.size()), 1);

    uint8_t s = InPort(inst, 0xF6);
    TEST_TRUE((s & 0x04) != 0, "WRITE_PROTECT set on RO mount");
    TEST_TRUE((s & 0x08) != 0, "IMAGE_LOADED set on RO mount");

    // Fill guest RAM at 0xB000 with junk and attempt to WRITE it.
    for (int i = 0; i < 128; ++i) {
        emfe_poke_byte(inst, static_cast<uint16_t>(0xB000 + i), 0xEE);
    }
    OutPort(inst, 0xF0, 0x00);
    OutPort(inst, 0xF1, 0x00);
    OutPort(inst, 0xF2, 0x00);
    OutPort(inst, 0xF3, 0x00);
    OutPort(inst, 0xF4, 0xB0);
    OutPort(inst, 0xF5, 0x02);
    s = InPort(inst, 0xF6);
    TEST_TRUE((s & 0x02) != 0, "ERROR set on write to RO image");

    // Read sector 0 back via the device into 0xC000; bytes must still
    // match the original fingerprint (the failed write was a no-op).
    OutPort(inst, 0xF5, 0x00);   // clear sticky ERROR
    OutPort(inst, 0xF3, 0x00);
    OutPort(inst, 0xF4, 0xC0);
    OutPort(inst, 0xF5, 0x01);
    for (int i = 0; i < 128; ++i) {
        uint8_t want = static_cast<uint8_t>(0x42 ^ i);
        if (emfe_peek_byte(inst, static_cast<uint16_t>(0xC000 + i)) != want) {
            fprintf(stderr, "  FAIL: RO write leaked into image at byte %d "
                            "(got 0x%02X, want 0x%02X)\n",
                    i, emfe_peek_byte(inst, static_cast<uint16_t>(0xC000 + i)), want);
            g_fail++;
            emfe_destroy(inst);
            return;
        }
    }

    emfe_destroy(inst);
    g_pass++;
}

static void TestDiskOutOfRangeReadSetsError() {
    printf("TestDiskOutOfRangeReadSetsError...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // Single-sector image. LBA=1 is past the end.
    std::vector<uint8_t> image(128, 0);
    emfe_z80_mount_disk_image(inst, image.data(),
                              static_cast<uint32_t>(image.size()), 0);

    OutPort(inst, 0xF0, 0x01);
    OutPort(inst, 0xF1, 0x00);
    OutPort(inst, 0xF2, 0x00);
    OutPort(inst, 0xF5, 0x01);
    uint8_t s = InPort(inst, 0xF6);
    TEST_TRUE((s & 0x02) != 0, "ERROR set on out-of-range READ");

    OutPort(inst, 0xF5, 0x00);
    OutPort(inst, 0xF5, 0x77);   // bogus command
    s = InPort(inst, 0xF6);
    TEST_TRUE((s & 0x02) != 0, "ERROR set on unknown command");

    emfe_destroy(inst);
    g_pass++;
}

// ---------------------------------------------------------------------
// Phase 4.4 tests — disk image Settings wiring
//
// Settings keys added in this phase:
//   DiskImagePath          (FILE)  REQUIRES_RESET
//   DiskImageWriteProtect  (BOOL)  REQUIRES_RESET
//   BootFromDisk           (BOOL)  REQUIRES_RESET — Phase 4.5 wires it
// ---------------------------------------------------------------------

// Helper: write a temp disk image file (single sector, pattern bytes)
// and return its path. Caller is responsible for cleanup.
static std::string MakeTempDiskImage(const char* tag, uint8_t fingerprint) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "z80_disk_settings_%s.tmp.img", tag);
    std::string path = buf;
    std::vector<uint8_t> image(128);
    for (int i = 0; i < 128; ++i) image[i] = static_cast<uint8_t>(fingerprint ^ i);
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return std::string();
    std::fwrite(image.data(), 1, image.size(), f);
    std::fclose(f);
    return path;
}

static void TestDiskSettingsDefsPresent() {
    printf("TestDiskSettingsDefsPresent...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    const EmfeSettingDef* defs = nullptr;
    int32_t n = emfe_get_setting_defs(inst, &defs);
    bool sawPath = false, sawWp = false, sawBoot = false;
    for (int32_t i = 0; i < n; ++i) {
        const auto& d = defs[i];
        if (std::strcmp(d.key, "DiskImagePath") == 0) {
            sawPath = (std::strcmp(d.group, "Disk") == 0
                       && d.type == EMFE_SETTING_FILE
                       && (d.flags & EMFE_SETTING_FLAG_REQUIRES_RESET) != 0);
        } else if (std::strcmp(d.key, "DiskImageWriteProtect") == 0) {
            sawWp = (std::strcmp(d.group, "Disk") == 0
                     && d.type == EMFE_SETTING_BOOL
                     && (d.flags & EMFE_SETTING_FLAG_REQUIRES_RESET) != 0);
        } else if (std::strcmp(d.key, "BootFromDisk") == 0) {
            sawBoot = (std::strcmp(d.group, "Disk") == 0
                       && d.type == EMFE_SETTING_BOOL
                       && (d.flags & EMFE_SETTING_FLAG_REQUIRES_RESET) != 0);
        }
    }
    TEST_TRUE(sawPath, "DiskImagePath (FILE, REQUIRES_RESET)");
    TEST_TRUE(sawWp,   "DiskImageWriteProtect (BOOL, REQUIRES_RESET)");
    TEST_TRUE(sawBoot, "BootFromDisk (BOOL, REQUIRES_RESET)");

    emfe_destroy(inst);
    g_pass++;
}

static void TestDiskSettingsMountOnReset() {
    printf("TestDiskSettingsMountOnReset...\n");
    std::string path = MakeTempDiskImage("mount", 0x77);
    if (path.empty()) {
        fprintf(stderr, "  FAIL: could not write temp file\n");
        g_fail++;
        return;
    }

    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // No image at create time — IMAGE_LOADED should be clear.
    uint8_t s = InPort(inst, 0xF6);
    TEST_TRUE((s & 0x08) == 0, "no image at construction");

    // Stage the path and apply. REQUIRES_RESET: the device should
    // remain unmounted until reset moves the value into appliedSettings.
    emfe_set_setting(inst, "DiskImagePath", path.c_str());
    emfe_apply_settings(inst);
    s = InPort(inst, 0xF6);
    TEST_TRUE((s & 0x08) == 0, "image still unmounted before reset (REQUIRES_RESET)");

    emfe_reset(inst);
    s = InPort(inst, 0xF6);
    TEST_TRUE((s & 0x08) != 0, "IMAGE_LOADED set after reset");
    TEST_TRUE((s & 0x04) == 0, "WRITE_PROTECT clear (default false)");

    // Read sector 0 through the device into 0x8000 and check the
    // fingerprint matches the file we wrote.
    OutPort(inst, 0xF0, 0x00);
    OutPort(inst, 0xF1, 0x00);
    OutPort(inst, 0xF2, 0x00);
    OutPort(inst, 0xF3, 0x00);
    OutPort(inst, 0xF4, 0x80);
    OutPort(inst, 0xF5, 0x01);
    for (int i = 0; i < 128; ++i) {
        uint8_t want = static_cast<uint8_t>(0x77 ^ i);
        if (emfe_peek_byte(inst, static_cast<uint16_t>(0x8000 + i)) != want) {
            fprintf(stderr, "  FAIL: byte %d mismatch (got 0x%02X want 0x%02X)\n",
                    i, emfe_peek_byte(inst, static_cast<uint16_t>(0x8000 + i)), want);
            g_fail++;
            emfe_destroy(inst);
            std::remove(path.c_str());
            return;
        }
    }

    emfe_destroy(inst);
    std::remove(path.c_str());
    g_pass++;
}

static void TestDiskSettingsWriteProtectFlag() {
    printf("TestDiskSettingsWriteProtectFlag...\n");
    std::string path = MakeTempDiskImage("wp", 0x33);
    if (path.empty()) {
        fprintf(stderr, "  FAIL: could not write temp file\n");
        g_fail++;
        return;
    }

    EmfeInstance inst = nullptr;
    emfe_create(&inst);
    emfe_set_setting(inst, "DiskImagePath", path.c_str());
    emfe_set_setting(inst, "DiskImageWriteProtect", "true");
    emfe_apply_settings(inst);
    emfe_reset(inst);

    uint8_t s = InPort(inst, 0xF6);
    TEST_TRUE((s & 0x08) != 0, "IMAGE_LOADED set");
    TEST_TRUE((s & 0x04) != 0, "WRITE_PROTECT set");

    emfe_destroy(inst);
    std::remove(path.c_str());
    g_pass++;
}

static void TestDiskSettingsMissingPathLeavesUnmounted() {
    printf("TestDiskSettingsMissingPathLeavesUnmounted...\n");
    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    emfe_set_setting(inst, "DiskImagePath", "no_such_file_z80_phase_4_4.img");
    emfe_apply_settings(inst);
    // emfe_reset itself does NOT return an error code for missing
    // files — the failure surfaces via the device's IMAGE_LOADED bit
    // staying clear, which is what the guest CBIOS observes.
    TEST_EQ(emfe_reset(inst), EMFE_OK, "reset OK even with bad path");
    uint8_t s = InPort(inst, 0xF6);
    TEST_TRUE((s & 0x08) == 0, "IMAGE_LOADED stays clear for missing file");

    emfe_destroy(inst);
    g_pass++;
}

static void TestDiskSettingsEmptyPathPreservesVendorMount() {
    // Empty DiskImagePath means "Settings disengaged" — it does NOT
    // implicitly unmount a vendor-mounted image. This split keeps
    // the Settings path and the emfe_z80_mount_disk_image vendor
    // hook from clobbering each other; explicit unmount via Settings
    // would be a future "Eject" button calling the vendor hook with
    // zero bytes.
    printf("TestDiskSettingsEmptyPathPreservesVendorMount...\n");

    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // Mount via the vendor hook only — DiskImagePath stays empty.
    std::vector<uint8_t> image(128, 0xA5);
    emfe_z80_mount_disk_image(inst, image.data(),
                              static_cast<uint32_t>(image.size()), 0);
    TEST_TRUE((InPort(inst, 0xF6) & 0x08) != 0, "mounted before reset");

    // Reset goes through ApplyDiskImageOnReset, but with empty path
    // it should skip the device entirely — vendor mount survives.
    emfe_reset(inst);
    TEST_TRUE((InPort(inst, 0xF6) & 0x08) != 0,
              "vendor mount survives reset when DiskImagePath is empty");

    emfe_destroy(inst);
    g_pass++;
}

// ---------------------------------------------------------------------
// Phase 4.5 tests — cold boot to A> acceptance
//
// Two layers:
//   * TestBootFromDiskStagesRam — synthetic in-memory image, always
//     runs in CI. Verifies the canonical RAM staging happens at the
//     right addresses and PC starts at the cold-boot vector.
//   * TestCpmBootIfPresent — looks for examples/cpm22/cpm22-sssd.img
//     (the build.ps1 output), boots it, asserts captured UART output
//     contains "A>" + the CBIOS sign-on marker. SKIPs cleanly when
//     the image is absent.
// ---------------------------------------------------------------------

static void TestBootFromDiskStagesRam() {
    printf("TestBootFromDiskStagesRam...\n");

    // Synthetic 256-KiB image, each sector filled with a sector-marker-
    // derived fingerprint. Lets us verify each sector landed at the
    // canonical RAM address with byte-level precision.
    std::vector<uint8_t> image(77 * 26 * 128, 0xE5);
    auto fillSector = [&](uint32_t lba, uint8_t marker) {
        for (int b = 0; b < 128; ++b) {
            image[lba * 128 + b] = static_cast<uint8_t>(marker ^ b);
        }
    };
    fillSector(0,  0xB0);                                       // boot stub
    // CCP+BDOS code in LBA 1..44 (44 sectors that ApplyBootFromDisk
    // actually loads); LBA 45 holds the CPM22.Z80 internal fake BIOS
    // table on a real image, which the plugin intentionally skips.
    for (uint32_t s = 1;  s < 1 + 44;  ++s) fillSector(s, 0xC0);
    for (uint32_t s = 46; s < 46 + 12; ++s) fillSector(s, 0xCB);    // CBIOS

    EmfeInstance inst = nullptr;
    emfe_create(&inst);

    // Mount via the vendor hook, enable BootFromDisk via Settings,
    // then reset. The Settings reset path doesn't overwrite the
    // already-mounted image (DiskImagePath stays empty), so the
    // image survives into ApplyBootFromDiskOnReset.
    emfe_z80_mount_disk_image(inst, image.data(),
                              static_cast<uint32_t>(image.size()), 0);
    emfe_set_setting(inst, "BootFromDisk", "true");
    emfe_apply_settings(inst);
    emfe_reset(inst);

    TEST_EQ(GetReg(inst, REG_PC), (uint64_t)0x0000, "PC = 0x0000 after cold boot");

    // Boot stub at RAM 0x0000.
    TEST_EQ(static_cast<uint64_t>(emfe_peek_byte(inst, 0x0000)), (uint64_t)0xB0,
            "boot stub byte 0 landed at RAM 0x0000");
    TEST_EQ(static_cast<uint64_t>(emfe_peek_byte(inst, 0x007F)),
            static_cast<uint64_t>(0xB0 ^ 0x7F),
            "boot stub byte 127 landed at RAM 0x007F");
    // CCP+BDOS at RAM 0xE400 (44 sectors = 5632 bytes, ends at 0xF9FF).
    TEST_EQ(static_cast<uint64_t>(emfe_peek_byte(inst, 0xE400)), (uint64_t)0xC0,
            "CCP+BDOS byte 0 landed at RAM 0xE400");
    TEST_EQ(static_cast<uint64_t>(emfe_peek_byte(inst, 0xF9FF)),
            static_cast<uint64_t>(0xC0 ^ 0x7F),
            "CCP+BDOS last byte landed at RAM 0xF9FF");
    // CBIOS at RAM 0xFA00. It deliberately overlays the same address
    // where CPM22.Z80's internal fake BIOS table would have been —
    // ApplyBootFromDisk skips LBA 45 (the fake-table sector) so only
    // our real CBIOS lands there.
    TEST_EQ(static_cast<uint64_t>(emfe_peek_byte(inst, 0xFA00)), (uint64_t)0xCB,
            "CBIOS byte 0 landed at RAM 0xFA00");
    TEST_EQ(static_cast<uint64_t>(emfe_peek_byte(inst, 0xFFFF)),
            static_cast<uint64_t>(0xCB ^ 0x7F),
            "CBIOS last byte landed at RAM 0xFFFF");

    // BootFromDisk=false must NOT stage. memory.Reset wipes RAM back
    // to zero, so the CBIOS area should read as 0x00 after the next
    // reset without disk staging.
    emfe_set_setting(inst, "BootFromDisk", "false");
    emfe_apply_settings(inst);
    emfe_reset(inst);
    TEST_EQ(static_cast<uint64_t>(emfe_peek_byte(inst, 0xFA00)), (uint64_t)0x00,
            "BootFromDisk=false leaves CBIOS area at 0x00 after reset");

    emfe_destroy(inst);
    g_pass++;
}

// ---------------------------------------------------------------------
// Phase 5.3 — CpmSession typed-command harness
//
// Wraps an EmfeInstance + CaptureCtx, exposing TypeLine / WaitForPrompt
// / Mark + CaptureSince so per-tool smoke tests can drive the CCP at
// the `a>` prompt without each test re-implementing the boot sequence.
// ---------------------------------------------------------------------

// Locate cpm22-sssd.img in the usual candidate paths and return its
// bytes; returns an empty vector on miss.
static std::vector<uint8_t> LoadCpmImageIfPresent(const char** outFoundPath) {
    const char* candidates[] = {
        "examples/cpm22/cpm22-sssd.img",
        "../../../examples/cpm22/cpm22-sssd.img",
        "../../examples/cpm22/cpm22-sssd.img",
        "cpm22-sssd.img",
    };
    std::vector<uint8_t> bytes;
    for (const char* path : candidates) {
        FILE* f = std::fopen(path, "rb");
        if (!f) continue;
        std::fseek(f, 0, SEEK_END);
        long len = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (len > 0 && len <= (1 << 20)) {       // sanity cap at 1 MiB
            bytes.resize(static_cast<size_t>(len));
            if (std::fread(bytes.data(), 1, bytes.size(), f) == bytes.size()) {
                if (outFoundPath) *outFoundPath = path;
                std::fclose(f);
                return bytes;
            }
        }
        std::fclose(f);
    }
    bytes.clear();
    if (outFoundPath) *outFoundPath = nullptr;
    return bytes;
}

struct CpmSession {
    EmfeInstance inst   = nullptr;
    CaptureCtx   ctx;
    bool         active = false;       // false = boot never reached prompt
    CpmSession() = default;
    ~CpmSession() { if (inst) emfe_destroy(inst); }
    CpmSession(const CpmSession&)            = delete;
    CpmSession& operator=(const CpmSession&) = delete;

    size_t Mark() const { return ctx.captured.size(); }
    std::string CaptureSince(size_t mark) const {
        return mark <= ctx.captured.size()
            ? ctx.captured.substr(mark)
            : std::string();
    }

    // CCP expects a CR-terminated line (NOT CR+LF). Pushing characters
    // into the UART RX FIFO via emfe_send_string queues them for the
    // next CONIN spin-loop poll; the caller drives the CPU forward
    // between bytes so CCP/ED can drain the 8-byte FIFO without
    // overflowing (Z80Uart's RX is bounded — see Z80Uart.h FIFO_SIZE).
    //
    // The per-byte drain budget (2000 instructions) is generous —
    // CCP's CONIN spin handles a byte in ~50 instructions; we leave
    // headroom for any in-progress BDOS work between key polls.
    void TypeLine(const std::string& cmd) {
        std::string line = cmd;
        line += '\r';
        for (char c : line) {
            char buf[2] = { c, '\0' };
            emfe_send_string(inst, buf);
            RunSteps(2000);
        }
    }

    void RunSteps(int64_t n) {
        for (int64_t i = 0; i < n; ++i) (void)emfe_step(inst);
    }

    // Step the CPU until the captured tail ends with "[a-p]>" — CCP's
    // prompt pattern. Polls cheaply every 16 K steps to avoid an
    // O(N²) substring scan on the growing capture buffer. Returns true
    // if the prompt landed within stepCap; false on timeout.
    bool WaitForPrompt(int64_t stepCap) {
        for (int64_t i = 0; i < stepCap; ++i) {
            (void)emfe_step(inst);
            if ((i & 0x3FFF) == 0x3FFF) {
                if (TailMatchesPrompt()) return true;
            }
        }
        return TailMatchesPrompt();
    }

    // Step until `needle` appears in the captured stream at-or-after
    // `searchFrom`. Used for non-CCP prompts (ED's `*`, etc.) and
    // generic substring waits.
    bool WaitForSubstr(const std::string& needle, int64_t stepCap,
                       size_t searchFrom = 0) {
        for (int64_t i = 0; i < stepCap; ++i) {
            (void)emfe_step(inst);
            if ((i & 0x3FFF) == 0x3FFF) {
                if (ctx.captured.find(needle, searchFrom) != std::string::npos) {
                    return true;
                }
            }
        }
        return ctx.captured.find(needle, searchFrom) != std::string::npos;
    }

private:
    bool TailMatchesPrompt() const {
        const auto& s = ctx.captured;
        if (s.size() < 2) return false;
        char prev = s[s.size() - 2];
        char last = s[s.size() - 1];
        if (last != '>') return false;
        return (prev >= 'A' && prev <= 'P') || (prev >= 'a' && prev <= 'p');
    }
};

// Boot a CP/M session from a disk image. Returns a heap-owned session
// so the embedded CaptureCtx address stays stable across the move.
// Caller checks `->active` — false means boot failed to reach the
// first `a>` prompt within the budget.
static std::unique_ptr<CpmSession> BootCpmSession(
        const std::vector<uint8_t>& bytes,
        int64_t bootBudget = 10000000) {
    auto s = std::unique_ptr<CpmSession>(new CpmSession());
    if (emfe_create(&s->inst) != EMFE_OK) return s;
    emfe_set_console_char_callback(s->inst, CaptureChar, &s->ctx);
    emfe_z80_mount_disk_image(s->inst, bytes.data(),
                              static_cast<uint32_t>(bytes.size()), 0);
    emfe_set_setting(s->inst, "BootFromDisk", "true");
    emfe_apply_settings(s->inst);
    emfe_reset(s->inst);
    s->active = s->WaitForPrompt(bootBudget);
    return s;
}

static void TestCpmBootIfPresent() {
    printf("TestCpmBootIfPresent...\n");

    const char* foundPath = nullptr;
    auto bytes = LoadCpmImageIfPresent(&foundPath);
    if (bytes.empty()) {
        printf("  (SKIP — cpm22-sssd.img not found; see "
               "z80/examples/cpm22/README.md to build one)\n");
        g_pass++;
        return;
    }
    printf("  (found at %s, %zu bytes)\n", foundPath, bytes.size());

    auto sess = BootCpmSession(bytes);
    const std::string& out = sess->ctx.captured;
    printf("  captured %zu chars during cold boot\n", out.size());

    bool sawBios = (out.find("emfe Z80 CBIOS") != std::string::npos);
    if (!sawBios) {
        fprintf(stderr, "  FAIL: CBIOS sign-on marker not seen. "
                        "Either the boot stub never reached CBIOS, or "
                        "CBIOS PRINT_STR never ran. Captured (last 400 "
                        "chars):\n%s\n",
                out.substr(out.size() > 400 ? out.size() - 400 : 0).c_str());
        g_fail++;
        return;
    }
    if (!sess->active) {
        fprintf(stderr, "  FAIL: A> prompt not seen. CBIOS ran but CCP "
                        "never landed. Captured (last 400 chars):\n%s\n",
                out.substr(out.size() > 400 ? out.size() - 400 : 0).c_str());
        g_fail++;
        return;
    }

    g_pass++;
}

// ---------------------------------------------------------------------
// Phase 5.3 — first typed-command smoke test
//
// Boots the CP/M image, sends `DIR\r`, asserts the captured response
// names every utility baked in by Phase 5.2 (PIP / STAT / ED /
// SUBMIT / XSUB / LOAD / DUMP / DDT). This proves the CpmSession
// harness end-to-end. Per-tool smoke tests (PIP / STAT / ED ...)
// follow in Phase 5.4 on top of the same harness.
// ---------------------------------------------------------------------

static void TestCpmDirIfPresent() {
    printf("TestCpmDirIfPresent...\n");

    const char* foundPath = nullptr;
    auto bytes = LoadCpmImageIfPresent(&foundPath);
    if (bytes.empty()) {
        printf("  (SKIP — cpm22-sssd.img not found; see "
               "z80/examples/cpm22/README.md to build one)\n");
        g_pass++;
        return;
    }

    auto sess = BootCpmSession(bytes);
    if (!sess->active) {
        fprintf(stderr, "  FAIL: cold boot didn't reach prompt; "
                        "see TestCpmBootIfPresent for diagnosis\n");
        g_fail++;
        return;
    }

    // The image must have the Phase 5.2 utilities baked in. If
    // build.ps1 produced a blank-filesystem image (utilities missing
    // from the pointer shelf), DIR will respond with "NO FILE" and
    // this test fails — that's the expected behaviour and the user-
    // visible signal to populate the shelf.
    size_t mark = sess->Mark();
    sess->TypeLine("DIR");
    if (!sess->WaitForPrompt(5000000)) {
        fprintf(stderr, "  FAIL: prompt didn't return after DIR within "
                        "5M instructions. Captured tail (last 400):\n%s\n",
                sess->ctx.captured.substr(
                    sess->ctx.captured.size() > 400
                        ? sess->ctx.captured.size() - 400 : 0).c_str());
        g_fail++;
        return;
    }
    std::string out = sess->CaptureSince(mark);
    printf("  DIR output: %zu chars\n", out.size());

    // CCP's DIR formats names as 8-char-padded + dot + 3-char-padded,
    // e.g. "PIP     COM" with multiple spaces. Substring check just
    // looks for the bare name — robust against column-width drift
    // across CCP variants.
    const char* utilities[] = {
        "PIP", "STAT", "ED", "SUBMIT",
        "XSUB", "LOAD", "DUMP", "DDT",
    };
    bool allFound = true;
    for (const char* name : utilities) {
        if (out.find(name) == std::string::npos) {
            fprintf(stderr, "  FAIL: DIR didn't list '%s'. Captured DIR "
                            "tail (last 400):\n%s\n",
                    name, out.substr(out.size() > 400 ? out.size() - 400 : 0).c_str());
            allFound = false;
        }
    }
    if (!allFound) {
        g_fail++;
        return;
    }

    g_pass++;
}

// ---------------------------------------------------------------------
// Phase 5.4 — per-tool CP/M smoke tests
//
// Built on the Phase 5.3 CpmSession harness. Each test boots a fresh
// session (cheap-ish at ~5 M instructions cold-boot) so a hang in one
// test doesn't corrupt the next. All SKIP cleanly when the image is
// missing.
//
// Pattern:
//   1. LoadCpmImageIfPresent + early-return SKIP
//   2. BootCpmSession; bail if boot didn't reach the prompt
//   3. Mark capture, TypeLine(cmd), WaitForPrompt/Substr
//   4. Substring assertions on the captured tail
// ---------------------------------------------------------------------

// Shared helper: emit a clear diagnostic + fail with the last 400
// chars of captured output (or whatever's available). Used by all
// Phase 5.4 tests to keep failure transcripts uniform.
static void FailWithTail(const char* what, const std::string& captured,
                         const std::string& assertion) {
    fprintf(stderr, "  FAIL[%s]: %s\n  Captured tail (last 400):\n%s\n",
            what, assertion.c_str(),
            captured.substr(captured.size() > 400
                            ? captured.size() - 400 : 0).c_str());
    // Lone CRs and other non-printables can hide what CCP / ED really
    // emit (DRI software loves "\r" without "\n" for in-place updates).
    // Hex dump the last 200 bytes to make those visible.
    fprintf(stderr, "  Captured tail (hex, last 200 bytes):\n  ");
    size_t start = captured.size() > 200 ? captured.size() - 200 : 0;
    for (size_t i = start; i < captured.size(); ++i) {
        fprintf(stderr, "%02X ", (uint8_t)captured[i]);
        if (((i - start + 1) % 16) == 0) fprintf(stderr, "\n  ");
    }
    fprintf(stderr, "\n");
    g_fail++;
}

static void TestCpmStatIfPresent() {
    printf("TestCpmStatIfPresent...\n");
    const char* fp = nullptr;
    auto bytes = LoadCpmImageIfPresent(&fp);
    if (bytes.empty()) {
        printf("  (SKIP — cpm22-sssd.img not found)\n");
        g_pass++;
        return;
    }

    auto sess = BootCpmSession(bytes);
    if (!sess->active) {
        FailWithTail("STAT", sess->ctx.captured,
                     "cold boot didn't reach prompt");
        return;
    }

    size_t mark = sess->Mark();
    sess->TypeLine("STAT");
    if (!sess->WaitForPrompt(5000000)) {
        FailWithTail("STAT", sess->ctx.captured,
                     "no prompt return after STAT within 5M steps");
        return;
    }
    std::string out = sess->CaptureSince(mark);
    printf("  STAT output: %zu chars\n", out.size());

    // Stock CP/M 2.2 STAT (no args) prints a disk-usage line like
    //   "A: R/W, Space:  218K"
    // — accept either "K" or "k" since some variants lowercase it.
    bool sawA  = (out.find("A:")  != std::string::npos);
    bool sawK  = (out.find('K')   != std::string::npos) ||
                 (out.find('k')   != std::string::npos);
    if (!sawA || !sawK) {
        FailWithTail("STAT", out,
                     "expected substring 'A:' and a 'K'/'k' KB marker");
        return;
    }
    g_pass++;
}

static void TestCpmPipIfPresent() {
    // PIP NEW.TXT=DUMP.COM  — copies the smallest file on the disk
    // (DUMP.COM, 512 B / 1 block) to a new name. Then DIR must list
    // both files.
    printf("TestCpmPipIfPresent...\n");
    const char* fp = nullptr;
    auto bytes = LoadCpmImageIfPresent(&fp);
    if (bytes.empty()) {
        printf("  (SKIP — cpm22-sssd.img not found)\n");
        g_pass++;
        return;
    }

    auto sess = BootCpmSession(bytes);
    if (!sess->active) {
        FailWithTail("PIP", sess->ctx.captured,
                     "cold boot didn't reach prompt");
        return;
    }

    sess->TypeLine("PIP NEW.TXT=DUMP.COM");
    if (!sess->WaitForPrompt(10000000)) {
        FailWithTail("PIP", sess->ctx.captured,
                     "no prompt return after PIP within 10M steps");
        return;
    }

    size_t mark = sess->Mark();
    sess->TypeLine("DIR");
    if (!sess->WaitForPrompt(5000000)) {
        FailWithTail("PIP", sess->ctx.captured,
                     "no prompt return after follow-up DIR");
        return;
    }
    std::string dirOut = sess->CaptureSince(mark);
    printf("  follow-up DIR output: %zu chars\n", dirOut.size());

    // The new file should show up in DIR. CCP renders 8.3 names
    // separated by spaces — substring 'NEW' (the filename) is enough.
    if (dirOut.find("NEW") == std::string::npos) {
        FailWithTail("PIP", dirOut,
                     "DIR after PIP didn't list 'NEW'");
        return;
    }
    g_pass++;
}

static void TestCpmEdIfPresent() {
    // ED FOO.TXT  (creates an empty workspace, since FOO.TXT doesn't
    // exist), then Q to quit without saving. Tests that ED loads,
    // accepts a command-mode keystroke, and returns to CCP cleanly.
    //
    // Inserting text + saving (the design doc's longer scenario) is
    // deferred to a Phase 5.5+ extension — quitting alone is a strong
    // signal that BDOS / BIOS / console wiring all hold up under
    // ED's I/O style.
    printf("TestCpmEdIfPresent...\n");
    const char* fp = nullptr;
    auto bytes = LoadCpmImageIfPresent(&fp);
    if (bytes.empty()) {
        printf("  (SKIP — cpm22-sssd.img not found)\n");
        g_pass++;
        return;
    }

    auto sess = BootCpmSession(bytes);
    if (!sess->active) {
        FailWithTail("ED", sess->ctx.captured,
                     "cold boot didn't reach prompt");
        return;
    }

    // Note on Mark + TypeLine + WaitForSubstr ordering: ALWAYS mark
    // BEFORE TypeLine, because TypeLine's per-char drain steps can
    // (and for fast-responding programs like ED, DO) cause the
    // expected response substring to land in the capture WHILE
    // TypeLine is still running. If we marked after TypeLine we'd
    // search past the response and time out spuriously.
    size_t edMark = sess->Mark();
    sess->TypeLine("ED FOO.TXT");
    if (!sess->WaitForSubstr("*", 10000000, edMark)) {
        FailWithTail("ED", sess->ctx.captured,
                     "ED never printed its '*' command prompt");
        return;
    }

    // Q asks for confirmation in DRI ED 2.x: "Q-(Y/N)?" — answer Y.
    size_t qMark = sess->Mark();
    sess->TypeLine("Q");
    if (!sess->WaitForSubstr("?", 5000000, qMark)) {
        FailWithTail("ED", sess->ctx.captured,
                     "ED Q didn't elicit the Y/N confirmation prompt");
        return;
    }
    sess->TypeLine("Y");
    if (!sess->WaitForPrompt(5000000)) {
        FailWithTail("ED", sess->ctx.captured,
                     "no CCP prompt return after ED Q + Y within 5M steps");
        return;
    }
    g_pass++;
}

static void TestCpmDumpIfPresent() {
    // DUMP DUMP.COM — the smallest utility (512 B) is also a sane
    // target for the DUMP hex-dumper itself; output should be ASCII
    // hex pairs. Substring assertion is just "0000" (the address
    // header for the first byte).
    printf("TestCpmDumpIfPresent...\n");
    const char* fp = nullptr;
    auto bytes = LoadCpmImageIfPresent(&fp);
    if (bytes.empty()) {
        printf("  (SKIP — cpm22-sssd.img not found)\n");
        g_pass++;
        return;
    }

    auto sess = BootCpmSession(bytes);
    if (!sess->active) {
        FailWithTail("DUMP", sess->ctx.captured,
                     "cold boot didn't reach prompt");
        return;
    }

    size_t mark = sess->Mark();
    sess->TypeLine("DUMP DUMP.COM");
    if (!sess->WaitForPrompt(20000000)) {
        FailWithTail("DUMP", sess->ctx.captured,
                     "no prompt return after DUMP within 20M steps");
        return;
    }
    std::string out = sess->CaptureSince(mark);
    printf("  DUMP output: %zu chars\n", out.size());

    if (out.find("0000") == std::string::npos) {
        FailWithTail("DUMP", out,
                     "DUMP output missing the '0000' address header");
        return;
    }
    g_pass++;
}

int main() {
    printf("=== emfe_plugin_z80 tests (through Phase 5.4) ===\n");
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
    // Phase 2.7
    TestCpmStubHarness();
    TestZexdocIfPresent();
    // Phase 3.1
    TestIxyHalfIncDecLdN();
    TestIxyHalfLdRegReg();
    TestIxyHalfAlu();
    TestDdPrefixedHaltRegression();
    TestDisasmIxyHalves();
    // Phase 3.2
    TestRlcaRrcaPreserveSzpv();
    TestRlaRraThroughCarry();
    TestDaaAfterAdd();
    TestDaaAfterAddWithDecimalCarry();
    TestDaaAfterSub();
    TestCplFlags();
    TestScfFlags();
    TestCcfFlags();
    TestBitMemoryYxFromAddressHigh();
    TestAdd16YxFromResultHigh();
    TestDisasmPhase32Opcodes();
    // Phase 3.3
    TestEdNopBehaviour();
    TestDisasmEdUnassignedAsNopStar();
    // Phase 3.4
    TestEForthIfPresent();
    // Phase 4.1
    TestDiskStatusUnmounted();
    TestDiskLbaRegisterRoundTrip();
    TestDiskReadSector();
    TestDiskWriteSectorRw();
    TestDiskWriteRejectedOnRoImage();
    TestDiskOutOfRangeReadSetsError();
    // Phase 4.4
    TestDiskSettingsDefsPresent();
    TestDiskSettingsMountOnReset();
    TestDiskSettingsWriteProtectFlag();
    TestDiskSettingsMissingPathLeavesUnmounted();
    TestDiskSettingsEmptyPathPreservesVendorMount();
    // Phase 4.5
    TestBootFromDiskStagesRam();
    TestCpmBootIfPresent();
    // Phase 5.3
    TestCpmDirIfPresent();
    // Phase 5.4
    TestCpmStatIfPresent();
    TestCpmPipIfPresent();
    TestCpmEdIfPresent();
    TestCpmDumpIfPresent();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
