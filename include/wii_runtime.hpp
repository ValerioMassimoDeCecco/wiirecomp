#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include "wii_types.hpp"
#include "gx_fifo.hpp"
#include "ios_ipc.hpp"

namespace WiiPort {

constexpr size_t MEM1_SIZE = 24 * 1024 * 1024; // 24 MB
constexpr size_t MEM2_SIZE = 64 * 1024 * 1024; // 64 MB

class NativeWiiRuntime {
public:
    uint8_t* mem1;
    uint8_t* mem2;
    void (*dispatchTable[MEM1_SIZE / 4])(PPCContext*);

    GXCommandProcessor gx;
    IOSIPCServer ios;

    // Registri Video Interface (VI)
    uint32_t currentFramebufferAddress = 0;
    bool frameReady = false;

    NativeWiiRuntime() {
        mem1 = (uint8_t*)malloc(MEM1_SIZE);
        mem2 = (uint8_t*)malloc(MEM2_SIZE);
        std::memset(mem1, 0, MEM1_SIZE);
        std::memset(mem2, 0, MEM2_SIZE);
        std::memset(dispatchTable, 0, sizeof(dispatchTable));
    }

    ~NativeWiiRuntime() {
        free(mem1);
        free(mem2);
    }

    void RegisterFunction(uint32_t ppcAddr, void (*nativeFunc)(PPCContext*)) {
        if ((ppcAddr & 0xFE000000) == 0x80000000) {
            uint32_t idx = (ppcAddr & 0x017FFFFF) >> 2;
            dispatchTable[idx] = nativeFunc;
        }
    }

    inline void DispatchIndirectBranch(PPCContext* ctx, uint32_t targetAddress) {
        if ((targetAddress & 0xFE000000) == 0x80000000) {
            uint32_t idx = (targetAddress & 0x017FFFFF) >> 2;
            auto func = dispatchTable[idx];
            if (func) {
                func(ctx);
                return;
            }
        }
        std::printf("[RUNTIME] Errore critico: Salto indiretto verso indirizzo non registrato: 0x%08X\n", targetAddress);
    }

    // ================= MEMORY ACCESS PRIMITIVES =================
    inline uint32_t Read32(uint32_t addr) {
        if ((addr & 0xFF000000) == 0xCC000000 || (addr & 0xFF000000) == 0xCD000000) {
            return HandleMMIO_Read(addr);
        }
        if ((addr & 0x7E000000) == 0x00000000 || (addr & 0x7E000000) == 0x80000000) {
            uint32_t offset = addr & (MEM1_SIZE - 1);
            return WII_BYTESWAP32(*reinterpret_cast<uint32_t*>(mem1 + offset));
        }
        if ((addr & 0x70000000) == 0x10000000 || (addr & 0x70000000) == 0x90000000) {
            uint32_t offset = (addr - 0x10000000) & (MEM2_SIZE - 1);
            return WII_BYTESWAP32(*reinterpret_cast<uint32_t*>(mem2 + offset));
        }
        return 0;
    }

    inline void Write32(uint32_t addr, uint32_t val) {
        if ((addr & 0xFF000000) == 0xCC000000 || (addr & 0xFF000000) == 0xCD000000) {
            HandleMMIO_Write(addr, val);
            return;
        }
        if ((addr & 0x7E000000) == 0x00000000 || (addr & 0x7E000000) == 0x80000000) {
            uint32_t offset = addr & (MEM1_SIZE - 1);
            *reinterpret_cast<uint32_t*>(mem1 + offset) = WII_BYTESWAP32(val);
            return;
        }
        if ((addr & 0x70000000) == 0x10000000 || (addr & 0x70000000) == 0x90000000) {
            uint32_t offset = (addr - 0x10000000) & (MEM2_SIZE - 1);
            *reinterpret_cast<uint32_t*>(mem2 + offset) = WII_BYTESWAP32(val);
            return;
        }
    }

    inline uint8_t Read8(uint32_t addr) {
        if ((addr & 0x7E000000) == 0x00000000 || (addr & 0x7E000000) == 0x80000000) {
            return mem1[addr & (MEM1_SIZE - 1)];
        }
        return 0;
    }

    inline void Write8(uint32_t addr, uint8_t val) {
        if (addr == 0xCC008000) {
            gx.WriteByte(val);
            return;
        }
        if ((addr & 0x7E000000) == 0x00000000 || (addr & 0x7E000000) == 0x80000000) {
            mem1[addr & (MEM1_SIZE - 1)] = val;
        }
    }

    uint32_t HandleMMIO_Read(uint32_t addr) {
        // CP Status Register: FIFO sempre pronto per ricevere vertici
        if (addr == 0xCC000000) return 0x00000001;
        // VI Vertical Beam Position (emulazione 60Hz per frame sync)
        if (addr == 0xCC00202C) return 0x00000100;
        return 0;
    }

    void HandleMMIO_Write(uint32_t addr, uint32_t val) {
        // 1. WGPIPE: invio diretto di dati grafici al command processor
        if (addr == 0xCC008000) {
            gx.WriteWord(val);
            return;
        }

        // 2. Hollywood Mailbox IPC (Starlet/IOS): invio richiesta
        if (addr == 0xCD800000) {
            ios.ProcessRequest(mem1, val);
            return;
        }

        // 3. VI Top Framebuffer Address: cambio buffer a video (Swap Chain Present)
        if (addr == 0xCC00201C) {
            currentFramebufferAddress = val;
            frameReady = true;
            return;
        }
    }
};

extern NativeWiiRuntime g_Memory;

// Helper globale per dequantizzare psq_l
inline void DequantizeLoad(PPCContext* ctx, uint32_t fD, uint32_t addr, bool w, uint32_t gqrIdx) {
    const auto& gqr = ctx->gqr[gqrIdx];
    float scale = (float)(1 << gqr.l_scale);

    auto ReadComponent = [&](uint32_t a) -> float {
        switch (gqr.l_type) {
            case 0: { // float32 standard
                uint32_t u = g_Memory.Read32(a);
                float f;
                std::memcpy(&f, &u, 4);
                return f;
            }
            case 4: return (float)g_Memory.Read8(a) / scale;            // u8
            case 5: return (float)(uint16_t)g_Memory.Read32(a) / scale; // u16
            case 6: return (float)(int8_t)g_Memory.Read8(a) / scale;    // s8
            case 7: return (float)(int16_t)g_Memory.Read32(a) / scale;   // s16
            default: return 0.0f;
        }
    };

    ctx->f[fD].ps[0] = ReadComponent(addr);
    if (!w) {
        uint32_t stride = (gqr.l_type == 0 || gqr.l_type == 5 || gqr.l_type == 7) ? 4 : 1;
        ctx->f[fD].ps[1] = ReadComponent(addr + stride);
    } else {
        ctx->f[fD].ps[1] = 1.0f; // W=1 imposta automaticamente ps1 a 1.0f
    }
}

// Helper globale per quantizzare psq_st
inline void QuantizeStore(PPCContext* ctx, uint32_t fS, uint32_t addr, bool w, uint32_t gqrIdx) {
    if (gqrIdx == 0) {
        float f0 = ctx->f[fS].ps[0];
        uint32_t u0;
        std::memcpy(&u0, &f0, 4);
        g_Memory.Write32(addr, u0);
        if (!w) {
            float f1 = ctx->f[fS].ps[1];
            uint32_t u1;
            std::memcpy(&u1, &f1, 4);
            g_Memory.Write32(addr + 4, u1);
        }
    }
}

inline bool EvaluateCondition(PPCContext* ctx, uint32_t bo, uint32_t bi) {
    bool ctrOk = true;
    if (!(bo & 4)) {
        ctx->ctr--;
        ctrOk = (bo & 2) ? (ctx->ctr == 0) : (ctx->ctr != 0);
    }
    bool condOk = true;
    if (!(bo & 16)) {
        bool bit = (ctx->cr >> (31 - bi)) & 1;
        condOk = (bo & 8) ? bit : !bit;
    }
    return ctrOk && condOk;
}

inline void UpdateCR0(PPCContext* ctx, uint32_t res) {
    uint32_t cr0 = 0;
    if ((int32_t)res < 0) cr0 |= 0x8;      // LT
    else if ((int32_t)res > 0) cr0 |= 0x4; // GT
    else cr0 |= 0x2;                       // EQ
    if (ctx->xer & 0x80000000) cr0 |= 0x1; // SO
    ctx->SetCRField(0, cr0);
}

} // namespace WiiPort
