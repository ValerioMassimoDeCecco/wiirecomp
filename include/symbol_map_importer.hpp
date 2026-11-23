#pragma once
#include <string>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstdint>

namespace WiiPort {

// Gestore della mappa dei simboli (compatibile con i formati .map di CodeWarrior, Ghidra e Dolphin)
class SymbolMapImporter {
public:
    std::unordered_map<uint32_t, std::string> addressToName;
    std::unordered_map<std::string, uint32_t> nameToAddress;

    // Carica un file .map (es. esportato da Dolphin o da un decompilation project)
    // Formato tipico:
    // 80004000 00000120 80004000 0 OSInit
    // oppure:
    // 80005a20: Func_Name
    bool LoadMapFile(const std::string& path) {
        std::ifstream file(path);
        if (!file.is_open()) {
            std::cerr << "[SYMBOLS] Impossibile aprire file mappa simboli: " << path << std::endl;
            return false;
        }

        std::string line;
        uint32_t count = 0;

        while (std::getline(file, line)) {
            if (line.empty() || line[0] == '#' || line[0] == '.') continue;

            std::stringstream ss(line);
            std::string token1, token2, token3, token4, token5;
            ss >> token1 >> token2;

            uint32_t addr = 0;
            std::string name;

            // Formato 1: Dolphin map "80003100 00000040 80003100 0 NomeFunzione"
            if (token1.length() >= 8 && (token1.substr(0, 2) == "80" || token1.substr(0, 2) == "90")) {
                addr = std::stoul(token1, nullptr, 16);
                if (ss >> token3 >> token4 >> token5) {
                    name = token5;
                } else if (!token2.empty()) {
                    name = token2;
                }
            }

            // Sanitizza il nome per C++ (rimuove caratteri speciali come ':', '<', '>')
            for (char& c : name) {
                if (c == ':' || c == '<' || c == '>' || c == ' ' || c == '$') {
                    c = '_';
                }
            }

            if (addr != 0 && !name.empty()) {
                addressToName[addr] = name;
                nameToAddress[name] = addr;
                count++;
            }
        }

        std::cout << "[SYMBOLS] Caricati " << count << " simboli con successo da " << path << "\n";
        return true;
    }

    bool HasSymbol(uint32_t addr) const {
        return addressToName.find(addr) != addressToName.end();
    }

    std::string GetName(uint32_t addr) const {
        auto it = addressToName.find(addr);
        if (it != addressToName.end()) return it->second;
        return "";
    }
};

} // namespace WiiPort
