#pragma once
#include <string>
#include <vector>
#include <set>
#include <map>
#include <sstream>
#include <iomanip>
#include "wii_types.hpp"
#include "dol_loader.hpp"
#include "ppc_bitops.hpp"
#include "mod_engine.hpp"

namespace WiiPort {

inline std::string RStr(uint32_t reg) {
    return "ctx->r[" + std::to_string(reg) + "]";
}

inline std::string FStr(uint32_t reg) {
    return "ctx->f[" + std::to_string(reg) + "]";
}

class PPCLifter {
public:
    const DolExecutable& dol;
    std::set<uint32_t> discoveredFunctions;
    std::map<uint32_t, std::string> symbolNames;

    PPCLifter(const DolExecutable& exec) : dol(exec) {
        if (dol.entryPoint != 0) {
            discoveredFunctions.insert(dol.entryPoint);
        }
    }

    void AddSymbol(uint32_t addr, const std::string& name) {
        symbolNames[addr] = name;
        discoveredFunctions.insert(addr);
    }

    std::string GetFunctionName(uint32_t addr) {
        auto it = symbolNames.find(addr);
        if (it != symbolNames.end()) return it->second;
        std::stringstream ss;
        ss << "Func_0x" << std::uppercase << std::hex << std::setfill('0') << std::setw(8) << addr;
        return ss.str();
    }

    void DiscoverAllFunctions() {
        for (const auto& sec : dol.sections) {
            if (!sec.isText) continue;
            discoveredFunctions.insert(sec.address);
            for (uint32_t offset = 0; offset + 4 <= sec.size; offset += 4) {
                uint32_t pc = sec.address + offset;
                uint32_t instr = 0;
                if (!dol.ReadInstruction(pc, instr)) continue;

                // Function prologue: stwu r1, -frame(r1)
                if ((instr & 0xFFFF0000) == 0x94210000) {
                    discoveredFunctions.insert(pc);
                }

                uint32_t op = (instr >> 26) & 0x3F;
                if (op == 18 && (instr & 1)) { // bl
                    int32_t li = (instr & 0x03FFFFFC);
                    if (li & 0x02000000) li |= 0xFC000000;
                    uint32_t target = (instr & 2) ? li : (pc + li);
                    if (dol.IsCodeAddress(target)) {
                        discoveredFunctions.insert(target);
                    }
                }
            }
        }
        // Scansiona anche le sezioni dati per trovare puntatori a funzioni (vtables, callback)
        for (const auto& sec : dol.sections) {
            if (sec.isText) continue;
            for (uint32_t offset = 0; offset + 4 <= sec.size; offset += 4) {
                uint32_t ptr = ((uint32_t)sec.data[offset] << 24) |
                               ((uint32_t)sec.data[offset+1] << 16) |
                               ((uint32_t)sec.data[offset+2] << 8) |
                               (uint32_t)sec.data[offset+3];
                if ((ptr & 3) == 0 && dol.IsCodeAddress(ptr)) {
                    uint32_t firstInstr = 0;
                    if (dol.ReadInstruction(ptr, firstInstr) && firstInstr != 0) {
                        discoveredFunctions.insert(ptr);
                    }
                }
            }
        }
        if (dol.entryPoint != 0) {
            discoveredFunctions.insert(dol.entryPoint);
        }

        // Riconoscimento tail calls (b target preceduto da epilogo stack o stub)
        for (const auto& sec : dol.sections) {
            if (!sec.isText) continue;
            for (uint32_t offset = 0; offset + 4 <= sec.size; offset += 4) {
                uint32_t pc = sec.address + offset;
                uint32_t instr = 0;
                if (!dol.ReadInstruction(pc, instr)) continue;
                uint32_t op = (instr >> 26) & 0x3F;
                if (op == 18 && (instr & 1) == 0) { // b target
                    int32_t li = (instr & 0x03FFFFFC);
                    if (li & 0x02000000) li |= 0xFC000000;
                    uint32_t target = (instr & 2) ? li : (pc + li);
                    if (!dol.IsCodeAddress(target)) continue;

                    uint32_t prev = 0, prev2 = 0;
                    if (pc >= sec.address + 4) dol.ReadInstruction(pc - 4, prev);
                    if (pc >= sec.address + 8) dol.ReadInstruction(pc - 8, prev2);
                    bool isTailCall = false;
                    if ((prev & 0xFFFF0000) == 0x38210000 || (prev2 & 0xFFFF0000) == 0x38210000) isTailCall = true;
                    if (prev == 0x7C0803A6 || prev2 == 0x7C0803A6) isTailCall = true;
                    if (discoveredFunctions.count(pc) || discoveredFunctions.count(pc - 4)) isTailCall = true;

                    if (isTailCall) {
                        discoveredFunctions.insert(target);
                    }
                }
            }
        }

        // Propaga la scoperta delle funzioni attraverso trampolini e salti di coda (es. newlib stubs sbrk, isatty, write)
        bool addedTrampoline = true;
        while (addedTrampoline) {
            addedTrampoline = false;
            for (uint32_t f : discoveredFunctions) {
                uint32_t first = 0;
                if (dol.ReadInstruction(f, first)) {
                    uint32_t op = (first >> 26) & 0x3F;
                    if (op == 18 && (first & 1) == 0) { // b dest
                        int32_t li = (first & 0x03FFFFFC);
                        if (li & 0x02000000) li |= 0xFC000000;
                        uint32_t dest = (first & 2) ? li : (f + li);
                        if (dol.IsCodeAddress(dest)) {
                            if (discoveredFunctions.insert(dest).second) {
                                addedTrampoline = true;
                            }
                        }
                    }
                }
            }
        }

        // Funzioni critiche libogc / devkitPPC esplicite per garantire AOT completo
        const uint32_t criticalFuncs[] = {
            0x80028C54, 0x80028EF4, 0x80029100, 0x80029108, 0x8002911C, 0x8002916C,
            0x800174C0, 0x80017544, 0x8003A084, 0x8003A434, 0x8003BB58, 0x8001FBA4, 0x8001FBBC
        };
        for (uint32_t cf : criticalFuncs) {
            if (dol.IsCodeAddress(cf)) {
                discoveredFunctions.insert(cf);
            }
        }
    }

    std::string LiftFunction(uint32_t funcAddress) {
        std::stringstream out;
        std::string fname = GetFunctionName(funcAddress);

        out << "// Function at " << HexStr(funcAddress) << "\n";
        out << "void " << fname << "(PPCContext* ctx) {\n";
        out << "    ctx->pc = " << HexStr(funcAddress) << ";\n";
        out << "    if (g_ModEngine.ExecutePreHook(" << HexStr(funcAddress) << ", ctx)) return;\n";

        auto nextIt = discoveredFunctions.upper_bound(funcAddress);
        uint32_t nextFuncAddr = (nextIt != discoveredFunctions.end()) ? *nextIt : (dol.sections[0].address + dol.sections[0].size);

        // Pass 1: Calcola la fine della funzione
        // Scansiona fino a nextFuncAddr tenendo traccia del massimo salto in avanti
        uint32_t maxBranchTarget = funcAddress;
        uint32_t termPc = 0;
        uint32_t scanPc = funcAddress;
        uint32_t instr = 0;

        while (scanPc < nextFuncAddr && dol.ReadInstruction(scanPc, instr)) {
            uint32_t op = (instr >> 26) & 0x3F;
            if (op == 18 || op == 16) {
                int32_t target = 0;
                if (op == 18) {
                    int32_t li = (instr & 0x03FFFFFC);
                    if (li & 0x02000000) li |= 0xFC000000;
                    target = (instr & 2) ? li : (scanPc + li);
                } else {
                    int32_t bd = (instr & 0xFFFC);
                    if (bd & 0x8000) bd |= 0xFFFF0000;
                    target = (instr & 2) ? bd : (scanPc + bd);
                }
                if (target >= (int32_t)funcAddress && target < (int32_t)nextFuncAddr) {
                    if ((uint32_t)target > maxBranchTarget) {
                        maxBranchTarget = (uint32_t)target;
                    }
                }
            }

            bool isTerminator = (instr == 0x4E800020) || (instr == 0x4E800420) || (op == 18 && !(instr & 1)) || (instr == 0x7FE00008);
            if (isTerminator && scanPc >= maxBranchTarget) {
                // Se tutte le istruzioni rimanenti prima di nextFuncAddr sono padding (0 o nop), possiamo terminare qui
                bool allPad = true;
                for (uint32_t p = scanPc + 4; p < nextFuncAddr; p += 4) {
                    uint32_t pw = 0;
                    if (dol.ReadInstruction(p, pw) && pw != 0x00000000 && pw != 0x60000000) {
                        allPad = false;
                        break;
                    }
                }
                if (allPad) {
                    termPc = scanPc + 4;
                    break;
                }
            }
            scanPc += 4;
        }

        uint32_t funcEnd = (termPc != 0) ? termPc : nextFuncAddr;

        // Identifica etichette interne strettamente dentro [funcAddress, funcEnd)
        std::set<uint32_t> internalLabels;
        scanPc = funcAddress;
        while (scanPc < funcEnd && dol.ReadInstruction(scanPc, instr)) {
            uint32_t op = (instr >> 26) & 0x3F;
            if (op == 18 || op == 16) {
                int32_t target = 0;
                if (op == 18) {
                    int32_t li = (instr & 0x03FFFFFC);
                    if (li & 0x02000000) li |= 0xFC000000;
                    target = (instr & 2) ? li : (scanPc + li);
                } else {
                    int32_t bd = (instr & 0xFFFC);
                    if (bd & 0x8000) bd |= 0xFFFF0000;
                    target = (instr & 2) ? bd : (scanPc + bd);
                }
                if (target >= (int32_t)funcAddress && target < (int32_t)funcEnd) {
                    internalLabels.insert(target);
                }
            }
            scanPc += 4;
        }

        // Pass 2: Generazione del corpo C++
        uint32_t pc = funcAddress;
        uint32_t lastInstr = 0;
        while (pc < funcEnd && dol.ReadInstruction(pc, instr)) {
            if (internalLabels.count(pc)) {
                out << "loc_0x" << std::hex << pc << ":\n";
            }

            out << "    /* " << HexStr(pc) << ": " << HexStr(instr) << " */ "
                << LiftInstruction(instr, pc, internalLabels) << "\n";

            lastInstr = instr;
            pc += 4;
        }

        // Se la funzione è terminata raggiungendo funcEnd (nextFuncAddr) senza una terminazione esplicita (blr o b uncond),
        // allora in assembly si tratta di un fallthrough naturale verso la funzione successiva!
        if (termPc == 0 && nextFuncAddr != 0 && dol.IsCodeAddress(nextFuncAddr)) {
            uint32_t lastOp = (lastInstr >> 26) & 0x3F;
            bool isTerminator = (lastInstr == 0x4E800020) || (lastInstr == 0x4E800420) || (lastOp == 18 && !(lastInstr & 1)) || (lastInstr == 0x7FE00008);
            if (!isTerminator) {
                out << "    " << GetFunctionName(nextFuncAddr) << "(ctx); return;\n";
            }
        }

        out << "}\n\n";
        return out.str();
    }

private:
    std::string LiftInstruction(uint32_t instr, uint32_t pc, const std::set<uint32_t>& internalLabels) {
        uint32_t op = (instr >> 26) & 0x3F;
        uint32_t rD = (instr >> 21) & 0x1F;
        uint32_t rA = (instr >> 16) & 0x1F;
        uint32_t rB = (instr >> 11) & 0x1F;
        int16_t simm = (int16_t)(instr & 0xFFFF);
        uint16_t uimm = (uint16_t)(instr & 0xFFFF);
        uint32_t xo10 = (instr >> 1) & 0x3FF;
        bool rc = (instr & 1);

        std::string s;

        switch (op) {
            case 14: { // addi / li
                if (rA == 0) s = RStr(rD) + " = " + std::to_string((int32_t)simm) + ";";
                else s = RStr(rD) + " = " + RStr(rA) + " + " + std::to_string((int32_t)simm) + ";";
                break;
            }
            case 15: { // addis / lis
                uint32_t uval = (uint32_t)uimm << 16;
                if (rA == 0) s = RStr(rD) + " = " + HexStr(uval) + ";";
                else s = RStr(rD) + " = " + RStr(rA) + " + " + HexStr(uval) + ";";
                break;
            }
            case 24: { // ori
                if (rA == 0 && rD == 0 && uimm == 0) s = "/* nop */;";
                else s = RStr(rA) + " = " + RStr(rD) + " | " + HexStr(uimm) + ";";
                break;
            }
            case 25: { // oris
                uint32_t uval = (uint32_t)uimm << 16;
                s = RStr(rA) + " = " + RStr(rD) + " | " + HexStr(uval) + ";";
                break;
            }
            case 26: { // xori
                s = RStr(rA) + " = " + RStr(rD) + " ^ " + HexStr(uimm) + ";";
                break;
            }
            case 28: { // andi.
                s = RStr(rA) + " = " + RStr(rD) + " & " + HexStr(uimm) + "; UpdateCR0(ctx, " + RStr(rA) + ");";
                break;
            }
            case 10: { // cmpli
                uint32_t crfD = (instr >> 23) & 7;
                s = "UpdateCRFieldLogical(ctx, " + std::to_string(crfD) + ", " + RStr(rA) + ", " + HexStr(uimm) + ");";
                break;
            }
            case 11: { // cmpi
                uint32_t crfD = (instr >> 23) & 7;
                s = "UpdateCRField(ctx, " + std::to_string(crfD) + ", (int32_t)" + RStr(rA) + ", " + std::to_string((int32_t)simm) + ");";
                break;
            }
            case 12: { // addic
                s = RStr(rD) + " = " + (rA == 0 ? "0" : RStr(rA)) + " + " + std::to_string((int32_t)simm) + ";";
                break;
            }
            case 13: { // addic.
                s = RStr(rD) + " = " + (rA == 0 ? "0" : RStr(rA)) + " + " + std::to_string((int32_t)simm) + "; UpdateCR0(ctx, " + RStr(rD) + ");";
                break;
            }
            case 27: { // xoris
                s = RStr(rA) + " = " + RStr(rD) + " ^ " + HexStr((uint32_t)uimm << 16) + ";";
                break;
            }
            case 29: { // andis.
                s = RStr(rA) + " = " + RStr(rD) + " & " + HexStr((uint32_t)uimm << 16) + "; UpdateCR0(ctx, " + RStr(rA) + ");";
                break;
            }
            case 7: { // mulli
                s = RStr(rD) + " = (uint32_t)((int32_t)" + RStr(rA) + " * " + std::to_string((int32_t)simm) + ");";
                break;
            }
            case 8: { // subfic
                s = RStr(rD) + " = " + std::to_string((int32_t)simm) + " - " + RStr(rA) + ";";
                break;
            }
            case 21: { // rlwinm
                uint32_t sh = (instr >> 11) & 0x1F;
                uint32_t mb = (instr >> 6)  & 0x1F;
                uint32_t me = (instr >> 1)  & 0x1F;
                s = RStr(rA) + " = PPC_rlwinm(" + RStr(rD) + ", " + std::to_string(sh) + ", " + std::to_string(mb) + ", " + std::to_string(me) + ");";
                if (rc) s += " UpdateCR0(ctx, " + RStr(rA) + ");";
                break;
            }
            case 20: { // rlwimi
                uint32_t sh = (instr >> 11) & 0x1F;
                uint32_t mb = (instr >> 6)  & 0x1F;
                uint32_t me = (instr >> 1)  & 0x1F;
                s = RStr(rA) + " = PPC_rlwimi(" + RStr(rA) + ", " + RStr(rD) + ", " + std::to_string(sh) + ", " + std::to_string(mb) + ", " + std::to_string(me) + ");";
                if (rc) s += " UpdateCR0(ctx, " + RStr(rA) + ");";
                break;
            }
            case 23: { // rlwnm
                uint32_t mb = (instr >> 6)  & 0x1F;
                uint32_t me = (instr >> 1)  & 0x1F;
                s = RStr(rA) + " = PPC_rlwinm(" + RStr(rD) + ", " + RStr(rB) + " & 31, " + std::to_string(mb) + ", " + std::to_string(me) + ");";
                if (rc) s += " UpdateCR0(ctx, " + RStr(rA) + ");";
                break;
            }
            case 32: { // lwz
                std::string addr = (rA == 0) ? std::to_string((int32_t)simm) : (RStr(rA) + " + " + std::to_string((int32_t)simm));
                s = RStr(rD) + " = g_Memory.Read32(" + addr + ");";
                break;
            }
            case 33: { // lwzu
                s = RStr(rA) + " += " + std::to_string((int32_t)simm) + "; " + RStr(rD) + " = g_Memory.Read32(" + RStr(rA) + ");";
                break;
            }
            case 36: { // stw
                std::string addr = (rA == 0) ? std::to_string((int32_t)simm) : (RStr(rA) + " + " + std::to_string((int32_t)simm));
                s = "g_Memory.Write32(" + addr + ", " + RStr(rD) + ");";
                break;
            }
            case 37: { // stwu
                s = "{ uint32_t val = " + RStr(rD) + "; " + RStr(rA) + " += " + std::to_string((int32_t)simm) + "; g_Memory.Write32(" + RStr(rA) + ", val); }";
                break;
            }
            case 34: { // lbz
                std::string addr = (rA == 0) ? std::to_string((int32_t)simm) : (RStr(rA) + " + " + std::to_string((int32_t)simm));
                s = RStr(rD) + " = g_Memory.Read8(" + addr + ");";
                break;
            }
            case 35: { // lbzu
                s = RStr(rA) + " += " + std::to_string((int32_t)simm) + "; " + RStr(rD) + " = g_Memory.Read8(" + RStr(rA) + ");";
                break;
            }
            case 38: { // stb
                std::string addr = (rA == 0) ? std::to_string((int32_t)simm) : (RStr(rA) + " + " + std::to_string((int32_t)simm));
                s = "g_Memory.Write8(" + addr + ", (uint8_t)" + RStr(rD) + ");";
                break;
            }
            case 39: { // stbu
                s = "{ uint8_t val = (uint8_t)" + RStr(rD) + "; " + RStr(rA) + " += " + std::to_string((int32_t)simm) + "; g_Memory.Write8(" + RStr(rA) + ", val); }";
                break;
            }
            case 40: { // lhz
                std::string addr = (rA == 0) ? std::to_string((int32_t)simm) : (RStr(rA) + " + " + std::to_string((int32_t)simm));
                s = RStr(rD) + " = g_Memory.Read16(" + addr + ");";
                break;
            }
            case 41: { // lhzu
                s = RStr(rA) + " += " + std::to_string((int32_t)simm) + "; " + RStr(rD) + " = g_Memory.Read16(" + RStr(rA) + ");";
                break;
            }
            case 42: { // lha
                std::string addr = (rA == 0) ? std::to_string((int32_t)simm) : (RStr(rA) + " + " + std::to_string((int32_t)simm));
                s = RStr(rD) + " = (int16_t)g_Memory.Read16(" + addr + ");";
                break;
            }
            case 43: { // lhau
                s = RStr(rA) + " += " + std::to_string((int32_t)simm) + "; " + RStr(rD) + " = (int16_t)g_Memory.Read16(" + RStr(rA) + ");";
                break;
            }
            case 44: { // sth
                std::string addr = (rA == 0) ? std::to_string((int32_t)simm) : (RStr(rA) + " + " + std::to_string((int32_t)simm));
                s = "g_Memory.Write16(" + addr + ", (uint16_t)" + RStr(rD) + ");";
                break;
            }
            case 45: { // sthu
                s = "{ uint16_t val = (uint16_t)" + RStr(rD) + "; " + RStr(rA) + " += " + std::to_string((int32_t)simm) + "; g_Memory.Write16(" + RStr(rA) + ", val); }";
                break;
            }
            case 46: { // lmw
                s = "";
                for (uint32_t reg = rD; reg < 32; ++reg) {
                    std::string addr = (rA == 0) ? std::to_string((int32_t)simm + (reg - rD) * 4) : (RStr(rA) + " + " + std::to_string((int32_t)simm + (reg - rD) * 4));
                    s += RStr(reg) + " = g_Memory.Read32(" + addr + "); ";
                }
                break;
            }
            case 47: { // stmw
                s = "";
                for (uint32_t reg = rD; reg < 32; ++reg) {
                    std::string addr = (rA == 0) ? std::to_string((int32_t)simm + (reg - rD) * 4) : (RStr(rA) + " + " + std::to_string((int32_t)simm + (reg - rD) * 4));
                    s += "g_Memory.Write32(" + addr + ", " + RStr(reg) + "); ";
                }
                break;
            }
            case 48: { // lfs
                std::string addr = (rA == 0) ? std::to_string((int32_t)simm) : (RStr(rA) + " + " + std::to_string((int32_t)simm));
                s = "{ uint32_t u = g_Memory.Read32(" + addr + "); float f; std::memcpy(&f, &u, 4); " + FStr(rD) + ".ps[0] = f; " + FStr(rD) + ".ps[1] = f; }";
                break;
            }
            case 49: { // lfsu
                s = "{ " + RStr(rA) + " += " + std::to_string((int32_t)simm) + "; uint32_t u = g_Memory.Read32(" + RStr(rA) + "); float f; std::memcpy(&f, &u, 4); " + FStr(rD) + ".ps[0] = f; " + FStr(rD) + ".ps[1] = f; }";
                break;
            }
            case 50: { // stfs
                std::string addr = (rA == 0) ? std::to_string((int32_t)simm) : (RStr(rA) + " + " + std::to_string((int32_t)simm));
                s = "{ float f = " + FStr(rD) + ".ps[0]; uint32_t u; std::memcpy(&u, &f, 4); g_Memory.Write32(" + addr + ", u); }";
                break;
            }
            case 51: { // stfsu
                s = "{ float f = " + FStr(rD) + ".ps[0]; uint32_t u; std::memcpy(&u, &f, 4); " + RStr(rA) + " += " + std::to_string((int32_t)simm) + "; g_Memory.Write32(" + RStr(rA) + ", u); }";
                break;
            }
            case 52: { // stfd
                std::string addr = (rA == 0) ? std::to_string((int32_t)simm) : (RStr(rA) + " + " + std::to_string((int32_t)simm));
                s = "{ g_Memory.Write32(" + addr + ", (uint32_t)(" + FStr(rD) + ".u64 >> 32)); g_Memory.Write32(" + addr + " + 4, (uint32_t)" + FStr(rD) + ".u64); }";
                break;
            }
            case 53: { // stfdu
                s = "{ " + RStr(rA) + " += " + std::to_string((int32_t)simm) + "; g_Memory.Write32(" + RStr(rA) + ", (uint32_t)(" + FStr(rD) + ".u64 >> 32)); g_Memory.Write32(" + RStr(rA) + " + 4, (uint32_t)" + FStr(rD) + ".u64); }";
                break;
            }
            case 54: { // lfd
                std::string addr = (rA == 0) ? std::to_string((int32_t)simm) : (RStr(rA) + " + " + std::to_string((int32_t)simm));
                s = "{ uint64_t hi = g_Memory.Read32(" + addr + "); uint64_t lo = g_Memory.Read32(" + addr + " + 4); " + FStr(rD) + ".u64 = (hi << 32) | lo; }";
                break;
            }
            case 55: { // lfdu
                s = "{ " + RStr(rA) + " += " + std::to_string((int32_t)simm) + "; uint64_t hi = g_Memory.Read32(" + RStr(rA) + "); uint64_t lo = g_Memory.Read32(" + RStr(rA) + " + 4); " + FStr(rD) + ".u64 = (hi << 32) | lo; }";
                break;
            }
            case 56: { // psq_l
                bool w = (instr >> 15) & 1;
                uint32_t gqrIdx = (instr >> 12) & 7;
                int16_t d = (int16_t)(instr & 0xFFF);
                if (d & 0x800) d |= 0xF000;
                std::string addr = (rA == 0) ? std::to_string((int32_t)d) : (RStr(rA) + " + " + std::to_string((int32_t)d));
                s = "DequantizeLoad(ctx, " + std::to_string(rD) + ", " + addr + ", " + (w ? "true" : "false") + ", " + std::to_string(gqrIdx) + ");";
                break;
            }
            case 60: { // psq_st
                bool w = (instr >> 15) & 1;
                uint32_t gqrIdx = (instr >> 12) & 7;
                int16_t d = (int16_t)(instr & 0xFFF);
                if (d & 0x800) d |= 0xF000;
                std::string addr = (rA == 0) ? std::to_string((int32_t)d) : (RStr(rA) + " + " + std::to_string((int32_t)d));
                s = "QuantizeStore(ctx, " + std::to_string(rD) + ", " + addr + ", " + (w ? "true" : "false") + ", " + std::to_string(gqrIdx) + ");";
                break;
            }
            case 18: { // b, bl, ba, bla
                bool lk = instr & 1;
                int32_t li = (instr & 0x03FFFFFC);
                if (li & 0x02000000) li |= 0xFC000000;
                uint32_t target = (instr & 2) ? li : (pc + li);

                if (lk) {
                    s = "ctx->lr = " + HexStr(pc + 4) + "; ";
                    if (discoveredFunctions.count(target)) {
                        s += GetFunctionName(target) + "(ctx);";
                    } else {
                        s += "g_Memory.DispatchIndirectBranch(ctx, " + HexStr(target) + ");";
                    }
                } else {
                    if (internalLabels.count(target)) {
                        s = "goto loc_0x" + std::to_string(target) + ";"; // will replace with hex label
                        std::stringstream lss;
                        lss << "goto loc_0x" << std::hex << target << ";";
                        s = lss.str();
                    } else if (discoveredFunctions.count(target)) {
                        s = GetFunctionName(target) + "(ctx); return;";
                    } else {
                        s = "g_Memory.DispatchIndirectBranch(ctx, " + HexStr(target) + "); return;";
                    }
                }
                break;
            }
            case 16: { // bc, bcl, etc.
                bool lk = instr & 1;
                uint32_t bo = (instr >> 21) & 0x1F;
                uint32_t bi = (instr >> 16) & 0x1F;
                int32_t bd = (instr & 0xFFFC);
                if (bd & 0x8000) bd |= 0xFFFF0000;
                uint32_t target = (instr & 2) ? bd : (pc + bd);

                bool unconditional = ((bo & 0x14) == 0x14);

                std::stringstream ss;
                if (!unconditional) {
                    ss << "if (EvaluateCondition(ctx, " << bo << ", " << bi << ")) ";
                }

                if (lk) {
                    if (target == pc + 4) {
                        ss << "ctx->lr = " << HexStr(pc + 4) << ";";
                    } else if (internalLabels.count(target)) {
                        ss << "{ ctx->lr = " << HexStr(pc + 4) << "; goto loc_0x" << std::hex << target << "; }";
                    } else if (discoveredFunctions.count(target)) {
                        ss << "{ ctx->lr = " << HexStr(pc + 4) << "; " << GetFunctionName(target) << "(ctx); }";
                    } else {
                        ss << "{ ctx->lr = " << HexStr(pc + 4) << "; g_Memory.DispatchIndirectBranch(ctx, " << HexStr(target) << "); }";
                    }
                } else {
                    if (internalLabels.count(target)) {
                        ss << "goto loc_0x" << std::hex << target << ";";
                    } else if (discoveredFunctions.count(target)) {
                        ss << "{ " << GetFunctionName(target) << "(ctx); return; }";
                    } else {
                        ss << "{ g_Memory.DispatchIndirectBranch(ctx, " << HexStr(target) << "); return; }";
                    }
                }
                s = ss.str();
                break;
            }
            case 19: {
                uint32_t xo = (instr >> 1) & 0x3FF;
                if (xo == 16) { // bclr / blr / bclrl
                    bool lk = instr & 1;
                    uint32_t bo = (instr >> 21) & 0x1F;
                    uint32_t bi = (instr >> 16) & 0x1F;
                    if (lk) {
                        std::stringstream ss;
                        if ((bo & 0x14) != 0x14) {
                            ss << "if (EvaluateCondition(ctx, " << bo << ", " << bi << ")) { ";
                        }
                        ss << "uint32_t target = ctx->lr; ctx->lr = " << HexStr(pc + 4) << "; g_Memory.DispatchIndirectBranch(ctx, target);";
                        if ((bo & 0x14) != 0x14) {
                            ss << " }";
                        }
                        s = ss.str();
                    } else {
                        if ((bo & 0x14) == 0x14) {
                            s = "return;";
                        } else {
                            s = "if (EvaluateCondition(ctx, " + std::to_string(bo) + ", " + std::to_string(bi) + ")) return;";
                        }
                    }
                } else if (xo == 528) { // bcctr / bctr / bctrl
                    bool lk = instr & 1;
                    uint32_t bo = (instr >> 21) & 0x1F;
                    uint32_t bi = (instr >> 16) & 0x1F;
                    bool unconditional = ((bo & 0x14) == 0x14);

                    std::stringstream ss;
                    if (!unconditional) {
                        ss << "if (EvaluateCondition(ctx, " << bo << ", " << bi << ")) ";
                    }
                    if (lk) {
                        ss << "{ ctx->lr = " << HexStr(pc + 4) << "; g_Memory.DispatchIndirectBranch(ctx, ctx->ctr); }";
                    } else {
                        ss << "{ g_Memory.DispatchIndirectBranch(ctx, ctx->ctr); return; }";
                    }
                    s = ss.str();
                } else if (xo == 193) { // crxor
                    uint32_t crD = (instr >> 21) & 0x1F;
                    uint32_t crA = (instr >> 16) & 0x1F;
                    uint32_t crB = (instr >> 11) & 0x1F;
                    s = "{ uint32_t bA = (ctx->cr >> (31 - " + std::to_string(crA) + ")) & 1; ";
                    s += "uint32_t bB = (ctx->cr >> (31 - " + std::to_string(crB) + ")) & 1; ";
                    s += "uint32_t res = bA ^ bB; ";
                    s += "ctx->cr = (ctx->cr & ~(1u << (31 - " + std::to_string(crD) + "))) | (res << (31 - " + std::to_string(crD) + ")); }";
                } else {
                    s = "/* Op19 XO " + std::to_string(xo) + " */;";
                }
                break;
            }
            case 31: {
                uint32_t xo = (instr >> 1) & 0x3FF;
                switch (xo) {
                    case 0: { // cmp
                        uint32_t crfD = (instr >> 23) & 7;
                        s = "UpdateCRField(ctx, " + std::to_string(crfD) + ", (int32_t)" + RStr(rA) + ", (int32_t)" + RStr(rB) + ");";
                        break;
                    }
                    case 32: { // cmpl
                        uint32_t crfD = (instr >> 23) & 7;
                        s = "UpdateCRFieldLogical(ctx, " + std::to_string(crfD) + ", " + RStr(rA) + ", " + RStr(rB) + ");";
                        break;
                    }
                    case 266: // add
                        s = RStr(rD) + " = " + RStr(rA) + " + " + RStr(rB) + ";";
                        if (rc) s += " UpdateCR0(ctx, " + RStr(rD) + ");";
                        break;
                    case 10: // addc
                        s = RStr(rD) + " = " + RStr(rA) + " + " + RStr(rB) + ";";
                        break;
                    case 40: // subf
                        s = RStr(rD) + " = " + RStr(rB) + " - " + RStr(rA) + ";";
                        if (rc) s += " UpdateCR0(ctx, " + RStr(rD) + ");";
                        break;
                    case 8: // subfc
                        s = RStr(rD) + " = " + RStr(rB) + " - " + RStr(rA) + ";";
                        break;
                    case 104: // neg
                        s = RStr(rD) + " = (uint32_t)(-(int32_t)" + RStr(rA) + ");";
                        if (rc) s += " UpdateCR0(ctx, " + RStr(rD) + ");";
                        break;
                    case 444: // or / mr
                        if (rD == rB) s = RStr(rA) + " = " + RStr(rD) + ";";
                        else s = RStr(rA) + " = " + RStr(rD) + " | " + RStr(rB) + ";";
                        if (rc) s += " UpdateCR0(ctx, " + RStr(rA) + ");";
                        break;
                    case 28: // and
                        s = RStr(rA) + " = " + RStr(rD) + " & " + RStr(rB) + ";";
                        if (rc) s += " UpdateCR0(ctx, " + RStr(rA) + ");";
                        break;
                    case 316: // xor
                        s = RStr(rA) + " = " + RStr(rD) + " ^ " + RStr(rB) + ";";
                        if (rc) s += " UpdateCR0(ctx, " + RStr(rA) + ");";
                        break;
                    case 124: // nor
                        s = RStr(rA) + " = ~(" + RStr(rD) + " | " + RStr(rB) + ");";
                        if (rc) s += " UpdateCR0(ctx, " + RStr(rA) + ");";
                        break;
                    case 24: // slw
                        s = RStr(rA) + " = (" + RStr(rB) + " & 32) ? 0 : (" + RStr(rD) + " << (" + RStr(rB) + " & 31));";
                        if (rc) s += " UpdateCR0(ctx, " + RStr(rA) + ");";
                        break;
                    case 536: // srw
                        s = RStr(rA) + " = (" + RStr(rB) + " & 32) ? 0 : (" + RStr(rD) + " >> (" + RStr(rB) + " & 31));";
                        if (rc) s += " UpdateCR0(ctx, " + RStr(rA) + ");";
                        break;
                    case 824: { // srawi
                        uint32_t sh = (instr >> 11) & 0x1F;
                        s = RStr(rA) + " = (uint32_t)((int32_t)" + RStr(rD) + " >> " + std::to_string(sh) + ");";
                        if (rc) s += " UpdateCR0(ctx, " + RStr(rA) + ");";
                        break;
                    }
                    case 23: { // lwzx
                        std::string addr = (rA == 0) ? RStr(rB) : (RStr(rA) + " + " + RStr(rB));
                        s = RStr(rD) + " = g_Memory.Read32(" + addr + ");";
                        break;
                    }
                    case 151: { // stwx
                        std::string addr = (rA == 0) ? RStr(rB) : (RStr(rA) + " + " + RStr(rB));
                        s = "g_Memory.Write32(" + addr + ", " + RStr(rD) + ");";
                        break;
                    }
                    case 87: { // lbzx
                        std::string addr = (rA == 0) ? RStr(rB) : (RStr(rA) + " + " + RStr(rB));
                        s = RStr(rD) + " = g_Memory.Read8(" + addr + ");";
                        break;
                    }
                    case 215: { // stbx
                        std::string addr = (rA == 0) ? RStr(rB) : (RStr(rA) + " + " + RStr(rB));
                        s = "g_Memory.Write8(" + addr + ", (uint8_t)" + RStr(rD) + ");";
                        break;
                    }
                    case 279: { // lhzx
                        std::string addr = (rA == 0) ? RStr(rB) : (RStr(rA) + " + " + RStr(rB));
                        s = RStr(rD) + " = g_Memory.Read16(" + addr + ");";
                        break;
                    }
                    case 407: { // sthx
                        std::string addr = (rA == 0) ? RStr(rB) : (RStr(rA) + " + " + RStr(rB));
                        s = "g_Memory.Write16(" + addr + ", (uint16_t)" + RStr(rD) + ");";
                        break;
                    }
                    case 790: { // lhbrx
                        std::string addr = (rA == 0) ? RStr(rB) : (RStr(rA) + " + " + RStr(rB));
                        s = "{ uint16_t v = g_Memory.Read16(" + addr + "); " + RStr(rD) + " = WII_BYTESWAP16(v); }";
                        break;
                    }
                    case 534: { // lwbrx
                        std::string addr = (rA == 0) ? RStr(rB) : (RStr(rA) + " + " + RStr(rB));
                        s = "{ uint32_t v = g_Memory.Read32(" + addr + "); " + RStr(rD) + " = WII_BYTESWAP32(v); }";
                        break;
                    }
                    case 918: { // sthbrx
                        std::string addr = (rA == 0) ? RStr(rB) : (RStr(rA) + " + " + RStr(rB));
                        s = "g_Memory.Write16(" + addr + ", WII_BYTESWAP16((uint16_t)" + RStr(rD) + "));";
                        break;
                    }
                    case 662: { // stwbrx
                        std::string addr = (rA == 0) ? RStr(rB) : (RStr(rA) + " + " + RStr(rB));
                        s = "g_Memory.Write32(" + addr + ", WII_BYTESWAP32(" + RStr(rD) + "));";
                        break;
                    }
                    case 467: { // mtspr
                        uint32_t spr = (rB << 5) | rA;
                        if (spr == 8) s = "ctx->lr = " + RStr(rD) + ";";
                        else if (spr == 9) s = "ctx->ctr = " + RStr(rD) + ";";
                        else if (spr == 1) s = "ctx->xer = " + RStr(rD) + ";";
                        else if (spr >= 912 && spr <= 919) s = "ctx->gqr[" + std::to_string(spr - 912) + "].SetRaw(" + RStr(rD) + ");";
                        else s = "/* mtspr: " + std::to_string(spr) + " */;";
                        break;
                    }
                    case 371: { // mftb / mftbu
                        uint32_t tbr = (rB << 5) | rA;
                        if (tbr == 269) {
                            s = RStr(rD) + " = (uint32_t)(GetTimeBaseTicks() >> 32);";
                        } else {
                            s = RStr(rD) + " = (uint32_t)(GetTimeBaseTicks() & 0xFFFFFFFF);";
                        }
                        break;
                    }
                    case 339: { // mfspr
                        uint32_t spr = (rB << 5) | rA;
                        if (spr == 8) s = RStr(rD) + " = ctx->lr;";
                        else if (spr == 9) s = RStr(rD) + " = ctx->ctr;";
                        else if (spr == 1) s = RStr(rD) + " = ctx->xer;";
                        else if (spr == 268) s = RStr(rD) + " = (uint32_t)(GetTimeBaseTicks() & 0xFFFFFFFF);";
                        else if (spr == 269) s = RStr(rD) + " = (uint32_t)(GetTimeBaseTicks() >> 32);";
                        else if (spr >= 912 && spr <= 919) s = RStr(rD) + " = ctx->gqr[" + std::to_string(spr - 912) + "].GetRaw();";
                        else s = RStr(rD) + " = 0;";
                        break;
                    }
                    case 83: // mfmsr
                        s = RStr(rD) + " = 0x00002030;";
                        break;
                    case 146: // mtmsr
                        s = "/* mtmsr */;";
                        break;
                    case 19: // mfcr
                        s = RStr(rD) + " = ctx->cr;";
                        break;
                    case 11: // mulhwu
                        s = RStr(rD) + " = (uint32_t)(((uint64_t)" + RStr(rA) + " * (uint64_t)" + RStr(rB) + ") >> 32);";
                        if (rc) s += " UpdateCR0(ctx, " + RStr(rD) + ");";
                        break;
                    case 75: // mulhw
                        s = RStr(rD) + " = (uint32_t)(((int64_t)(int32_t)" + RStr(rA) + " * (int64_t)(int32_t)" + RStr(rB) + ") >> 32);";
                        if (rc) s += " UpdateCR0(ctx, " + RStr(rD) + ");";
                        break;
                    case 235: // mullw
                        s = RStr(rD) + " = (uint32_t)((int32_t)" + RStr(rA) + " * (int32_t)" + RStr(rB) + ");";
                        if (rc) s += " UpdateCR0(ctx, " + RStr(rD) + ");";
                        break;
                    case 459: // divwu
                        s = RStr(rD) + " = (" + RStr(rB) + " == 0) ? 0 : (" + RStr(rA) + " / " + RStr(rB) + ");";
                        if (rc) s += " UpdateCR0(ctx, " + RStr(rD) + ");";
                        break;
                    case 491: // divw
                        s = RStr(rD) + " = ((int32_t)" + RStr(rB) + " == 0 || ((int32_t)" + RStr(rA) + " == (int32_t)0x80000000 && (int32_t)" + RStr(rB) + " == -1)) ? 0 : (uint32_t)((int32_t)" + RStr(rA) + " / (int32_t)" + RStr(rB) + ");";
                        if (rc) s += " UpdateCR0(ctx, " + RStr(rD) + ");";
                        break;
                    case 26: { // cntlzw
                        s = "{ uint32_t val = " + RStr(rD) + "; uint32_t lz = 0; while (lz < 32 && !(val & (0x80000000 >> lz))) lz++; " + RStr(rA) + " = lz; }";
                        if (rc) s += " UpdateCR0(ctx, " + RStr(rA) + ");";
                        break;
                    }
                    case 60: // andc
                        s = RStr(rA) + " = " + RStr(rD) + " & ~" + RStr(rB) + ";";
                        if (rc) s += " UpdateCR0(ctx, " + RStr(rA) + ");";
                        break;
                    case 792: // sraw
                        s = RStr(rA) + " = (uint32_t)((int32_t)" + RStr(rD) + " >> (" + RStr(rB) + " & 31));";
                        if (rc) s += " UpdateCR0(ctx, " + RStr(rA) + ");";
                        break;
                    case 922: // extsh
                        s = RStr(rA) + " = (uint32_t)(int32_t)(int16_t)" + RStr(rD) + ";";
                        if (rc) s += " UpdateCR0(ctx, " + RStr(rA) + ");";
                        break;
                    case 954: // extsb
                        s = RStr(rA) + " = (uint32_t)(int32_t)(int8_t)" + RStr(rD) + ";";
                        if (rc) s += " UpdateCR0(ctx, " + RStr(rA) + ");";
                        break;
                    case 144: { // mtcrf
                        uint32_t crm = (instr >> 12) & 0xFF;
                        s = "{ uint32_t v = " + RStr(rD) + "; ";
                        for (int i = 0; i < 8; ++i) {
                            if (crm & (1 << (7 - i))) {
                                s += "ctx->SetCRField(" + std::to_string(i) + ", (v >> " + std::to_string(28 - i * 4) + ") & 0xF); ";
                            }
                        }
                        s += "}";
                        break;
                    }
                    case 183: { // stwux
                        std::string addr = RStr(rA) + " + " + RStr(rB);
                        s = "{ uint32_t ea = " + addr + "; g_Memory.Write32(ea, " + RStr(rD) + "); " + RStr(rA) + " = ea; }";
                        break;
                    }
                    case 138: // adde
                        s = "{ uint64_t ca = (ctx->xer >> 29) & 1; uint64_t res = (uint64_t)" + RStr(rA) + " + (uint64_t)" + RStr(rB) + " + ca; ctx->xer = (ctx->xer & ~0x20000000u) | ((res >> 32) ? 0x20000000u : 0); " + RStr(rD) + " = (uint32_t)res; }";
                        if (rc) s += " UpdateCR0(ctx, " + RStr(rD) + ");";
                        break;
                    case 136: // subfe
                        s = "{ uint64_t ca = (ctx->xer >> 29) & 1; uint64_t res = (uint64_t)~" + RStr(rA) + " + (uint64_t)" + RStr(rB) + " + ca; ctx->xer = (ctx->xer & ~0x20000000u) | ((res >> 32) ? 0x20000000u : 0); " + RStr(rD) + " = (uint32_t)res; }";
                        if (rc) s += " UpdateCR0(ctx, " + RStr(rD) + ");";
                        break;
                    case 202: // addze
                        s = "{ uint64_t ca = (ctx->xer >> 29) & 1; uint64_t res = (uint64_t)" + RStr(rA) + " + ca; ctx->xer = (ctx->xer & ~0x20000000u) | ((res >> 32) ? 0x20000000u : 0); " + RStr(rD) + " = (uint32_t)res; }";
                        if (rc) s += " UpdateCR0(ctx, " + RStr(rD) + ");";
                        break;
                    case 200: // subfze
                        s = "{ uint64_t ca = (ctx->xer >> 29) & 1; uint64_t res = (uint64_t)~" + RStr(rA) + " + ca; ctx->xer = (ctx->xer & ~0x20000000u) | ((res >> 32) ? 0x20000000u : 0); " + RStr(rD) + " = (uint32_t)res; }";
                        if (rc) s += " UpdateCR0(ctx, " + RStr(rD) + ");";
                        break;
                    case 234: // addme
                        s = "{ uint64_t ca = (ctx->xer >> 29) & 1; uint64_t res = (uint64_t)" + RStr(rA) + " + ca - 1; ctx->xer = (ctx->xer & ~0x20000000u) | ((res >> 32) ? 0x20000000u : 0); " + RStr(rD) + " = (uint32_t)res; }";
                        if (rc) s += " UpdateCR0(ctx, " + RStr(rD) + ");";
                        break;
                    case 535: { // lfsx
                        std::string addr = (rA == 0) ? RStr(rB) : (RStr(rA) + " + " + RStr(rB));
                        s = "{ uint32_t u = g_Memory.Read32(" + addr + "); float f; std::memcpy(&f, &u, 4); " + FStr(rD) + ".ps[0] = f; " + FStr(rD) + ".ps[1] = f; }";
                        break;
                    }
                    case 599: { // lfdx
                        std::string addr = (rA == 0) ? RStr(rB) : (RStr(rA) + " + " + RStr(rB));
                        s = "{ uint64_t u = ((uint64_t)g_Memory.Read32(" + addr + ") << 32) | g_Memory.Read32(" + addr + " + 4); double d; std::memcpy(&d, &u, 8); " + FStr(rD) + ".d = d; }";
                        break;
                    }
                    case 663: { // stfsx
                        std::string addr = (rA == 0) ? RStr(rB) : (RStr(rA) + " + " + RStr(rB));
                        s = "{ float f = (float)" + FStr(rD) + ".ps[0]; uint32_t u; std::memcpy(&u, &f, 4); g_Memory.Write32(" + addr + ", u); }";
                        break;
                    }
                    case 983: { // stfiwx
                        std::string addr = (rA == 0) ? RStr(rB) : (RStr(rA) + " + " + RStr(rB));
                        s = "{ uint32_t u = (uint32_t)" + FStr(rD) + ".u64; g_Memory.Write32(" + addr + ", u); }";
                        break;
                    }
                    case 4: { // tw
                        uint32_t to = (instr >> 21) & 0x1F;
                        if (to == 31) {
                            s = "/* tw trap */ return;";
                        } else {
                            s = "/* tw */;";
                        }
                        break;
                    }
                    case 598: case 854: case 86: case 470: case 54: case 278: case 1014: case 982: case 210: case 595:
                        s = "/* sync/cache/mmu nop */;";
                        break;
                    default:
                        s = "/* Op31 XO: " + std::to_string(xo) + " */;";
                        break;
                }
                break;
            }
            case 59: { // single fp ops
                uint32_t xo = (instr >> 1) & 0x3FF;
                uint32_t fD = (instr >> 21) & 0x1F;
                uint32_t fA = (instr >> 16) & 0x1F;
                uint32_t fB = (instr >> 11) & 0x1F;
                uint32_t fC = (instr >> 6) & 0x1F;
                if (xo == 21) { // fadds
                    s = FStr(fD) + ".ps[0] = " + FStr(fA) + ".ps[0] + " + FStr(fB) + ".ps[0]; " + FStr(fD) + ".ps[1] = " + FStr(fD) + ".ps[0];";
                } else if (xo == 20) { // fsubs
                    s = FStr(fD) + ".ps[0] = " + FStr(fA) + ".ps[0] - " + FStr(fB) + ".ps[0]; " + FStr(fD) + ".ps[1] = " + FStr(fD) + ".ps[0];";
                } else if (xo == 25) { // fmuls
                    s = FStr(fD) + ".ps[0] = " + FStr(fA) + ".ps[0] * " + FStr(fC) + ".ps[0]; " + FStr(fD) + ".ps[1] = " + FStr(fD) + ".ps[0];";
                } else if (xo == 18) { // fdivs
                    s = FStr(fD) + ".ps[0] = " + FStr(fA) + ".ps[0] / " + FStr(fB) + ".ps[0]; " + FStr(fD) + ".ps[1] = " + FStr(fD) + ".ps[0];";
                } else if (xo == 29) { // fmadds
                    s = FStr(fD) + ".ps[0] = " + FStr(fA) + ".ps[0] * " + FStr(fC) + ".ps[0] + " + FStr(fB) + ".ps[0]; " + FStr(fD) + ".ps[1] = " + FStr(fD) + ".ps[0];";
                } else if (xo == 28) { // fmsubs
                    s = FStr(fD) + ".ps[0] = " + FStr(fA) + ".ps[0] * " + FStr(fC) + ".ps[0] - " + FStr(fB) + ".ps[0]; " + FStr(fD) + ".ps[1] = " + FStr(fD) + ".ps[0];";
                } else {
                    s = "/* Op59 XO: " + std::to_string(xo) + " */;";
                }
                break;
            }
            case 63: { // floating point double ops / fmr
                uint32_t xo = (instr >> 1) & 0x3FF;
                uint32_t fD = (instr >> 21) & 0x1F;
                uint32_t fA = (instr >> 16) & 0x1F;
                uint32_t fB = (instr >> 11) & 0x1F;
                uint32_t fC = (instr >> 6) & 0x1F;
                if (xo == 72) { // fmr
                    s = FStr(fD) + ".d = " + FStr(fB) + ".d;";
                } else if (xo == 264) { // fabs
                    s = FStr(fD) + ".d = std::abs(" + FStr(fB) + ".d);";
                } else if (xo == 40) { // fneg
                    s = FStr(fD) + ".d = -" + FStr(fB) + ".d;";
                } else if (xo == 21) { // fadd
                    s = FStr(fD) + ".d = " + FStr(fA) + ".d + " + FStr(fB) + ".d;";
                } else if (xo == 20) { // fsub
                    s = FStr(fD) + ".d = " + FStr(fA) + ".d - " + FStr(fB) + ".d;";
                } else if (xo == 25) { // fmul
                    s = FStr(fD) + ".d = " + FStr(fA) + ".d * " + FStr(fC) + ".d;";
                } else if (xo == 18) { // fdiv
                    s = FStr(fD) + ".d = " + FStr(fA) + ".d / " + FStr(fB) + ".d;";
                } else if (xo == 29) { // fmadd
                    s = FStr(fD) + ".d = " + FStr(fA) + ".d * " + FStr(fC) + ".d + " + FStr(fB) + ".d;";
                } else if (xo == 28) { // fmsub
                    s = FStr(fD) + ".d = " + FStr(fA) + ".d * " + FStr(fC) + ".d - " + FStr(fB) + ".d;";
                } else if (xo == 12) { // frsp
                    s = FStr(fD) + ".d = (double)(float)" + FStr(fB) + ".d;";
                } else if (xo == 14) { // fctiw
                    s = FStr(fD) + ".u64 = (uint32_t)(int32_t)std::round(" + FStr(fB) + ".d);";
                } else if (xo == 15) { // fctiwz
                    s = FStr(fD) + ".u64 = (uint32_t)(int32_t)" + FStr(fB) + ".d;";
                } else if (xo == 0) { // fcmpu
                    uint32_t crfD = (instr >> 23) & 7;
                    s = "{ double a = " + FStr(fA) + ".d, b = " + FStr(fB) + ".d; uint32_t c = (a < b) ? 8 : ((a > b) ? 4 : 2); ctx->SetCRField(" + std::to_string(crfD) + ", c); }";
                } else if (xo == 281) { // fnabs
                    s = FStr(fD) + ".d = -std::abs(" + FStr(fB) + ".d);";
                } else if (xo == 313) { // fnmsub
                    s = FStr(fD) + ".d = -(" + FStr(fA) + ".d * " + FStr(fC) + ".d - " + FStr(fB) + ".d);";
                } else if (xo == 285) { // fnmadd
                    s = FStr(fD) + ".d = -(" + FStr(fA) + ".d * " + FStr(fC) + ".d + " + FStr(fB) + ".d);";
                } else {
                    s = "/* Op63 XO: " + std::to_string(xo) + " */;";
                }
                break;
            }
            default:
                s = "/* Opcode: " + std::to_string(op) + " */;";
                break;
        }

        return s;
    }
};

} // namespace WiiPort
