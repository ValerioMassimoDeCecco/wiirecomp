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

struct BasicBlock {
    uint32_t startAddress;
    uint32_t endAddress; // Esclusivo
    std::vector<uint32_t> successors;
};

struct RecompiledFunction {
    uint32_t entryAddress;
    std::string name;
    std::vector<BasicBlock> blocks;
};

class PPCLifter {
public:
    const DolExecutable& dol;
    std::set<uint32_t> discoveredFunctions;
    std::map<uint32_t, std::string> symbolNames;

    PPCLifter(const DolExecutable& exec) : dol(exec) {
        // Aggiunge l'EntryPoint come prima funzione nota
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

    // Scansione e scoperta automatica delle funzioni tramite analisi statica del flusso di controllo (CFG)
    void DiscoverAllFunctions() {
        bool changed = true;
        std::set<uint32_t> analyzed;

        while (changed) {
            changed = false;
            std::vector<uint32_t> currentQueue(discoveredFunctions.begin(), discoveredFunctions.end());

            for (uint32_t funcAddr : currentQueue) {
                if (analyzed.count(funcAddr)) continue;
                analyzed.insert(funcAddr);

                uint32_t pc = funcAddr;
                uint32_t instr = 0;
                while (dol.ReadInstruction(pc, instr)) {
                    uint32_t op = (instr >> 26) & 0x3F;

                    // Istruzione di Branch (b, bl, ba, bla)
                    if (op == 18) {
                        bool lk = instr & 1;
                        int32_t li = (instr & 0x03FFFFFC);
                        if (li & 0x02000000) li |= 0xFC000000; // Sign-extend a 32-bit
                        uint32_t target = (instr & 2) ? (uint32_t)li : (pc + li);

                        // Se è una chiamata a funzione (Link bit = 1)
                        if (lk && dol.IsCodeAddress(target)) {
                            if (discoveredFunctions.insert(target).second) {
                                changed = true;
                            }
                        }
                    }

                    // Se incontriamo un ritorno da funzione incondizionato (blr)
                    if (instr == 0x4E800020) { // blr (bclr 20, 0)
                        break;
                    }

                    pc += 4;
                }
            }
        }
    }

    // Genera il codice C++ per una singola funzione
    std::string LiftFunction(uint32_t funcAddress) {
        std::stringstream out;
        std::string fname = GetFunctionName(funcAddress);

        out << "// Function at 0x" << std::hex << funcAddress << "\n";
        out << "void " << fname << "(PPCContext* ctx) {\n";
        out << "    if (g_ModEngine.ExecutePreHook(0x" << std::hex << funcAddress << ", ctx)) return;\n";

        uint32_t pc = funcAddress;
        uint32_t instr = 0;
        std::set<uint32_t> internalLabels;

        // Pass 1: Identifica i salti interni alla funzione per posizionare le etichette 'loc_XXXX'
        uint32_t scanPc = funcAddress;
        while (dol.ReadInstruction(scanPc, instr)) {
            uint32_t op = (instr >> 26) & 0x3F;
            if (op == 18 || op == 16) { // b o bc
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
                if (dol.IsCodeAddress(target)) {
                    internalLabels.insert(target);
                }
            }
            if (instr == 0x4E800020) break; // blr
            scanPc += 4;
        }

        // Pass 2: Generazione del corpo della funzione in C++
        while (dol.ReadInstruction(pc, instr)) {
            if (internalLabels.count(pc)) {
                out << "loc_0x" << std::hex << pc << ":\n";
            }

            out << "    /* 0x" << std::hex << pc << ": 0x" << std::setfill('0') << std::setw(8) << instr << " */ "
                << LiftInstruction(instr, pc) << "\n";

            if (instr == 0x4E800020) { // blr
                break;
            }
            pc += 4;
        }

        out << "}\n\n";
        return out.str();
    }

private:
    std::string LiftInstruction(uint32_t instr, uint32_t pc) {
        uint32_t op = (instr >> 26) & 0x3F;
        std::stringstream ss;

        switch (op) {
            // ================= INTEGER ARITHMETIC / LOGIC =================
            case 14: { // addi / li
                uint32_t rD = (instr >> 21) & 0x1F;
                uint32_t rA = (instr >> 16) & 0x1F;
                int16_t simm = (int16_t)(instr & 0xFFFF);
                if (rA == 0) {
                    ss << "ctx->r[" << rD << "] = " << (int32_t)simm << ";";
                } else {
                    ss << "ctx->r[" << rD << "] = ctx->r[" << rA << "] + " << (int32_t)simm << ";";
                }
                break;
            }
            case 15: { // addis / lis (Add Immediate Shifted)
                uint32_t rD = (instr >> 21) & 0x1F;
                uint32_t rA = (instr >> 16) & 0x1F;
                uint32_t uimm = (instr & 0xFFFF) << 16;
                if (rA == 0) {
                    ss << "ctx->r[" << rD << "] = 0x" << std::hex << uimm << ";";
                } else {
                    ss << "ctx->r[" << rD << "] = ctx->r[" << rA << "] + 0x" << std::hex << uimm << ";";
                }
                break;
            }
            case 28: { // andi.
                uint32_t rA = (instr >> 16) & 0x1F;
                uint32_t rS = (instr >> 21) & 0x1F;
                uint16_t uimm = instr & 0xFFFF;
                ss << "ctx->r[" << rA << "] = ctx->r[" << rS << "] & 0x" << std::hex << uimm << "; "
                   << "UpdateCR0(ctx, ctx->r[" << rA << "]);";
                break;
            }
            case 24: { // ori (spesso usato come nop: ori 0,0,0)
                uint32_t rA = (instr >> 16) & 0x1F;
                uint32_t rS = (instr >> 21) & 0x1F;
                uint16_t uimm = instr & 0xFFFF;
                if (rA == 0 && rS == 0 && uimm == 0) {
                    ss << "/* nop */;";
                } else {
                    ss << "ctx->r[" << rA << "] = ctx->r[" << rS << "] | 0x" << std::hex << uimm << ";";
                }
                break;
            }
            case 21: { // rlwinm (Rotate Left Word Immediate then AND with Mask)
                uint32_t rA = (instr >> 16) & 0x1F;
                uint32_t rS = (instr >> 21) & 0x1F;
                uint32_t sh = (instr >> 11) & 0x1F;
                uint32_t mb = (instr >> 6)  & 0x1F;
                uint32_t me = (instr >> 1)  & 0x1F;
                bool rc = instr & 1;
                ss << "ctx->r[" << rA << "] = PPC_rlwinm(ctx->r[" << rS << "], " 
                   << sh << ", " << mb << ", " << me << ");";
                if (rc) ss << " UpdateCR0(ctx, ctx->r[" << rA << "]);";
                break;
            }
            case 20: { // rlwimi (Rotate Left Word Immediate then Mask Insert)
                uint32_t rA = (instr >> 16) & 0x1F;
                uint32_t rS = (instr >> 21) & 0x1F;
                uint32_t sh = (instr >> 11) & 0x1F;
                uint32_t mb = (instr >> 6)  & 0x1F;
                uint32_t me = (instr >> 1)  & 0x1F;
                bool rc = instr & 1;
                ss << "ctx->r[" << rA << "] = PPC_rlwimi(ctx->r[" << rA << "], ctx->r[" << rS << "], " 
                   << sh << ", " << mb << ", " << me << ");";
                if (rc) ss << " UpdateCR0(ctx, ctx->r[" << rA << "]);";
                break;
            }
            case 23: { // rlwnm (Rotate Left Word then AND with Mask)
                uint32_t rA = (instr >> 16) & 0x1F;
                uint32_t rS = (instr >> 21) & 0x1F;
                uint32_t rB = (instr >> 11) & 0x1F;
                uint32_t mb = (instr >> 6)  & 0x1F;
                uint32_t me = (instr >> 1)  & 0x1F;
                bool rc = instr & 1;
                ss << "ctx->r[" << rA << "] = PPC_rlwinm(ctx->r[" << rS << "], ctx->r[" << rB << "] & 31, " 
                   << mb << ", " << me << ");";
                if (rc) ss << " UpdateCR0(ctx, ctx->r[" << rA << "]);";
                break;
            }

            // ================= MEMORY LOAD / STORE =================
            case 32: { // lwz (Load Word and Zero)
                uint32_t rD = (instr >> 21) & 0x1F;
                uint32_t rA = (instr >> 16) & 0x1F;
                int16_t d = (int16_t)(instr & 0xFFFF);
                ss << "ctx->r[" << rD << "] = g_Memory.Read32(";
                if (rA == 0) ss << d << ");";
                else ss << "ctx->r[" << rA << "] + " << d << ");";
                break;
            }
            case 36: { // stw (Store Word)
                uint32_t rS = (instr >> 21) & 0x1F;
                uint32_t rA = (instr >> 16) & 0x1F;
                int16_t d = (int16_t)(instr & 0xFFFF);
                ss << "g_Memory.Write32(";
                if (rA == 0) ss << d;
                else ss << "ctx->r[" << rA << "] + " << d;
                ss << ", ctx->r[" << rS << "]);";
                break;
            }
            case 34: { // lbz (Load Byte and Zero)
                uint32_t rD = (instr >> 21) & 0x1F;
                uint32_t rA = (instr >> 16) & 0x1F;
                int16_t d = (int16_t)(instr & 0xFFFF);
                ss << "ctx->r[" << rD << "] = g_Memory.Read8(";
                if (rA == 0) ss << d << ");";
                else ss << "ctx->r[" << rA << "] + " << d << ");";
                break;
            }
            case 38: { // stb (Store Byte)
                uint32_t rS = (instr >> 21) & 0x1F;
                uint32_t rA = (instr >> 16) & 0x1F;
                int16_t d = (int16_t)(instr & 0xFFFF);
                ss << "g_Memory.Write8(";
                if (rA == 0) ss << d;
                else ss << "ctx->r[" << rA << "] + " << d;
                ss << ", (uint8_t)ctx->r[" << rS << "]);";
                break;
            }

            // ================= BRANCHES =================
            case 18: { // b, bl, ba, bla
                bool lk = instr & 1;
                int32_t li = (instr & 0x03FFFFFC);
                if (li & 0x02000000) li |= 0xFC000000;
                uint32_t target = (instr & 2) ? li : (pc + li);

                if (lk) { // Chiamata di funzione
                    ss << "ctx->lr = 0x" << std::hex << (pc + 4) << "; "
                       << GetFunctionName(target) << "(ctx);";
                } else { // Salto interno o salto di coda
                    ss << "goto loc_0x" << std::hex << target << ";";
                }
                break;
            }
            case 16: { // bc (Branch Conditional)
                uint32_t bo = (instr >> 21) & 0x1F;
                uint32_t bi = (instr >> 16) & 0x1F;
                int32_t bd = (instr & 0xFFFC);
                if (bd & 0x8000) bd |= 0xFFFF0000;
                uint32_t target = (instr & 2) ? bd : (pc + bd);

                ss << "if (EvaluateCondition(ctx, " << bo << ", " << bi << ")) "
                   << "goto loc_0x" << std::hex << target << ";";
                break;
            }

            // ================= PAIRED-SINGLE SIMD (BROADWAY EXTENSIONS) =================
            case 56: { // psq_l (Paired Single Quantized Load)
                uint32_t fD = (instr >> 21) & 0x1F;
                uint32_t rA = (instr >> 16) & 0x1F;
                bool w = (instr >> 15) & 1;
                uint32_t gqrIndex = (instr >> 12) & 0x7;
                int16_t d = (int16_t)(instr & 0xFFF);
                if (d & 0x800) d |= 0xF000; // Sign extend 12-bit

                ss << "DequantizeLoad(ctx, " << fD << ", ";
                if (rA == 0) ss << d;
                else ss << "ctx->r[" << rA << "] + " << d;
                ss << ", " << (w ? "true" : "false") << ", " << gqrIndex << ");";
                break;
            }
            case 60: { // psq_st (Paired Single Quantized Store)
                uint32_t fS = (instr >> 21) & 0x1F;
                uint32_t rA = (instr >> 16) & 0x1F;
                bool w = (instr >> 15) & 1;
                uint32_t gqrIndex = (instr >> 12) & 0x7;
                int16_t d = (int16_t)(instr & 0xFFF);
                if (d & 0x800) d |= 0xF000;

                ss << "QuantizeStore(ctx, " << fS << ", ";
                if (rA == 0) ss << d;
                else ss << "ctx->r[" << rA << "] + " << d;
                ss << ", " << (w ? "true" : "false") << ", " << gqrIndex << ");";
                break;
            }

            // ================= SUB-OPCODES: OP 4 (Paired-Single Arithmetic) =================
            case 4: {
                uint32_t xo = (instr >> 1) & 0x3FF;
                uint32_t fD = (instr >> 21) & 0x1F;
                uint32_t fA = (instr >> 16) & 0x1F;
                uint32_t fB = (instr >> 11) & 0x1F;
                uint32_t fC = (instr >> 6)  & 0x1F;

                switch (xo) {
                    case 10: // ps_add
                        ss << "ctx->f[" << fD << "].ps[0] = ctx->f[" << fA << "].ps[0] + ctx->f[" << fB << "].ps[0]; "
                           << "ctx->f[" << fD << "].ps[1] = ctx->f[" << fA << "].ps[1] + ctx->f[" << fB << "].ps[1];";
                        break;
                    case 21: // ps_sub
                        ss << "ctx->f[" << fD << "].ps[0] = ctx->f[" << fA << "].ps[0] - ctx->f[" << fB << "].ps[0]; "
                           << "ctx->f[" << fD << "].ps[1] = ctx->f[" << fA << "].ps[1] - ctx->f[" << fB << "].ps[1];";
                        break;
                    case 25: // ps_mul
                        ss << "ctx->f[" << fD << "].ps[0] = ctx->f[" << fA << "].ps[0] * ctx->f[" << fC << "].ps[0]; "
                           << "ctx->f[" << fD << "].ps[1] = ctx->f[" << fA << "].ps[1] * ctx->f[" << fC << "].ps[1];";
                        break;
                    case 29: // ps_madd
                        ss << "ctx->f[" << fD << "].ps[0] = (ctx->f[" << fA << "].ps[0] * ctx->f[" << fC << "].ps[0]) + ctx->f[" << fB << "].ps[0]; "
                           << "ctx->f[" << fD << "].ps[1] = (ctx->f[" << fA << "].ps[1] * ctx->f[" << fC << "].ps[1]) + ctx->f[" << fB << "].ps[1];";
                        break;
                    case 560: // ps_merge00
                        ss << "ctx->f[" << fD << "].ps[0] = ctx->f[" << fA << "].ps[0]; "
                           << "ctx->f[" << fD << "].ps[1] = ctx->f[" << fB << "].ps[0];";
                        break;
                    case 592: // ps_merge01
                        ss << "ctx->f[" << fD << "].ps[0] = ctx->f[" << fA << "].ps[0]; "
                           << "ctx->f[" << fD << "].ps[1] = ctx->f[" << fB << "].ps[1];";
                        break;
                    default:
                        ss << "/* Op4 Unhandled XO: " << xo << " */;";
                        break;
                }
                break;
            }

            // ================= SUB-OPCODES: OP 19 & 31 (System / SPR / Indirect Branch) =================
            case 19: {
                uint32_t xo = (instr >> 1) & 0x3FF;
                if (xo == 16) { // bclr / blr
                    ss << "return;";
                } else if (xo == 528) { // bcctr / bctrl (Salto indiretto dinamico!)
                    bool lk = instr & 1;
                    if (lk) {
                        ss << "ctx->lr = 0x" << std::hex << (pc + 4) << "; ";
                    }
                    ss << "g_Memory.DispatchIndirectBranch(ctx, ctx->ctr);";
                } else {
                    ss << "/* Op19 Unhandled XO: " << xo << " */;";
                }
                break;
            }
            case 31: {
                uint32_t xo = (instr >> 1) & 0x3FF;
                uint32_t rD = (instr >> 21) & 0x1F;
                uint32_t rA = (instr >> 16) & 0x1F;
                uint32_t rB = (instr >> 11) & 0x1F;

                switch (xo) {
                    case 266: { // add
                        ss << "ctx->r[" << rD << "] = ctx->r[" << rA << "] + ctx->r[" << rB << "];";
                        break;
                    }
                    case 40: { // subf (Subtract From: rD = rB - rA)
                        ss << "ctx->r[" << rD << "] = ctx->r[" << rB << "] - ctx->r[" << rA << "];";
                        break;
                    }
                    case 467: { // mtspr (Move to Special Purpose Register)
                        uint32_t spr = ((rA & 0x1F) << 5) | ((rA >> 5) & 0x1F); // Split SPR encoding
                        if (spr == 8) ss << "ctx->lr = ctx->r[" << rD << "];";
                        else if (spr == 9) ss << "ctx->ctr = ctx->r[" << rD << "];";
                        else if (spr == 1) ss << "ctx->xer = ctx->r[" << rD << "];";
                        else if (spr >= 912 && spr <= 919) {
                            ss << "ctx->gqr[" << (spr - 912) << "].SetRaw(ctx->r[" << rD << "]);";
                        } else {
                            ss << "/* mtspr unhandled: " << spr << " */;";
                        }
                        break;
                    }
                    case 339: { // mfspr (Move from Special Purpose Register)
                        uint32_t spr = ((rA & 0x1F) << 5) | ((rA >> 5) & 0x1F);
                        if (spr == 8) ss << "ctx->r[" << rD << "] = ctx->lr;";
                        else if (spr == 9) ss << "ctx->r[" << rD << "] = ctx->ctr;";
                        else if (spr == 1) ss << "ctx->r[" << rD << "] = ctx->xer;";
                        else if (spr >= 912 && spr <= 919) {
                            ss << "ctx->r[" << rD << "] = ctx->gqr[" << (spr - 912) << "].GetRaw();";
                        } else {
                            ss << "/* mfspr unhandled: " << spr << " */;";
                        }
                        break;
                    }
                    default:
                        ss << "/* Op31 Unhandled XO: " << xo << " */;";
                        break;
                }
                break;
            }

            default:
                ss << "/* Opcode non mappato: " << op << " */;";
                break;
        }

        return ss.str();
    }
};

} // namespace WiiPort
