#include <iostream>
#include <fstream>
#include <string>
#include <cstring>
#include <cstdio>
#include <vector>
#include "dol_loader.hpp"
#include "ppc_lifter.hpp"
#include "symbol_map_importer.hpp"
#include "iso_fs_reader.hpp"
#include "libogc_resolver.hpp"

static std::string H(uint32_t v) {
    char b[16];
    snprintf(b, sizeof(b), "0x%08X", v);
    return b;
}

// Cerca nelle prime 'maxWords' istruzioni della funzione un "lwz rD, d(r13)" (lettura da SDA).
// Se rt >= 0 richiede che il registro destinazione sia proprio rt. Ritorna true e lo spiazzamento in outOff.
static bool FindR13Load(const WiiPort::DolExecutable& dol, uint32_t funcAddr, uint32_t maxWords, int rt, int32_t& outOff) {
    for (uint32_t i = 0; i < maxWords; ++i) {
        uint32_t w = 0;
        if (!dol.ReadInstruction(funcAddr + i * 4, w)) return false;
        if ((w & 0xFC1F0000) == 0x800D0000) {                 // lwz rD, d(r13)
            int regD = (int)((w >> 21) & 31);
            if (rt < 0 || regD == rt) {
                outOff = (int16_t)(w & 0xFFFF);
                return true;
            }
        }
    }
    return false;
}

int main(int argc, char* argv[]) {
    std::cout << "=========================================================\n";
    std::cout << "    Wii Universal AOT Recompiler (PPC750CL -> C++)\n";
    std::cout << "=========================================================\n";

    if (argc < 2) {
        std::cout << "Uso: wiirecomp <file.dol> [output.cpp] [--symbols <map_file>]\n";
        return 1;
    }

    std::string dolPath = argv[1];
    std::string outputPath = "recompiled_game.cpp";
    std::string symbolMapPath = "";

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--symbols" && i + 1 < argc) {
            symbolMapPath = argv[++i];
        } else if (arg[0] != '-') {
            outputPath = arg;
        }
    }

    WiiPort::DolExecutable dol;
    if (!dol.Load(dolPath)) {
        std::cerr << "Impossibile caricare il file DOL specificato.\n";
        return 1;
    }

    WiiPort::PPCLifter lifter(dol);

    // Importazione facoltativa della mappa dei simboli (Ghidra / Dolphin / CodeWarrior)
    if (!symbolMapPath.empty()) {
        WiiPort::SymbolMapImporter symbols;
        if (symbols.LoadMapFile(symbolMapPath)) {
            for (const auto& [addr, name] : symbols.addressToName) {
                lifter.AddSymbol(addr, name);
            }
        }
    }

    std::cout << "[CFG] Analisi delle funzioni e ricostruzione del grafo di controllo in corso...\n";
    lifter.DiscoverAllFunctions();

    // ------------------------------------------------------------------
    // 0. Risoluzione PER FIRMA delle funzioni libogc/newlib da agganciare.
    //    Gli indirizzi cambiano da un DOL all'altro: agganciare indirizzi fissi significa
    //    sostituire funzioni a caso (es. leggere r3 di un'altra funzione come "framebuffer").
    // ------------------------------------------------------------------
    WiiPort::LibogcResolveResult libogc = WiiPort::ResolveLibogcSymbols(dol);
    std::cout << "[LIBOGC] Funzioni libogc riconosciute: " << libogc.addr.size() << "/" << WiiPort::kLibogcSignatureCount;
    if (libogc.sameLayoutAsReference) std::cout << " (layout identico al DOL di riferimento)";
    std::cout << "\n";
    for (const auto& k : libogc.missing)   std::cout << "[LIBOGC]   non trovata : " << k << "\n";
    for (const auto& k : libogc.ambiguous) std::cout << "[LIBOGC]   ambigua     : " << k << " (hook non applicato)\n";
    if (!libogc.Has("video_setnextfb")) {
        std::cout << "[LIBOGC] ATTENZIONE: VIDEO_SetNextFramebuffer non trovata. Il framebuffer potra' essere\n"
                     "         rilevato solo dai registri VI; se lo schermo resta scuro questo DOL usa una\n"
                     "         versione di libogc diversa da quella di riferimento.\n";
    }
    for (const auto& kv : libogc.addr) lifter.discoveredFunctions.insert(kv.second);
    lifter.setjmpAddr  = libogc.Get("setjmp");
    lifter.longjmpAddr = libogc.Get("longjmp");
    if (lifter.setjmpAddr)  std::cout << "[LIBOGC]   setjmp      : " << H(lifter.setjmpAddr) << "\n";
    if (lifter.longjmpAddr) std::cout << "[LIBOGC]   longjmp     : " << H(lifter.longjmpAddr) << "\n";

    // Funzioni aggiuntive note solo nel layout del DOL di riferimento (non hanno firma propria)
    static const uint32_t kLegacyFunctions[] = {
        0x8001C6B8, 0x8001C6BC, 0x8003A434, 0x8003A42C, 0x8001FBBC, 0x8001FBA4, 0x8003D6BC, 0x8003D30C,
        0x8003D274, 0x8003D560, 0x8003D628, 0x8003D778, 0x80018140, 0x80009038, 0x80028C54, 0x80028EF4,
        0x80029100, 0x80029108, 0x8002911C, 0x8002916C, 0x800174C0, 0x80017544
    };
    const uint32_t kLegacyInitCpp = 0x80004170;
    if (libogc.sameLayoutAsReference) {
        for (uint32_t a : kLegacyFunctions) {
            if (dol.IsCodeAddress(a)) lifter.discoveredFunctions.insert(a);
        }
    }

    // 1. Rilevamento automatico di devkitPPC _start bootstrap
    uint32_t devkitStartAddr = 0;
    const uint8_t startSig[] = { 0x94, 0x21, 0xFF, 0xF0, 0x7C, 0x08, 0x02, 0xA6, 0x42, 0x9F, 0x00, 0x05, 0x93, 0xC1, 0x00, 0x08 };
    for (const auto& sec : dol.sections) {
        if (!sec.isText || sec.size < sizeof(startSig)) continue;
        for (uint32_t off = 0; off + sizeof(startSig) <= sec.size; off += 4) {
            if (std::memcmp(&sec.data[off], startSig, sizeof(startSig)) == 0) {
                devkitStartAddr = sec.address + off;
                std::cout << "[ENTRY] Rilevato devkitPPC _start bootstrap a " << H(devkitStartAddr) << "\n";
                lifter.discoveredFunctions.insert(devkitStartAddr);
                break;
            }
        }
        if (devkitStartAddr != 0) break;
    }

    uint32_t detectedMain = 0;
    const uint32_t subsysInitAddr = libogc.Get("subsys_init");
    if (devkitStartAddr != 0) {
        for (uint32_t off = devkitStartAddr; off < devkitStartAddr + 0x60; off += 4) {
            uint32_t w = 0;
            if (dol.ReadInstruction(off, w)) {
                if (((w >> 26) & 0x3F) == 18 && (w & 1)) {
                    int32_t li = (w & 0x03FFFFFC);
                    if (li & 0x02000000) li |= 0xFC000000;
                    uint32_t t = (w & 2) ? li : (off + li);
                    if (dol.IsCodeAddress(t) && t != subsysInitAddr) {
                        detectedMain = t;
                        std::cout << "[MAIN] Rilevato main() a " << H(detectedMain) << "\n";
                        lifter.discoveredFunctions.insert(detectedMain);
                        break;
                    }
                }
            }
        }
    }

    // 2. Rilevamento dinamico basi SDA (Small Data Area: r2 e r13)
    uint32_t detectedR2 = 0;
    uint32_t detectedR13 = 0;
    for (const auto& sec : dol.sections) {
        if (!sec.isText) continue;
        for (uint32_t off = 0; off + 8 <= sec.size; off += 4) {
            uint32_t w1 = ((uint32_t)sec.data[off] << 24) | ((uint32_t)sec.data[off+1] << 16) | ((uint32_t)sec.data[off+2] << 8) | sec.data[off+3];
            uint32_t w2 = ((uint32_t)sec.data[off+4] << 24) | ((uint32_t)sec.data[off+5] << 16) | ((uint32_t)sec.data[off+6] << 8) | sec.data[off+7];
            if ((w1 & 0xFFFF0000) == 0x3C400000) { // lis r2, hi
                if ((w2 & 0xFFFF0000) == 0x60420000) { // ori r2, r2, lo
                    detectedR2 = ((w1 & 0xFFFF) << 16) | (w2 & 0xFFFF);
                } else if ((w2 & 0xFFFF0000) == 0x38420000) { // addi r2, r2, lo
                    detectedR2 = ((w1 & 0xFFFF) << 16) + (int16_t)(w2 & 0xFFFF);
                }
            }
            if ((w1 & 0xFFFF0000) == 0x3DA00000) { // lis r13, hi
                if ((w2 & 0xFFFF0000) == 0x61AD0000) { // ori r13, r13, lo
                    detectedR13 = ((w1 & 0xFFFF) << 16) | (w2 & 0xFFFF);
                } else if ((w2 & 0xFFFF0000) == 0x39AD0000) { // addi r13, r13, lo
                    detectedR13 = ((w1 & 0xFFFF) << 16) + (int16_t)(w2 & 0xFFFF);
                }
            }
        }
    }
    if (detectedR2)  std::cout << "[SDA] Rilevato _SDA2_BASE_ (r2): " << H(detectedR2) << "\n";
    if (detectedR13) std::cout << "[SDA] Rilevato _SDA_BASE_ (r13): " << H(detectedR13) << "\n";

    // 3. VIDEO_WaitVSync: senza interrupt di retrace emulati il gioco resterebbe bloccato qui per sempre.
    //    Si usa la firma; in mancanza, il vecchio pattern (che contiene pero' un offset SDA fisso).
    uint32_t waitVSyncAddr = libogc.Get("video_waitvsync");
    int32_t  retraceCountOff = 0;
    if (waitVSyncAddr != 0) {
        // il primo "lwz rD, d(r13)" della funzione legge il contatore di retrace
        if (!FindR13Load(dol, waitVSyncAddr, 32, -1, retraceCountOff)) retraceCountOff = 0;
    } else {
        for (const auto& sec : dol.sections) {
            if (!sec.isText || sec.size < 16) continue;
            for (uint32_t off = 0; off + 12 <= sec.size; off += 4) {
                uint32_t w1 = ((uint32_t)sec.data[off] << 24) | ((uint32_t)sec.data[off+1] << 16) | ((uint32_t)sec.data[off+2] << 8) | sec.data[off+3];
                uint32_t w2 = ((uint32_t)sec.data[off+4] << 24) | ((uint32_t)sec.data[off+5] << 16) | ((uint32_t)sec.data[off+6] << 8) | sec.data[off+7];
                uint32_t w3 = ((uint32_t)sec.data[off+8] << 24) | ((uint32_t)sec.data[off+9] << 16) | ((uint32_t)sec.data[off+10] << 8) | sec.data[off+11];
                if ((w1 & 0xFFFF0000) == 0x812D0000 && (w2 & 0xFFFF07FF) == 0x7C090040 &&
                    (w3 & 0xFFFF0003) == 0x41820000 && (w3 & 0x8000) != 0) {
                    uint32_t fOff = off;
                    while (fOff >= 4) {
                        uint32_t prev = ((uint32_t)sec.data[fOff] << 24) | ((uint32_t)sec.data[fOff+1] << 16) | ((uint32_t)sec.data[fOff+2] << 8) | sec.data[fOff+3];
                        if ((prev & 0xFFFF0000) == 0x94210000) {
                            waitVSyncAddr = sec.address + fOff;
                            retraceCountOff = (int16_t)(w1 & 0xFFFF);
                            break;
                        }
                        fOff -= 4;
                    }
                    if (waitVSyncAddr != 0) break;
                }
            }
            if (waitVSyncAddr != 0) break;
        }
    }
    if (waitVSyncAddr != 0) {
        std::cout << "[VIDEO] Rilevata funzione VIDEO_WaitVSync a " << H(waitVSyncAddr) << "\n";
        lifter.discoveredFunctions.insert(waitVSyncAddr);
    } else {
        std::cout << "[VIDEO] ATTENZIONE: VIDEO_WaitVSync non trovata, il gioco potrebbe bloccarsi in attesa del VSync.\n";
    }

    // 3b. Rilevamento __VIRetraceHandler (gestisce retraceCount, flush shadow VI regs, pre/postRetraceCB come __console_vipostcb)
    uint32_t viRetraceHandlerAddr = 0;
    for (const auto& sec : dol.sections) {
        if (!sec.isText || sec.size < 128) continue;
        auto readW = [&](uint32_t byteOff) -> uint32_t {
            return ((uint32_t)sec.data[byteOff] << 24) | ((uint32_t)sec.data[byteOff+1] << 16) |
                   ((uint32_t)sec.data[byteOff+2] << 8) | sec.data[byteOff+3];
        };
        for (uint32_t off = 0; off + 160 <= sec.size; off += 4) {
            uint32_t w = readW(off);
            if ((w & 0xFC00FFFF) == 0x60002030 || (w & 0xFC00FFFF) == 0x38002030) {
                bool h34 = false, h38 = false, h3c = false;
                for (uint32_t k = 4; k <= 160 && off + k < sec.size; k += 4) {
                    uint32_t wk = readW(off + k);
                    if ((wk & 0xFC00FFFF) == 0x60002034 || (wk & 0xFC00FFFF) == 0x38002034) h34 = true;
                    if ((wk & 0xFC00FFFF) == 0x60002038 || (wk & 0xFC00FFFF) == 0x38002038) h38 = true;
                    if ((wk & 0xFC00FFFF) == 0x6000203C || (wk & 0xFC00FFFF) == 0x3800203C) h3c = true;
                }
                if (h34 && h38 && h3c) {
                    uint32_t cur = off;
                    while (cur >= 4 && (readW(cur) & 0xFFFF0000) != 0x94210000) cur -= 4;
                    if ((readW(cur) & 0xFFFF0000) == 0x94210000) {
                        if (cur >= 4 && readW(cur - 4) == 0x7C0802A6) cur -= 4;
                        viRetraceHandlerAddr = sec.address + cur;
                        break;
                    }
                }
            }
        }
        if (viRetraceHandlerAddr != 0) break;
    }
    if (viRetraceHandlerAddr != 0) {
        std::cout << "[VIDEO] Rilevato __VIRetraceHandler a " << H(viRetraceHandlerAddr) << "\n";
        lifter.discoveredFunctions.insert(viRetraceHandlerAddr);
    }

    // 4. Offset SDA (rispetto a r13) delle variabili arena di libogc, letti dal codice di
    //    SYS_GetArena1Lo/Hi ("lwz r3, d(r13)"). Servono a impostare l'arena MEM1 corretta.
    int32_t arena1LoOff = 0, arena1HiOff = 0;
    bool haveArenaOffsets = false;
    if (libogc.Has("arena1lo") && libogc.Has("arena1hi")) {
        haveArenaOffsets = FindR13Load(dol, libogc.Get("arena1lo"), 16, 3, arena1LoOff) &&
                           FindR13Load(dol, libogc.Get("arena1hi"), 16, 3, arena1HiOff);
    }
    if (haveArenaOffsets) {
        std::cout << "[SYS] Offset SDA arena1: lo=" << arena1LoOff << " hi=" << arena1HiOff << "\n";
    }

    std::cout << "[CFG] Scoperte " << std::to_string(lifter.discoveredFunctions.size()) << " funzioni uniche.\n";

    std::cout << "[LIFTER] Generazione codice C++ nativo su " << outputPath << "...\n";
    std::ofstream out(outputPath);
    if (!out.is_open()) {
        std::cerr << "Impossibile creare il file di output: " << outputPath << "\n";
        return 1;
    }

    // Header del file generato
    out << "// Codice generato automaticamente da Wii Universal AOT Recompiler\n";
    out << "#ifndef NOMINMAX\n";
    out << "#define NOMINMAX\n";
    out << "#endif\n";
    out << "#include <windows.h>\n";
    out << "#include <iostream>\n";
    out << "#include <thread>\n";
    out << "#include <chrono>\n";
    out << "#include <algorithm>\n";
    out << "#include \"wii_runtime.hpp\"\n";
    out << "#include \"ppc_bitops.hpp\"\n";
    out << "#include \"mod_engine.hpp\"\n";
    out << "using namespace WiiPort;\n\n";
    out << "namespace WiiPort { PPCContext g_GameContext; }\n\n";

    out << "// Hook nativo ad alta efficienza per sincronizzazione VSync\n";
    out << "static void Hook_VIDEO_WaitVSync(PPCContext* ctx) {\n";
    if (viRetraceHandlerAddr != 0) {
        out << "    PPCContext saved = *ctx;\n";
        out << "    PPCContext cbCtx = *ctx;\n";
        out << "    cbCtx.r[1] -= 64;\n";
        out << "    cbCtx.r[3] = 4;\n";
        out << "    cbCtx.r[4] = 0;\n";
        out << "    cbCtx.lr = 0;\n";
        out << "    g_Memory.Call(" << H(viRetraceHandlerAddr) << ", &cbCtx);\n";
        out << "    *ctx = saved;\n";
    } else if (retraceCountOff != 0) {
        out << "    if (ctx->r[13] >= 0x80000000 && ctx->r[13] < 0x88000000) {\n";
        out << "        uint32_t off = (ctx->r[13] + (uint32_t)(int32_t)" << retraceCountOff << ") & (MEM1_SIZE - 1);\n";
        out << "        uint32_t val = WII_BYTESWAP32(*reinterpret_cast<uint32_t*>(g_Memory.mem1 + off));\n";
        out << "        *reinterpret_cast<uint32_t*>(g_Memory.mem1 + off) = WII_BYTESWAP32(val + 1);\n";
        out << "    }\n";
    }
    out << "    ctx->r[3] = 1;\n";
    out << "    g_Memory.frameReady = true;\n";
    out << "    std::this_thread::sleep_for(std::chrono::milliseconds(16));\n";
    out << "}\n\n";

    // Prototipi di tutte le funzioni scoperte
    out << "// Prototipi delle funzioni ricompilate\n";
    for (uint32_t addr : lifter.discoveredFunctions) {
        out << "void " << lifter.GetFunctionName(addr) << "(PPCContext* ctx);\n";
    }
    out << "\n";

    // Generazione del corpo di ogni funzione
    for (uint32_t addr : lifter.discoveredFunctions) {
        out << lifter.LiftFunction(addr);
    }

    // Generazione della funzione di inizializzazione della Dispatch Table
    out << "// Registrazione della Global Dispatch Table\n";
    out << "void InitializeGameDispatchTable() {\n";
    for (uint32_t addr : lifter.discoveredFunctions) {
        out << "    g_Memory.RegisterFunction(" << H(addr) << ", &" << lifter.GetFunctionName(addr) << ");\n";
    }
    if (waitVSyncAddr != 0) {
        out << "    g_Memory.RegisterFunction(" << H(waitVSyncAddr) << ", &Hook_VIDEO_WaitVSync);\n";
    }
    out << "}\n\n";

    // Patch alle variabili SDA di libogc (arena MEM1). Gli offset sono derivati dal codice del DOL;
    // i valori "storici" (offset fissi) si applicano solo se il layout e' identico al DOL di riferimento.
    out << "// Variabili SDA di libogc relative all'arena MEM1\n";
    out << "static void ApplyArenaHiSdaPatches(uint32_t r13) {\n";
    out << "    if (r13 == 0) return;\n";
    out << "    uint32_t newHi = 0x80000000 + (uint32_t)MEM1_SIZE - 0x1000;\n";
    if (haveArenaOffsets) {
        out << "    g_Memory.Write32(r13 + (uint32_t)(int32_t)" << arena1HiOff << ", newHi);\n";
    }
    if (libogc.sameLayoutAsReference) {
        out << "    g_Memory.Write32(r13 + 552, newHi);\n";
    }
    out << "}\n\n";
    out << "static void ApplyInitialSdaPatches(uint32_t r13) {\n";
    out << "    if (r13 == 0) return;\n";
    out << "    ApplyArenaHiSdaPatches(r13);\n";
    if (haveArenaOffsets) {
        out << "    g_Memory.Write32(r13 + (uint32_t)(int32_t)" << arena1LoOff << ", g_Memory.arenaLo);\n";
    }
    if (libogc.sameLayoutAsReference) {
        out << "    g_Memory.Write32(r13 + 132, 0xDEADBEEF);\n";
        out << "    g_Memory.Write32(r13 + 144, 0x00000001);\n";
        out << "    g_Memory.Write32(r13 + 556, g_Memory.arenaLo);\n";
        out << "    g_Memory.Write32(r13 + 580, 0x90002000);\n";
        out << "    g_Memory.Write32(r13 + 576, 0x933E0000);\n";
    }
    out << "}\n\n";

    std::string safeDolPath = dolPath;
    for (char& c : safeDolPath) {
        if (c == '\\') c = '/';
    }

    out << "int StartRecompiledGame() {\n";
    out << "    g_Memory.LoadDol(\"" << safeDolPath << "\");\n";
    out << "    InitializeGameDispatchTable();\n";

    // Riepilogo a runtime: rende evidente sulla console se gli hook sono stati applicati
    {
        std::string summary = "[HOOKS] Funzioni libogc agganciate: " + std::to_string(libogc.addr.size()) + "/" +
                              std::to_string(WiiPort::kLibogcSignatureCount);
        if (!libogc.missing.empty() || !libogc.ambiguous.empty()) {
            summary += " (non risolte:";
            for (const auto& k : libogc.missing)   summary += " " + k;
            for (const auto& k : libogc.ambiguous) summary += " " + k + "?";
            summary += ")";
        }
        out << "    LogConsole(\"" << summary << "\");\n";
    }

    // Heap nativo: parte sopra il bss del DOL (non su un indirizzo fisso)
    out << "    static uint32_t s_NativeHeap = ((std::max)(0x80600000u, g_Memory.arenaLo + 0x00100000u) + 31u) & ~31u;\n";
    out << "    static bool s_SysInitDone = false;\n";
    out << "    static auto nativeAlloc = [](uint32_t align, uint32_t sz, bool zero) -> uint32_t {\n";
    out << "        if (align < 32u || (align & (align - 1u)) != 0 || align > 0x00100000u) align = 32u;\n";
    out << "        if (sz == 0u) sz = 32u;\n";
    out << "        uint32_t ptr = (s_NativeHeap + 4u + align - 1u) & ~(align - 1u);\n";
    out << "        uint32_t end = (ptr + sz + 31u) & ~31u;\n";
    out << "        s_NativeHeap = end;\n";
    out << "        uint32_t chunkBytes = (end - (ptr - 4u)) & ~3u;\n";
    out << "        g_Memory.Write32(ptr - 4u, chunkBytes | 1u);\n";
    out << "        if (zero) {\n";
    out << "            for (uint32_t i = 0; i < sz; ++i) g_Memory.Write8(ptr + i, 0);\n";
    out << "        }\n";
    out << "        return ptr;\n";
    out << "    };\n\n";

    if (waitVSyncAddr != 0) {
        out << "    g_ModEngine.RegisterPreHook(" << H(waitVSyncAddr) << ", [](PPCContext* ctx) -> bool {\n";
        out << "        Hook_VIDEO_WaitVSync(ctx);\n";
        out << "        return true;\n";
        out << "    });\n\n";
    }

    // Emette un hook solo se la funzione e' stata riconosciuta nel DOL, usando l'indirizzo TROVATO.
    auto emitHookAt = [&](uint32_t a, const char* key, const char* comment, const char* body) {
        out << "    // " << comment << " [" << key << " @ " << H(a) << "]\n";
        out << "    g_ModEngine.RegisterPreHook(" << H(a) << ", [](PPCContext* ctx) -> bool {\n" << body << "    });\n\n";
    };
    auto emitHook = [&](const char* key, const char* comment, const char* body) {
        if (!libogc.Has(key)) return;
        emitHookAt(libogc.Get(key), key, comment, body);
    };

    // ---- bootstrap / init ----
    emitHook("crt0", "Diagnostica crt0", R"HK(        Log("[DIAG] crt0 entry, LR=" + HexStr(ctx->lr) + " SP=" + HexStr(ctx->r[1]));
        return false;
)HK");
    emitHook("init_user_rt", "Bypass __init_user_runtime", R"HK(        Log("[INIT] Bypass __init_user_runtime per procedere direttamente all'avvio del gioco");
        return true;
)HK");
    if (libogc.sameLayoutAsReference && dol.IsCodeAddress(kLegacyInitCpp)) {
        emitHookAt(kLegacyInitCpp, "init_cpp", "Bypass __init_cpp (solo layout di riferimento)", R"HK(        Log("[INIT] Bypass __init_cpp");
        return true;
)HK");
    }
    emitHook("init_array", "Bypass __init_array", R"HK(        Log("[INIT] Bypass __init_array");
        return true;
)HK");
    emitHook("register_frame", "Bypass __register_frame_info", R"HK(        Log("[INIT] Bypass __register_frame_info");
        return true;
)HK");

    // ---- video ----
    emitHook("vi_write_i2c", "Evita il bit-banging I2C verso l'encoder TV", R"HK(        return true;
)HK");
    emitHook("vi_init_encoder", "Evita l'init dell'encoder video", R"HK(        return true;
)HK");
    emitHook("video_init", "Diagnostica VIDEO_Init", R"HK(        Log("[DIAG] VIDEO_Init entry, LR=" + HexStr(ctx->lr));
        return false;
)HK");
    emitHook("con_initex", "Diagnostica CON_InitEx", R"HK(        Log("[DIAG] CON_InitEx entry, LR=" + HexStr(ctx->lr));
        return false;
)HK");
    // VIDEO_SetNextFramebuffer: cattura soltanto l'indirizzo XFB (validato) e lascia eseguire la
    // funzione originale, che aggiorna da sola le strutture interne di libogc.
    emitHook("video_setnextfb", "Cattura l'indirizzo XFB", R"HK(        g_Memory.SetXfbAddress(ctx->r[3], "VIDEO_SetNextFramebuffer");
        return false;
)HK");

    // ---- arena / sistema ----
    emitHook("arena1lo", "SYS_GetArena1Lo: parte dopo il bss del DOL", R"HK(        ctx->r[3] = g_Memory.arenaLo;
        return true;
)HK");
    emitHook("arena1hi", "SYS_GetArena1Hi: arena MEM1 nativa a 128 MB", R"HK(        ctx->r[3] = 0x80000000 + (uint32_t)MEM1_SIZE - 0x1000;
        return true;
)HK");
    emitHook("subsys_init", "Bypass init sottosistemi hardware libogc (IRQ/LWP/RTC)", R"HK(        Log("[LIBOGC] Bypass subsys hardware init");
        return true;
)HK");
    emitHook("lwp_heap_alloc", "__lwp_heap_allocate: heap nativo", R"HK(        uint32_t size = ctx->r[4];
        uint32_t prev = s_NativeHeap;
        s_NativeHeap = (s_NativeHeap + size + 31) & ~31;
        for (uint32_t i = 0; i < size; ++i) g_Memory.Write8(prev + i, 0);
        ctx->r[3] = prev;
        Log("[LWP_HEAP] Allocati " + std::to_string(size) + " bytes a " + HexStr(prev) + " da LR=" + HexStr(ctx->lr));
        return true;
)HK");
    emitHook("lwp_dispatch_a", "__lwp_thread_dispatch (A)", R"HK(        return true;
)HK");
    emitHook("lwp_dispatch_b", "__lwp_thread_dispatch (B)", R"HK(        return true;
)HK");
    emitHook("lwp_misc", "Schedulatore LWP", R"HK(        ctx->r[3] = 0;
        return true;
)HK");

    // ---- WPAD ----
    emitHook("wpad_init", "WPAD_Init", R"HK(        Log("[WPAD] WPAD_Init bypass: ritorno OK (0)");
        ctx->r[3] = 0;
        return true;
)HK");
    emitHook("wpad_a", "WPAD (A) - ScanPads", R"HK(        ctx->r[3] = 1;
        return true;
)HK");
    emitHook("wpad_b", "WPAD_ButtonsDown con input da tastiera", R"HK(        static uint32_t s_LastHeldKeys = 0;
        uint32_t cur = 0;
        if ((GetAsyncKeyState(VK_RETURN) & 0x8000) || (GetAsyncKeyState(VK_SPACE) & 0x8000) || (GetAsyncKeyState('A') & 0x8000) || (GetAsyncKeyState('Z') & 0x8000)) cur |= 0x0008; // A
        if ((GetAsyncKeyState('X') & 0x8000) || (GetAsyncKeyState('B') & 0x8000) || (GetAsyncKeyState(VK_BACK) & 0x8000)) cur |= 0x0004; // B
        if ((GetAsyncKeyState('1') & 0x8000) || (GetAsyncKeyState(VK_NUMPAD1) & 0x8000)) cur |= 0x0002; // 1
        if ((GetAsyncKeyState('2') & 0x8000) || (GetAsyncKeyState(VK_NUMPAD2) & 0x8000)) cur |= 0x0001; // 2
        if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) || (GetAsyncKeyState('H') & 0x8000)) cur |= 0x0080; // HOME
        if (GetAsyncKeyState(VK_LEFT) & 0x8000) cur |= 0x0100; // LEFT
        if (GetAsyncKeyState(VK_RIGHT) & 0x8000) cur |= 0x0200; // RIGHT
        if (GetAsyncKeyState(VK_DOWN) & 0x8000) cur |= 0x0400; // DOWN
        if (GetAsyncKeyState(VK_UP) & 0x8000) cur |= 0x0800; // UP
        if ((GetAsyncKeyState(VK_OEM_PLUS) & 0x8000) || (GetAsyncKeyState('E') & 0x8000)) cur |= 0x1000; // PLUS
        if ((GetAsyncKeyState(VK_OEM_MINUS) & 0x8000) || (GetAsyncKeyState('Q') & 0x8000)) cur |= 0x0010; // MINUS

        uint32_t down = cur & ~s_LastHeldKeys;
        s_LastHeldKeys = cur;
        ctx->r[3] = (ctx->r[3] == 0) ? down : 0;
        return true;
)HK");
    emitHook("wpad_c", "WPAD_ButtonsHeld con input da tastiera", R"HK(        uint32_t cur = 0;
        if ((GetAsyncKeyState(VK_RETURN) & 0x8000) || (GetAsyncKeyState(VK_SPACE) & 0x8000) || (GetAsyncKeyState('A') & 0x8000) || (GetAsyncKeyState('Z') & 0x8000)) cur |= 0x0008; // A
        if ((GetAsyncKeyState('X') & 0x8000) || (GetAsyncKeyState('B') & 0x8000) || (GetAsyncKeyState(VK_BACK) & 0x8000)) cur |= 0x0004; // B
        if ((GetAsyncKeyState('1') & 0x8000) || (GetAsyncKeyState(VK_NUMPAD1) & 0x8000)) cur |= 0x0002; // 1
        if ((GetAsyncKeyState('2') & 0x8000) || (GetAsyncKeyState(VK_NUMPAD2) & 0x8000)) cur |= 0x0001; // 2
        if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) || (GetAsyncKeyState('H') & 0x8000)) cur |= 0x0080; // HOME
        if (GetAsyncKeyState(VK_LEFT) & 0x8000) cur |= 0x0100; // LEFT
        if (GetAsyncKeyState(VK_RIGHT) & 0x8000) cur |= 0x0200; // RIGHT
        if (GetAsyncKeyState(VK_DOWN) & 0x8000) cur |= 0x0400; // DOWN
        if (GetAsyncKeyState(VK_UP) & 0x8000) cur |= 0x0800; // UP
        if ((GetAsyncKeyState(VK_OEM_PLUS) & 0x8000) || (GetAsyncKeyState('E') & 0x8000)) cur |= 0x1000; // PLUS
        if ((GetAsyncKeyState(VK_OEM_MINUS) & 0x8000) || (GetAsyncKeyState('Q') & 0x8000)) cur |= 0x0010; // MINUS

        ctx->r[3] = (ctx->r[3] == 0) ? cur : 0;
        return true;
)HK");
    emitHook("wpad_d", "WPAD (D)", R"HK(        ctx->r[3] = 0;
        return true;
)HK");

    // ---- IOS / IPC / DSP ----
    emitHook("ipc_a", "IOS IPC (A)", R"HK(        ctx->r[3] = 0;
        return true;
)HK");
    emitHook("ipc_b", "IOS IPC (B)", R"HK(        return true;
)HK");
    emitHook("ipc_init", "__ipc_init", R"HK(        Log("[IPC] Hook __ipc_init bypass: simulazione IPC/DI hardware completata");
        ctx->r[3] = 0;
        return true;
)HK");
    emitHook("dsp_init", "DSP_Init", R"HK(        Log("[DSP] Hook DSP_Init bypass: DSP pronto");
        g_Memory.dspMailReady = true;
        ctx->r[3] = 0;
        return true;
)HK");
    emitHook("gx_drawdone", "GX_DrawDone / GX_WaitDrawDone: esecuzione sincrona su host", R"HK(        g_Memory.gx.peStatus |= 0x000Cu;
        return true;
)HK");
    emitHook("sys_init", "SYS_Init: bypass hardware init su PC", R"HK(        Log("[SYS] Hook SYS_Init bypass completato da LR=" + HexStr(ctx->lr));
        return true;
)HK");

    // ---- allocatori / newlib ----
    emitHook("memalign", "memalign", R"HK(        uint32_t align = ctx->r[3];
        uint32_t size = ctx->r[4];
        uint32_t prev = nativeAlloc(align, size, true);
        ctx->r[3] = prev;
        Log("[MEMALIGN] Alloca align=" + std::to_string(align) + " size=" + std::to_string(size) + " -> " + HexStr(prev));
        return true;
)HK");
    emitHook("memalign_r", "_memalign_r", R"HK(        uint32_t align = ctx->r[4];
        uint32_t size = ctx->r[5];
        uint32_t prev = nativeAlloc(align, size, true);
        ctx->r[3] = prev;
        Log("[_MEMALIGN_R] Alloca align=" + std::to_string(align) + " size=" + std::to_string(size) + " -> " + HexStr(prev));
        return true;
)HK");
    emitHook("free", "free", R"HK(        return true;
)HK");
    emitHook("free_r", "_free_r", R"HK(        return true;
)HK");
    emitHook("exit", "exit: uscita ordinata", R"HK(        Log("[EXIT] Richiesta uscita ordinata dal gioco.");
        return true;
)HK");
    emitHook("sbrk_r", "_sbrk_r: heap continuo per malloc/calloc", R"HK(        int32_t incr = (int32_t)ctx->r[4];
        uint32_t prev = s_NativeHeap;
        if (incr > 0) {
            s_NativeHeap = (s_NativeHeap + incr + 31) & ~31;
        }
        ctx->r[3] = prev;
        Log("[SBRK] Heap alloca: " + std::to_string(incr) + " bytes -> " + HexStr(prev) + " (nuovo break: " + HexStr(s_NativeHeap) + ")");
        return true;
)HK");
    emitHook("abort", "abort: diagnostica", R"HK(        Log("[CRITICAL] abort() chiamato da LR=" + HexStr(ctx->lr));
        std::cerr << "[CRITICAL] abort() chiamato da LR=" << HexStr(ctx->lr) << "\n";
        return false;
)HK");
    emitHook("calloc", "calloc", R"HK(        uint32_t num = ctx->r[3];
        uint32_t size = ctx->r[4];
        uint32_t total = num * size;
        uint32_t prev = nativeAlloc(32, total, true);
        ctx->r[3] = prev;
        Log("[CALLOC] Alloca " + std::to_string(num) + " x " + std::to_string(size) + " (" + std::to_string(total) + " bytes) -> " + HexStr(prev));
        return true;
)HK");
    emitHook("calloc_r", "_calloc_r", R"HK(        uint32_t num = ctx->r[4];
        uint32_t size = ctx->r[5];
        uint32_t total = num * size;
        uint32_t prev = nativeAlloc(32, total, true);
        ctx->r[3] = prev;
        Log("[_CALLOC_R] Alloca " + std::to_string(num) + " x " + std::to_string(size) + " (" + std::to_string(total) + " bytes) -> " + HexStr(prev));
        return true;
)HK");
    emitHook("malloc", "malloc", R"HK(        uint32_t size = ctx->r[3];
        uint32_t prev = nativeAlloc(32, size, true);
        ctx->r[3] = prev;
        Log("[MALLOC] Alloca " + std::to_string(size) + " bytes -> " + HexStr(prev) + " (nuovo break: " + HexStr(s_NativeHeap) + ")");
        return true;
)HK");
    emitHook("malloc_r", "_malloc_r", R"HK(        uint32_t size = ctx->r[4];
        uint32_t prev = nativeAlloc(32, size, true);
        ctx->r[3] = prev;
        Log("[_MALLOC_R] Alloca " + std::to_string(size) + " bytes -> " + HexStr(prev) + " (nuovo break: " + HexStr(s_NativeHeap) + ")");
        return true;
)HK");
    emitHook("write_r", "_write_r: stdout/stderr del gioco sulla console", R"HK(        uint32_t fd = ctx->r[4];
        uint32_t ptr = ctx->r[5];
        uint32_t len = ctx->r[6];
        if ((fd == 1 || fd == 2) && len > 0 && len < 65536) {
            std::string text;
            for (uint32_t i = 0; i < len; ++i) {
                char c = (char)g_Memory.Read8(ptr + i);
                text += c;
            }
            std::cout << text << std::flush;
            Log("[CONSOLE] " + text);
        }
        return false;
)HK");

    uint32_t consoleWriteAddr = libogc.Get("console_write");
    if (consoleWriteAddr == 0 && libogc.sameLayoutAsReference) {
        consoleWriteAddr = 0x80014BA0;
    }
    uint32_t printfAddr = libogc.Get("printf");
    if (printfAddr == 0 && libogc.sameLayoutAsReference) {
        printfAddr = 0x80032CA8;
    }
    uint32_t putsAddr = libogc.Get("puts");

    if (consoleWriteAddr != 0) {
        out << "    // __console_write: mirror output console libogc sui log nativi\n";
        out << "    g_ModEngine.RegisterPreHook(" << H(consoleWriteAddr) << ", [](PPCContext* ctx) -> bool {\n";
        out << "        uint32_t ptr = ctx->r[5];\n";
        out << "        uint32_t len = ctx->r[6];\n";
        out << "        if (len > 0 && len < 65536) {\n";
        out << "            std::string text;\n";
        out << "            for (uint32_t i = 0; i < len; ++i) {\n";
        out << "                char c = (char)g_Memory.Read8(ptr + i);\n";
        out << "                text += c;\n";
        out << "            }\n";
        out << "            std::cout << text << std::flush;\n";
        out << "            Log(\"[CONSOLE] \" + text);\n";
        out << "        }\n";
        out << "        return false;\n";
        out << "    });\n\n";
    }

    out << "    static const uint32_t s_ConsoleWriteAddr = " << H(consoleWriteAddr) << ";\n\n";

    if (putsAddr != 0) {
        out << "    // puts: hook sicuro che preserva i registri callee-saved ed emette su console libogc\n";
        out << "    g_ModEngine.RegisterPreHook(" << H(putsAddr) << ", [](PPCContext* ctx) -> bool {\n";
        out << "        PPCContext saved = *ctx;\n";
        out << "        uint32_t strPtr = ctx->r[3];\n";
        out << "        std::string s;\n";
        out << "        for (int i = 0; i < 4096; ++i) {\n";
        out << "            char c = (char)g_Memory.Read8(strPtr + i);\n";
        out << "            if (!c) break;\n";
        out << "            s += c;\n";
        out << "        }\n";
        out << "        s += '\\n';\n";
        out << "        std::cout << s << std::flush;\n";
        out << "        Log(\"[PUTS] \" + s);\n";
        out << "        if (s_ConsoleWriteAddr != 0) {\n";
        out << "            uint32_t consoleBuf = 0x87FF0000;\n";
        out << "            for (size_t i = 0; i < s.size() && i < 4096; ++i) {\n";
        out << "                g_Memory.Write8(consoleBuf + (uint32_t)i, (uint8_t)s[i]);\n";
        out << "            }\n";
        out << "            PPCContext callCtx;\n";
        out << "            callCtx.Reset();\n";
        out << "            callCtx.r[1] = 0x87FFF000;\n";
        out << "            callCtx.r[2] = ctx->r[2];\n";
        out << "            callCtx.r[13] = ctx->r[13];\n";
        out << "            callCtx.r[3] = 0;\n";
        out << "            callCtx.r[4] = 0;\n";
        out << "            callCtx.r[5] = consoleBuf;\n";
        out << "            callCtx.r[6] = (uint32_t)s.size();\n";
        out << "            callCtx.lr = 0;\n";
        out << "            g_Memory.Call(s_ConsoleWriteAddr, &callCtx);\n";
        out << "        }\n";
        out << "        *ctx = saved;\n";
        out << "        ctx->r[3] = (uint32_t)s.size();\n";
        out << "        return true;\n";
        out << "    });\n\n";
    }

    if (printfAddr != 0) {
        out << "    // printf: hook sicuro che preserva i registri non-volatili ed emette su console libogc\n";
        out << "    g_ModEngine.RegisterPreHook(" << H(printfAddr) << ", [](PPCContext* ctx) -> bool {\n";
        out << "        PPCContext saved = *ctx;\n";
        out << "        uint32_t fmtPtr = ctx->r[3];\n";
        out << "        std::string fmt;\n";
        out << "        for (int i = 0; i < 512; ++i) {\n";
        out << "            char c = (char)g_Memory.Read8(fmtPtr + i);\n";
        out << "            if (c == 0) break;\n";
        out << "            fmt += c;\n";
        out << "        }\n";
        out << "        std::string outStr;\n";
        out << "        size_t argIdx = 4;\n";
        out << "        size_t fpIdx = 1;\n";
        out << "        for (size_t i = 0; i < fmt.size(); ) {\n";
        out << "            if (fmt[i] == '%' && i + 1 < fmt.size()) {\n";
        out << "                if (fmt[i+1] == '%') {\n";
        out << "                    outStr += '%';\n";
        out << "                    i += 2;\n";
        out << "                    continue;\n";
        out << "                }\n";
        out << "                size_t j = i + 1;\n";
        out << "                while (j < fmt.size() && (fmt[j] == '-' || fmt[j] == '+' || fmt[j] == ' ' || fmt[j] == '#' || fmt[j] == '0' || (fmt[j] >= '0' && fmt[j] <= '9') || fmt[j] == '.' || fmt[j] == 'l' || fmt[j] == 'h' || fmt[j] == 'z')) {\n";
        out << "                    j++;\n";
        out << "                }\n";
        out << "                if (j < fmt.size()) {\n";
        out << "                    char spec = fmt[j];\n";
        out << "                    std::string specStr = fmt.substr(i, j - i + 1);\n";
        out << "                    bool isLongLong = (specStr.find(\"ll\") != std::string::npos);\n";
        out << "                    if (spec == 'f' || spec == 'F' || spec == 'e' || spec == 'E' || spec == 'g' || spec == 'G') {\n";
        out << "                        double dval = (fpIdx <= 8) ? ctx->f[fpIdx++].d : 0.0;\n";
        out << "                        char tmp[64];\n";
        out << "                        snprintf(tmp, sizeof(tmp), specStr.c_str(), dval);\n";
        out << "                        outStr += tmp;\n";
        out << "                    } else if (spec == 's') {\n";
        out << "                        uint32_t val = (argIdx <= 10) ? ctx->r[argIdx++] : 0;\n";
        out << "                        std::string sArg;\n";
        out << "                        for (int k = 0; k < 1024; ++k) {\n";
        out << "                            char sc = (char)g_Memory.Read8(val + k);\n";
        out << "                            if (sc == 0) break;\n";
        out << "                            sArg += sc;\n";
        out << "                        }\n";
        out << "                        outStr += sArg;\n";
        out << "                    } else if (spec == 'c') {\n";
        out << "                        uint32_t val = (argIdx <= 10) ? ctx->r[argIdx++] : 0;\n";
        out << "                        outStr += (char)val;\n";
        out << "                    } else if (spec == 'p') {\n";
        out << "                        uint32_t val = (argIdx <= 10) ? ctx->r[argIdx++] : 0;\n";
        out << "                        char tmp[32];\n";
        out << "                        snprintf(tmp, sizeof(tmp), \"0x%08x\", val);\n";
        out << "                        outStr += tmp;\n";
        out << "                    } else if (spec == 'd' || spec == 'i' || spec == 'u' || spec == 'x' || spec == 'X' || spec == 'o') {\n";
        out << "                        if (isLongLong) {\n";
        out << "                            if (argIdx & 1) argIdx++;\n";
        out << "                            uint64_t hi = (argIdx <= 10) ? ctx->r[argIdx++] : 0;\n";
        out << "                            uint64_t lo = (argIdx <= 10) ? ctx->r[argIdx++] : 0;\n";
        out << "                            char tmp[64];\n";
        out << "                            snprintf(tmp, sizeof(tmp), specStr.c_str(), (hi << 32) | lo);\n";
        out << "                            outStr += tmp;\n";
        out << "                        } else {\n";
        out << "                            uint32_t val = (argIdx <= 10) ? ctx->r[argIdx++] : 0;\n";
        out << "                            char tmp[64];\n";
        out << "                            snprintf(tmp, sizeof(tmp), specStr.c_str(), val);\n";
        out << "                            outStr += tmp;\n";
        out << "                        }\n";
        out << "                    } else {\n";
        out << "                        outStr += specStr;\n";
        out << "                    }\n";
        out << "                    i = j + 1;\n";
        out << "                    continue;\n";
        out << "                }\n";
        out << "            }\n";
        out << "            outStr += fmt[i++];\n";
        out << "        }\n";
        out << "        std::cout << outStr << std::flush;\n";
        out << "        Log(\"[PRINTF] \" + outStr);\n";
        out << "        if (s_ConsoleWriteAddr != 0) {\n";
        out << "            uint32_t consoleBuf = 0x87FF0000;\n";
        out << "            for (size_t i = 0; i < outStr.size() && i < 4096; ++i) {\n";
        out << "                g_Memory.Write8(consoleBuf + (uint32_t)i, (uint8_t)outStr[i]);\n";
        out << "            }\n";
        out << "            PPCContext callCtx;\n";
        out << "            callCtx.Reset();\n";
        out << "            callCtx.r[1] = 0x87FFF000;\n";
        out << "            callCtx.r[2] = ctx->r[2];\n";
        out << "            callCtx.r[13] = ctx->r[13];\n";
        out << "            callCtx.r[3] = 0;\n";
        out << "            callCtx.r[4] = 0;\n";
        out << "            callCtx.r[5] = consoleBuf;\n";
        out << "            callCtx.r[6] = (uint32_t)outStr.size();\n";
        out << "            callCtx.lr = 0;\n";
        out << "            g_Memory.Call(s_ConsoleWriteAddr, &callCtx);\n";
        out << "        }\n";
        out << "        *ctx = saved;\n";
        out << "        ctx->r[3] = (uint32_t)outStr.size();\n";
        out << "        return true;\n";
        out << "    });\n\n";
    }

    if (detectedMain) {
        out << "    g_ModEngine.RegisterPreHook(" << H(detectedMain) << ", [](PPCContext* ctx) -> bool {\n";
        out << "        Log(\"[NATIVE] >>> MAIN (" << H(detectedMain) << ") RAGGIUNTO CON SUCCESSO! <<<\");\n";
        out << "        ApplyArenaHiSdaPatches(ctx->r[13]);\n";
        out << "        return false;\n";
        out << "    });\n";
    }
    out << "    PPCContext& ctx = g_GameContext;\n";
    out << "    ctx.Reset();\n";
    out << "    ctx.r[1] = 0x87FFF000;\n";
    if (detectedR2)  out << "    ctx.r[2] = " << H(detectedR2) << ";\n";
    if (detectedR13) out << "    ctx.r[13] = " << H(detectedR13) << ";\n";
    out << "    ApplyInitialSdaPatches(ctx.r[13]);\n";
    out << "    Log(\"[NATIVE] Esecuzione crt0 / bootstrap / SYS_Init a " << H(dol.entryPoint) << "\");\n";
    out << "    " << lifter.GetFunctionName(dol.entryPoint) << "(&ctx);\n";
    out << "    ApplyArenaHiSdaPatches(ctx.r[13]);\n";
    if (devkitStartAddr != 0 && devkitStartAddr != dol.entryPoint) {
        out << "    ctx.r[1] = 0x87FFF000;\n";
        if (detectedR2)  out << "    ctx.r[2] = " << H(detectedR2) << ";\n";
        if (detectedR13) out << "    ctx.r[13] = " << H(detectedR13) << ";\n";
        out << "    Log(\"[NATIVE] Avvio runtime e main() a " << H(devkitStartAddr) << "\");\n";
        out << "    try {\n";
        out << "        " << lifter.GetFunctionName(devkitStartAddr) << "(&ctx);\n";
        out << "    } catch (const WiiPort::PPCLongJmpException& lj) {\n";
        out << "        LogConsole(\"[LONGJMP] Eccezione PPCLongJmpException non gestita, targetLr=\" + HexStr(lj.targetLr));\n";
        out << "    }\n";
    }
    out << "    return 0;\n";
    out << "}\n";

    std::cout << "[COMPLETATO] Generazione completata con successo in " << outputPath << "!\n";
    return 0;
}
