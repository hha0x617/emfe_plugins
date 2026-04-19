/*
 * Z8000Disasm.cpp - Z8000 Family Disassembler
 *
 * Phase 1a coverage: mirrors the instruction set implemented in Z8000Cpu.cpp.
 * Everything else falls through to a "DW $xxxx" data-word rendering.
 *
 * Copyright (c) 2026 emfe Project
 * SPDX-License-Identifier: MIT
 */
#include "pch.h"
#include "Z8000Cpu.h"
#include "Z8000Memory.h"

#include <cstdio>
#include <string>

namespace {

const char* WordRegName(int n) {
    static const char* names[16] = {
        "R0","R1","R2","R3","R4","R5","R6","R7",
        "R8","R9","R10","R11","R12","R13","R14","R15"
    };
    return names[n & 0x0F];
}

const char* ByteRegName(int n) {
    // n = 0..7  -> RH0..RH7 (high byte)
    // n = 8..15 -> RL0..RL7 (low byte of R0..R7)
    static const char* names[16] = {
        "RH0","RH1","RH2","RH3","RH4","RH5","RH6","RH7",
        "RL0","RL1","RL2","RL3","RL4","RL5","RL6","RL7"
    };
    return names[n & 0x0F];
}

const char* ConditionName(int cc) {
    // Z8000 manual mnemonics (alternate syntax in parens, not emitted)
    static const char* names[16] = {
        "F",   // 0x0 never
        "LT",  // 0x1
        "LE",  // 0x2
        "ULE", // 0x3
        "OV",  // 0x4 (PE)
        "MI",  // 0x5
        "Z",   // 0x6 (EQ)
        "C",   // 0x7 (ULT)
        "",    // 0x8 always — emit as unconditional
        "GE",  // 0x9
        "GT",  // 0xA
        "UGT", // 0xB
        "NOV", // 0xC (PO)
        "PL",  // 0xD
        "NE",  // 0xE (NZ)
        "NC",  // 0xF (UGE)
    };
    return names[cc & 0x0F];
}

std::string FormatRegRegWord(uint16_t op) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%s,%s",
                  WordRegName(op & 0x0F),
                  WordRegName((op >> 4) & 0x0F));
    return buf;
}

std::string FormatRegRegByte(uint16_t op) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%s,%s",
                  ByteRegName(op & 0x0F),
                  ByteRegName((op >> 4) & 0x0F));
    return buf;
}

} // anonymous namespace

Z8000Cpu::DisasmResult Z8000Cpu::Disassemble(uint16_t addr) const {
    DisasmResult out{};
    uint16_t op = Memory ? Memory->PeekWord(addr) : 0;

    char raw[12];
    std::snprintf(raw, sizeof(raw), "%04X", op);
    out.rawBytes = raw;
    out.length = 2;

    uint8_t hi = static_cast<uint8_t>(op >> 8);

    // Register-register ALU / LD
    struct { uint8_t hi; const char* mnem; bool isByte; } rr_table[] = {
        {0x80, "ADDB", true},  {0x81, "ADD",  false},
        {0x82, "SUBB", true},  {0x83, "SUB",  false},
        {0x84, "ORB",  true},  {0x85, "OR",   false},
        {0x86, "ANDB", true},  {0x87, "AND",  false},
        {0x88, "XORB", true},  {0x89, "XOR",  false},
        {0x8A, "CPB",  true},  {0x8B, "CP",   false},
        {0xA0, "LDB",  true},  {0xA1, "LD",   false},
    };
    for (auto& e : rr_table) {
        if (hi == e.hi) {
            out.mnemonic = e.mnem;
            out.operands = e.isByte ? FormatRegRegByte(op) : FormatRegRegWord(op);
            return out;
        }
    }

    // LDB Rbd,#imm8  ($Cdnn)
    if ((hi & 0xF0) == 0xC0) {
        int d = (op >> 8) & 0x0F;
        uint8_t imm = static_cast<uint8_t>(op & 0xFF);
        out.mnemonic = "LDB";
        char buf[20];
        std::snprintf(buf, sizeof(buf), "%s,#$%02X", ByteRegName(d), imm);
        out.operands = buf;
        return out;
    }

    // LD Rd,#imm16  ($210d imm16)
    if (hi == 0x21 && (op & 0xF0) == 0x00) {
        int d = op & 0x0F;
        uint16_t imm = Memory ? Memory->PeekWord(static_cast<uint16_t>(addr + 2)) : 0;
        out.length = 4;
        char rawEx[12];
        std::snprintf(rawEx, sizeof(rawEx), "%04X%04X", op, imm);
        out.rawBytes = rawEx;
        out.mnemonic = "LD";
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%s,#$%04X", WordRegName(d), imm);
        out.operands = buf;
        return out;
    }

    // JR cc,disp  ($Ecdd)
    if ((hi & 0xF0) == 0xE0) {
        uint8_t cc = hi & 0x0F;
        int8_t disp = static_cast<int8_t>(op & 0xFF);
        uint16_t target = static_cast<uint16_t>(addr + 2 + static_cast<int16_t>(disp) * 2);
        out.mnemonic = "JR";
        char buf[24];
        const char* ccName = ConditionName(cc);
        if (*ccName) std::snprintf(buf, sizeof(buf), "%s,$%04X", ccName, target);
        else         std::snprintf(buf, sizeof(buf), "$%04X", target);
        out.operands = buf;
        return out;
    }

    // --- Phase 1b: memory via register indirect ---
    // LDB Rbd,@Rs  = $20[s][d]   (s≠0)
    // LD  Rd,@Rs   = $21[s][d]   (s≠0; s=0 falls through to immediate below)
    // LDB @Rd,Rbs  = $2E[d][s]
    // LD  @Rd,Rs   = $2F[d][s]
    if (hi == 0x20 || (hi == 0x21 && ((op >> 4) & 0x0F) != 0) || hi == 0x2E || hi == 0x2F) {
        bool isByte = (hi == 0x20) || (hi == 0x2E);
        bool isStore = (hi == 0x2E) || (hi == 0x2F);
        out.mnemonic = isStore ? (isByte ? "LDB" : "LD") : (isByte ? "LDB" : "LD");
        char buf[24];
        if (isStore) {
            int d = (op >> 4) & 0x0F, s = op & 0x0F;
            std::snprintf(buf, sizeof(buf), "@%s,%s", WordRegName(d),
                          isByte ? ByteRegName(s) : WordRegName(s));
        } else {
            int s = (op >> 4) & 0x0F, d = op & 0x0F;
            std::snprintf(buf, sizeof(buf), "%s,@%s",
                          isByte ? ByteRegName(d) : WordRegName(d), WordRegName(s));
        }
        out.operands = buf;
        return out;
    }

    // --- Phase 1b: memory via direct address ---
    // LDB Rbd,addr = $70[0][d] addr16 ; LD Rd,addr = $71[0][d] addr16
    // LDB addr,Rbs = $78[0][s] addr16 ; LD addr,Rs = $79[0][s] addr16
    if ((hi == 0x70 || hi == 0x71 || hi == 0x78 || hi == 0x79) && ((op >> 4) & 0x0F) == 0) {
        bool isByte = (hi == 0x70) || (hi == 0x78);
        bool isStore = (hi == 0x78) || (hi == 0x79);
        int r = op & 0x0F;
        uint16_t a = Memory ? Memory->PeekWord(static_cast<uint16_t>(addr + 2)) : 0;
        out.length = 4;
        char rawEx[12];
        std::snprintf(rawEx, sizeof(rawEx), "%04X%04X", op, a);
        out.rawBytes = rawEx;
        out.mnemonic = isByte ? "LDB" : "LD";
        char buf[24];
        if (isStore) std::snprintf(buf, sizeof(buf), "$%04X,%s", a,
                                   isByte ? ByteRegName(r) : WordRegName(r));
        else         std::snprintf(buf, sizeof(buf), "%s,$%04X",
                                   isByte ? ByteRegName(r) : WordRegName(r), a);
        out.operands = buf;
        return out;
    }

    // --- Phase 1b: stack ---
    if (hi == 0x13) {  // PUSH @Rd,Rs
        int d = (op >> 4) & 0x0F, s = op & 0x0F;
        out.mnemonic = "PUSH";
        char buf[20]; std::snprintf(buf, sizeof(buf), "@%s,%s", WordRegName(d), WordRegName(s));
        out.operands = buf;
        return out;
    }
    if (hi == 0x17) {  // POP Rd,@Rs
        int s = (op >> 4) & 0x0F, d = op & 0x0F;
        out.mnemonic = "POP";
        char buf[20]; std::snprintf(buf, sizeof(buf), "%s,@%s", WordRegName(d), WordRegName(s));
        out.operands = buf;
        return out;
    }

    // --- Phase 1b: subroutines / jumps ---
    if ((hi & 0xF0) == 0xD0) {
        uint16_t disp = op & 0x0FFF;
        uint16_t target = static_cast<uint16_t>(addr + 2 - disp * 2);
        out.mnemonic = "CALR";
        char buf[16]; std::snprintf(buf, sizeof(buf), "$%04X", target);
        out.operands = buf;
        return out;
    }
    if (hi == 0x5F) {
        if ((op & 0x00FF) == 0) { // CALL addr
            uint16_t a = Memory ? Memory->PeekWord(static_cast<uint16_t>(addr + 2)) : 0;
            out.length = 4;
            char rawEx[12]; std::snprintf(rawEx, sizeof(rawEx), "%04X%04X", op, a);
            out.rawBytes = rawEx;
            out.mnemonic = "CALL";
            char buf[16]; std::snprintf(buf, sizeof(buf), "$%04X", a);
            out.operands = buf;
        } else { // CALL @Rd
            int d = (op >> 4) & 0x0F;
            out.mnemonic = "CALL";
            char buf[16]; std::snprintf(buf, sizeof(buf), "@%s", WordRegName(d));
            out.operands = buf;
        }
        return out;
    }
    if (hi == 0x5E) {
        uint8_t cc = op & 0x0F;
        uint16_t a = Memory ? Memory->PeekWord(static_cast<uint16_t>(addr + 2)) : 0;
        out.length = 4;
        char rawEx[12]; std::snprintf(rawEx, sizeof(rawEx), "%04X%04X", op, a);
        out.rawBytes = rawEx;
        out.mnemonic = "JP";
        const char* ccName = ConditionName(cc);
        char buf[24];
        if (*ccName) std::snprintf(buf, sizeof(buf), "%s,$%04X", ccName, a);
        else         std::snprintf(buf, sizeof(buf), "$%04X", a);
        out.operands = buf;
        return out;
    }
    if (hi == 0x1E) {
        int d = (op >> 4) & 0x0F;
        uint8_t cc = op & 0x0F;
        out.mnemonic = "JP";
        const char* ccName = ConditionName(cc);
        char buf[24];
        if (*ccName) std::snprintf(buf, sizeof(buf), "%s,@%s", ccName, WordRegName(d));
        else         std::snprintf(buf, sizeof(buf), "@%s", WordRegName(d));
        out.operands = buf;
        return out;
    }
    if (hi == 0x9E) {
        uint8_t cc = op & 0x0F;
        out.mnemonic = "RET";
        const char* ccName = ConditionName(cc);
        out.operands = *ccName ? ccName : "";
        return out;
    }
    if (hi == 0x7C && (op & 0xF8) == 0x00) {
        out.mnemonic = (op & 0x04) ? "EI" : "DI";
        uint8_t mask = op & 0x03;
        char buf[16];
        // mask bit1=VI, bit0=NVI
        if (mask == 0x03)      std::snprintf(buf, sizeof(buf), "VI,NVI");
        else if (mask == 0x02) std::snprintf(buf, sizeof(buf), "VI");
        else if (mask == 0x01) std::snprintf(buf, sizeof(buf), "NVI");
        else                   buf[0] = 0;
        out.operands = buf;
        return out;
    }

    // --- Phase 1b: I/O indirect ($3C-$3F) ---
    if (hi == 0x3C || hi == 0x3D || hi == 0x3E || hi == 0x3F) {
        bool isWord = (hi & 0x01);
        bool isOut  = (hi & 0x02);
        int a = (op >> 4) & 0x0F; // pointer
        int b = op & 0x0F;        // register
        out.mnemonic = isOut ? (isWord ? "OUT" : "OUTB") : (isWord ? "IN" : "INB");
        char buf[24];
        if (isOut) std::snprintf(buf, sizeof(buf), "@%s,%s", WordRegName(a),
                                 isWord ? WordRegName(b) : ByteRegName(b));
        else       std::snprintf(buf, sizeof(buf), "%s,@%s",
                                 isWord ? WordRegName(b) : ByteRegName(b), WordRegName(a));
        out.operands = buf;
        return out;
    }

    // --- Phase 1b: I/O immediate ($3A/$3B sub=4,6) ---
    if ((hi == 0x3A || hi == 0x3B) && ((op & 0x0F) == 0x04 || (op & 0x0F) == 0x06)) {
        bool isWord = (hi == 0x3B);
        bool isOut  = ((op & 0x0F) == 0x06);
        int reg = (op >> 4) & 0x0F;
        uint16_t port = Memory ? Memory->PeekWord(static_cast<uint16_t>(addr + 2)) : 0;
        out.length = 4;
        char rawEx[12]; std::snprintf(rawEx, sizeof(rawEx), "%04X%04X", op, port);
        out.rawBytes = rawEx;
        out.mnemonic = isOut ? (isWord ? "OUT" : "OUTB") : (isWord ? "IN" : "INB");
        char buf[24];
        if (isOut) std::snprintf(buf, sizeof(buf), "#$%04X,%s", port,
                                 isWord ? WordRegName(reg) : ByteRegName(reg));
        else       std::snprintf(buf, sizeof(buf), "%s,#$%04X",
                                 isWord ? WordRegName(reg) : ByteRegName(reg), port);
        out.operands = buf;
        return out;
    }

    // Single-word specials
    if (op == 0x8D07) { out.mnemonic = "NOP";  out.operands = ""; return out; }
    if (op == 0x7A00) { out.mnemonic = "HALT"; out.operands = ""; return out; }
    if (op == 0x7B00) { out.mnemonic = "IRET"; out.operands = ""; return out; }

    // Fallback — emit as data word
    out.mnemonic = "DW";
    char ops[16];
    std::snprintf(ops, sizeof(ops), "$%04X", op);
    out.operands = ops;
    return out;
}
