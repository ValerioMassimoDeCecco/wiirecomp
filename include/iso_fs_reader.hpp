#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <fstream>
#include <iostream>
#include <cstring>
#include "wii_types.hpp"

namespace WiiPort {

#pragma pack(push, 1)
// Entry della File String Table (FST) della Wii
struct FSTEntryRaw {
    uint8_t  isDir;      // 0 = File, 1 = Directory
    uint8_t  nameOffsetHi;
    uint16_t nameOffsetLo;
    uint32_t fileOffset; // Se file: offset su disco (in blocchi da 4 byte). Se dir: parent index.
    uint32_t fileLength; // Se file: dimensione in byte. Se dir: next index non contenuto nella dir.
};
#pragma pack(pop)

struct VirtualFile {
    std::string path;
    uint64_t discOffset;
    uint32_t size;
    bool isDirectory;
};

// Parser del Filesystem Wii (FST / ISO / WBFS / Cartella estratta)
class WiiFileSystem {
public:
    std::unordered_map<std::string, VirtualFile> fileTable;
    std::string rootExtractedPath;
    bool isExtractedFolder = false;

    // Modalità 1: Inizializzazione da cartella estratta (es. dump con Wiimms ISO Tools)
    bool MountExtractedFolder(const std::string& folderPath) {
        rootExtractedPath = folderPath;
        isExtractedFolder = true;
        std::cout << "[VFS] Montata cartella risorse gioco: " << folderPath << "\n";
        return true;
    }

    // Modalità 2: Parsing della FST binaria dal dump ISO della Wii
    bool MountFromFST(const uint8_t* fstData, size_t fstSize, uint64_t dataPartitionOffset) {
        if (fstSize < sizeof(FSTEntryRaw)) return false;

        const FSTEntryRaw* root = reinterpret_cast<const FSTEntryRaw*>(fstData);
        uint32_t totalEntries = WII_BYTESWAP32(root->fileLength);
        const char* stringTable = reinterpret_cast<const char*>(fstData + (totalEntries * sizeof(FSTEntryRaw)));

        std::vector<std::string> dirStack = { "" };

        for (uint32_t i = 1; i < totalEntries; ++i) {
            const FSTEntryRaw* entry = reinterpret_cast<const FSTEntryRaw*>(fstData + (i * sizeof(FSTEntryRaw)));
            uint32_t nameOffset = ((uint32_t)entry->nameOffsetHi << 16) | WII_BYTESWAP16(entry->nameOffsetLo);
            const char* fileName = stringTable + nameOffset;

            VirtualFile vf;
            vf.isDirectory = (entry->isDir != 0);

            if (vf.isDirectory) {
                dirStack.push_back(fileName);
            } else {
                vf.size = WII_BYTESWAP32(entry->fileLength);
                vf.discOffset = dataPartitionOffset + ((uint64_t)WII_BYTESWAP32(entry->fileOffset) << 2);
                
                std::string fullPath = "";
                for (const auto& d : dirStack) {
                    if (!d.empty()) fullPath += "/" + d;
                }
                fullPath += "/" + std::string(fileName);
                vf.path = fullPath;

                fileTable[fullPath] = vf;
            }
        }

        std::cout << "[VFS] FST montata con successo: " << fileTable.size() << " file indicizzati.\n";
        return true;
    }

    // Lettura trasparente di un file di gioco per il driver /dev/di
    bool ReadFile(const std::string& virtualPath, std::vector<uint8_t>& outBuffer) {
        if (isExtractedFolder) {
            std::string realPath = rootExtractedPath + virtualPath;
            std::ifstream f(realPath, std::ios::binary);
            if (!f.is_open()) return false;

            f.seekg(0, std::ios::end);
            size_t size = f.tellg();
            f.seekg(0, std::ios::beg);

            outBuffer.resize(size);
            f.read(reinterpret_cast<char*>(outBuffer.data()), size);
            return true;
        }

        auto it = fileTable.find(virtualPath);
        if (it == fileTable.end()) return false;

        // In una lettura ISO diretta, qui si legge dall'offset discOffset
        return false;
    }
};

} // namespace WiiPort
