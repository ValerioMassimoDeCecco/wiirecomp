#pragma once
#include <cstdint>
#include <vector>
#include <iostream>
#include <cstring>
#include "wii_types.hpp"

namespace WiiPort {

// Tipi di primitive geometriche GX inviate alla GPU
enum class GXPrimitive : uint8_t {
    Points         = 0xB8,
    Lines          = 0xA8,
    LineStrip      = 0xB0,
    Triangles      = 0x80, // GX_TRIANGLES
    TriangleStrip  = 0x98, // GX_TRIANGLESTRIP
    TriangleFan    = 0xA0, // GX_TRIANGLEFAN
    Quads          = 0x80  // GX_QUADS
};

// Formato degli attributi dei vertici
struct GXVertexFormat {
    uint8_t posFormat;    // 0=None, 1=Direct, 2=Index8, 3=Index16
    uint8_t colorFormat;  // RGBA8 / RGB8
    uint8_t texCoordFormat; // Direct / Index
};

// Struttura di un vertice universale per il backend DirectX 12 / Vulkan
struct UniversalVertex {
    float x, y, z;
    uint32_t colorRGBA;
    float u, v;
};

// Configurazione dello stadio TEV (Texture Environment) della GPU Hollywood
struct TEVStage {
    uint8_t colorInA, colorInB, colorInC, colorInD;
    uint8_t colorOp;     // Add, Sub
    uint8_t colorScale;  // Scale 1x, 2x, 4x, Div2
    uint8_t colorClamp;  // Clamp to 0..1
    uint8_t colorRegId;  // Prev, Reg0, Reg1, Reg2

    uint8_t alphaInA, alphaInB, alphaInC, alphaInD;
    uint8_t alphaOp;
    uint8_t alphaScale;
    uint8_t alphaClamp;
    uint8_t alphaRegId;
};

// Decodificatore del flusso di comandi grafici FIFO inviato su WGPIPE (0xCC008000)
class GXCommandProcessor {
public:
    std::vector<UniversalVertex> vertexOutputBuffer;
    TEVStage tevStages[16];
    uint8_t activeTevStages = 1;
    GXVertexFormat activeFormat;

    // Buffer circolare interno per accumulare i byte della pipe FIFO
    std::vector<uint8_t> fifoStream;

    GXCommandProcessor() {
        std::memset(tevStages, 0, sizeof(tevStages));
        // Default TEV Stage 0: Pass-through Color & Texture
        tevStages[0].colorInA = 0; // C0 (Texture)
        tevStages[0].colorInB = 1; // C1 (Vertex Color)
        tevStages[0].colorInC = 2; // C2 (Zero)
        tevStages[0].colorInD = 3; // Zero
        tevStages[0].colorOp  = 0; // Add: (A * (1-C) + B * C) + D
    }

    // Riceve una sequenza di byte scritti dalla CPU su WGPIPE
    void PushData(const uint8_t* data, size_t size) {
        fifoStream.insert(fifoStream.end(), data, data + size);
        ProcessFIFO();
    }

    inline void WriteByte(uint8_t byte) {
        fifoStream.push_back(byte);
        if (fifoStream.size() >= 3) {
            ProcessFIFO();
        }
    }

    inline void WriteWord(uint32_t word) {
        uint32_t swapped = WII_BYTESWAP32(word);
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&swapped);
        fifoStream.insert(fifoStream.end(), bytes, bytes + 4);
        ProcessFIFO();
    }

private:
    void ProcessFIFO() {
        size_t cursor = 0;
        while (cursor < fifoStream.size()) {
            uint8_t cmd = fifoStream[cursor];

            // Comandi di Disegno Geometrico (Primitive Command)
            if ((cmd & 0x80) != 0 && (cmd & 0x78) <= 0x38) {
                if (cursor + 3 > fifoStream.size()) break; // Mancano byte per header primitiva

                uint8_t primType = cmd & 0x78;
                uint16_t vtxCount = (fifoStream[cursor + 1] << 8) | fifoStream[cursor + 2];
                cursor += 3;

                // Calcolo dimensione per singolo vertice (es. Pos float32 + Color RGBA8 + UV float32 = 12 + 4 + 8 = 24 bytes)
                size_t vtxSize = 24; 
                size_t totalBytesNeeded = vtxCount * vtxSize;

                if (cursor + totalBytesNeeded > fifoStream.size()) {
                    cursor -= 3; // Aspetta che arrivi il resto del flusso
                    break;
                }

                // Decodifica la geometria
                for (uint16_t i = 0; i < vtxCount; ++i) {
                    UniversalVertex v;
                    
                    // Coordinate X, Y, Z (Float32 Big-Endian)
                    uint32_t rawX, rawY, rawZ;
                    std::memcpy(&rawX, &fifoStream[cursor], 4);
                    std::memcpy(&rawY, &fifoStream[cursor + 4], 4);
                    std::memcpy(&rawZ, &fifoStream[cursor + 8], 4);
                    rawX = WII_BYTESWAP32(rawX);
                    rawY = WII_BYTESWAP32(rawY);
                    rawZ = WII_BYTESWAP32(rawZ);
                    std::memcpy(&v.x, &rawX, 4);
                    std::memcpy(&v.y, &rawY, 4);
                    std::memcpy(&v.z, &rawZ, 4);
                    cursor += 12;

                    // Colore RGBA8
                    std::memcpy(&v.colorRGBA, &fifoStream[cursor], 4);
                    v.colorRGBA = WII_BYTESWAP32(v.colorRGBA);
                    cursor += 4;

                    // Coordinate Texture U, V (Float32 Big-Endian)
                    uint32_t rawU, rawV;
                    std::memcpy(&rawU, &fifoStream[cursor], 4);
                    std::memcpy(&rawV, &fifoStream[cursor + 4], 4);
                    rawU = WII_BYTESWAP32(rawU);
                    rawV = WII_BYTESWAP32(rawV);
                    std::memcpy(&v.u, &rawU, 4);
                    std::memcpy(&v.v, &rawV, 4);
                    cursor += 8;

                    vertexOutputBuffer.push_back(v);
                }

                continue;
            }

            // Comandi di Configurazione Registri Hardware BP / CP (0x61 = Load BP Register)
            if (cmd == 0x61) {
                if (cursor + 5 > fifoStream.size()) break;
                uint32_t bpVal = (fifoStream[cursor + 1] << 24) |
                                 (fifoStream[cursor + 2] << 16) |
                                 (fifoStream[cursor + 3] << 8)  |
                                 fifoStream[cursor + 4];
                uint8_t bpReg = fifoStream[cursor + 1]; // Registro di destinazione
                cursor += 5;
                HandleBPRegister(bpReg, bpVal & 0x00FFFFFF);
                continue;
            }

            // Byte sconosciuto o di padding, avanza
            cursor++;
        }

        // Rimuove i byte già elaborati dal buffer circolare
        if (cursor > 0) {
            fifoStream.erase(fifoStream.begin(), fifoStream.begin() + cursor);
        }
    }

    void HandleBPRegister(uint8_t reg, uint32_t val) {
        // Intercetta la configurazione dei registri TEV e Blend Mode
        if (reg >= 0xC0 && reg <= 0xDF) {
            uint8_t stageIdx = (reg - 0xC0) >> 1;
            if (stageIdx < 16) {
                // Configura lo stadio TEV per gli shader moderni
                tevStages[stageIdx].colorInA = (val >> 12) & 0xF;
                tevStages[stageIdx].colorInB = (val >> 8) & 0xF;
                tevStages[stageIdx].colorInC = (val >> 4) & 0xF;
                tevStages[stageIdx].colorInD = val & 0xF;
            }
        }
    }
};

} // namespace WiiPort
