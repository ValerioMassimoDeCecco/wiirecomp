#include <iostream>
#include <fstream>
#include <string>
#include "dol_loader.hpp"
#include "ppc_lifter.hpp"
#include "symbol_map_importer.hpp"
#include "iso_fs_reader.hpp"

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

    // 1. Rilevamento automatico di devkitPPC _start bootstrap
    uint32_t devkitStartAddr = 0;
    const uint8_t startSig[] = { 0x94, 0x21, 0xFF, 0xF0, 0x7C, 0x08, 0x02, 0xA6, 0x42, 0x9F, 0x00, 0x05, 0x93, 0xC1, 0x00, 0x08 };
    for (const auto& sec : dol.sections) {
        if (!sec.isText || sec.size < sizeof(startSig)) continue;
        for (uint32_t off = 0; off + sizeof(startSig) <= sec.size; off += 4) {
            if (std::memcmp(&sec.data[off], startSig, sizeof(startSig)) == 0) {
                devkitStartAddr = sec.address + off;
                std::cout << "[ENTRY] Rilevato devkitPPC _start bootstrap a 0x" << std::hex << devkitStartAddr << "\n";
                lifter.discoveredFunctions.insert(devkitStartAddr);
                break;
            }
        }
        if (devkitStartAddr != 0) break;
    }

    uint32_t detectedMain = 0;
    if (devkitStartAddr != 0) {
        for (uint32_t off = devkitStartAddr; off < devkitStartAddr + 0x60; off += 4) {
            uint32_t w = 0;
            if (dol.ReadInstruction(off, w)) {
                if (((w >> 26) & 0x3F) == 18 && (w & 1)) {
                    int32_t li = (w & 0x03FFFFFC);
                    if (li & 0x02000000) li |= 0xFC000000;
                    uint32_t t = (w & 2) ? li : (off + li);
                    if (dol.IsCodeAddress(t) && t != 0x8001A89C) {
                        detectedMain = t;
                        std::cout << "[MAIN] Rilevato main() a 0x" << std::hex << detectedMain << "\n";
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
    if (detectedR2) std::cout << "[SDA] Rilevato _SDA2_BASE_ (r2): 0x" << std::hex << detectedR2 << "\n";
    if (detectedR13) std::cout << "[SDA] Rilevato _SDA_BASE_ (r13): 0x" << std::hex << detectedR13 << "\n";

    // Registrazione preventiva di simboli libogc critici (VIDEO_SetNextFramebuffer, SYS_GetArena1Lo/Hi, _sbrk_r, abort, _write_r, allocatori)
    if (dol.IsCodeAddress(0x80018F5C)) lifter.discoveredFunctions.insert(0x80018F5C);
    if (dol.IsCodeAddress(0x8001A89C)) lifter.discoveredFunctions.insert(0x8001A89C);
    if (dol.IsCodeAddress(0x8001773C)) lifter.discoveredFunctions.insert(0x8001773C);
    if (dol.IsCodeAddress(0x80016BF0)) lifter.discoveredFunctions.insert(0x80016BF0);
    if (dol.IsCodeAddress(0x80016C78)) lifter.discoveredFunctions.insert(0x80016C78);
    if (dol.IsCodeAddress(0x800255BC)) lifter.discoveredFunctions.insert(0x800255BC);
    if (dol.IsCodeAddress(0x80032D44)) lifter.discoveredFunctions.insert(0x80032D44);
    if (dol.IsCodeAddress(0x80030490)) lifter.discoveredFunctions.insert(0x80030490);
    if (dol.IsCodeAddress(0x80031604)) lifter.discoveredFunctions.insert(0x80031604);
    if (dol.IsCodeAddress(0x80030588)) lifter.discoveredFunctions.insert(0x80030588);
    if (dol.IsCodeAddress(0x8001ACF8)) lifter.discoveredFunctions.insert(0x8001ACF8);
    if (dol.IsCodeAddress(0x8001AD30)) lifter.discoveredFunctions.insert(0x8001AD30);
    if (dol.IsCodeAddress(0x80020290)) lifter.discoveredFunctions.insert(0x80020290);
    if (dol.IsCodeAddress(0x8002ED58)) lifter.discoveredFunctions.insert(0x8002ED58);
    if (dol.IsCodeAddress(0x8002FF14)) lifter.discoveredFunctions.insert(0x8002FF14);
    if (dol.IsCodeAddress(0x800302D0)) lifter.discoveredFunctions.insert(0x800302D0);
    if (dol.IsCodeAddress(0x8003064C)) lifter.discoveredFunctions.insert(0x8003064C);
    if (dol.IsCodeAddress(0x800315D4)) lifter.discoveredFunctions.insert(0x800315D4);
    if (dol.IsCodeAddress(0x800329A4)) lifter.discoveredFunctions.insert(0x800329A4);
    if (dol.IsCodeAddress(0x8003D6C4)) lifter.discoveredFunctions.insert(0x8003D6C4);
    if (dol.IsCodeAddress(0x80007630)) lifter.discoveredFunctions.insert(0x80007630);
    if (dol.IsCodeAddress(0x80007928)) lifter.discoveredFunctions.insert(0x80007928);
    if (dol.IsCodeAddress(0x800079B8)) lifter.discoveredFunctions.insert(0x800079B8);
    if (dol.IsCodeAddress(0x800079E0)) lifter.discoveredFunctions.insert(0x800079E0);
    if (dol.IsCodeAddress(0x80007904)) lifter.discoveredFunctions.insert(0x80007904);
    if (dol.IsCodeAddress(0x8001C5E0)) lifter.discoveredFunctions.insert(0x8001C5E0);
    if (dol.IsCodeAddress(0x8001C57C)) lifter.discoveredFunctions.insert(0x8001C57C);
    if (dol.IsCodeAddress(0x8001D5EC)) lifter.discoveredFunctions.insert(0x8001D5EC);
    if (dol.IsCodeAddress(0x8001C6B8)) lifter.discoveredFunctions.insert(0x8001C6B8);
    if (dol.IsCodeAddress(0x8001C6BC)) lifter.discoveredFunctions.insert(0x8001C6BC);
    if (dol.IsCodeAddress(0x8001B744)) lifter.discoveredFunctions.insert(0x8001B744);
    if (dol.IsCodeAddress(0x8001B8FC)) lifter.discoveredFunctions.insert(0x8001B8FC);
    if (dol.IsCodeAddress(0x8003A434)) lifter.discoveredFunctions.insert(0x8003A434);
    if (dol.IsCodeAddress(0x8003A42C)) lifter.discoveredFunctions.insert(0x8003A42C);
    if (dol.IsCodeAddress(0x8001FBBC)) lifter.discoveredFunctions.insert(0x8001FBBC);
    if (dol.IsCodeAddress(0x8001FBA4)) lifter.discoveredFunctions.insert(0x8001FBA4);
    if (dol.IsCodeAddress(0x8003D6BC)) lifter.discoveredFunctions.insert(0x8003D6BC);
    if (dol.IsCodeAddress(0x8003D30C)) lifter.discoveredFunctions.insert(0x8003D30C);
    if (dol.IsCodeAddress(0x8003D274)) lifter.discoveredFunctions.insert(0x8003D274);
    if (dol.IsCodeAddress(0x8003D560)) lifter.discoveredFunctions.insert(0x8003D560);
    if (dol.IsCodeAddress(0x8003D628)) lifter.discoveredFunctions.insert(0x8003D628);
    if (dol.IsCodeAddress(0x8003D778)) lifter.discoveredFunctions.insert(0x8003D778);
    if (dol.IsCodeAddress(0x80018140)) lifter.discoveredFunctions.insert(0x80018140);
    if (dol.IsCodeAddress(0x80009038)) lifter.discoveredFunctions.insert(0x80009038);
    if (dol.IsCodeAddress(0x80028C54)) lifter.discoveredFunctions.insert(0x80028C54);
    if (dol.IsCodeAddress(0x80028EF4)) lifter.discoveredFunctions.insert(0x80028EF4);
    if (dol.IsCodeAddress(0x80029100)) lifter.discoveredFunctions.insert(0x80029100);
    if (dol.IsCodeAddress(0x80029108)) lifter.discoveredFunctions.insert(0x80029108);
    if (dol.IsCodeAddress(0x8002911C)) lifter.discoveredFunctions.insert(0x8002911C);
    if (dol.IsCodeAddress(0x8002916C)) lifter.discoveredFunctions.insert(0x8002916C);
    if (dol.IsCodeAddress(0x800174C0)) lifter.discoveredFunctions.insert(0x800174C0);
    if (dol.IsCodeAddress(0x80017544)) lifter.discoveredFunctions.insert(0x80017544);

    // 3. Rilevamento VIDEO_WaitVSync per evitare deadlock su sistemi senza interrupt timer HW
    uint32_t waitVSyncAddr = 0;
    for (const auto& sec : dol.sections) {
        if (!sec.isText || sec.size < 16) continue;
        for (uint32_t off = 0; off + 12 <= sec.size; off += 4) {
            uint32_t w1 = ((uint32_t)sec.data[off] << 24) | ((uint32_t)sec.data[off+1] << 16) | ((uint32_t)sec.data[off+2] << 8) | sec.data[off+3];
            uint32_t w2 = ((uint32_t)sec.data[off+4] << 24) | ((uint32_t)sec.data[off+5] << 16) | ((uint32_t)sec.data[off+6] << 8) | sec.data[off+7];
            uint32_t w3 = ((uint32_t)sec.data[off+8] << 24) | ((uint32_t)sec.data[off+9] << 16) | ((uint32_t)sec.data[off+10] << 8) | sec.data[off+11];
            if (w1 == 0x812D01F0 && w2 == 0x7C09F040 && w3 == 0x4182FFF0) {
                uint32_t fOff = off;
                while (fOff >= 4) {
                    uint32_t prev = ((uint32_t)sec.data[fOff] << 24) | ((uint32_t)sec.data[fOff+1] << 16) | ((uint32_t)sec.data[fOff+2] << 8) | sec.data[fOff+3];
                    if ((prev & 0xFFFF0000) == 0x94210000) {
                        waitVSyncAddr = sec.address + fOff;
                        break;
                    }
                    fOff -= 4;
                }
                if (waitVSyncAddr != 0) {
                    std::cout << "[VIDEO] Rilevata funzione VIDEO_WaitVSync a 0x" << std::hex << waitVSyncAddr << "\n";
                    lifter.discoveredFunctions.insert(waitVSyncAddr);
                    break;
                }
            }
        }
        if (waitVSyncAddr != 0) break;
    }

    std::cout << "[CFG] Scoperte " << lifter.discoveredFunctions.size() << " funzioni uniche.\n";

    std::cout << "[LIFTER] Generazione codice C++ nativo su " << outputPath << "...\n";
    std::ofstream out(outputPath);
    if (!out.is_open()) {
        std::cerr << "Impossibile creare il file di output: " << outputPath << "\n";
        return 1;
    }

    // Header del file generato
    out << "// Codice generato automaticamente da Wii Universal AOT Recompiler\n";
    out << "#include <iostream>\n";
    out << "#include <thread>\n";
    out << "#include <chrono>\n";
    out << "#include \"wii_runtime.hpp\"\n";
    out << "#include \"ppc_bitops.hpp\"\n";
    out << "#include \"mod_engine.hpp\"\n";
    out << "using namespace WiiPort;\n\n";
    out << "namespace WiiPort { PPCContext g_GameContext; }\n\n";

    out << "// Hook nativo ad alta efficienza per sincronizzazione VSync\n";
    out << "static void Hook_VIDEO_WaitVSync(PPCContext* ctx) {\n";
    out << "    if (ctx->r[13] >= 0x80000000 && ctx->r[13] < 0x88000000) {\n";
    out << "        uint32_t off = (ctx->r[13] + 0x1F0) & (MEM1_SIZE - 1);\n";
    out << "        uint32_t val = WII_BYTESWAP32(*reinterpret_cast<uint32_t*>(g_Memory.mem1 + off));\n";
    out << "        *reinterpret_cast<uint32_t*>(g_Memory.mem1 + off) = WII_BYTESWAP32(val + 1);\n";
    out << "    }\n";
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
        out << "    g_Memory.RegisterFunction(0x" << std::hex << addr << ", &" 
            << lifter.GetFunctionName(addr) << ");\n";
    }
    if (waitVSyncAddr != 0) {
        out << "    g_Memory.RegisterFunction(0x" << std::hex << waitVSyncAddr << ", &Hook_VIDEO_WaitVSync);\n";
    }
    out << "}\n\n";

    std::string safeDolPath = dolPath;
    for (char& c : safeDolPath) {
        if (c == '\\') c = '/';
    }

    out << "int StartRecompiledGame() {\n";
    out << "    g_Memory.LoadDol(\"" << safeDolPath << "\");\n";
    out << "    InitializeGameDispatchTable();\n";
    if (waitVSyncAddr != 0) {
        out << "    g_ModEngine.RegisterPreHook(0x" << std::hex << waitVSyncAddr << ", [](PPCContext* ctx) -> bool {\n";
        out << "        Hook_VIDEO_WaitVSync(ctx);\n";
        out << "        return true;\n";
        out << "    });\n";
    }

    out << "    // Diagnostic hooks per bootstrap e init\n";
    out << "    g_ModEngine.RegisterPreHook(0x80004020, [](PPCContext* ctx) -> bool {\n";
    out << "        Log(\"[DIAG] 0x80004020 (crt0) entry, LR=0x\" + HexStr(ctx->lr) + \" SP=0x\" + HexStr(ctx->r[1]));\n";
    out << "        return false;\n";
    out << "    });\n";
    out << "    g_ModEngine.RegisterPreHook(0x80005634, [](PPCContext* ctx) -> bool {\n";
    out << "        Log(\"[INIT] Bypass __init_user_runtime (0x80005634) per procedere direttamente all'avvio del gioco\");\n";
    out << "        return true;\n";
    out << "    });\n";
    out << "    g_ModEngine.RegisterPreHook(0x80004170, [](PPCContext* ctx) -> bool {\n";
    out << "        Log(\"[INIT] Bypass __init_cpp (0x80004170)\");\n";
    out << "        return true;\n";
    out << "    });\n";
    out << "    g_ModEngine.RegisterPreHook(0x8003D780, [](PPCContext* ctx) -> bool {\n";
    out << "        Log(\"[INIT] Bypass __init_array (0x8003D780)\");\n";
    out << "        return true;\n";
    out << "    });\n";
    out << "    g_ModEngine.RegisterPreHook(0x8003A084, [](PPCContext* ctx) -> bool {\n";
    out << "        Log(\"[INIT] Bypass __register_frame_info (0x8003A084)\");\n";
    out << "        return true;\n";
    out << "    });\n";
    out << "    // Hook __vi_write_i2c (0x8001815C) e __vi_init_encoder (0x8001952C) per evitare blocco hardware I2C\n";
    out << "    g_ModEngine.RegisterPreHook(0x8001815C, [](PPCContext* ctx) -> bool {\n";
    out << "        return true;\n";
    out << "    });\n";
    out << "    g_ModEngine.RegisterPreHook(0x8001952C, [](PPCContext* ctx) -> bool {\n";
    out << "        return true;\n";
    out << "    });\n\n";
    out << "    g_ModEngine.RegisterPreHook(0x800197F8, [](PPCContext* ctx) -> bool {\n";
    out << "        Log(\"[DIAG] 0x800197F8 (VIDEO_Init) entry, LR=0x\" + HexStr(ctx->lr));\n";
    out << "        return false;\n";
    out << "    });\n";
    out << "    g_ModEngine.RegisterPreHook(0x800187F4, [](PPCContext* ctx) -> bool {\n";
    out << "        Log(\"[DIAG] 0x800187F4 (CON_InitEx) entry, LR=0x\" + HexStr(ctx->lr));\n";
    out << "        return false;\n";
    out << "    });\n\n";
    out << "    // Hook VIDEO_SetNextFramebuffer (0x80018F5C) per catturare l'indirizzo XFB\n";
    out << "    g_ModEngine.RegisterPreHook(0x80018F5C, [](PPCContext* ctx) -> bool {\n";
    out << "        uint32_t fb = ctx->r[3];\n";
    out << "        g_Memory.currentFramebufferAddress = fb & (MEM1_SIZE - 1);\n";
    out << "        g_Memory.frameReady = true;\n";
    out << "        if (ctx->r[13] != 0) {\n";
    out << "            g_Memory.Write32(0x800BA7B0, fb);\n";
    out << "            g_Memory.Write32(ctx->r[13] + 540, 1);\n";
    out << "        }\n";
    out << "        Log(\"[VI] VIDEO_SetNextFramebuffer intercettato: fb=0x\" + HexStr(fb) + \" -> phys 0x\" + HexStr(g_Memory.currentFramebufferAddress));\n";
    out << "        return true;\n";
    out << "    });\n\n";

    out << "    // Hook SYS_GetArena1Lo (0x8001ACF8)\n";
    out << "    g_ModEngine.RegisterPreHook(0x8001ACF8, [](PPCContext* ctx) -> bool {\n";
    out << "        ctx->r[3] = 0x800E4960;\n";
    out << "        return true;\n";
    out << "    });\n\n";

    out << "    // Hook SYS_GetArena1Hi (0x8001AD30) per garantire che l'Arena1 nativa sia 128 MB\n";
    out << "    g_ModEngine.RegisterPreHook(0x8001AD30, [](PPCContext* ctx) -> bool {\n";
    out << "        ctx->r[3] = 0x80000000 + (uint32_t)MEM1_SIZE - 0x1000;\n";
    out << "        return true;\n";
    out << "    });\n\n";

    out << "    // Hook 0x8001A89C: bypass init sottosistemi hardware libogc (IRQ/LWP/RTC) su Windows\n";
    out << "    g_ModEngine.RegisterPreHook(0x8001A89C, [](PPCContext* ctx) -> bool {\n";
    out << "        Log(\"[LIBOGC] Bypass subsys hardware init a 0x8001A89C\");\n";
    out << "        return true;\n";
    out << "    });\n\n";

    out << "    // Hook 0x8001773C: __lwp_heap_allocate - fallback su heap nativo per evitare loop su liste libogc non inizializzate\n";
    out << "    static uint32_t s_NativeHeap = 0x80600000;\n";
    out << "    g_ModEngine.RegisterPreHook(0x8001773C, [](PPCContext* ctx) -> bool {\n";
    out << "        uint32_t size = ctx->r[4];\n";
    out << "        uint32_t prev = s_NativeHeap;\n";
    out << "        s_NativeHeap = (s_NativeHeap + size + 31) & ~31;\n";
    out << "        for (uint32_t i = 0; i < size; ++i) g_Memory.Write8(prev + i, 0);\n";
    out << "        ctx->r[3] = prev;\n";
    out << "        Log(\"[LWP_HEAP] Allocati \" + std::to_string(size) + \" bytes a 0x\" + HexStr(prev) + \" da LR=0x\" + HexStr(ctx->lr));\n";
    out << "        return true;\n";
    out << "    });\n\n";

    out << "    // Hook __lwp_thread_dispatch (0x80016BF0, 0x80016C78) e Func_0x800255BC per evitare blocco schedulatore LWP\n";
    out << "    g_ModEngine.RegisterPreHook(0x80016BF0, [](PPCContext* ctx) -> bool {\n";
    out << "        return true;\n";
    out << "    });\n\n";
    out << "    g_ModEngine.RegisterPreHook(0x80016C78, [](PPCContext* ctx) -> bool {\n";
    out << "        return true;\n";
    out << "    });\n\n";
    out << "    g_ModEngine.RegisterPreHook(0x800255BC, [](PPCContext* ctx) -> bool {\n";
    out << "        ctx->r[3] = 0;\n";
    out << "        return true;\n";
    out << "    });\n\n";

    out << "    // Hook WPAD (0x80007630, 0x80007928, 0x800079B8, 0x800079E0, 0x80007904) per evitare blocco controller IOS/Bluetooth\n";
    out << "    g_ModEngine.RegisterPreHook(0x80007630, [](PPCContext* ctx) -> bool {\n";
    out << "        Log(\"[WPAD] WPAD_Init bypass: ritorno OK (0)\");\n";
    out << "        ctx->r[3] = 0;\n";
    out << "        return true;\n";
    out << "    });\n\n";
    out << "    g_ModEngine.RegisterPreHook(0x80007928, [](PPCContext* ctx) -> bool {\n";
    out << "        return true;\n";
    out << "    });\n\n";
    out << "    g_ModEngine.RegisterPreHook(0x800079B8, [](PPCContext* ctx) -> bool {\n";
    out << "        ctx->r[3] = 0;\n";
    out << "        return true;\n";
    out << "    });\n\n";
    out << "    g_ModEngine.RegisterPreHook(0x800079E0, [](PPCContext* ctx) -> bool {\n";
    out << "        ctx->r[3] = 0;\n";
    out << "        return true;\n";
    out << "    });\n\n";
    out << "    g_ModEngine.RegisterPreHook(0x80007904, [](PPCContext* ctx) -> bool {\n";
    out << "        ctx->r[3] = 0;\n";
    out << "        return true;\n";
    out << "    });\n\n";

    out << "    // Hook IOS IPC (0x8001C5E0, 0x8001C57C, 0x8001D5EC) per evitare deadlock IPC con Starlet/DI\n";
    out << "    g_ModEngine.RegisterPreHook(0x8001C5E0, [](PPCContext* ctx) -> bool {\n";
    out << "        ctx->r[3] = 0;\n";
    out << "        return true;\n";
    out << "    });\n\n";
    out << "    g_ModEngine.RegisterPreHook(0x8001C57C, [](PPCContext* ctx) -> bool {\n";
    out << "        return true;\n";
    out << "    });\n\n";
    out << "    g_ModEngine.RegisterPreHook(0x8001D5EC, [](PPCContext* ctx) -> bool {\n";
    out << "        Log(\"[IPC] Hook __ipc_init (0x8001D5EC) bypass: simulazione IPC/DI hardware completata\");\n";
    out << "        ctx->r[3] = 0;\n";
    out << "        return true;\n";
    out << "    });\n\n";

    out << "    // Hook DSP_Init (0x8001B744) per bypassare loop e interrupt DSP\n";
    out << "    g_ModEngine.RegisterPreHook(0x8001B744, [](PPCContext* ctx) -> bool {\n";
    out << "        Log(\"[DSP] Hook DSP_Init (0x8001B744) bypass: DSP pronto\");\n";
    out << "        ctx->r[3] = 0;\n";
    out << "        return true;\n";
    out << "    });\n\n";

    out << "    // Hook SYS_Init (0x8001B8FC) per evitare esecuzioni multiple da ctors\n";
    out << "    static bool s_SysInitDone = false;\n";
    out << "    g_ModEngine.RegisterPreHook(0x8001B8FC, [](PPCContext* ctx) -> bool {\n";
    out << "        if (s_SysInitDone) {\n";
    out << "            static int s_SysCount = 0;\n";
    out << "            if (++s_SysCount <= 5) {\n";
    out << "                Log(\"[SYS] Hook SYS_Init (0x8001B8FC) secondaria evitata: LR=0x\" + HexStr(ctx->lr) + \" PC=0x\" + HexStr(ctx->pc) + \" SP=0x\" + HexStr(ctx->r[1]));\n";
    out << "            }\n";
    out << "            return true;\n";
    out << "        }\n";
    out << "        s_SysInitDone = true;\n";
    out << "        Log(\"[SYS] Hook SYS_Init (0x8001B8FC) prima esecuzione avviata da LR=0x\" + HexStr(ctx->lr));\n";
    out << "        return false;\n";
    out << "    });\n\n";

    out << "    // Hook memalign (0x80032D44) e _memalign_r (0x80030490)\n";
    out << "    g_ModEngine.RegisterPreHook(0x80032D44, [](PPCContext* ctx) -> bool {\n";
    out << "        uint32_t align = ctx->r[3];\n";
    out << "        if (align < 32) align = 32;\n";
    out << "        uint32_t size = ctx->r[4];\n";
    out << "        s_NativeHeap = (s_NativeHeap + align - 1) & ~(align - 1);\n";
    out << "        uint32_t prev = s_NativeHeap;\n";
    out << "        s_NativeHeap = (s_NativeHeap + size + 31) & ~31;\n";
    out << "        for (uint32_t i = 0; i < size; ++i) g_Memory.Write8(prev + i, 0);\n";
    out << "        ctx->r[3] = prev;\n";
    out << "        Log(\"[MEMALIGN] Alloca align=\" + std::to_string(align) + \" size=\" + std::to_string(size) + \" -> 0x\" + HexStr(prev));\n";
    out << "        return true;\n";
    out << "    });\n\n";

    out << "    g_ModEngine.RegisterPreHook(0x80030490, [](PPCContext* ctx) -> bool {\n";
    out << "        uint32_t align = ctx->r[4];\n";
    out << "        if (align < 32) align = 32;\n";
    out << "        uint32_t size = ctx->r[5];\n";
    out << "        s_NativeHeap = (s_NativeHeap + align - 1) & ~(align - 1);\n";
    out << "        uint32_t prev = s_NativeHeap;\n";
    out << "        s_NativeHeap = (s_NativeHeap + size + 31) & ~31;\n";
    out << "        for (uint32_t i = 0; i < size; ++i) g_Memory.Write8(prev + i, 0);\n";
    out << "        ctx->r[3] = prev;\n";
    out << "        Log(\"[_MEMALIGN_R] Alloca align=\" + std::to_string(align) + \" size=\" + std::to_string(size) + \" -> 0x\" + HexStr(prev));\n";
    out << "        return true;\n";
    out << "    });\n\n";

    out << "    // Hook free (0x80031604) e _free_r (0x80030588)\n";
    out << "    g_ModEngine.RegisterPreHook(0x80031604, [](PPCContext* ctx) -> bool { return true; });\n";
    out << "    g_ModEngine.RegisterPreHook(0x80030588, [](PPCContext* ctx) -> bool { return true; });\n\n";

    out << "    // Hook exit (0x800329A4) per uscita ordinata\n";
    out << "    g_ModEngine.RegisterPreHook(0x800329A4, [](PPCContext* ctx) -> bool {\n";
    out << "        Log(\"[EXIT] Richiesta uscita ordinata dal gioco.\");\n";
    out << "        return true;\n";
    out << "    });\n\n";

    out << "    // Hook _sbrk_r (0x80020290) - Allocatore nativo heap continuo per malloc/calloc\n";
    out << "    g_ModEngine.RegisterPreHook(0x80020290, [](PPCContext* ctx) -> bool {\n";
    out << "        int32_t incr = (int32_t)ctx->r[4];\n";
    out << "        uint32_t prev = s_NativeHeap;\n";
    out << "        if (incr > 0) {\n";
    out << "            s_NativeHeap = (s_NativeHeap + incr + 31) & ~31;\n";
    out << "        }\n";
    out << "        ctx->r[3] = prev;\n";
    out << "        Log(\"[SBRK] Heap alloca: \" + std::to_string(incr) + \" bytes -> 0x\" + HexStr(prev) + \" (nuovo break: 0x\" + HexStr(s_NativeHeap) + \")\");\n";
    out << "        return true;\n";
    out << "    });\n\n";

    out << "    // Hook abort (0x8002ED58) per diagnostica dettagliata\n";
    out << "    g_ModEngine.RegisterPreHook(0x8002ED58, [](PPCContext* ctx) -> bool {\n";
    out << "        Log(\"[CRITICAL] abort() chiamato da LR=0x\" + HexStr(ctx->lr));\n";
    out << "        std::cerr << \"[CRITICAL] abort() chiamato da LR=0x\" << std::hex << ctx->lr << \"\\n\";\n";
    out << "        return false;\n";
    out << "    });\n\n";

    out << "    // Hook calloc (0x8002FF14) e _calloc_r (0x800302D0) per garantire allocazione immediata e sicura\n";
    out << "    g_ModEngine.RegisterPreHook(0x8002FF14, [](PPCContext* ctx) -> bool {\n";
    out << "        uint32_t num = ctx->r[3];\n";
    out << "        uint32_t size = ctx->r[4];\n";
    out << "        uint32_t total = num * size;\n";
    out << "        uint32_t prev = s_NativeHeap;\n";
    out << "        s_NativeHeap = (s_NativeHeap + total + 31) & ~31;\n";
    out << "        for (uint32_t i = 0; i < total; ++i) g_Memory.Write8(prev + i, 0);\n";
    out << "        ctx->r[3] = prev;\n";
    out << "        Log(\"[CALLOC] Alloca \" + std::to_string(num) + \" x \" + std::to_string(size) + \" (\" + std::to_string(total) + \" bytes) -> 0x\" + HexStr(prev));\n";
    out << "        return true;\n";
    out << "    });\n\n";

    out << "    g_ModEngine.RegisterPreHook(0x800302D0, [](PPCContext* ctx) -> bool {\n";
    out << "        uint32_t num = ctx->r[4];\n";
    out << "        uint32_t size = ctx->r[5];\n";
    out << "        uint32_t total = num * size;\n";
    out << "        uint32_t prev = s_NativeHeap;\n";
    out << "        s_NativeHeap = (s_NativeHeap + total + 31) & ~31;\n";
    out << "        for (uint32_t i = 0; i < total; ++i) g_Memory.Write8(prev + i, 0);\n";
    out << "        ctx->r[3] = prev;\n";
    out << "        Log(\"[_CALLOC_R] Alloca \" + std::to_string(num) + \" x \" + std::to_string(size) + \" (\" + std::to_string(total) + \" bytes) -> 0x\" + HexStr(prev));\n";
    out << "        return true;\n";
    out << "    });\n\n";

    out << "    // Hook malloc (0x800315D4) e _malloc_r (0x8003064C)\n";
    out << "    g_ModEngine.RegisterPreHook(0x800315D4, [](PPCContext* ctx) -> bool {\n";
    out << "        uint32_t size = ctx->r[3];\n";
    out << "        uint32_t prev = s_NativeHeap;\n";
    out << "        s_NativeHeap = (s_NativeHeap + size + 31) & ~31;\n";
    out << "        ctx->r[3] = prev;\n";
    out << "        Log(\"[MALLOC] Alloca \" + std::to_string(size) + \" bytes -> 0x\" + HexStr(prev) + \" (nuovo break: 0x\" + HexStr(s_NativeHeap) + \")\");\n";
    out << "        return true;\n";
    out << "    });\n\n";

    out << "    g_ModEngine.RegisterPreHook(0x8003064C, [](PPCContext* ctx) -> bool {\n";
    out << "        uint32_t size = ctx->r[4];\n";
    out << "        uint32_t prev = s_NativeHeap;\n";
    out << "        s_NativeHeap = (s_NativeHeap + size + 31) & ~31;\n";
    out << "        ctx->r[3] = prev;\n";
    out << "        Log(\"[_MALLOC_R] Alloca \" + std::to_string(size) + \" bytes -> 0x\" + HexStr(prev) + \" (nuovo break: 0x\" + HexStr(s_NativeHeap) + \")\");\n";
    out << "        return true;\n";
    out << "    });\n\n";

    out << "    // Hook _write_r (0x8003D6C4) per console companion e log\n";
    out << "    g_ModEngine.RegisterPreHook(0x8003D6C4, [](PPCContext* ctx) -> bool {\n";
    out << "        uint32_t fd = ctx->r[4];\n";
    out << "        uint32_t ptr = ctx->r[5];\n";
    out << "        uint32_t len = ctx->r[6];\n";
    out << "        if ((fd == 1 || fd == 2) && len > 0 && len < 65536) {\n";
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

    if (detectedMain) {
        out << "    g_ModEngine.RegisterPreHook(0x" << std::hex << detectedMain << ", [](PPCContext* ctx) -> bool {\n";
        out << "        Log(\"[NATIVE] >>> MAIN (0x" << std::hex << detectedMain << ") RAGGIUNTO CON SUCCESSO! <<<\");\n";
        out << "        if (ctx->r[13] != 0) {\n";
        out << "            uint32_t newHi = 0x80000000 + (uint32_t)MEM1_SIZE - 0x1000;\n";
        out << "            g_Memory.Write32(ctx->r[13] + 584, newHi);\n";
        out << "            g_Memory.Write32(ctx->r[13] + 552, newHi);\n";
        out << "            Log(\"[SYS] Arena1Hi configurata a 0x\" + HexStr(newHi) + \" (128 MB)\");\n";
        out << "        }\n";
        out << "        return false;\n";
        out << "    });\n";
    }
    out << "    PPCContext& ctx = g_GameContext;\n";
    out << "    ctx.Reset();\n";
    out << "    ctx.r[1] = 0x817FFF00;\n";
    if (detectedR2) out << "    ctx.r[2] = 0x" << std::hex << detectedR2 << ";\n";
    if (detectedR13) out << "    ctx.r[13] = 0x" << std::hex << detectedR13 << ";\n";
    out << "    if (ctx.r[13] != 0) {\n";
    out << "        g_Memory.Write32(ctx.r[13] + 132, 0xDEADBEEF);\n";
    out << "        g_Memory.Write32(ctx.r[13] + 144, 0x00000001);\n";
    out << "        uint32_t newHi = 0x80000000 + (uint32_t)MEM1_SIZE - 0x1000;\n";
    out << "        g_Memory.Write32(ctx.r[13] + 584, newHi);\n";
    out << "        g_Memory.Write32(ctx.r[13] + 552, newHi);\n";
    out << "        g_Memory.Write32(ctx.r[13] + 588, 0x800E4960);\n";
    out << "        g_Memory.Write32(ctx.r[13] + 556, 0x800E4960);\n";
    out << "        g_Memory.Write32(ctx.r[13] + 580, 0x90002000);\n";
    out << "        g_Memory.Write32(ctx.r[13] + 576, 0x933E0000);\n";
    out << "    }\n";
    out << "    Log(\"[NATIVE] Esecuzione crt0 / bootstrap / SYS_Init a 0x" 
        << std::hex << dol.entryPoint << "\");\n";
    out << "    " << lifter.GetFunctionName(dol.entryPoint) << "(&ctx);\n";
    out << "    if (ctx.r[13] != 0) {\n";
    out << "        uint32_t newHi = 0x80000000 + (uint32_t)MEM1_SIZE - 0x1000;\n";
    out << "        g_Memory.Write32(ctx.r[13] + 584, newHi);\n";
    out << "        g_Memory.Write32(ctx.r[13] + 552, newHi);\n";
    out << "    }\n";
    if (devkitStartAddr != 0 && devkitStartAddr != dol.entryPoint) {
        out << "    ctx.r[1] = 0x817FFF00;\n";
        if (detectedR2) out << "    ctx.r[2] = 0x" << std::hex << detectedR2 << ";\n";
        if (detectedR13) out << "    ctx.r[13] = 0x" << std::hex << detectedR13 << ";\n";
        out << "    Log(\"[NATIVE] Avvio runtime e main() a 0x" 
            << std::hex << devkitStartAddr << "\");\n";
        out << "    " << lifter.GetFunctionName(devkitStartAddr) << "(&ctx);\n";
    }
    out << "    return 0;\n";
    out << "}\n";

    std::cout << "[COMPLETATO] Generazione completata con successo in " << outputPath << "!\n";
    return 0;
}
