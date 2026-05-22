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

static void TestStepReturnsUnsupported() {
    // Phase 1.2: no opcodes implemented yet, so emfe_step must trap.
    printf("TestStepReturnsUnsupported...\n");

    EmfeInstance inst = nullptr;
    TEST_EQ(emfe_create(&inst), EMFE_OK, "create OK");
    TEST_EQ(emfe_step(inst), EMFE_ERR_UNSUPPORTED, "step traps as unsupported");
    TEST_EQ(emfe_get_instruction_count(inst), (int64_t)0,
            "no instruction executed");
    emfe_destroy(inst);
    g_pass++;
}

int main() {
    printf("=== emfe_plugin_z80 tests (through Phase 1.2) ===\n");
    TestNegotiate();
    TestBoardInfo();
    TestLifecycle();
    TestMemoryPeekPoke();
    TestConsoleSendChar();
    TestConsoleCallback();
    TestStepReturnsUnsupported();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
