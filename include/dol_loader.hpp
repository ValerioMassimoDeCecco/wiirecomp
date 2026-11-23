#pragma once
#include <string>
#include <vector>
#include <fstream>
#include <cstdint>
#include <iostream>
#include "wii_types.hpp"

namespace WiiPort {

#pragma pack(push, 1)
struct DolHeaderRaw {
    uint32_t textOffset[7];
    uint32_t dataOffset[11];
    uint32_t textAddress[7];
    uint32_t dataAddress[11];
    uint32_t textSize[7];
    uint32_t dataSize[11];
    uint32_t bssAddress;
    uint32_t bssSize;
    uint32_t entryPoint;
    uint32_t unused[7];
};
#pragma pack(pop)

struct DolSection {
    uint32_t offset;
    uint32_t address;
    uint32_t size;
    bool isText;
    std::vector<uint8_t> data;
};

class DolExecutable {
public:
    uint32_t entryPoint = 0;
    uint32_t bssAddress = 0;
    uint32_t bssSize = 0;
    std::vector<DolSection> sections;

    bool Load(const std::string& filePath) {
        std::ifstream file(filePath, std::ios::binary);
        if (!file.is_open()) {
            std::cerr << "[DOL] Errore: impossibile aprire il file " << filePath << std::endl;
            return false;
        }

        DolHeaderRaw rawHeader;
        file.read(reinterpret_cast<char*>(&rawHeader), sizeof(DolHeaderRaw));
        if (file.gcount() != sizeof(DolHeaderRaw)) {
            std::cerr << "[DOL] Errore: header corrotto o incompleto.\n";
            return false;
        }

        entryPoint = WII_BYTESWAP32(rawHeader.entryPoint);
        bssAddress = WII_BYTESWAP32(rawHeader.bssAddress);
        bssSize    = WII_BYTESWAP32(rawHeader.bssSize);

        sections.clear();

        // 7 sezioni di codice (.text0 - .text6)
        for (int i = 0; i < 7; ++i) {
            uint32_t offset = WII_BYTESWAP32(rawHeader.textOffset[i]);
            uint32_t addr   = WII_BYTESWAP32(rawHeader.textAddress[i]);
            uint32_t size   = WII_BYTESWAP32(rawHeader.textSize[i]);

            if (offset > 0 && size > 0) {
                DolSection sec;
                sec.offset = offset;
                sec.address = addr;
                sec.size = size;
                sec.isText = true;
                sec.data.resize(size);

                file.seekg(offset, std::ios::beg);
                file.read(reinterpret_cast<char*>(sec.data.data()), size);
                sections.push_back(std::move(sec));
            }
        }

        // 11 sezioni di dati (.data0 - .data10)
        for (int i = 0; i < 11; ++i) {
            uint32_t offset = WII_BYTESWAP32(rawHeader.dataOffset[i]);
            uint32_t addr   = WII_BYTESWAP32(rawHeader.dataAddress[i]);
            uint32_t size   = WII_BYTESWAP32(rawHeader.dataSize[i]);

            if (offset > 0 && size > 0) {
                DolSection sec;
                sec.offset = offset;
                sec.address = addr;
                sec.size = size;
                sec.isText = false;
                sec.data.resize(size);

                file.seekg(offset, std::ios::beg);
                file.read(reinterpret_cast<char*>(sec.data.data()), size);
                sections.push_back(std::move(sec));
            }
        }

        std::cout << "[DOL] Caricato con successo: " << sections.size() << " sezioni, EntryPoint: 0x"
                  << std::hex << entryPoint << std::dec << std::endl;
        return true;
    }

    // Verifica se un indirizzo ricade in una sezione di codice eseguibile
    bool IsCodeAddress(uint32_t addr) const {
        for (const auto& sec : sections) {
            if (sec.isText && addr >= sec.address && addr < (sec.address + sec.size)) {
                return true;
            }
        }
        return false;
    }

    // Legge una parola a 32-bit (istruzione PowerPC) dall'indirizzo specificato
    bool ReadInstruction(uint32_t addr, uint32_t& outInstr) const {
        for (const auto& sec : sections) {
            if (sec.isText && addr >= sec.address && addr < (sec.address + sec.size)) {
                uint32_t offset = addr - sec.address;
                if (offset + 4 <= sec.size) {
                    uint32_t raw;
                    std::memcpy(&raw, &sec.data[offset], 4);
                    outInstr = WII_BYTESWAP32(raw); // Broadway memorizza le istruzioni big-endian
                    return true;
                }
            }
        }
        return false;
    }
};

} // namespace WiiPort
