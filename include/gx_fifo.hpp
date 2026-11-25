#pragma once
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include <array>
#include <unordered_map>
#include <functional>
#include <algorithm>
#include "wii_types.hpp"

namespace WiiPort {

enum class GXPrimitive : uint8_t {
    Quads         = 0x80,
    Quads2        = 0x88,
    Triangles     = 0x90,
    TriangleStrip = 0x98,
    TriangleFan   = 0xA0,
    Lines         = 0xA8,
    LineStrip     = 0xB0,
    Points        = 0xB8
};

struct UniversalVertex {
    float x, y, z;
    uint32_t colorRGBA;
    float u, v;
};

struct TEVStage {
    uint8_t colorInA = 15, colorInB = 15, colorInC = 15, colorInD = 15;
    uint8_t colorBias = 0, colorOp = 0, colorClamp = 1, colorScale = 0, colorRegId = 0;

    uint8_t alphaInA = 7, alphaInB = 7, alphaInC = 7, alphaInD = 7;
    uint8_t alphaBias = 0, alphaOp = 0, alphaClamp = 1, alphaScale = 0, alphaRegId = 0;
    uint8_t rswap = 0, tswap = 0;

    uint8_t texMap = 0, texCoord = 0, rasChan = 7;
    bool texEnable = false;
};

struct GXTextureState {
    uint32_t addr = 0;
    uint32_t width = 1, height = 1;
    uint32_t format = 6; // RGBA8
    uint32_t wrapS = 0, wrapT = 0; // 0=clamp, 1=repeat, 2=mirror
    uint32_t tlutAddr = 0;
    uint32_t tlutFmt = 0;
    uint32_t tmemOffset = 0;
};

struct DecodedGXVertex {
    float pos[3] = {0, 0, 0};
    float nrm[3] = {0, 0, 1};
    uint8_t col[2][4] = {{255, 255, 255, 255}, {255, 255, 255, 255}};
    float tex[8][2] = {};
    uint8_t posMtxIdx = 0;
    uint8_t texMtxIdx[8] = {60, 60, 60, 60, 60, 60, 60, 60};
};

struct RasterVertex {
    float sx = 0, sy = 0, sz = 0, invW = 1.0f;
    float r = 1, g = 1, b = 1, a = 1;
    float u[8] = {}, v[8] = {};
};

/// Processore completo della pipeline GPU Hollywood (GX FIFO, CP, XF, BP, TEV, Texture e copia EFB->XFB).
class GXCommandProcessor {
public:
    static constexpr uint32_t EFB_MAX_W = 720;
    static constexpr uint32_t EFB_MAX_H = 576;

    std::vector<UniversalVertex> vertexOutputBuffer;
    std::vector<uint8_t> fifoStream;

    TEVStage tevStages[16];
    uint8_t activeTevStages = 1;
    uint8_t numTexGens = 1;
    uint8_t numColorChans = 1;
    uint8_t cullMode = 0; // 0=none, 1=front, 2=back, 3=all

    // PE / EFB / Copy state
    uint16_t peToken = 0;
    uint16_t peStatus = 0; // bit 2 = token, bit 3 = finish
    uint32_t efbCopySrcX = 0, efbCopySrcY = 0;
    uint32_t efbCopySrcW = 640, efbCopySrcH = 480;
    uint32_t xfbCopyDstAddr = 0;
    uint32_t xfbCopyStrideBytes = 640 * 2;
    uint8_t  clearColor[4] = {0, 0, 0, 255}; // R, G, B, A
    uint32_t clearZ = 0x00FFFFFF;

    // Blending & Depth & Alpha compare
    bool zEnable = true, zUpdate = true;
    uint8_t zFunc = 3; // LEQUAL
    bool blendEnable = false, colorUpdate = true, alphaUpdate = true;
    uint8_t srcFactor = 4, dstFactor = 5, blendSubtract = 0; // SRC_ALPHA, INV_SRC_ALPHA
    uint8_t alphaComp0 = 7, alphaComp1 = 7, alphaLogic = 0;
    uint8_t alphaRef0 = 0, alphaRef1 = 0;

    // Scissor (offset 342 su Flipper/Hollywood)
    int32_t scissorX0 = 0, scissorY0 = 0, scissorX1 = 639, scissorY1 = 479;

    // CP state
    uint32_t vcdLo[8] = {};
    uint32_t vcdHi[8] = {};
    uint32_t vatA[8] = {};
    uint32_t vatB[8] = {};
    uint32_t vatC[8] = {};
    uint32_t arrayBase[16] = {};
    uint32_t arrayStride[16] = {};
    uint8_t  curPosMtxIdx = 0;
    uint8_t  curTexMtxIdx[8] = {60, 60, 60, 60, 60, 60, 60, 60};

    // XF state
    uint32_t xfMem[0x1000] = {};
    float    vpHalfW = 320.0f, vpHalfH = -240.0f, vpZRange = 16777215.0f;
    float    vpOffX = 342.0f + 320.0f, vpOffY = 342.0f + 240.0f, vpOffZ = 16777215.0f;
    float    projParams[6] = {1.0f / 320.0f, -1.0f, -1.0f / 240.0f, 1.0f, -1.0f, 0.0f};
    uint32_t projType = 1; // 0=persp, 1=ortho
    uint32_t xfAmbColor[2] = {0xFFFFFFFFu, 0xFFFFFFFFu};
    uint32_t xfMatColor[2] = {0xFFFFFFFFu, 0xFFFFFFFFu};
    uint32_t xfColorChan[4] = {1, 1, 1, 1}; // default MatSrc=Vertex
    uint32_t xfTexGen[8] = {};

    // TEV registers (0=prev, 1..3=c0..c2) e konst (0..3)
    int32_t tevReg[4][4] = {};
    int32_t tevKonst[4][4] = {};
    uint8_t tevKSel[8] = {};
    uint8_t swapTable[4][4] = {{0,1,2,3}, {0,1,2,3}, {0,1,2,3}, {0,1,2,3}};

    // Textures & TMEM TLUT
    GXTextureState textures[8];
    uint16_t tmemTlut[0x4000] = {}; // 32 KB TLUT palette storage
    struct TlutInfo { uint32_t addr = 0; uint32_t fmt = 0; uint32_t tmem = 0; };
    std::unordered_map<uint32_t, TlutInfo> tmemToTlut;

    // EFB (640x528 RGBA8 + Z)
    std::vector<uint32_t> efbColor;
    std::vector<float>    efbDepth;

    // Texture decode cache per frame
    struct CachedTexture {
        uint32_t width = 0, height = 0;
        std::vector<uint32_t> rgba;
    };
    std::unordered_map<uint64_t, CachedTexture> texCache;

    GXCommandProcessor() {
        efbColor.assign(EFB_MAX_W * EFB_MAX_H, 0xFF000000u);
        efbDepth.assign(EFB_MAX_W * EFB_MAX_H, 1.0f);
        ResetState();
    }

    void ResetState() {
        std::memset(tevStages, 0, sizeof(tevStages));
        tevStages[0].colorInA = 15; tevStages[0].colorInB = 15; tevStages[0].colorInC = 15; tevStages[0].colorInD = 10; // RASC
        tevStages[0].colorClamp = 1;
        tevStages[0].alphaInA = 7; tevStages[0].alphaInB = 7; tevStages[0].alphaInC = 7; tevStages[0].alphaInD = 5; // RASA
        tevStages[0].alphaClamp = 1;
        tevStages[0].rasChan = 0;
        activeTevStages = 1;
        for (int i = 0; i < 4; ++i) {
            tevReg[i][0] = tevReg[i][1] = tevReg[i][2] = tevReg[i][3] = 255;
            tevKonst[i][0] = tevKonst[i][1] = tevKonst[i][2] = tevKonst[i][3] = 255;
        }
        // Inizializza matrice identita' negli slot principali (0 e 60)
        auto SetIdentity = [&](uint32_t base) {
            if (base + 12 <= 0x1000) {
                std::memset(&xfMem[base], 0, 12 * 4);
                float one = 1.0f;
                uint32_t uOne; std::memcpy(&uOne, &one, 4);
                xfMem[base + 0] = uOne;
                xfMem[base + 5] = uOne;
                xfMem[base + 10] = uOne;
            }
        };
        SetIdentity(0);
        SetIdentity(60 * 4);
        for (int i = 0; i < 8; ++i) {
            curTexMtxIdx[i] = 60;
            xfTexGen[i] = (4 + i) << 7; // GX_TG_TEXi
        }
    }

    void BindMemory(uint8_t* m1, size_t m1Sz, uint8_t* m2, size_t m2Sz,
                    std::function<void(uint32_t, const char*)> xfbCb) {
        mem1 = m1; mem1Size = m1Sz;
        mem2 = m2; mem2Size = m2Sz;
        onXfbCopy = std::move(xfbCb);
    }

    void PushData(const uint8_t* data, size_t size) {
        if (!data || size == 0) return;
        fifoStream.insert(fifoStream.end(), data, data + size);
        ProcessStream(fifoStream, false);
    }

    inline void WriteByte(uint8_t byte) {
        fifoStream.push_back(byte);
        ProcessStream(fifoStream, false);
    }

    inline void WriteHalf(uint16_t half) {
        fifoStream.push_back((uint8_t)(half >> 8));
        fifoStream.push_back((uint8_t)(half & 0xFF));
        ProcessStream(fifoStream, false);
    }

    inline void WriteWord(uint32_t word) {
        fifoStream.push_back((uint8_t)(word >> 24));
        fifoStream.push_back((uint8_t)(word >> 16));
        fifoStream.push_back((uint8_t)(word >> 8));
        fifoStream.push_back((uint8_t)(word & 0xFF));
        ProcessStream(fifoStream, false);
    }

private:
    uint8_t* mem1 = nullptr;
    size_t   mem1Size = 0;
    uint8_t* mem2 = nullptr;
    size_t   mem2Size = 0;
    std::function<void(uint32_t, const char*)> onXfbCopy;

    const uint8_t* GuestPtr(uint32_t addr, size_t len = 1) const {
        uint8_t top1 = (addr >> 24) & 0xF8;
        if (top1 == 0x00 || top1 == 0x80 || top1 == 0xC0 || top1 == 0xE0) {
            uint32_t off = addr & (uint32_t)(mem1Size - 1);
            return (mem1 && off + len <= mem1Size) ? (mem1 + off) : nullptr;
        }
        uint8_t top2 = (addr >> 24) & 0xFC;
        if (top2 == 0x10 || top2 == 0x90 || top2 == 0xD0) {
            uint32_t off = addr & (uint32_t)(mem2Size - 1);
            return (mem2 && off + len <= mem2Size) ? (mem2 + off) : nullptr;
        }
        return nullptr;
    }

    uint8_t* GuestPtrMut(uint32_t addr, size_t len = 1) {
        return const_cast<uint8_t*>(GuestPtr(addr, len));
    }

    static inline uint16_t BE16(const uint8_t* p) {
        return ((uint16_t)p[0] << 8) | p[1];
    }
    static inline uint32_t BE32(const uint8_t* p) {
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    }
    static inline float BEFloat(const uint8_t* p) {
        uint32_t u = BE32(p);
        float f;
        std::memcpy(&f, &u, 4);
        return f;
    }
    static inline float WordAsFloat(uint32_t u) {
        float f;
        std::memcpy(&f, &u, 4);
        return f;
    }

    uint8_t ReadMem8(uint32_t addr) const {
        const uint8_t* p = GuestPtr(addr, 1);
        return p ? *p : 0;
    }
    uint16_t ReadMem16(uint32_t addr) const {
        const uint8_t* p = GuestPtr(addr, 2);
        return p ? BE16(p) : 0;
    }
    uint32_t ReadMem32(uint32_t addr) const {
        const uint8_t* p = GuestPtr(addr, 4);
        return p ? BE32(p) : 0;
    }

    void ProcessStream(std::vector<uint8_t>& stream, bool inDisplayList) {
        size_t cursor = 0;
        const size_t sz = stream.size();

        while (cursor < sz) {
            uint8_t cmd = stream[cursor];

            // 0x00: NOP (padding / GX_Flush)
            if (cmd == 0x00) {
                cursor++;
                continue;
            }

            // 0x08: Load CP Register (6 bytes)
            if (cmd == 0x08) {
                if (cursor + 6 > sz) break;
                uint8_t reg = stream[cursor + 1];
                uint32_t val = BE32(&stream[cursor + 2]);
                HandleCPRegister(reg, val);
                cursor += 6;
                continue;
            }

            // 0x10: Load XF Register (5 + 4*count bytes)
            if (cmd == 0x10) {
                if (cursor + 5 > sz) break;
                uint16_t lenMinus1 = BE16(&stream[cursor + 1]);
                uint16_t xfAddr = BE16(&stream[cursor + 3]);
                uint32_t count = (uint32_t)(lenMinus1 & 0xFFFF) + 1;
                if (count > 256) { cursor++; continue; } // guardia contro stream corrotti
                if (cursor + 5 + (size_t)count * 4 > sz) break;
                for (uint32_t i = 0; i < count; ++i) {
                    uint32_t w = BE32(&stream[cursor + 5 + i * 4]);
                    HandleXFRegister((uint16_t)(xfAddr + i), w);
                }
                cursor += 5 + (size_t)count * 4;
                continue;
            }

            // 0x20/0x28/0x30/0x38: Load XF Matrix Indexed (5 bytes)
            if (cmd == 0x20 || cmd == 0x28 || cmd == 0x30 || cmd == 0x38) {
                if (cursor + 5 > sz) break;
                uint16_t index = BE16(&stream[cursor + 1]);
                uint16_t info  = BE16(&stream[cursor + 3]);
                uint32_t count = ((info >> 12) & 0xF) + 1;
                uint16_t xfAddr = info & 0xFFF;
                uint32_t arrIdx = (cmd == 0x20) ? 12 : (cmd == 0x28) ? 13 : (cmd == 0x30) ? 14 : 15;
                uint32_t base = arrayBase[arrIdx];
                uint32_t stride = arrayStride[arrIdx];
                uint32_t srcAddr = base + (uint32_t)index * stride;
                for (uint32_t i = 0; i < count; ++i) {
                    HandleXFRegister((uint16_t)(xfAddr + i), ReadMem32(srcAddr + i * 4));
                }
                cursor += 5;
                continue;
            }

            // 0x40: Call Display List (9 bytes)
            if (cmd == 0x40) {
                if (cursor + 9 > sz) break;
                uint32_t dlAddr = BE32(&stream[cursor + 1]);
                uint32_t dlSize = BE32(&stream[cursor + 5]);
                cursor += 9;
                if (!inDisplayList && dlSize > 0 && dlSize <= 16 * 1024 * 1024) {
                    const uint8_t* ptr = GuestPtr(dlAddr, dlSize);
                    if (ptr) {
                        std::vector<uint8_t> dlBuf(ptr, ptr + dlSize);
                        ProcessStream(dlBuf, true);
                    }
                }
                continue;
            }

            // 0x48: Invalidate Vertex Cache (1 byte)
            if (cmd == 0x48) {
                cursor++;
                continue;
            }

            // 0x61: Load BP Register (5 bytes)
            if (cmd == 0x61) {
                if (cursor + 5 > sz) break;
                uint8_t reg = stream[cursor + 1];
                uint32_t val = BE32(&stream[cursor + 1]) & 0x00FFFFFFu;
                HandleBPRegister(reg, val);
                cursor += 5;
                continue;
            }

            // 0x80..0xBF: Draw Primitive
            if ((cmd & 0x80) != 0 && (cmd & 0x78) <= 0x38) {
                if (cursor + 3 > sz) break;
                uint8_t primType = cmd & 0xF8;
                uint8_t vtxFmt = cmd & 0x07;
                uint16_t vtxCount = BE16(&stream[cursor + 1]);
                size_t vtxSize = ComputeVertexByteSize(vtxFmt);
                size_t totalBytes = (size_t)vtxCount * vtxSize;
                if (cursor + 3 + totalBytes > sz) break;

                size_t vcur = cursor + 3;
                if (vtxCount > 0 && vtxSize > 0) {
                    std::vector<DecodedGXVertex> verts(vtxCount);
                    for (uint16_t i = 0; i < vtxCount; ++i) {
                        DecodeVertex(&stream[vcur], vtxFmt, verts[i]);
                        vcur += vtxSize;
                    }
                    EmitAndRasterizePrimitive(primType, verts);
                }
                cursor += 3 + totalBytes;
                continue;
            }

            // Altro byte sconosciuto
            cursor++;
        }

        if (cursor > 0 && !inDisplayList) {
            stream.erase(stream.begin(), stream.begin() + cursor);
        }
    }

    void HandleCPRegister(uint8_t reg, uint32_t val) {
        if (reg == 0x30) {
            curPosMtxIdx = val & 0x3F;
            curTexMtxIdx[0] = (val >> 6) & 0x3F;
            curTexMtxIdx[1] = (val >> 12) & 0x3F;
            curTexMtxIdx[2] = (val >> 18) & 0x3F;
            curTexMtxIdx[3] = (val >> 24) & 0x3F;
        } else if (reg == 0x40) {
            curTexMtxIdx[4] = val & 0x3F;
            curTexMtxIdx[5] = (val >> 6) & 0x3F;
            curTexMtxIdx[6] = (val >> 12) & 0x3F;
            curTexMtxIdx[7] = (val >> 18) & 0x3F;
        } else if (reg >= 0x50 && reg <= 0x57) {
            vcdLo[reg - 0x50] = val;
        } else if (reg >= 0x60 && reg <= 0x67) {
            vcdHi[reg - 0x60] = val;
        } else if (reg >= 0x70 && reg <= 0x77) {
            vatA[reg - 0x70] = val;
        } else if (reg >= 0x80 && reg <= 0x87) {
            vatB[reg - 0x80] = val;
        } else if (reg >= 0x90 && reg <= 0x97) {
            vatC[reg - 0x90] = val;
        } else if (reg >= 0xA0 && reg <= 0xAF) {
            arrayBase[reg - 0xA0] = val;
        } else if (reg >= 0xB0 && reg <= 0xBF) {
            arrayStride[reg - 0xB0] = val;
        }
    }

    void HandleXFRegister(uint16_t addr, uint32_t val) {
        if (addr < 0x1000) {
            xfMem[addr] = val;
            return;
        }
        switch (addr) {
            case 0x1009: numColorChans = (uint8_t)(val & 3); break;
            case 0x100A: xfAmbColor[0] = val; break;
            case 0x100B: xfAmbColor[1] = val; break;
            case 0x100C: xfMatColor[0] = val; break;
            case 0x100D: xfMatColor[1] = val; break;
            case 0x100E: case 0x100F: case 0x1010: case 0x1011:
                xfColorChan[addr - 0x100E] = val; break;
            case 0x1018:
                curPosMtxIdx = val & 0x3F;
                curTexMtxIdx[0] = (val >> 6) & 0x3F;
                curTexMtxIdx[1] = (val >> 12) & 0x3F;
                curTexMtxIdx[2] = (val >> 18) & 0x3F;
                curTexMtxIdx[3] = (val >> 24) & 0x3F;
                break;
            case 0x1019:
                curTexMtxIdx[4] = val & 0x3F;
                curTexMtxIdx[5] = (val >> 6) & 0x3F;
                curTexMtxIdx[6] = (val >> 12) & 0x3F;
                curTexMtxIdx[7] = (val >> 18) & 0x3F;
                break;
            case 0x101A: vpHalfW  = WordAsFloat(val); break;
            case 0x101B: vpHalfH  = WordAsFloat(val); break;
            case 0x101C: vpZRange = WordAsFloat(val); break;
            case 0x101D: vpOffX   = WordAsFloat(val); break;
            case 0x101E: vpOffY   = WordAsFloat(val); break;
            case 0x101F: vpOffZ   = WordAsFloat(val); break;
            case 0x1020: case 0x1021: case 0x1022:
            case 0x1023: case 0x1024: case 0x1025:
                projParams[addr - 0x1020] = WordAsFloat(val); break;
            case 0x1026: projType = val; break;
            case 0x103F: numTexGens = (uint8_t)(val & 0xF); break;
            default:
                if (addr >= 0x1040 && addr <= 0x1047) {
                    xfTexGen[addr - 0x1040] = val;
                }
                break;
        }
    }

    void HandleBPRegister(uint8_t reg, uint32_t val) {
        if (reg == 0x00) { // GENMODE
            numTexGens = (uint8_t)(val & 0xF);
            numColorChans = (uint8_t)((val >> 4) & 7);
            activeTevStages = (uint8_t)(((val >> 10) & 0xF) + 1);
            cullMode = (uint8_t)((val >> 14) & 3);
        } else if (reg == 0x20) { // SCISSOR_TL
            scissorY0 = (int32_t)(val & 0xFFF) - 342;
            scissorX0 = (int32_t)((val >> 12) & 0xFFF) - 342;
        } else if (reg == 0x21) { // SCISSOR_BR
            scissorY1 = (int32_t)(val & 0xFFF) - 342;
            scissorX1 = (int32_t)((val >> 12) & 0xFFF) - 342;
        } else if (reg >= 0x28 && reg <= 0x2F) { // TEV_ORDER
            uint8_t s0 = (reg - 0x28) * 2;
            uint8_t s1 = s0 + 1;
            if (s0 < 16) {
                tevStages[s0].texMap = val & 7;
                tevStages[s0].texCoord = (val >> 3) & 7;
                tevStages[s0].texEnable = ((val >> 6) & 1) != 0;
                tevStages[s0].rasChan = (val >> 7) & 7;
            }
            if (s1 < 16) {
                tevStages[s1].texMap = (val >> 12) & 7;
                tevStages[s1].texCoord = (val >> 15) & 7;
                tevStages[s1].texEnable = ((val >> 18) & 1) != 0;
                tevStages[s1].rasChan = (val >> 19) & 7;
            }
        } else if (reg == 0x40) { // ZMODE
            zEnable = (val & 1) != 0;
            zFunc = (val >> 1) & 7;
            zUpdate = ((val >> 4) & 1) != 0;
        } else if (reg == 0x41) { // CMODE0
            blendEnable = (val & 1) != 0;
            colorUpdate = ((val >> 3) & 1) != 0;
            alphaUpdate = ((val >> 4) & 1) != 0;
            dstFactor = (val >> 5) & 7;
            srcFactor = (val >> 8) & 7;
            blendSubtract = (val >> 11) & 1;
        } else if (reg == 0x45) { // PE_FINISH
            peStatus |= 0x08;
        } else if (reg == 0x47 || reg == 0x48) { // PE_TOKEN
            peToken = (uint16_t)(val & 0xFFFF);
            peStatus |= 0x04;
        } else if (reg == 0x49) { // EFB_SRC_TOP_LEFT
            efbCopySrcX = val & 0x3FF;
            efbCopySrcY = (val >> 10) & 0x3FF;
        } else if (reg == 0x4A) { // EFB_SRC_WH
            efbCopySrcW = (val & 0x3FF) + 1;
            efbCopySrcH = ((val >> 10) & 0x3FF) + 1;
        } else if (reg == 0x4B) { // COPY_DST_ADDR
            xfbCopyDstAddr = (val & 0x00FFFFFFu) << 5;
        } else if (reg == 0x4D) { // XFB_COPY_STRIDE
            xfbCopyStrideBytes = (val & 0x3FF) << 5;
        } else if (reg == 0x4F) { // CLEAR_AR
            clearColor[3] = (uint8_t)((val >> 8) & 0xFF);
            clearColor[0] = (uint8_t)(val & 0xFF);
        } else if (reg == 0x50) { // CLEAR_GB
            clearColor[1] = (uint8_t)((val >> 8) & 0xFF);
            clearColor[2] = (uint8_t)(val & 0xFF);
        } else if (reg == 0x51) { // CLEAR_Z
            clearZ = val & 0x00FFFFFFu;
        } else if (reg == 0x52) { // PE_COPY_EXECUTE (GX_CopyDisp / GX_CopyTex)
            ExecuteEFBCopy(val);
        } else if (reg == 0x64) { // LOAD_TLUT0
            tmemToTlut[0].addr = (val & 0x00FFFFFFu) << 5;
        } else if (reg == 0x65) { // LOAD_TLUT1
            uint32_t tmem = (val & 0x3FF) * 16; // in blocchi da 32 byte = 16 entry uint16
            uint32_t count = ((val >> 10) & 0x3FF) * 16;
            uint32_t src = tmemToTlut[0].addr;
            if (count > 0 && count <= 0x4000) {
                for (uint32_t i = 0; i < count; ++i) {
                    if (tmem + i < 0x4000) {
                        tmemTlut[tmem + i] = ReadMem16(src + i * 2);
                    }
                }
            }
            texCache.clear();
        } else if (reg == 0x66) { // TX_INVALIDATE
            texCache.clear();
        } else if ((reg >= 0x80 && reg <= 0x83) || (reg >= 0xA0 && reg <= 0xA3)) {
            uint8_t idx = (reg >= 0xA0) ? (4 + (reg - 0xA0)) : (reg - 0x80);
            textures[idx].wrapS = val & 3;
            textures[idx].wrapT = (val >> 2) & 3;
        } else if ((reg >= 0x88 && reg <= 0x8B) || (reg >= 0xA8 && reg <= 0xAB)) {
            uint8_t idx = (reg >= 0xA8) ? (4 + (reg - 0xA8)) : (reg - 0x88);
            textures[idx].width  = (val & 0x3FF) + 1;
            textures[idx].height = ((val >> 10) & 0x3FF) + 1;
            textures[idx].format = (val >> 20) & 0xF;
        } else if ((reg >= 0x94 && reg <= 0x97) || (reg >= 0xB4 && reg <= 0xB7)) { // TX_SETIMAGE3 (indirizzo RAM)
            uint8_t idx = (reg >= 0xB4) ? (4 + (reg - 0xB4)) : (reg - 0x94);
            textures[idx].addr = (val & 0x00FFFFFFu) << 5;
        } else if ((reg >= 0x98 && reg <= 0x9B) || (reg >= 0xB8 && reg <= 0xBB)) { // TX_SETTLUT
            uint8_t idx = (reg >= 0xB8) ? (4 + (reg - 0xB8)) : (reg - 0x98);
            textures[idx].tmemOffset = (val & 0x3FF) * 16;
            textures[idx].tlutFmt = (val >> 10) & 3;
        } else if (reg >= 0xC0 && reg <= 0xDF) {
            uint8_t stageIdx = (reg - 0xC0) >> 1;
            if (stageIdx < 16) {
                if ((reg & 1) == 0) {
                    tevStages[stageIdx].colorInA = (val >> 12) & 0xF;
                    tevStages[stageIdx].colorInB = (val >> 8) & 0xF;
                    tevStages[stageIdx].colorInC = (val >> 4) & 0xF;
                    tevStages[stageIdx].colorInD = val & 0xF;
                    tevStages[stageIdx].colorBias = (val >> 16) & 3;
                    tevStages[stageIdx].colorOp   = (val >> 18) & 1;
                    tevStages[stageIdx].colorClamp = (val >> 19) & 1;
                    tevStages[stageIdx].colorScale = (val >> 20) & 3;
                    tevStages[stageIdx].colorRegId = (val >> 22) & 3;
                } else {
                    tevStages[stageIdx].rswap = val & 3;
                    tevStages[stageIdx].tswap = (val >> 2) & 3;
                    tevStages[stageIdx].alphaInA = (val >> 13) & 7;
                    tevStages[stageIdx].alphaInB = (val >> 10) & 7;
                    tevStages[stageIdx].alphaInC = (val >> 7) & 7;
                    tevStages[stageIdx].alphaInD = (val >> 4) & 7;
                    tevStages[stageIdx].alphaBias = (val >> 16) & 3;
                    tevStages[stageIdx].alphaOp   = (val >> 18) & 1;
                    tevStages[stageIdx].alphaClamp = (val >> 19) & 1;
                    tevStages[stageIdx].alphaScale = (val >> 20) & 3;
                    tevStages[stageIdx].alphaRegId = (val >> 22) & 3;
                }
            }
        } else if (reg >= 0xE0 && reg <= 0xE7) {
            uint8_t regId = (reg - 0xE0) >> 1;
            bool isKonst = ((val >> 23) & 1) != 0;
            auto Sign11 = [](uint32_t v) -> int32_t {
                v &= 0x7FF;
                return (v & 0x400) ? (int32_t)(v | 0xFFFFF800u) : (int32_t)v;
            };
            int32_t (*tbl)[4] = isKonst ? tevKonst : tevReg;
            if ((reg & 1) == 0) {
                tbl[regId][0] = Sign11(val);
                tbl[regId][3] = Sign11(val >> 12);
            } else {
                tbl[regId][2] = Sign11(val);
                tbl[regId][1] = Sign11(val >> 12);
            }
        } else if (reg == 0xF3) { // ALPHAFUNC
            alphaRef0 = (uint8_t)(val & 0xFF);
            alphaRef1 = (uint8_t)((val >> 8) & 0xFF);
            alphaComp0 = (uint8_t)((val >> 16) & 7);
            alphaComp1 = (uint8_t)((val >> 19) & 7);
            alphaLogic = (uint8_t)((val >> 22) & 3);
        } else if (reg >= 0xF6 && reg <= 0xFD) {
            uint8_t idx = reg - 0xF6;
            if ((idx & 1) == 0) {
                uint8_t t = idx >> 1;
                swapTable[t][0] = val & 3;
                swapTable[t][1] = (val >> 2) & 3;
            } else {
                uint8_t t = idx >> 1;
                swapTable[t][2] = val & 3;
                swapTable[t][3] = (val >> 2) & 3;
            }
            if (idx * 2 < 16) {
                // KSEL color/alpha per due stadi
            }
            tevKSel[idx] = (uint8_t)((val >> 4) & 0xFF);
            // Conserviamo i 20 bit completi per KCSEL/KASEL negli array separati:
            kSelRaw[idx] = val;
        }
    }

    uint32_t kSelRaw[8] = {};

    static inline size_t CoordSize(uint8_t fmt, uint8_t cnt) {
        size_t elemSz = (fmt <= 1) ? 1 : (fmt <= 3) ? 2 : 4;
        return elemSz * cnt;
    }

    static inline size_t ColorSize(uint8_t fmt) {
        switch (fmt) {
            case 0: case 3: return 2;
            case 1: case 4: return 3;
            default:        return 4;
        }
    }

    size_t ComputeVertexByteSize(uint8_t vtxFmt) const {
        const uint32_t lo = vcdLo[vtxFmt];
        const uint32_t hi = vcdHi[vtxFmt];
        const uint32_t a  = vatA[vtxFmt];
        const uint32_t b  = vatB[vtxFmt];
        const uint32_t c  = vatC[vtxFmt];
        size_t sz = 0;

        // MatIdx bits 0..8
        for (int i = 0; i < 9; ++i) {
            if (lo & (1u << i)) sz += 1;
        }

        auto AttrSize = [&](uint32_t mode, size_t directSize) -> size_t {
            if (mode == 0) return 0;
            if (mode == 1) return directSize;
            return (mode == 2) ? 1 : 2;
        };

        // Pos
        uint8_t posMode = (lo >> 9) & 3;
        uint8_t posCnt  = (a & 1) ? 3 : 2;
        uint8_t posFmt  = (a >> 1) & 7;
        sz += AttrSize(posMode, CoordSize(posFmt, posCnt));

        // Nrm
        uint8_t nrmMode = (lo >> 11) & 3;
        uint8_t nrmCnt  = ((a >> 9) & 1) ? 9 : 3;
        uint8_t nrmFmt  = (a >> 10) & 7;
        if (nrmMode == 1) sz += CoordSize(nrmFmt, nrmCnt);
        else if (nrmMode >= 2) {
            bool nrmIdx3 = ((a >> 31) & 1) != 0 && ((a >> 9) & 1);
            sz += ((nrmMode == 2) ? 1 : 2) * (nrmIdx3 ? 3 : 1);
        }

        // Col0, Col1
        uint8_t col0Mode = (lo >> 13) & 3;
        uint8_t col0Fmt  = (a >> 14) & 7;
        sz += AttrSize(col0Mode, ColorSize(col0Fmt));

        uint8_t col1Mode = (lo >> 15) & 3;
        uint8_t col1Fmt  = (a >> 18) & 7;
        sz += AttrSize(col1Mode, ColorSize(col1Fmt));

        // Tex0..7
        for (int t = 0; t < 8; ++t) {
            uint8_t tMode = (hi >> (t * 2)) & 3;
            if (tMode == 0) continue;
            uint8_t tCnt = 2, tFmt = 4;
            switch (t) {
                case 0: tCnt = ((a >> 21) & 1) ? 2 : 1; tFmt = (a >> 22) & 7; break;
                case 1: tCnt = ((b >> 0) & 1) ? 2 : 1;  tFmt = (b >> 1) & 7;  break;
                case 2: tCnt = ((b >> 9) & 1) ? 2 : 1;  tFmt = (b >> 10) & 7; break;
                case 3: tCnt = ((b >> 18) & 1) ? 2 : 1; tFmt = (b >> 19) & 7; break;
                case 4: tCnt = ((b >> 27) & 1) ? 2 : 1; tFmt = (b >> 28) & 7; break;
                case 5: tCnt = ((c >> 5) & 1) ? 2 : 1;  tFmt = (c >> 6) & 7;  break;
                case 6: tCnt = ((c >> 14) & 1) ? 2 : 1; tFmt = (c >> 15) & 7; break;
                case 7: tCnt = ((c >> 23) & 1) ? 2 : 1; tFmt = (c >> 24) & 7; break;
            }
            sz += AttrSize(tMode, CoordSize(tFmt, tCnt));
        }

        return sz;
    }

    static float ReadComp(const uint8_t* p, uint8_t fmt, float scale, size_t& advance) {
        switch (fmt) {
            case 0: advance = 1; return (float)p[0] * scale;
            case 1: advance = 1; return (float)(int8_t)p[0] * scale;
            case 2: advance = 2; return (float)BE16(p) * scale;
            case 3: advance = 2; return (float)(int16_t)BE16(p) * scale;
            default: advance = 4; return BEFloat(p);
        }
    }

    static void ReadColor(const uint8_t* p, uint8_t fmt, uint8_t out[4]) {
        switch (fmt) {
            case 0: { // RGB565
                uint16_t v = BE16(p);
                out[0] = (uint8_t)(((v >> 11) & 0x1F) * 255 / 31);
                out[1] = (uint8_t)(((v >> 5) & 0x3F) * 255 / 63);
                out[2] = (uint8_t)((v & 0x1F) * 255 / 31);
                out[3] = 255;
                break;
            }
            case 1: // RGB8
                out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = 255;
                break;
            case 2: // RGBX8
                out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = 255;
                break;
            case 3: { // RGBA4
                uint16_t v = BE16(p);
                out[0] = (uint8_t)(((v >> 12) & 0xF) * 17);
                out[1] = (uint8_t)(((v >> 8) & 0xF) * 17);
                out[2] = (uint8_t)(((v >> 4) & 0xF) * 17);
                out[3] = (uint8_t)((v & 0xF) * 17);
                break;
            }
            case 4: { // RGBA6
                uint32_t v = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
                out[0] = (uint8_t)(((v >> 18) & 0x3F) * 255 / 63);
                out[1] = (uint8_t)(((v >> 12) & 0x3F) * 255 / 63);
                out[2] = (uint8_t)(((v >> 6) & 0x3F) * 255 / 63);
                out[3] = (uint8_t)((v & 0x3F) * 255 / 63);
                break;
            }
            default: // RGBA8
                out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = p[3];
                break;
        }
    }

    void DecodeVertex(const uint8_t* p, uint8_t vtxFmt, DecodedGXVertex& v) const {
        const uint32_t lo = vcdLo[vtxFmt];
        const uint32_t hi = vcdHi[vtxFmt];
        const uint32_t a  = vatA[vtxFmt];
        const uint32_t b  = vatB[vtxFmt];
        const uint32_t c  = vatC[vtxFmt];
        const bool byteDequant = ((a >> 30) & 1) != 0;
        size_t off = 0;

        v.posMtxIdx = curPosMtxIdx;
        for (int t = 0; t < 8; ++t) v.texMtxIdx[t] = curTexMtxIdx[t];

        if (lo & 1) { v.posMtxIdx = p[off++] & 0x3F; }
        for (int t = 0; t < 8; ++t) {
            if (lo & (2u << t)) { v.texMtxIdx[t] = p[off++] & 0x3F; }
        }

        auto ResolvePtr = [&](uint32_t mode, uint32_t arrIdx, size_t directSz, const uint8_t*& outPtr) {
            if (mode == 1) {
                outPtr = p + off;
                off += directSz;
            } else if (mode == 2) {
                uint8_t idx = p[off++];
                outPtr = GuestPtr(arrayBase[arrIdx] + (uint32_t)idx * arrayStride[arrIdx], directSz);
            } else if (mode == 3) {
                uint16_t idx = BE16(p + off);
                off += 2;
                outPtr = GuestPtr(arrayBase[arrIdx] + (uint32_t)idx * arrayStride[arrIdx], directSz);
            } else {
                outPtr = nullptr;
            }
        };

        // Pos
        {
            uint8_t posMode = (lo >> 9) & 3;
            uint8_t posCnt  = (a & 1) ? 3 : 2;
            uint8_t posFmt  = (a >> 1) & 7;
            uint8_t posShft = (a >> 4) & 31;
            float scale = (posFmt == 4 || (!byteDequant && posFmt <= 1)) ? 1.0f : std::ldexp(1.0f, -(int)posShft);
            const uint8_t* src = nullptr;
            ResolvePtr(posMode, 0, CoordSize(posFmt, posCnt), src);
            if (src) {
                size_t adv = 0;
                for (uint8_t i = 0; i < posCnt; ++i) {
                    v.pos[i] = ReadComp(src, posFmt, scale, adv);
                    src += adv;
                }
            }
        }

        // Nrm (skip/decode)
        {
            uint8_t nrmMode = (lo >> 11) & 3;
            uint8_t nrmCnt  = ((a >> 9) & 1) ? 9 : 3;
            uint8_t nrmFmt  = (a >> 10) & 7;
            if (nrmMode == 1) {
                const uint8_t* src = p + off;
                off += CoordSize(nrmFmt, nrmCnt);
                float scale = (nrmFmt == 1) ? (1.0f / 64.0f) : (nrmFmt == 3) ? (1.0f / 16384.0f) : 1.0f;
                size_t adv = 0;
                for (int i = 0; i < 3; ++i) { v.nrm[i] = ReadComp(src, nrmFmt, scale, adv); src += adv; }
            } else if (nrmMode >= 2) {
                bool nrmIdx3 = ((a >> 31) & 1) != 0 && ((a >> 9) & 1);
                off += ((nrmMode == 2) ? 1 : 2) * (nrmIdx3 ? 3 : 1);
            }
        }

        // Col0, Col1
        for (int ci = 0; ci < 2; ++ci) {
            uint8_t cMode = (lo >> (13 + ci * 2)) & 3;
            uint8_t cFmt  = (a >> (14 + ci * 4)) & 7;
            if (cMode != 0) {
                const uint8_t* src = nullptr;
                ResolvePtr(cMode, 2 + ci, ColorSize(cFmt), src);
                if (src) ReadColor(src, cFmt, v.col[ci]);
            } else {
                uint32_t mc = xfMatColor[ci];
                v.col[ci][0] = (uint8_t)((mc >> 24) & 0xFF);
                v.col[ci][1] = (uint8_t)((mc >> 16) & 0xFF);
                v.col[ci][2] = (uint8_t)((mc >> 8) & 0xFF);
                v.col[ci][3] = (uint8_t)(mc & 0xFF);
            }
        }

        // Tex0..7
        for (int t = 0; t < 8; ++t) {
            uint8_t tMode = (hi >> (t * 2)) & 3;
            if (tMode == 0) continue;
            uint8_t tCnt = 2, tFmt = 4, tShft = 0;
            switch (t) {
                case 0: tCnt = ((a >> 21) & 1) ? 2 : 1; tFmt = (a >> 22) & 7; tShft = (a >> 25) & 31; break;
                case 1: tCnt = ((b >> 0) & 1) ? 2 : 1;  tFmt = (b >> 1) & 7;  tShft = (b >> 4) & 31;  break;
                case 2: tCnt = ((b >> 9) & 1) ? 2 : 1;  tFmt = (b >> 10) & 7; tShft = (b >> 13) & 31; break;
                case 3: tCnt = ((b >> 18) & 1) ? 2 : 1; tFmt = (b >> 19) & 7; tShft = (b >> 22) & 31; break;
                case 4: tCnt = ((b >> 27) & 1) ? 2 : 1; tFmt = (b >> 28) & 7; tShft = c & 31;         break;
                case 5: tCnt = ((c >> 5) & 1) ? 2 : 1;  tFmt = (c >> 6) & 7;  tShft = (c >> 9) & 31;  break;
                case 6: tCnt = ((c >> 14) & 1) ? 2 : 1; tFmt = (c >> 15) & 7; tShft = (c >> 18) & 31; break;
                case 7: tCnt = ((c >> 23) & 1) ? 2 : 1; tFmt = (c >> 24) & 7; tShft = (c >> 27) & 31; break;
            }
            float scale = (tFmt == 4 || (!byteDequant && tFmt <= 1)) ? 1.0f : std::ldexp(1.0f, -(int)tShft);
            const uint8_t* src = nullptr;
            ResolvePtr(tMode, 4 + t, CoordSize(tFmt, tCnt), src);
            if (src) {
                size_t adv = 0;
                for (uint8_t i = 0; i < tCnt; ++i) {
                    v.tex[t][i] = ReadComp(src, tFmt, scale, adv);
                    src += adv;
                }
            }
        }
    }

    RasterVertex TransformVertex(const DecodedGXVertex& v) const {
        RasterVertex rv;
        uint32_t mBase = (uint32_t)v.posMtxIdx * 4;
        float ex = v.pos[0], ey = v.pos[1], ez = v.pos[2];
        if (mBase + 12 <= 0x1000) {
            float tx = WordAsFloat(xfMem[mBase + 0]) * v.pos[0] + WordAsFloat(xfMem[mBase + 1]) * v.pos[1] + WordAsFloat(xfMem[mBase + 2]) * v.pos[2] + WordAsFloat(xfMem[mBase + 3]);
            float ty = WordAsFloat(xfMem[mBase + 4]) * v.pos[0] + WordAsFloat(xfMem[mBase + 5]) * v.pos[1] + WordAsFloat(xfMem[mBase + 6]) * v.pos[2] + WordAsFloat(xfMem[mBase + 7]);
            float tz = WordAsFloat(xfMem[mBase + 8]) * v.pos[0] + WordAsFloat(xfMem[mBase + 9]) * v.pos[1] + WordAsFloat(xfMem[mBase + 10]) * v.pos[2] + WordAsFloat(xfMem[mBase + 11]);
            ex = tx; ey = ty; ez = tz;
        }

        float cx, cy, cz, cw;
        if (projType == 0) { // Perspective
            cx = projParams[0] * ex + projParams[1] * ez;
            cy = projParams[2] * ey + projParams[3] * ez;
            cz = projParams[4] * ez + projParams[5];
            cw = -ez;
        } else { // Orthographic
            cx = projParams[0] * ex + projParams[1];
            cy = projParams[2] * ey + projParams[3];
            cz = projParams[4] * ez + projParams[5];
            cw = 1.0f;
        }

        if (std::abs(cw) < 1e-6f) cw = (cw >= 0) ? 1e-6f : -1e-6f;
        rv.invW = 1.0f / cw;
        float ndcX = cx * rv.invW;
        float ndcY = cy * rv.invW;
        float ndcZ = cz * rv.invW;

        rv.sx = (vpOffX - 342.0f) + ndcX * vpHalfW;
        rv.sy = (vpOffY - 342.0f) + ndcY * vpHalfH;
        rv.sz = (vpOffZ + ndcZ * vpZRange) / 16777215.0f;

        // Material source selection for Channel 0
        const uint8_t* colSrc = v.col[0];
        uint32_t matPacked = 0;
        if ((xfColorChan[0] & 1) == 0) {
            matPacked = xfMatColor[0];
            rv.r = ((matPacked >> 24) & 0xFF) / 255.0f;
            rv.g = ((matPacked >> 16) & 0xFF) / 255.0f;
            rv.b = ((matPacked >> 8) & 0xFF) / 255.0f;
            rv.a = (matPacked & 0xFF) / 255.0f;
        } else {
            rv.r = colSrc[0] / 255.0f;
            rv.g = colSrc[1] / 255.0f;
            rv.b = colSrc[2] / 255.0f;
            rv.a = colSrc[3] / 255.0f;
        }

        // TexCoord generation
        for (int t = 0; t < 8; ++t) {
            uint32_t tg = xfTexGen[t];
            uint32_t srcRow = (tg >> 7) & 0x1F;
            float s0 = 0, t0 = 0, r0 = 1.0f;
            if (srcRow == 0) {
                s0 = v.pos[0]; t0 = v.pos[1]; r0 = v.pos[2];
            } else if (srcRow >= 4 && srcRow <= 11) {
                uint32_t idx = srcRow - 4;
                s0 = v.tex[idx][0];
                t0 = v.tex[idx][1];
            }
            uint8_t tm = v.texMtxIdx[t];
            if (tm != 60 && (uint32_t)tm * 4 + 12 <= 0x1000) {
                uint32_t tb = (uint32_t)tm * 4;
                float ns = WordAsFloat(xfMem[tb + 0]) * s0 + WordAsFloat(xfMem[tb + 1]) * t0 + WordAsFloat(xfMem[tb + 2]) * r0 + WordAsFloat(xfMem[tb + 3]);
                float nt = WordAsFloat(xfMem[tb + 4]) * s0 + WordAsFloat(xfMem[tb + 5]) * t0 + WordAsFloat(xfMem[tb + 6]) * r0 + WordAsFloat(xfMem[tb + 7]);
                rv.u[t] = ns;
                rv.v[t] = nt;
            } else {
                rv.u[t] = s0;
                rv.v[t] = t0;
            }
        }
        return rv;
    }

    void EmitAndRasterizePrimitive(uint8_t primType, const std::vector<DecodedGXVertex>& verts) {
        std::vector<RasterVertex> tv(verts.size());
        for (size_t i = 0; i < verts.size(); ++i) {
            tv[i] = TransformVertex(verts[i]);
            UniversalVertex uv;
            uv.x = tv[i].sx; uv.y = tv[i].sy; uv.z = tv[i].sz;
            uint8_t cr = (uint8_t)std::clamp(tv[i].r * 255.0f, 0.0f, 255.0f);
            uint8_t cg = (uint8_t)std::clamp(tv[i].g * 255.0f, 0.0f, 255.0f);
            uint8_t cb = (uint8_t)std::clamp(tv[i].b * 255.0f, 0.0f, 255.0f);
            uint8_t ca = (uint8_t)std::clamp(tv[i].a * 255.0f, 0.0f, 255.0f);
            uv.colorRGBA = (cr << 24) | (cg << 16) | (cb << 8) | ca;
            uv.u = tv[i].u[0]; uv.v = tv[i].v[0];
            vertexOutputBuffer.push_back(uv);
        }

        if (primType == 0x80 || primType == 0x88) { // Quads
            for (size_t i = 0; i + 3 < tv.size(); i += 4) {
                RasterizeTriangle(tv[i], tv[i + 1], tv[i + 2]);
                RasterizeTriangle(tv[i], tv[i + 2], tv[i + 3]);
            }
        } else if (primType == 0x90) { // Triangles
            for (size_t i = 0; i + 2 < tv.size(); i += 3) {
                RasterizeTriangle(tv[i], tv[i + 1], tv[i + 2]);
            }
        } else if (primType == 0x98) { // TriangleStrip
            for (size_t i = 0; i + 2 < tv.size(); ++i) {
                if ((i & 1) == 0) RasterizeTriangle(tv[i], tv[i + 1], tv[i + 2]);
                else              RasterizeTriangle(tv[i + 1], tv[i], tv[i + 2]);
            }
        } else if (primType == 0xA0) { // TriangleFan
            for (size_t i = 1; i + 1 < tv.size(); ++i) {
                RasterizeTriangle(tv[0], tv[i], tv[i + 1]);
            }
        } else if (primType == 0xA8) { // Lines
            for (size_t i = 0; i + 1 < tv.size(); i += 2) {
                RasterizeLine(tv[i], tv[i + 1]);
            }
        } else if (primType == 0xB0) { // LineStrip
            for (size_t i = 0; i + 1 < tv.size(); ++i) {
                RasterizeLine(tv[i], tv[i + 1]);
            }
        } else if (primType == 0xB8) { // Points
            for (const auto& p : tv) RasterizeLine(p, p);
        }
    }

    const CachedTexture& GetDecodedTexture(uint8_t mapId) {
        const auto& ts = textures[mapId & 7];
        uint64_t key = ((uint64_t)ts.addr << 24) ^ ((uint64_t)ts.format << 20) ^
                       ((uint64_t)ts.width << 10) ^ (uint64_t)ts.height ^ ((uint64_t)ts.tmemOffset << 12);
        auto it = texCache.find(key);
        if (it != texCache.end()) return it->second;

        CachedTexture ct;
        ct.width = std::max<uint32_t>(1, ts.width);
        ct.height = std::max<uint32_t>(1, ts.height);
        ct.rgba.assign((size_t)ct.width * ct.height, 0xFFFFFFFFu);

        auto DecodeTlutColor = [&](uint16_t raw) -> uint32_t {
            if (ts.tlutFmt == 0) { // IA8
                uint8_t a = raw >> 8, i = raw & 0xFF;
                return (i) | (i << 8) | (i << 16) | ((uint32_t)a << 24);
            } else if (ts.tlutFmt == 1) { // RGB565
                uint8_t r = (uint8_t)(((raw >> 11) & 0x1F) * 255 / 31);
                uint8_t g = (uint8_t)(((raw >> 5) & 0x3F) * 255 / 63);
                uint8_t b = (uint8_t)((raw & 0x1F) * 255 / 31);
                return r | (g << 8) | (b << 16) | 0xFF000000u;
            } else { // RGB5A3
                if (raw & 0x8000) {
                    uint8_t r = (uint8_t)(((raw >> 10) & 0x1F) * 255 / 31);
                    uint8_t g = (uint8_t)(((raw >> 5) & 0x1F) * 255 / 31);
                    uint8_t b = (uint8_t)((raw & 0x1F) * 255 / 31);
                    return r | (g << 8) | (b << 16) | 0xFF000000u;
                } else {
                    uint8_t a = (uint8_t)(((raw >> 12) & 7) * 255 / 7);
                    uint8_t r = (uint8_t)(((raw >> 8) & 0xF) * 17);
                    uint8_t g = (uint8_t)(((raw >> 4) & 0xF) * 17);
                    uint8_t b = (uint8_t)((raw & 0xF) * 17);
                    return r | (g << 8) | (b << 16) | ((uint32_t)a << 24);
                }
            }
        };

        uint32_t w = ct.width, h = ct.height;
        uint32_t addr = ts.addr;

        switch (ts.format) {
            case 0x0: { // I4 (8x8)
                uint32_t bw = (w + 7) / 8, bh = (h + 7) / 8;
                const uint8_t* src = GuestPtr(addr, (size_t)bw * bh * 32);
                if (src) {
                    for (uint32_t ty = 0; ty < bh; ++ty) for (uint32_t tx = 0; tx < bw; ++tx) {
                        const uint8_t* tile = src + ((ty * bw + tx) * 32);
                        for (uint32_t y = 0; y < 8; ++y) for (uint32_t x = 0; x < 8; ++x) {
                            uint32_t px = tx * 8 + x, py = ty * 8 + y;
                            if (px >= w || py >= h) continue;
                            uint8_t b = tile[y * 4 + (x >> 1)];
                            uint8_t v = ((x & 1) ? (b & 0xF) : (b >> 4)) * 17;
                            ct.rgba[py * w + px] = v | (v << 8) | (v << 16) | ((uint32_t)v << 24);
                        }
                    }
                }
                break;
            }
            case 0x1: case 0x2: { // I8 / IA4 (8x4)
                uint32_t bw = (w + 7) / 8, bh = (h + 3) / 4;
                const uint8_t* src = GuestPtr(addr, (size_t)bw * bh * 32);
                if (src) {
                    for (uint32_t ty = 0; ty < bh; ++ty) for (uint32_t tx = 0; tx < bw; ++tx) {
                        const uint8_t* tile = src + ((ty * bw + tx) * 32);
                        for (uint32_t y = 0; y < 4; ++y) for (uint32_t x = 0; x < 8; ++x) {
                            uint32_t px = tx * 8 + x, py = ty * 4 + y;
                            if (px >= w || py >= h) continue;
                            uint8_t v = tile[y * 8 + x];
                            if (ts.format == 0x1) {
                                ct.rgba[py * w + px] = v | (v << 8) | (v << 16) | ((uint32_t)v << 24);
                            } else {
                                uint8_t a = (v >> 4) * 17, c = (v & 0xF) * 17;
                                ct.rgba[py * w + px] = c | (c << 8) | (c << 16) | ((uint32_t)a << 24);
                            }
                        }
                    }
                }
                break;
            }
            case 0x3: case 0x4: case 0x5: { // IA8 / RGB565 / RGB5A3 (4x4, 16bpp)
                uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
                const uint8_t* src = GuestPtr(addr, (size_t)bw * bh * 32);
                if (src) {
                    for (uint32_t ty = 0; ty < bh; ++ty) for (uint32_t tx = 0; tx < bw; ++tx) {
                        const uint8_t* tile = src + ((ty * bw + tx) * 32);
                        for (uint32_t y = 0; y < 4; ++y) for (uint32_t x = 0; x < 4; ++x) {
                            uint32_t px = tx * 4 + x, py = ty * 4 + y;
                            if (px >= w || py >= h) continue;
                            uint16_t raw = BE16(tile + (y * 4 + x) * 2);
                            if (ts.format == 0x3) {
                                uint8_t a = raw >> 8, c = raw & 0xFF;
                                ct.rgba[py * w + px] = c | (c << 8) | (c << 16) | ((uint32_t)a << 24);
                            } else if (ts.format == 0x4) {
                                uint8_t r = (uint8_t)(((raw >> 11) & 0x1F) * 255 / 31);
                                uint8_t g = (uint8_t)(((raw >> 5) & 0x3F) * 255 / 63);
                                uint8_t b = (uint8_t)((raw & 0x1F) * 255 / 31);
                                ct.rgba[py * w + px] = r | (g << 8) | (b << 16) | 0xFF000000u;
                            } else {
                                if (raw & 0x8000) {
                                    uint8_t r = (uint8_t)(((raw >> 10) & 0x1F) * 255 / 31);
                                    uint8_t g = (uint8_t)(((raw >> 5) & 0x1F) * 255 / 31);
                                    uint8_t b = (uint8_t)((raw & 0x1F) * 255 / 31);
                                    ct.rgba[py * w + px] = r | (g << 8) | (b << 16) | 0xFF000000u;
                                } else {
                                    uint8_t a = (uint8_t)(((raw >> 12) & 7) * 255 / 7);
                                    uint8_t r = (uint8_t)(((raw >> 8) & 0xF) * 17);
                                    uint8_t g = (uint8_t)(((raw >> 4) & 0xF) * 17);
                                    uint8_t b = (uint8_t)((raw & 0xF) * 17);
                                    ct.rgba[py * w + px] = r | (g << 8) | (b << 16) | ((uint32_t)a << 24);
                                }
                            }
                        }
                    }
                }
                break;
            }
            case 0x6: { // RGBA8 (4x4, 64 bytes/tile: 32B AR + 32B GB)
                uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
                const uint8_t* src = GuestPtr(addr, (size_t)bw * bh * 64);
                if (src) {
                    for (uint32_t ty = 0; ty < bh; ++ty) for (uint32_t tx = 0; tx < bw; ++tx) {
                        const uint8_t* tile = src + ((ty * bw + tx) * 64);
                        for (uint32_t y = 0; y < 4; ++y) for (uint32_t x = 0; x < 4; ++x) {
                            uint32_t px = tx * 4 + x, py = ty * 4 + y;
                            if (px >= w || py >= h) continue;
                            uint32_t idx = (y * 4 + x) * 2;
                            uint8_t a = tile[idx], r = tile[idx + 1];
                            uint8_t g = tile[32 + idx], b = tile[32 + idx + 1];
                            ct.rgba[py * w + px] = r | (g << 8) | (b << 16) | ((uint32_t)a << 24);
                        }
                    }
                }
                break;
            }
            case 0x8: { // CI4 (8x8)
                uint32_t bw = (w + 7) / 8, bh = (h + 7) / 8;
                const uint8_t* src = GuestPtr(addr, (size_t)bw * bh * 32);
                if (src) {
                    for (uint32_t ty = 0; ty < bh; ++ty) for (uint32_t tx = 0; tx < bw; ++tx) {
                        const uint8_t* tile = src + ((ty * bw + tx) * 32);
                        for (uint32_t y = 0; y < 8; ++y) for (uint32_t x = 0; x < 8; ++x) {
                            uint32_t px = tx * 8 + x, py = ty * 8 + y;
                            if (px >= w || py >= h) continue;
                            uint8_t b = tile[y * 4 + (x >> 1)];
                            uint8_t ci = (x & 1) ? (b & 0xF) : (b >> 4);
                            uint32_t tIdx = (ts.tmemOffset + ci) & 0x3FFF;
                            ct.rgba[py * w + px] = DecodeTlutColor(tmemTlut[tIdx]);
                        }
                    }
                }
                break;
            }
            case 0x9: { // CI8 (8x4)
                uint32_t bw = (w + 7) / 8, bh = (h + 3) / 4;
                const uint8_t* src = GuestPtr(addr, (size_t)bw * bh * 32);
                if (src) {
                    for (uint32_t ty = 0; ty < bh; ++ty) for (uint32_t tx = 0; tx < bw; ++tx) {
                        const uint8_t* tile = src + ((ty * bw + tx) * 32);
                        for (uint32_t y = 0; y < 4; ++y) for (uint32_t x = 0; x < 8; ++x) {
                            uint32_t px = tx * 8 + x, py = ty * 4 + y;
                            if (px >= w || py >= h) continue;
                            uint8_t ci = tile[y * 8 + x];
                            uint32_t tIdx = (ts.tmemOffset + ci) & 0x3FFF;
                            ct.rgba[py * w + px] = DecodeTlutColor(tmemTlut[tIdx]);
                        }
                    }
                }
                break;
            }
            case 0xE: { // CMPR (8x8 tiles = 2x2 sub-blocks DXT1 4x4)
                uint32_t bw = (w + 7) / 8, bh = (h + 7) / 8;
                const uint8_t* src = GuestPtr(addr, (size_t)bw * bh * 32);
                if (src) {
                    for (uint32_t ty = 0; ty < bh; ++ty) for (uint32_t tx = 0; tx < bw; ++tx) {
                        const uint8_t* tile = src + ((ty * bw + tx) * 32);
                        for (uint32_t sb = 0; sb < 4; ++sb) {
                            const uint8_t* blk = tile + sb * 8;
                            uint16_t c0 = BE16(blk), c1 = BE16(blk + 2);
                            uint32_t bits = BE32(blk + 4);
                            uint8_t pal[4][4];
                            auto Exp565 = [](uint16_t c, uint8_t out[4]) {
                                out[0] = (uint8_t)(((c >> 11) & 0x1F) * 255 / 31);
                                out[1] = (uint8_t)(((c >> 5) & 0x3F) * 255 / 63);
                                out[2] = (uint8_t)((c & 0x1F) * 255 / 31);
                                out[3] = 255;
                            };
                            Exp565(c0, pal[0]); Exp565(c1, pal[1]);
                            if (c0 > c1) {
                                for (int k = 0; k < 3; ++k) {
                                    pal[2][k] = (uint8_t)((2 * pal[0][k] + pal[1][k]) / 3);
                                    pal[3][k] = (uint8_t)((pal[0][k] + 2 * pal[1][k]) / 3);
                                }
                                pal[2][3] = 255; pal[3][3] = 255;
                            } else {
                                for (int k = 0; k < 3; ++k) pal[2][k] = (uint8_t)((pal[0][k] + pal[1][k]) / 2);
                                pal[2][3] = 255;
                                pal[3][0] = pal[3][1] = pal[3][2] = pal[3][3] = 0;
                            }
                            uint32_t bx = (sb & 1) * 4, by = (sb >> 1) * 4;
                            for (uint32_t y = 0; y < 4; ++y) for (uint32_t x = 0; x < 4; ++x) {
                                uint32_t px = tx * 8 + bx + x, py = ty * 8 + by + y;
                                if (px >= w || py >= h) continue;
                                uint32_t idx = (bits >> (30 - (y * 4 + x) * 2)) & 3;
                                ct.rgba[py * w + px] = pal[idx][0] | (pal[idx][1] << 8) | (pal[idx][2] << 16) | ((uint32_t)pal[idx][3] << 24);
                            }
                        }
                    }
                }
                break;
            }
            default:
                break;
        }
        auto ins = texCache.emplace(key, std::move(ct));
        return ins.first->second;
    }

    static int32_t WrapCoord(int32_t c, int32_t dim, uint32_t wrap) {
        if (dim <= 1) return 0;
        if (wrap == 0) return std::clamp(c, 0, dim - 1); // clamp
        if (wrap == 1) { // repeat
            int32_t r = c % dim;
            return (r < 0) ? (r + dim) : r;
        }
        // mirror
        int32_t p = c % (dim * 2);
        if (p < 0) p += dim * 2;
        return (p < dim) ? p : (dim * 2 - 1 - p);
    }

    static inline uint32_t SampleCachedTexture(const CachedTexture& tex, const GXTextureState& ts, float u, float v) {
        int32_t tx = WrapCoord((int32_t)std::floor(u * tex.width), (int32_t)tex.width, ts.wrapS);
        int32_t ty = WrapCoord((int32_t)std::floor(v * tex.height), (int32_t)tex.height, ts.wrapT);
        return tex.rgba[(size_t)ty * tex.width + tx];
    }

    int32_t GetKonstComp(uint8_t ksel, int comp) const {
        if (ksel <= 7) {
            static const int32_t kFracs[8] = {255, 223, 191, 159, 128, 96, 64, 32};
            return kFracs[ksel];
        }
        if (ksel >= 12 && ksel <= 15) return tevKonst[ksel - 12][comp];
        if (ksel >= 16 && ksel <= 19) return tevKonst[ksel - 16][0];
        if (ksel >= 20 && ksel <= 23) return tevKonst[ksel - 20][1];
        if (ksel >= 24 && ksel <= 27) return tevKonst[ksel - 24][2];
        if (ksel >= 28 && ksel <= 31) return tevKonst[ksel - 28][3];
        return 255;
    }

    void EvaluateTEV(const int32_t ras[4], const float u[8], const float v[8],
                     const CachedTexture* const stageTex[16], uint8_t out[4]) const {
        int32_t regs[4][4];
        for (int i = 0; i < 4; ++i) for (int k = 0; k < 4; ++k) regs[i][k] = tevReg[i][k];

        for (uint8_t s = 0; s < activeTevStages && s < 16; ++s) {
            const auto& st = tevStages[s];
            int32_t texc[4] = {255, 255, 255, 255};
            if (st.texEnable && stageTex[s]) {
                uint8_t tc = st.texCoord & 7;
                uint32_t c = SampleCachedTexture(*stageTex[s], textures[st.texMap & 7], u[tc], v[tc]);
                int32_t rawT[4] = {
                    (int32_t)(c & 0xFF), (int32_t)((c >> 8) & 0xFF),
                    (int32_t)((c >> 16) & 0xFF), (int32_t)((c >> 24) & 0xFF)
                };
                const uint8_t* tsw = swapTable[st.tswap & 3];
                for (int k = 0; k < 4; ++k) texc[k] = rawT[tsw[k]];
            }
            int32_t rasSwapped[4] = {255, 255, 255, 255};
            if (st.rasChan == 7) {
                for (int k = 0; k < 4; ++k) rasSwapped[k] = regs[0][k];
            } else {
                const uint8_t* rsw = swapTable[st.rswap & 3];
                for (int k = 0; k < 4; ++k) rasSwapped[k] = ras[rsw[k]];
            }

            uint32_t kraw = kSelRaw[s >> 1];
            uint8_t kcsel = (s & 1) ? ((kraw >> 14) & 0x1F) : ((kraw >> 4) & 0x1F);
            uint8_t kasel = (s & 1) ? ((kraw >> 19) & 0x1F) : ((kraw >> 9) & 0x1F);

            auto PickColor = [&](uint8_t sel, int comp) -> int32_t {
                switch (sel) {
                    case 0:  return regs[0][comp];
                    case 1:  return regs[0][3];
                    case 2:  return regs[1][comp];
                    case 3:  return regs[1][3];
                    case 4:  return regs[2][comp];
                    case 5:  return regs[2][3];
                    case 6:  return regs[3][comp];
                    case 7:  return regs[3][3];
                    case 8:  return texc[comp];
                    case 9:  return texc[3];
                    case 10: return rasSwapped[comp];
                    case 11: return rasSwapped[3];
                    case 12: return 255; // ONE
                    case 13: return 128; // HALF
                    case 14: return GetKonstComp(kcsel, comp);
                    default: return 0;   // ZERO
                }
            };
            auto PickAlpha = [&](uint8_t sel) -> int32_t {
                switch (sel) {
                    case 0: return regs[0][3];
                    case 1: return regs[1][3];
                    case 2: return regs[2][3];
                    case 3: return regs[3][3];
                    case 4: return texc[3];
                    case 5: return rasSwapped[3];
                    case 6: return GetKonstComp(kasel, 3);
                    default: return 0;  // ZERO
                }
            };

            auto ApplyStage = [](int32_t a, int32_t b, int32_t c, int32_t d, uint8_t bias, uint8_t op, uint8_t scale, uint8_t clamp) -> int32_t {
                int32_t lerp = ((255 - c) * a + c * b + 127) / 255;
                int32_t res = (op == 0) ? (d + lerp) : (d - lerp);
                if (bias == 1) res += 128;
                else if (bias == 2) res -= 128;
                if (scale == 1) res *= 2;
                else if (scale == 2) res *= 4;
                else if (scale == 3) res /= 2;
                if (clamp) res = std::clamp(res, 0, 255);
                return res;
            };

            int32_t nr[4];
            if (st.colorBias == 3) {
                nr[0] = regs[0][0]; nr[1] = regs[0][1]; nr[2] = regs[0][2];
            } else {
                for (int comp = 0; comp < 3; ++comp) {
                    nr[comp] = ApplyStage(PickColor(st.colorInA, comp), PickColor(st.colorInB, comp),
                                          PickColor(st.colorInC, comp), PickColor(st.colorInD, comp),
                                          st.colorBias, st.colorOp, st.colorScale, st.colorClamp);
                }
            }
            if (st.alphaBias == 3) {
                nr[3] = regs[0][3];
            } else {
                nr[3] = ApplyStage(PickAlpha(st.alphaInA), PickAlpha(st.alphaInB),
                                   PickAlpha(st.alphaInC), PickAlpha(st.alphaInD),
                                   st.alphaBias, st.alphaOp, st.alphaScale, st.alphaClamp);
            }
            uint8_t cDest = st.colorRegId & 3;
            uint8_t aDest = st.alphaRegId & 3;
            regs[cDest][0] = nr[0]; regs[cDest][1] = nr[1]; regs[cDest][2] = nr[2];
            regs[aDest][3] = nr[3];
        }

        for (int k = 0; k < 4; ++k) out[k] = (uint8_t)std::clamp(regs[0][k], 0, 255);
    }

    static bool TestAlpha(uint8_t a, uint8_t ref, uint8_t comp) {
        switch (comp) {
            case 0: return false;
            case 1: return a < ref;
            case 2: return a == ref;
            case 3: return a <= ref;
            case 4: return a > ref;
            case 5: return a != ref;
            case 6: return a >= ref;
            default: return true;
        }
    }

    void WritePixelEFB(uint32_t x, uint32_t y, float z, const uint8_t src[4]) {
        if (x >= EFB_MAX_W || y >= EFB_MAX_H) return;

        auto CmpA = [&](uint8_t comp, uint8_t ref) { return TestAlpha(src[3], ref, comp); };
        bool pass0 = CmpA(alphaComp0, alphaRef0);
        bool pass1 = CmpA(alphaComp1, alphaRef1);
        bool aPass = (alphaLogic == 0) ? (pass0 && pass1) :
                     (alphaLogic == 1) ? (pass0 || pass1) :
                     (alphaLogic == 2) ? (pass0 != pass1) : (pass0 == pass1);
        if (!aPass) return;

        size_t idx = (size_t)y * EFB_MAX_W + x;
        if (zEnable) {
            uint32_t z24 = (uint32_t)(std::clamp(z, 0.0f, 1.0f) * 16777215.0f + 0.5f);
            uint32_t curZ24 = (uint32_t)(std::clamp(efbDepth[idx], 0.0f, 1.0f) * 16777215.0f + 0.5f);
            bool zPass = true;
            switch (zFunc) {
                case 0: zPass = false; break;
                case 1: zPass = z24 < curZ24; break;
                case 2: zPass = z24 == curZ24; break;
                case 3: zPass = z24 <= curZ24; break;
                case 4: zPass = z24 > curZ24; break;
                case 5: zPass = z24 != curZ24; break;
                case 6: zPass = z24 >= curZ24; break;
                default: zPass = true; break;
            }
            if (!zPass) return;
            if (zUpdate) efbDepth[idx] = (float)z24 / 16777215.0f;
        }

        uint32_t dstPx = efbColor[idx];
        uint8_t dst[4] = {
            (uint8_t)(dstPx & 0xFF),
            (uint8_t)((dstPx >> 8) & 0xFF),
            (uint8_t)((dstPx >> 16) & 0xFF),
            (uint8_t)((dstPx >> 24) & 0xFF)
        };

        uint8_t out[4] = {src[0], src[1], src[2], src[3]};
        if (blendEnable) {
            auto Factor = [&](uint8_t f, uint8_t outF[3]) {
                switch (f) {
                    case 0: outF[0] = outF[1] = outF[2] = 0; break;
                    case 1: outF[0] = outF[1] = outF[2] = 255; break;
                    case 2: outF[0] = dst[0]; outF[1] = dst[1]; outF[2] = dst[2]; break;
                    case 3: outF[0] = 255 - dst[0]; outF[1] = 255 - dst[1]; outF[2] = 255 - dst[2]; break;
                    case 4: outF[0] = outF[1] = outF[2] = src[3]; break;
                    case 5: outF[0] = outF[1] = outF[2] = 255 - src[3]; break;
                    case 6: outF[0] = outF[1] = outF[2] = dst[3]; break;
                    default: outF[0] = outF[1] = outF[2] = 255 - dst[3]; break;
                }
            };
            uint8_t sf[3], df[3];
            Factor(srcFactor, sf);
            Factor(dstFactor, df);
            for (int k = 0; k < 3; ++k) {
                int v = blendSubtract
                    ? ((src[k] * sf[k] - dst[k] * df[k]) / 255)
                    : ((src[k] * sf[k] + dst[k] * df[k]) / 255);
                out[k] = (uint8_t)std::clamp(v, 0, 255);
            }
        }
        if (!colorUpdate) { out[0] = dst[0]; out[1] = dst[1]; out[2] = dst[2]; }
        if (!alphaUpdate) { out[3] = dst[3]; }
        efbColor[idx] = out[0] | (out[1] << 8) | (out[2] << 16) | ((uint32_t)out[3] << 24);
    }

    void RasterizeLine(const RasterVertex& a, const RasterVertex& b) {
        int x0 = (int)std::lround(a.sx), y0 = (int)std::lround(a.sy);
        int x1 = (int)std::lround(b.sx), y1 = (int)std::lround(b.sy);
        int steps = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
        int32_t ras[4] = {
            (int32_t)(a.r * 255.0f), (int32_t)(a.g * 255.0f),
            (int32_t)(a.b * 255.0f), (int32_t)(a.a * 255.0f)
        };
        const CachedTexture* stageTex[16] = {};
        for (uint8_t s = 0; s < activeTevStages && s < 16; ++s) {
            if (tevStages[s].texEnable) stageTex[s] = &GetDecodedTexture(tevStages[s].texMap);
        }
        uint8_t px[4];
        EvaluateTEV(ras, a.u, a.v, stageTex, px);
        if (steps == 0) {
            if (x0 >= scissorX0 && x0 <= scissorX1 && y0 >= scissorY0 && y0 <= scissorY1) {
                WritePixelEFB((uint32_t)x0, (uint32_t)y0, a.sz, px);
            }
            return;
        }
        for (int s = 0; s <= steps; ++s) {
            float t = (float)s / (float)steps;
            int x = (int)std::lround(a.sx + (b.sx - a.sx) * t);
            int y = (int)std::lround(a.sy + (b.sy - a.sy) * t);
            if (x >= scissorX0 && x <= scissorX1 && y >= scissorY0 && y <= scissorY1) {
                WritePixelEFB((uint32_t)x, (uint32_t)y, a.sz, px);
            }
        }
    }

    void RasterizeTriangle(const RasterVertex& v0, const RasterVertex& v1, const RasterVertex& v2) {
        float area = (v1.sx - v0.sx) * (v2.sy - v0.sy) - (v1.sy - v0.sy) * (v2.sx - v0.sx);
        if (std::abs(area) < 1e-4f) return;
        if (cullMode == 3) return;
        if (cullMode == 1 && area < 0.0f) return;
        if (cullMode == 2 && area > 0.0f) return;

        int32_t minX = (std::max<int32_t>)({0, scissorX0, (int32_t)std::floor((std::min)({v0.sx, v1.sx, v2.sx}))});
        int32_t maxX = (std::min<int32_t>)({(int32_t)EFB_MAX_W - 1, scissorX1, (int32_t)std::ceil((std::max)({v0.sx, v1.sx, v2.sx}))});
        int32_t minY = (std::max<int32_t>)({0, scissorY0, (int32_t)std::floor((std::min)({v0.sy, v1.sy, v2.sy}))});
        int32_t maxY = (std::min<int32_t>)({(int32_t)EFB_MAX_H - 1, scissorY1, (int32_t)std::ceil((std::max)({v0.sy, v1.sy, v2.sy}))});
        if (minX > maxX || minY > maxY) return;

        const CachedTexture* stageTex[16] = {};
        for (uint8_t s = 0; s < activeTevStages && s < 16; ++s) {
            if (tevStages[s].texEnable) stageTex[s] = &GetDecodedTexture(tevStages[s].texMap);
        }

        double invAbsArea = 1.0 / std::abs((double)area);
        float sign = (area > 0.0f) ? 1.0f : -1.0f;

        for (int32_t y = minY; y <= maxY; ++y) {
            float py = y + 0.5f;
            for (int32_t x = minX; x <= maxX; ++x) {
                float px = x + 0.5f;
                float w0 = ((v2.sx - v1.sx) * (py - v1.sy) - (v2.sy - v1.sy) * (px - v1.sx)) * sign;
                float w1 = ((v0.sx - v2.sx) * (py - v2.sy) - (v0.sy - v2.sy) * (px - v2.sx)) * sign;
                float w2 = ((v1.sx - v0.sx) * (py - v0.sy) - (v1.sy - v0.sy) * (px - v0.sx)) * sign;
                if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;

                double b0d = (double)w0 * invAbsArea;
                double b1d = (double)w1 * invAbsArea;
                double b2d = std::max(0.0, 1.0 - b0d - b1d);
                float b0 = (float)b0d;
                float b1 = (float)b1d;
                float b2 = (float)b2d;

                float iw0 = b0 * v0.invW, iw1 = b1 * v1.invW, iw2 = b2 * v2.invW;
                float iwSum = iw0 + iw1 + iw2;
                float w = (std::abs(iwSum) > 1e-8f) ? (1.0f / iwSum) : 1.0f;
                float p0 = iw0 * w, p1 = iw1 * w, p2 = iw2 * w;

                float z = std::clamp((float)(b0d * (double)v0.sz + b1d * (double)v1.sz + b2d * (double)v2.sz), 0.0f, 1.0f);
                int32_t ras[4] = {
                    (int32_t)std::clamp((p0 * v0.r + p1 * v1.r + p2 * v2.r) * 255.0f, 0.0f, 255.0f),
                    (int32_t)std::clamp((p0 * v0.g + p1 * v1.g + p2 * v2.g) * 255.0f, 0.0f, 255.0f),
                    (int32_t)std::clamp((p0 * v0.b + p1 * v1.b + p2 * v2.b) * 255.0f, 0.0f, 255.0f),
                    (int32_t)std::clamp((p0 * v0.a + p1 * v1.a + p2 * v2.a) * 255.0f, 0.0f, 255.0f)
                };
                float iu[8], iv[8];
                for (uint8_t t = 0; t < numTexGens && t < 8; ++t) {
                    iu[t] = p0 * v0.u[t] + p1 * v1.u[t] + p2 * v2.u[t];
                    iv[t] = p0 * v0.v[t] + p1 * v1.v[t] + p2 * v2.v[t];
                }
                uint8_t outPx[4];
                EvaluateTEV(ras, iu, iv, stageTex, outPx);
                WritePixelEFB((uint32_t)x, (uint32_t)y, z, outPx);
            }
        }
    }

    void ExecuteEFBCopy(uint32_t val) {
        bool clearAfter = ((val >> 11) & 1) != 0;
        bool copyToXfb  = ((val >> 14) & 1) != 0;

        if (copyToXfb && xfbCopyDstAddr != 0) {
            uint32_t w = std::min<uint32_t>(efbCopySrcW, EFB_MAX_W - std::min(efbCopySrcX, EFB_MAX_W));
            uint32_t h = std::min<uint32_t>(efbCopySrcH, EFB_MAX_H - std::min(efbCopySrcY, EFB_MAX_H));
            uint32_t stride = xfbCopyStrideBytes ? xfbCopyStrideBytes : (w * 2);
            uint8_t* dstBase = GuestPtrMut(xfbCopyDstAddr, (size_t)h * stride);
            if (dstBase && w >= 2) {
                auto ClampByte = [](int v) -> uint8_t { return (uint8_t)std::clamp(v, 0, 255); };
                for (uint32_t y = 0; y < h; ++y) {
                    uint8_t* row = dstBase + (size_t)y * stride;
                    const uint32_t* srcRow = &efbColor[(size_t)(efbCopySrcY + y) * EFB_MAX_W + efbCopySrcX];
                    for (uint32_t x = 0; x + 1 < w; x += 2) {
                        uint32_t p0 = srcRow[x];
                        uint32_t p1 = srcRow[x + 1];
                        int r0 = p0 & 0xFF, g0 = (p0 >> 8) & 0xFF, b0 = (p0 >> 16) & 0xFF;
                        int r1 = p1 & 0xFF, g1 = (p1 >> 8) & 0xFF, b1 = (p1 >> 16) & 0xFF;
                        int rAvg = (r0 + r1) >> 1, gAvg = (g0 + g1) >> 1, bAvg = (b0 + b1) >> 1;

                        uint8_t y0 = ClampByte(16 + ((66 * r0 + 129 * g0 + 25 * b0 + 128) >> 8));
                        uint8_t y1 = ClampByte(16 + ((66 * r1 + 129 * g1 + 25 * b1 + 128) >> 8));
                        uint8_t u  = ClampByte(128 + ((-38 * rAvg - 74 * gAvg + 112 * bAvg + 128) >> 8));
                        uint8_t v  = ClampByte(128 + ((112 * rAvg - 94 * gAvg - 18 * bAvg + 128) >> 8));

                        row[x * 2 + 0] = y0;
                        row[x * 2 + 1] = u;
                        row[x * 2 + 2] = y1;
                        row[x * 2 + 3] = v;
                    }
                }
                if (onXfbCopy) onXfbCopy(xfbCopyDstAddr, "GX_CopyDisp");
            }
        } else if (!copyToXfb && xfbCopyDstAddr != 0) {
            // GX_CopyTex: salva EFB come texture RGBA8 (4x4 tiled) in RAM guest
            uint32_t texFmt = ((val >> 4) & 7) | (((val >> 9) & 1) << 3);
            uint32_t w = std::min<uint32_t>(efbCopySrcW, EFB_MAX_W - std::min(efbCopySrcX, EFB_MAX_W));
            uint32_t h = std::min<uint32_t>(efbCopySrcH, EFB_MAX_H - std::min(efbCopySrcY, EFB_MAX_H));
            if (texFmt == 6 && w > 0 && h > 0) {
                uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
                uint8_t* dst = GuestPtrMut(xfbCopyDstAddr, (size_t)bw * bh * 64);
                if (dst) {
                    for (uint32_t ty = 0; ty < bh; ++ty) for (uint32_t tx = 0; tx < bw; ++tx) {
                        uint8_t* tile = dst + ((ty * bw + tx) * 64);
                        for (uint32_t y = 0; y < 4; ++y) for (uint32_t x = 0; x < 4; ++x) {
                            uint32_t px = tx * 4 + x, py = ty * 4 + y;
                            uint32_t c = (px < w && py < h)
                                ? efbColor[(size_t)(efbCopySrcY + py) * EFB_MAX_W + (efbCopySrcX + px)]
                                : 0;
                            uint32_t idx = (y * 4 + x) * 2;
                            tile[idx]      = (uint8_t)((c >> 24) & 0xFF);
                            tile[idx + 1]  = (uint8_t)(c & 0xFF);
                            tile[32 + idx] = (uint8_t)((c >> 8) & 0xFF);
                            tile[32 + idx + 1] = (uint8_t)((c >> 16) & 0xFF);
                        }
                    }
                }
            }
        }

        if (clearAfter) {
            uint32_t cc = clearColor[0] | (clearColor[1] << 8) | (clearColor[2] << 16) | ((uint32_t)clearColor[3] << 24);
            std::fill(efbColor.begin(), efbColor.end(), cc);
            std::fill(efbDepth.begin(), efbDepth.end(), (float)clearZ / 16777215.0f);
        }
        texCache.clear();
    }
};

} // namespace WiiPort
