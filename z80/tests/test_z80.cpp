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

int main() {
    printf("=== emfe_plugin_z80 smoke tests (Phase 1.1) ===\n");
    TestNegotiate();
    TestBoardInfo();
    TestLifecycle();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
