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
    std::cout << "[CFG] Scoperte " << lifter.discoveredFunctions.size() << " funzioni uniche.\n";

    std::cout << "[LIFTER] Generazione codice C++ nativo su " << outputPath << "...\n";
    std::ofstream out(outputPath);
    if (!out.is_open()) {
        std::cerr << "Impossibile creare il file di output: " << outputPath << "\n";
        return 1;
    }

    // Header del file generato
    out << "// Codice generato automaticamente da Wii Universal AOT Recompiler\n";
    out << "#include \"wii_runtime.hpp\"\n";
    out << "using namespace WiiPort;\n\n";

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
    out << "}\n\n";

    out << "int StartRecompiledGame() {\n";
    out << "    InitializeGameDispatchTable();\n";
    out << "    PPCContext ctx;\n";
    out << "    ctx.Reset();\n";
    out << "    std::cout << \"[NATIVE] Avvio eseguibile nativo all'EntryPoint: 0x\" << std::hex << 0x" 
        << std::hex << dol.entryPoint << " << \"...\\n\";\n";
    out << "    " << lifter.GetFunctionName(dol.entryPoint) << "(&ctx);\n";
    out << "    return 0;\n";
    out << "}\n";

    std::cout << "[COMPLETATO] Generazione completata con successo in " << outputPath << "!\n";
    return 0;
}
