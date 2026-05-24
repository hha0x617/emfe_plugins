// SPDX-License-Identifier: MIT OR Apache-2.0
// Copyright (c) 2026 hha0x617
//
// Z80Disasm.cpp - Documented Z80 instruction set disassembler.
//
// Mirrors the prefix dispatch of Z80Cpu::ExecuteOne; produces text
// rather than executing. Undocumented opcodes (IXH/IXL halves,
// DD CB d xx register-echo, etc.) fall through to a `DB <bytes>`
// representation until Phase 3 adds them on the execution side.

#include "pch.h"
#include "Z80Disasm.h"
#include "Z80Memory.h"

namespace {
    // ----- Tables --------------------------------------------------------
    const char* const k_reg8[8] = {
        "B", "C", "D", "E", "H", "L", "(HL)", "A"
    };
    const char* const k_pair_sp[4] = {     // LD rr,nn / ADD HL,rr / INC rr
        "BC", "DE", "HL", "SP"
    };
    const char* const k_pair_af[4] = {     // PUSH / POP — slot 11 is AF, not SP
        "BC", "DE", "HL", "AF"
    };
    const char* const k_cc[8] = {
        "NZ", "Z", "NC", "C", "PO", "PE", "P", "M"
    };
    const char* const k_alu_mnem[8] = {
        "ADD", "ADC", "SUB", "SBC", "AND", "XOR", "OR", "CP"
    };
    // For ALU-on-A the operand form prefixes "A," for ADD/ADC/SBC.
    // SUB/AND/XOR/OR/CP traditionally written without the explicit A.
    bool k_alu_takes_a_prefix[8] = {
        true, true, false, true, false, false, false, false
    };
    const char* const k_rot_mnem[8] = {
        "RLC", "RRC", "RL", "RR", "SLA", "SRA", "SLL", "SRL"
    };

    // ----- Small formatters ---------------------------------------------
    std::string hex2(uint8_t v) {
        char buf[4];
        std::snprintf(buf, sizeof(buf), "%02X", v);
        return std::string(buf);
    }
    std::string hex4(uint16_t v) {
        char buf[6];
        std::snprintf(buf, sizeof(buf), "%04X", v);
        return std::string(buf);
    }
    std::string disp_str(int8_t d) {
        // Signed-displacement formatter for (IX+d) / (IY+d) etc.
        // Always renders the sign explicitly.
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%c%d", d < 0 ? '-' : '+', d < 0 ? -d : d);
        return std::string(buf);
    }
    std::string imm8(uint8_t v)  { return "$" + hex2(v); }
    std::string imm16(uint16_t v) { return "$" + hex4(v); }
    std::string ind16(uint16_t v) { return "($" + hex4(v) + ")"; }
    std::string indr8 (uint8_t v) { return "($" + hex2(v) + ")"; }

    // ----- Reader: pulls bytes from memory and accumulates raw hex ------
    struct Reader {
        Z80Memory* mem;
        uint16_t   pc;
        int        length = 0;
        std::string raw;

        explicit Reader(Z80Memory* m, uint16_t addr) : mem(m), pc(addr) {}

        uint8_t fetch() {
            uint8_t b = mem->ReadByte(pc);
            if (!raw.empty()) raw += ' ';
            raw += hex2(b);
            pc = static_cast<uint16_t>(pc + 1);
            ++length;
            return b;
        }
        uint16_t fetchWord() {
            uint8_t lo = fetch();
            uint8_t hi = fetch();
            return static_cast<uint16_t>(lo | (hi << 8));
        }
    };

    // ----- Helpers for IX/IY substitution form --------------------------
    // When ddfd_reg() is invoked under DD/FD prefix, the substitution
    // rules are: source reg code 6 → "(IXY+d)" with a fetched
    // displacement; codes 4/5 → IXH/IXL or IYH/IYL (undocumented in
    // Phase 2.5 — render as H/L unchanged so the output stays compact
    // and unsurprising for documented programs).
    std::string ddfd_reg(int code, Reader& r, const char* xy, bool& consumedDisp) {
        if (code == 6) {
            consumedDisp = true;
            int8_t d = static_cast<int8_t>(r.fetch());
            return std::string("(") + xy + disp_str(d) + ")";
        }
        return k_reg8[code];
    }

    // ----- CB-block decoder (used by both plain CB and DD/FD CB) --------
    void decodeCb(uint8_t cb, std::string targetExpr,
                  std::string& mnem, std::string& ops) {
        int op  = (cb >> 6) & 0x03;
        int idx = (cb >> 3) & 0x07;
        switch (op) {
        case 0: // rotation / shift
            mnem = k_rot_mnem[idx];
            ops  = targetExpr;
            break;
        case 1: // BIT b, target
            mnem = "BIT";
            ops  = std::to_string(idx) + ", " + targetExpr;
            break;
        case 2:
            mnem = "RES";
            ops  = std::to_string(idx) + ", " + targetExpr;
            break;
        case 3:
            mnem = "SET";
            ops  = std::to_string(idx) + ", " + targetExpr;
            break;
        }
    }

    // ----- Decoders -----------------------------------------------------

    void decodeEd(Reader& r, std::string& mnem, std::string& ops) {
        uint8_t sub = r.fetch();

        // ADC HL,rr / SBC HL,rr
        if ((sub & 0xCF) == 0x4A) {
            mnem = "ADC"; ops = "HL, " + std::string(k_pair_sp[(sub >> 4) & 3]); return;
        }
        if ((sub & 0xCF) == 0x42) {
            mnem = "SBC"; ops = "HL, " + std::string(k_pair_sp[(sub >> 4) & 3]); return;
        }
        // LD (nn),rr / LD rr,(nn)
        if ((sub & 0xCF) == 0x43) {
            uint16_t nn = r.fetchWord();
            mnem = "LD"; ops = ind16(nn) + ", " + k_pair_sp[(sub >> 4) & 3]; return;
        }
        if ((sub & 0xCF) == 0x4B) {
            uint16_t nn = r.fetchWord();
            mnem = "LD"; ops = std::string(k_pair_sp[(sub >> 4) & 3]) + ", " + ind16(nn); return;
        }
        // NEG (canonical + aliases)
        if ((sub & 0xC7) == 0x44) { mnem = "NEG"; return; }
        // RETN / RETI (treat all aliases as RETN; RETI is 0x4D specifically)
        if (sub == 0x4D)                                  { mnem = "RETI"; return; }
        if ((sub & 0xC7) == 0x45)                         { mnem = "RETN"; return; }
        // IM 0/1/2
        if (sub == 0x46 || sub == 0x66 || sub == 0x4E || sub == 0x6E) { mnem = "IM"; ops = "0"; return; }
        if (sub == 0x56 || sub == 0x76)                                { mnem = "IM"; ops = "1"; return; }
        if (sub == 0x5E || sub == 0x7E)                                { mnem = "IM"; ops = "2"; return; }
        // LD I/R, A and LD A, I/R
        if (sub == 0x47) { mnem = "LD"; ops = "I, A"; return; }
        if (sub == 0x4F) { mnem = "LD"; ops = "R, A"; return; }
        if (sub == 0x57) { mnem = "LD"; ops = "A, I"; return; }
        if (sub == 0x5F) { mnem = "LD"; ops = "A, R"; return; }
        // IN r,(C) / OUT (C),r — including ED 70/71 special cases
        if ((sub & 0xC7) == 0x40) {
            int dst = (sub >> 3) & 0x07;
            mnem = "IN";
            ops = (dst == 6 ? std::string("(C)") : (std::string(k_reg8[dst]) + ", (C)"));
            return;
        }
        if ((sub & 0xC7) == 0x41) {
            int src = (sub >> 3) & 0x07;
            mnem = "OUT";
            ops = (src == 6 ? std::string("(C), 0") : (std::string("(C), ") + k_reg8[src]));
            return;
        }
        // RRD / RLD
        if (sub == 0x67) { mnem = "RRD"; return; }
        if (sub == 0x6F) { mnem = "RLD"; return; }
        // Block ops
        switch (sub) {
        case 0xA0: mnem = "LDI";  return;
        case 0xA8: mnem = "LDD";  return;
        case 0xB0: mnem = "LDIR"; return;
        case 0xB8: mnem = "LDDR"; return;
        case 0xA1: mnem = "CPI";  return;
        case 0xA9: mnem = "CPD";  return;
        case 0xB1: mnem = "CPIR"; return;
        case 0xB9: mnem = "CPDR"; return;
        case 0xA2: mnem = "INI";  return;
        case 0xAA: mnem = "IND";  return;
        case 0xB2: mnem = "INIR"; return;
        case 0xBA: mnem = "INDR"; return;
        case 0xA3: mnem = "OUTI"; return;
        case 0xAB: mnem = "OUTD"; return;
        case 0xB3: mnem = "OTIR"; return;
        case 0xBB: mnem = "OTDR"; return;
        }
        // Unassigned ED slot. Real hardware treats every unrecognised
        // ED-prefix opcode as a 2-byte 8 T-state NOP (the prefix
        // consumes 4 T, the suffix consumes 4 T and is dropped). The
        // CPU side matches that — see Z80Cpu::ExecuteEdPrefix's
        // "Anything else under ED" fallback. We use the convention
        // `NOP*` (sometimes "NOPD") to flag the 2-byte form, which
        // tells the user the disasm did consume both bytes.
        mnem = "NOP*";
        ops  = "";
    }

    void decodeDdFd(Reader& r, const char* xy, std::string& mnem, std::string& ops) {
        uint8_t opcode = r.fetch();

        // 0x21 LD IXY, nn
        if (opcode == 0x21) { uint16_t nn = r.fetchWord(); mnem = "LD"; ops = std::string(xy) + ", " + imm16(nn); return; }
        // 0x22 LD (nn), IXY ; 0x2A LD IXY, (nn)
        if (opcode == 0x22) { uint16_t nn = r.fetchWord(); mnem = "LD"; ops = ind16(nn) + ", " + xy; return; }
        if (opcode == 0x2A) { uint16_t nn = r.fetchWord(); mnem = "LD"; ops = std::string(xy) + ", " + ind16(nn); return; }
        // 0x23 INC IXY / 0x2B DEC IXY
        if (opcode == 0x23) { mnem = "INC"; ops = xy; return; }
        if (opcode == 0x2B) { mnem = "DEC"; ops = xy; return; }
        // 0x09/0x19/0x29/0x39 ADD IXY,rr — note pp=10 is IXY itself
        if ((opcode & 0xCF) == 0x09) {
            int pp = (opcode >> 4) & 0x03;
            const char* src = (pp == 2) ? xy : k_pair_sp[pp];
            mnem = "ADD"; ops = std::string(xy) + ", " + src; return;
        }
        // 0xE5 PUSH IXY / 0xE1 POP IXY
        if (opcode == 0xE5) { mnem = "PUSH"; ops = xy; return; }
        if (opcode == 0xE1) { mnem = "POP";  ops = xy; return; }
        // 0xE9 JP (IXY) — Zilog "JP IXY" but printed as "JP (IXY)"
        if (opcode == 0xE9) { mnem = "JP"; ops = std::string("(") + xy + ")"; return; }
        // 0xF9 LD SP, IXY
        if (opcode == 0xF9) { mnem = "LD"; ops = std::string("SP, ") + xy; return; }

        // (HL)-substitution group — operand operates on (IXY+d) with
        // displacement byte fetched right after the suffix opcode.
        // 0x36 LD (IXY+d), n
        if (opcode == 0x36) {
            int8_t d = static_cast<int8_t>(r.fetch());
            uint8_t n = r.fetch();
            mnem = "LD"; ops = std::string("(") + xy + disp_str(d) + "), " + imm8(n); return;
        }
        // 0x46/0x4E/0x56/0x5E/0x66/0x6E/0x7E LD r, (IXY+d)
        // (0x76 fits the bit pattern but is HALT — fall through to
        // the unrecognised-suffix path so it renders as DB / DB.)
        if ((opcode & 0xC7) == 0x46 && opcode != 0x76) {
            int dst = (opcode >> 3) & 0x07;
            int8_t d = static_cast<int8_t>(r.fetch());
            mnem = "LD"; ops = std::string(k_reg8[dst]) + ", (" + xy + disp_str(d) + ")"; return;
        }
        // 0x70..0x77 LD (IXY+d), r (except 0x76 = HALT, which isn't
        // remapped under DD/FD per documented behaviour — treat the
        // prefix as NOP and re-emit HALT).
        if ((opcode & 0xF8) == 0x70 && opcode != 0x76) {
            int src = opcode & 0x07;
            int8_t d = static_cast<int8_t>(r.fetch());
            mnem = "LD"; ops = std::string("(") + xy + disp_str(d) + "), " + k_reg8[src]; return;
        }
        // 0x34 INC (IXY+d) / 0x35 DEC (IXY+d)
        if (opcode == 0x34 || opcode == 0x35) {
            int8_t d = static_cast<int8_t>(r.fetch());
            mnem = (opcode == 0x34) ? "INC" : "DEC";
            ops  = std::string("(") + xy + disp_str(d) + ")";
            return;
        }
        // 0x86/8E/96/9E/A6/AE/B6/BE ALU A, (IXY+d)
        if ((opcode & 0xC7) == 0x86) {
            int op = (opcode >> 3) & 0x07;
            int8_t d = static_cast<int8_t>(r.fetch());
            mnem = k_alu_mnem[op];
            std::string target = std::string("(") + xy + disp_str(d) + ")";
            ops  = k_alu_takes_a_prefix[op] ? ("A, " + target) : target;
            return;
        }
        // ----- Phase 3.1: 8-bit halves of IXY (undocumented but used) -----
        // Mirrors the Z80Cpu::ExecuteWithIndex handlers — see there for
        // the substitution rules. Helper: map H/L (codes 4/5) to the
        // IXYH/IXYL spelling, fall through to the standard k_reg8 names
        // for every other code.
        auto regH = std::string(xy) + "H";   // "IXH" / "IYH"
        auto regL = std::string(xy) + "L";   // "IXL" / "IYL"
        auto regName = [&](int code) -> std::string {
            if (code == 4) return regH;
            if (code == 5) return regL;
            return std::string(k_reg8[code]);
        };
        // INC / DEC IXYH / IXYL.
        if (opcode == 0x24) { mnem = "INC"; ops = regH; return; }
        if (opcode == 0x2C) { mnem = "INC"; ops = regL; return; }
        if (opcode == 0x25) { mnem = "DEC"; ops = regH; return; }
        if (opcode == 0x2D) { mnem = "DEC"; ops = regL; return; }
        // LD IXYH, n / LD IXYL, n.
        if (opcode == 0x26) { uint8_t n = r.fetch(); mnem = "LD"; ops = regH + ", " + imm8(n); return; }
        if (opcode == 0x2E) { uint8_t n = r.fetch(); mnem = "LD"; ops = regL + ", " + imm8(n); return; }
        // LD r, r' with H/L → IXYH/IXYL when (HL) is not involved.
        if ((opcode & 0xC0) == 0x40 && opcode != 0x76) {
            int dst = (opcode >> 3) & 0x07;
            int src = opcode & 0x07;
            if (dst != 6 && src != 6 && (dst == 4 || dst == 5 || src == 4 || src == 5)) {
                mnem = "LD"; ops = regName(dst) + ", " + regName(src); return;
            }
        }
        // ALU A, IXYH / IXYL.
        if ((opcode & 0xC0) == 0x80) {
            int src = opcode & 0x07;
            if (src == 4 || src == 5) {
                int op = (opcode >> 3) & 0x07;
                mnem = k_alu_mnem[op];
                std::string target = (src == 4) ? regH : regL;
                ops  = k_alu_takes_a_prefix[op] ? ("A, " + target) : target;
                return;
            }
        }

        // DD CB d xx — 4-byte form.
        if (opcode == 0xCB) {
            int8_t d  = static_cast<int8_t>(r.fetch());
            uint8_t cb = r.fetch();
            std::string target = std::string("(") + xy + disp_str(d) + ")";
            decodeCb(cb, target, mnem, ops);
            return;
        }

        // Unrecognised DD/FD suffix — render as "DB <prefix>, <suffix>"
        // (real hardware treats the prefix as NOP and re-decodes the
        // suffix; for disasm we make that explicit).
        mnem = "DB";
        ops  = std::string(std::strcmp(xy, "IX") == 0 ? "$DD" : "$FD") + ", " + imm8(opcode);
    }

    void decodePlain(Reader& r, std::string& mnem, std::string& ops) {
        uint8_t opcode = r.fetch();

        // 0x00 NOP / 0x76 HALT / 0xF3 DI / 0xFB EI
        if (opcode == 0x00) { mnem = "NOP";  return; }
        if (opcode == 0x76) { mnem = "HALT"; return; }
        if (opcode == 0xF3) { mnem = "DI";   return; }
        if (opcode == 0xFB) { mnem = "EI";   return; }

        // EX / EXX family (basic Z80, originally missed in Phase 1)
        if (opcode == 0x08) { mnem = "EX"; ops = "AF, AF'";  return; }
        if (opcode == 0xD9) { mnem = "EXX"; return; }
        if (opcode == 0xE3) { mnem = "EX"; ops = "(SP), HL"; return; }
        if (opcode == 0xEB) { mnem = "EX"; ops = "DE, HL";   return; }

        // Phase 3.2: single-byte accumulator rotates + DAA/CPL/SCF/CCF
        if (opcode == 0x07) { mnem = "RLCA"; return; }
        if (opcode == 0x0F) { mnem = "RRCA"; return; }
        if (opcode == 0x17) { mnem = "RLA";  return; }
        if (opcode == 0x1F) { mnem = "RRA";  return; }
        if (opcode == 0x27) { mnem = "DAA";  return; }
        if (opcode == 0x2F) { mnem = "CPL";  return; }
        if (opcode == 0x37) { mnem = "SCF";  return; }
        if (opcode == 0x3F) { mnem = "CCF";  return; }

        // 0x01/11/21/31 LD rr, nn
        if ((opcode & 0xCF) == 0x01) {
            uint16_t nn = r.fetchWord();
            mnem = "LD";
            ops  = std::string(k_pair_sp[(opcode >> 4) & 3]) + ", " + imm16(nn);
            return;
        }
        // 0x09/19/29/39 ADD HL, rr
        if ((opcode & 0xCF) == 0x09) {
            mnem = "ADD";
            ops  = "HL, " + std::string(k_pair_sp[(opcode >> 4) & 3]);
            return;
        }
        // 0x03/13/23/33 INC rr ; 0x0B/1B/2B/3B DEC rr
        if ((opcode & 0xCF) == 0x03) { mnem = "INC"; ops = k_pair_sp[(opcode >> 4) & 3]; return; }
        if ((opcode & 0xCF) == 0x0B) { mnem = "DEC"; ops = k_pair_sp[(opcode >> 4) & 3]; return; }
        // 0x04..0x3C INC r (pattern 00 ddd 100) ; 0x05..0x3D DEC r
        if ((opcode & 0xC7) == 0x04) { mnem = "INC"; ops = k_reg8[(opcode >> 3) & 7]; return; }
        if ((opcode & 0xC7) == 0x05) { mnem = "DEC"; ops = k_reg8[(opcode >> 3) & 7]; return; }

        // 0x02 LD (BC),A ; 0x12 LD (DE),A ; 0x0A LD A,(BC) ; 0x1A LD A,(DE)
        if (opcode == 0x02) { mnem = "LD"; ops = "(BC), A"; return; }
        if (opcode == 0x12) { mnem = "LD"; ops = "(DE), A"; return; }
        if (opcode == 0x0A) { mnem = "LD"; ops = "A, (BC)"; return; }
        if (opcode == 0x1A) { mnem = "LD"; ops = "A, (DE)"; return; }

        // 0x22 LD (nn),HL ; 0x2A LD HL,(nn) ; 0x32 LD (nn),A ; 0x3A LD A,(nn)
        if (opcode == 0x22) { uint16_t nn = r.fetchWord(); mnem = "LD"; ops = ind16(nn) + ", HL"; return; }
        if (opcode == 0x2A) { uint16_t nn = r.fetchWord(); mnem = "LD"; ops = "HL, " + ind16(nn); return; }
        if (opcode == 0x32) { uint16_t nn = r.fetchWord(); mnem = "LD"; ops = ind16(nn) + ", A";  return; }
        if (opcode == 0x3A) { uint16_t nn = r.fetchWord(); mnem = "LD"; ops = "A, " + ind16(nn);  return; }

        // 0x06/0E/16/1E/26/2E/36/3E LD r, n
        if ((opcode & 0xC7) == 0x06) {
            int dst = (opcode >> 3) & 0x07;
            uint8_t n = r.fetch();
            mnem = "LD"; ops = std::string(k_reg8[dst]) + ", " + imm8(n);
            return;
        }

        // 0x40..0x7F LD r, r' (except 0x76 = HALT already handled)
        if (opcode >= 0x40 && opcode <= 0x7F) {
            int dst = (opcode >> 3) & 0x07;
            int src = opcode & 0x07;
            mnem = "LD"; ops = std::string(k_reg8[dst]) + ", " + k_reg8[src];
            return;
        }

        // 0x80..0xBF : ALU A, r
        if ((opcode & 0xC0) == 0x80) {
            int op  = (opcode >> 3) & 0x07;
            int src = opcode & 0x07;
            mnem = k_alu_mnem[op];
            ops  = k_alu_takes_a_prefix[op] ? (std::string("A, ") + k_reg8[src]) : k_reg8[src];
            return;
        }

        // 0xC6/CE/D6/DE/E6/EE/F6/FE : ALU A, n
        if ((opcode & 0xC7) == 0xC6) {
            int op = (opcode >> 3) & 0x07;
            uint8_t n = r.fetch();
            mnem = k_alu_mnem[op];
            ops  = k_alu_takes_a_prefix[op] ? ("A, " + imm8(n)) : imm8(n);
            return;
        }

        // 0x18 JR e ; 0x10 DJNZ e ; 0x20/28/30/38 JR cc,e
        if (opcode == 0x18) {
            int8_t e = static_cast<int8_t>(r.fetch());
            // Target = end-of-instruction + e
            uint16_t target = static_cast<uint16_t>(r.pc + e);
            mnem = "JR"; ops = "$" + hex4(target);
            return;
        }
        if (opcode == 0x10) {
            int8_t e = static_cast<int8_t>(r.fetch());
            uint16_t target = static_cast<uint16_t>(r.pc + e);
            mnem = "DJNZ"; ops = "$" + hex4(target);
            return;
        }
        if ((opcode & 0xE7) == 0x20) {
            int cc = (opcode >> 3) & 0x03;
            int8_t e = static_cast<int8_t>(r.fetch());
            uint16_t target = static_cast<uint16_t>(r.pc + e);
            mnem = "JR"; ops = std::string(k_cc[cc]) + ", $" + hex4(target);
            return;
        }

        // 0xC3 JP nn ; 0xE9 JP (HL)
        if (opcode == 0xC3) { uint16_t nn = r.fetchWord(); mnem = "JP"; ops = imm16(nn); return; }
        if (opcode == 0xE9) { mnem = "JP"; ops = "(HL)"; return; }
        // 0xC2/CA/D2/DA/E2/EA/F2/FA JP cc, nn
        if ((opcode & 0xC7) == 0xC2) {
            int cc = (opcode >> 3) & 0x07;
            uint16_t nn = r.fetchWord();
            mnem = "JP"; ops = std::string(k_cc[cc]) + ", " + imm16(nn);
            return;
        }

        // 0xCD CALL nn ; 0xC4/CC/.../FC CALL cc, nn
        if (opcode == 0xCD) { uint16_t nn = r.fetchWord(); mnem = "CALL"; ops = imm16(nn); return; }
        if ((opcode & 0xC7) == 0xC4) {
            int cc = (opcode >> 3) & 0x07;
            uint16_t nn = r.fetchWord();
            mnem = "CALL"; ops = std::string(k_cc[cc]) + ", " + imm16(nn);
            return;
        }

        // 0xC9 RET ; 0xC0/C8/D0/D8/E0/E8/F0/F8 RET cc
        if (opcode == 0xC9) { mnem = "RET"; return; }
        if ((opcode & 0xC7) == 0xC0) {
            int cc = (opcode >> 3) & 0x07;
            mnem = "RET"; ops = k_cc[cc]; return;
        }

        // 0xC7/CF/D7/DF/E7/EF/F7/FF RST n
        if ((opcode & 0xC7) == 0xC7) {
            int n = (opcode >> 3) & 0x07;
            mnem = "RST"; ops = imm8(static_cast<uint8_t>(n * 8));
            return;
        }

        // PUSH rr / POP rr (rr from k_pair_af table)
        if ((opcode & 0xCF) == 0xC5) { mnem = "PUSH"; ops = k_pair_af[(opcode >> 4) & 3]; return; }
        if ((opcode & 0xCF) == 0xC1) { mnem = "POP";  ops = k_pair_af[(opcode >> 4) & 3]; return; }

        // IN A,(n) / OUT (n),A
        if (opcode == 0xDB) { uint8_t p = r.fetch(); mnem = "IN";  ops = "A, " + indr8(p); return; }
        if (opcode == 0xD3) { uint8_t p = r.fetch(); mnem = "OUT"; ops = indr8(p) + ", A"; return; }

        // Anything else (e.g. EX HL,DE = 0xEB not yet implemented) →
        // render as DB n for now. The instruction is "valid Z80" but
        // the plugin doesn't yet execute it; the disasm matches.
        mnem = "DB";
        ops  = imm8(opcode);
    }
} // namespace

Z80Disasm::Result Z80Disasm::Decode(Z80Memory* mem, uint16_t addr) {
    Result out{};
    if (!mem) {
        out.mnemonic = "??";
        out.length = 1;
        return out;
    }
    Reader r(mem, addr);
    uint8_t first = mem->ReadByte(addr);     // peek (don't consume) — we
                                              // need to know which prefix
                                              // table to enter before we
                                              // start building the raw
                                              // bytes string.

    if (first == 0xCB) {
        r.fetch();                            // consume CB into raw
        uint8_t cb = r.fetch();
        decodeCb(cb, k_reg8[cb & 0x07], out.mnemonic, out.operands);
    } else if (first == 0xDD) {
        r.fetch();
        decodeDdFd(r, "IX", out.mnemonic, out.operands);
    } else if (first == 0xFD) {
        r.fetch();
        decodeDdFd(r, "IY", out.mnemonic, out.operands);
    } else if (first == 0xED) {
        r.fetch();
        decodeEd(r, out.mnemonic, out.operands);
    } else {
        decodePlain(r, out.mnemonic, out.operands);
    }

    out.rawBytes = r.raw;
    out.length   = r.length;
    return out;
}
