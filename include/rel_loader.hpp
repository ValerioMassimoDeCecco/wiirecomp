#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <fstream>
#include <iostream>
#include <cstring>
#include "wii_types.hpp"

namespace WiiPort {

#pragma pack(push, 1)
struct RelHeaderRaw {
    uint32_t id;               // ID del modulo REL
    uint32_t next;             // Link a lista moduli
    uint32_t prev;
    uint32_t numSections;      // Numero totale di sezioni (.text, .data, ecc.)
    uint32_t sectionInfoOffset;// Offset della tabella SectionInfo
    uint32_t nameOffset;       // Offset del nome del modulo
    uint32_t nameSize;
    uint32_t version;          // Versione REL (tipicamente 1, 2, o 3)
    uint32_t bssSize;
    uint32_t relOffset;        // Offset delle tabelle di rilocazione
    uint32_t impOffset;        // Offset delle tabelle di importazione
    uint32_t impSize;
    uint8_t  prologSection;
    uint8_t  epilogSection;
    uint8_t  unresolvedSection;
    uint8_t  bssSection;
    uint32_t prologOffset;     // Entry point del modulo (funzione _prolog)
    uint32_t epilogOffset;     // Funzione di cleanup (_epilog)
    uint32_t unresolvedOffset;
};

struct RelSectionInfoRaw {
    uint32_t offset; // Bit 0 = Executable (isText)
    uint32_t size;
};

struct RelocationEntryRaw {
    uint16_t offset;  // Offset relativo dall'ultima rilocazione
    uint8_t  type;    // Tipo rilocazione (R_PPC_...) o codici di controllo (202 / 203)
    uint8_t  section; // ID sezione del simbolo bersaglio
    uint32_t addend;  // Addend (o indirizzo assoluto se riferito a main.dol)
};
#pragma pack(pop)

class RelModuleLoader {
public:
    uint32_t moduleId = 0;
    uint32_t prologAddress = 0;
    std::vector<uint8_t> rawBuffer;
    RelHeaderRaw header;

    bool Load(const std::string& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            std::cerr << "[REL] Impossibile aprire modulo: " << path << std::endl;
            return false;
        }

        file.seekg(0, std::ios::end);
        size_t size = file.tellg();
        file.seekg(0, std::ios::beg);

        rawBuffer.resize(size);
        file.read(reinterpret_cast<char*>(rawBuffer.data()), size);

        std::memcpy(&header, rawBuffer.data(), sizeof(RelHeaderRaw));
        moduleId = WII_BYTESWAP32(header.id);

        std::cout << "[REL] Caricato Modulo ID " << moduleId << " (Dimensione: " << size << " bytes)\n";
        return true;
    }

    // Applica le rilocazioni del modulo in memoria
    void ApplyRelocations(uint8_t* targetMemoryBase, uint32_t moduleLoadAddress) {
        uint32_t relOffset = WII_BYTESWAP32(header.relOffset);
        if (relOffset == 0 || relOffset >= rawBuffer.size()) return;

        const RelocationEntryRaw* entries = 
            reinterpret_cast<const RelocationEntryRaw*>(rawBuffer.data() + relOffset);

        uint32_t currentSectionAddress = moduleLoadAddress;
        uint32_t writePointer = currentSectionAddress;

        size_t index = 0;
        while (true) {
            uint16_t deltaOffset = WII_BYTESWAP16(entries[index].offset);
            uint8_t type = entries[index].type;
            uint8_t targetSection = entries[index].section;
            uint32_t addend = WII_BYTESWAP32(entries[index].addend);

            writePointer += deltaOffset;

            // Codice 203: Fine della lista di rilocazione
            if (type == 203) {
                break;
            }

            // Codice 202: Cambio di sezione
            if (type == 202) {
                writePointer = moduleLoadAddress; // Si sposta alla sezione targetSection
                index++;
                continue;
            }

            // Calcolo del valore del simbolo risolto
            uint32_t targetSymbolValue = addend; 

            uint32_t* patchLocation = reinterpret_cast<uint32_t*>(targetMemoryBase + (writePointer & 0x01FFFFFF));

            switch (type) {
                case 1: { // R_PPC_ADDR32 (Puntatore a 32-bit assoluto)
                    *patchLocation = WII_BYTESWAP32(targetSymbolValue);
                    break;
                }
                case 4: { // R_PPC_ADDR16_LO (Parte bassa 16-bit: ori/addi)
                    uint16_t* patch16 = reinterpret_cast<uint16_t*>(patchLocation);
                    *patch16 = WII_BYTESWAP16((uint16_t)(targetSymbolValue & 0xFFFF));
                    break;
                }
                case 6: { // R_PPC_ADDR16_HA (Parte alta con compensazione del segno: lis)
                    uint32_t val = (targetSymbolValue >> 16) + ((targetSymbolValue & 0x8000) ? 1 : 0);
                    uint16_t* patch16 = reinterpret_cast<uint16_t*>(patchLocation);
                    *patch16 = WII_BYTESWAP16((uint16_t)val);
                    break;
                }
                case 10: { // R_PPC_REL24 (Salto relativo per branch b / bl)
                    int32_t delta = (int32_t)targetSymbolValue - (int32_t)writePointer;
                    uint32_t instr = WII_BYTESWAP32(*patchLocation);
                    instr = (instr & ~0x03FFFFFC) | (delta & 0x03FFFFFC);
                    *patchLocation = WII_BYTESWAP32(instr);
                    break;
                }
                default:
                    break;
            }

            index++;
        }

        std::cout << "[REL] Modulo ID " << moduleId << " rilocato con successo a 0x" 
                  << std::hex << moduleLoadAddress << std::dec << "\n";
    }
};

} // namespace WiiPort
