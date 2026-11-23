#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <iostream>
#include <mutex>
#include "wii_types.hpp"
#include "gx_fifo.hpp"
#include "ios_ipc.hpp"
#include "dol_loader.hpp"
#include "ppc_bitops.hpp"

namespace WiiPort {

constexpr size_t MEM1_SIZE = 128 * 1024 * 1024; // 128 MB (espanso per grandi homebrew e buffer XFB)
constexpr size_t MEM2_SIZE = 64 * 1024 * 1024; // 64 MB

class NativeWiiRuntime {
public:
    uint8_t* mem1;
    uint8_t* mem2;
    void (**dispatchTable)(PPCContext*);

    GXCommandProcessor gx;
    IOSIPCServer ios;

    uint32_t currentFramebufferAddress = 0;
    bool frameReady = false;
    uint32_t entryPoint = 0x80004000;
    uint32_t beamPos = 0x100;
    uint16_t dspCsr = 0x0020;
    bool dspDmaDone = false;
    uint32_t mmioStore[0x8000] = {};

    NativeWiiRuntime() {
        mem1 = (uint8_t*)malloc(MEM1_SIZE);
        mem2 = (uint8_t*)malloc(MEM2_SIZE);
        dispatchTable = (void(**)(PPCContext*))calloc(MEM1_SIZE / 4, sizeof(void*));
        std::memset(mem1, 0, MEM1_SIZE);
        std::memset(mem2, 0, MEM2_SIZE);
        std::memset(mmioStore, 0, sizeof(mmioStore));
    }

    ~NativeWiiRuntime() {
        free(mem1);
        free(mem2);
        free(dispatchTable);
    }

    void RegisterFunction(uint32_t ppcAddr, void (*nativeFunc)(PPCContext*)) {
        uint8_t top = (ppcAddr >> 24);
        if (top == 0x80 || top == 0x00 || top == 0xC0) {
            uint32_t idx = (ppcAddr & (MEM1_SIZE - 1)) >> 2;
            if (idx < (MEM1_SIZE / 4)) {
                dispatchTable[idx] = nativeFunc;
            }
        }
    }

    bool LoadDol(const std::string& filePath) {
        DolExecutable dol;
        std::vector<std::string> searchPaths = { filePath, "boot.dol", "./boot.dol", "../boot.dol", "../../boot.dol", "C:/Users/Valerio/Desktop/wiirecomp/boot.dol" };
        bool loaded = false;
        for (const auto& p : searchPaths) {
            if (dol.Load(p)) {
                loaded = true;
                break;
            }
        }
        if (!loaded) {
            Log(std::string("[RUNTIME] Impossibile caricare il file DOL: ") + filePath);
            return false;
        }

        char buf[256];
        snprintf(buf, sizeof(buf), "[RUNTIME] DOL caricato da disco, allocazione memoria e copia sezioni... mem1=%p", (void*)mem1);
        Log(buf);
        std::memset(mem1, 0, MEM1_SIZE);
        std::memset(mem2, 0, MEM2_SIZE);

        for (const auto& sec : dol.sections) {
            snprintf(buf, sizeof(buf), "[RUNTIME] Copia sezione @ 0x%08X (size: %u)", sec.address, sec.size);
            Log(buf);
            uint8_t top = (sec.address >> 24);
            if (top == 0x00 || top == 0x80 || top == 0xC0) {
                uint32_t offset = sec.address & (MEM1_SIZE - 1);
                if (offset + sec.size <= MEM1_SIZE) {
                    std::memcpy(mem1 + offset, sec.data.data(), sec.size);
                }
            } else if (((top & 0xFC) == 0x90) || ((top & 0xFC) == 0xD0) || ((top & 0xFC) == 0x10)) {
                uint32_t offset = sec.address & (MEM2_SIZE - 1);
                if (offset + sec.size <= MEM2_SIZE) {
                    std::memcpy(mem2 + offset, sec.data.data(), sec.size);
                }
            }
        }

        if (dol.bssAddress && dol.bssSize) {
            uint8_t top = (dol.bssAddress >> 24);
            if (top == 0x00 || top == 0x80 || top == 0xC0) {
                uint32_t offset = dol.bssAddress & (MEM1_SIZE - 1);
                if (offset + dol.bssSize <= MEM1_SIZE) {
                    std::memset(mem1 + offset, 0, dol.bssSize);
                }
            }
        }

        // Setup Low Memory OS Globals per compatibilità Broadway / libogc
        Write32(0x80000028, (uint32_t)MEM1_SIZE); // 128 MB RAM fisica
        Write32(0x8000002C, 0x00000002);          // Wii retail
        uint32_t arenaLo = dol.bssAddress + dol.bssSize;
        if (arenaLo == 0) arenaLo = 0x80130000;
        Write32(0x80000034, arenaLo);
        Write32(0x80000038, 0x80000000 + (uint32_t)MEM1_SIZE - 0x1000); // 128 MB ArenaHi
        Write32(0x800000CC, 0);                   // NTSC
        Write32(0x800000E4, 0);
        Write32(0x800000F0, (uint32_t)MEM1_SIZE); // SimulatedMemSize
        Write32(0x800000F8, 0x0E7BE2C0);          // 243 MHz
        Write32(0x800000FC, 0x2B73A840);          // 729 MHz

        entryPoint = dol.entryPoint;
        snprintf(buf, sizeof(buf), "[RUNTIME] DOL caricato in memoria nativa con successo! EntryPoint: 0x%08X", entryPoint);
        Log(buf);
        return true;
    }

    // ================= MEMORY ACCESS PRIMITIVES =================
    inline uint32_t Read32(uint32_t addr) {
        if ((addr & 0xFF000000) == 0xCC000000 || (addr & 0xFF000000) == 0xCD000000) {
            return HandleMMIO_Read(addr);
        }
        uint8_t top = (addr >> 24);
        if (top == 0x00 || top == 0x80 || top == 0xC0) {
            uint32_t offset = addr & (MEM1_SIZE - 1);
            return WII_BYTESWAP32(*reinterpret_cast<uint32_t*>(mem1 + offset));
        }
        if (((top & 0xFC) == 0x90) || ((top & 0xFC) == 0xD0) || ((top & 0xFC) == 0x10)) {
            uint32_t offset = addr & (MEM2_SIZE - 1);
            return WII_BYTESWAP32(*reinterpret_cast<uint32_t*>(mem2 + offset));
        }
        return 0;
    }

    inline void Write32(uint32_t addr, uint32_t val) {
        if ((addr & 0xFF000000) == 0xCC000000 || (addr & 0xFF000000) == 0xCD000000) {
            HandleMMIO_Write(addr, val);
            return;
        }
        uint8_t top = (addr >> 24);
        if (top == 0x00 || top == 0x80 || top == 0xC0) {
            uint32_t offset = addr & (MEM1_SIZE - 1);
            *reinterpret_cast<uint32_t*>(mem1 + offset) = WII_BYTESWAP32(val);
            return;
        }
        if (((top & 0xFC) == 0x90) || ((top & 0xFC) == 0xD0) || ((top & 0xFC) == 0x10)) {
            uint32_t offset = addr & (MEM2_SIZE - 1);
            *reinterpret_cast<uint32_t*>(mem2 + offset) = WII_BYTESWAP32(val);
            return;
        }
    }

    inline uint16_t Read16(uint32_t addr) {
        if ((addr & 0xFF000000) == 0xCC000000 || (addr & 0xFF000000) == 0xCD000000) {
            return (uint16_t)HandleMMIO_Read(addr);
        }
        uint8_t top = (addr >> 24);
        if (top == 0x00 || top == 0x80 || top == 0xC0) {
            uint32_t offset = addr & (MEM1_SIZE - 1);
            return WII_BYTESWAP16(*reinterpret_cast<uint16_t*>(mem1 + offset));
        }
        if (((top & 0xFC) == 0x90) || ((top & 0xFC) == 0xD0) || ((top & 0xFC) == 0x10)) {
            uint32_t offset = addr & (MEM2_SIZE - 1);
            return WII_BYTESWAP16(*reinterpret_cast<uint16_t*>(mem2 + offset));
        }
        return 0;
    }

    inline void Write16(uint32_t addr, uint16_t val) {
        if (addr == 0xCC008000) {
            gx.WriteWord((uint32_t)val << 16);
            return;
        }
        if ((addr & 0xFF000000) == 0xCC000000 || (addr & 0xFF000000) == 0xCD000000) {
            HandleMMIO_Write(addr, val);
            return;
        }
        uint8_t top = (addr >> 24);
        if (top == 0x00 || top == 0x80 || top == 0xC0) {
            uint32_t offset = addr & (MEM1_SIZE - 1);
            *reinterpret_cast<uint16_t*>(mem1 + offset) = WII_BYTESWAP16(val);
            return;
        }
        if (((top & 0xFC) == 0x90) || ((top & 0xFC) == 0xD0) || ((top & 0xFC) == 0x10)) {
            uint32_t offset = addr & (MEM2_SIZE - 1);
            *reinterpret_cast<uint16_t*>(mem2 + offset) = WII_BYTESWAP16(val);
            return;
        }
    }

    inline uint8_t Read8(uint32_t addr) {
        if ((addr & 0xFF000000) == 0xCC000000 || (addr & 0xFF000000) == 0xCD000000) {
            return (uint8_t)HandleMMIO_Read(addr);
        }
        uint8_t top = (addr >> 24);
        if (top == 0x00 || top == 0x80 || top == 0xC0) {
            return mem1[addr & (MEM1_SIZE - 1)];
        }
        if (((top & 0xFC) == 0x90) || ((top & 0xFC) == 0xD0) || ((top & 0xFC) == 0x10)) {
            return mem2[addr & (MEM2_SIZE - 1)];
        }
        return 0;
    }

    inline void Write8(uint32_t addr, uint8_t val) {
        if (addr == 0xCC008000) {
            gx.WriteByte(val);
            return;
        }
        if ((addr & 0xFF000000) == 0xCC000000 || (addr & 0xFF000000) == 0xCD000000) {
            HandleMMIO_Write(addr, val);
            return;
        }
        uint8_t top = (addr >> 24);
        if (top == 0x00 || top == 0x80 || top == 0xC0) {
            mem1[addr & (MEM1_SIZE - 1)] = val;
            return;
        }
        if (((top & 0xFC) == 0x90) || ((top & 0xFC) == 0xD0) || ((top & 0xFC) == 0x10)) {
            mem2[addr & (MEM2_SIZE - 1)] = val;
            return;
        }
    }

    uint32_t HandleMMIO_Read(uint32_t addr) {
        // CP Status Register: FIFO sempre pronto per ricevere vertici
        if (addr == 0xCC000000 || addr == 0xCC000002) return 0x00000001;
        // VI Vertical Beam Position (emulazione 60Hz per frame sync)
        if (addr == 0xCC00202C) {
            beamPos = (beamPos + 1) % 525;
            return beamPos;
        }
        if (addr == 0xCC002002) return 0x00000001;
        // DSP Control/Status Register: segnala DSP pronto (Reset bit 0 = 0, Halt bit 10 = 0, ARAM DMA bit 5 = 1)
        if (addr == 0xCC00500A) return (dspCsr & ~0x0401) | 0x0020;
        // DSP Mailbox: prima del DMA ritorna 0 (& 0x8000 == 0), dopo il DMA ritorna 0x8000 (signed < 0) con auto-clear
        if (addr == 0xCC005004) {
            if (dspDmaDone) {
                dspDmaDone = false; // Auto-clear: messaggio consumato dal CPU
                return 0x8000;
            }
            return 0x0000;
        }
        if (addr == 0xCC005006) {
            dspDmaDone = false;
            return 0;
        }
        if (addr == 0xCC005012 || addr == 0xCC005020) return 0;
        if (addr == 0xCD8000C8 || addr == 0xCD8000C4 || addr == 0xCD8000C0) return 0x00008000; // VIP I2C ready/ACK
        if (addr == 0xCC006800 || (addr & 0xFFFFFF00) == 0xCD006800) return 0x00000000; // EXI channel ready
        if (addr == 0xCD800004 || addr == 0xCD000004 || addr == 0xCD006404) return 0x00000030; // IPC PPCCTRL / HW Revision
        if (addr == 0xCD80000C || addr == 0xCD00640C) return 0x00000030; // IPC ARMCTRL
        if (addr == 0xCD006434 || addr == 0xCC006434) return mmioStore[(addr & 0xFFFF) >> 1] & ~0x00000001; // DI command not busy (bit 0 = 0)
        if ((addr & 0xFFFFFF00) == 0xCD006400) {
            return mmioStore[(addr & 0xFFFF) >> 1] & ~0x00000001;
        }
        return mmioStore[(addr & 0xFFFF) >> 1];
    }

    void HandleMMIO_Write(uint32_t addr, uint32_t val) {
        if (addr == 0xCC00500A) {
            dspCsr = (uint16_t)(val & ~0x0401);
            return;
        }
        if (addr == 0xCC005028) {
            dspDmaDone = true; // Segnala completamento DMA ARAM
            return;
        }
        if (addr == 0xCC005004 || addr == 0xCC005000 || addr == 0xCC005002) {
            dspDmaDone = false;
            return;
        }
        // 1. WGPIPE: invio diretto di dati grafici al command processor
        if (addr == 0xCC008000 || addr == 0xCD008000) {
            gx.WriteWord(val);
            return;
        }

        // 2. Hollywood Mailbox IPC (Starlet/IOS): invio richiesta
        if (addr == 0xCD800000 || addr == 0xCD008000) {
            ios.ProcessRequest(mem1, val);
            return;
        }

        // 3. VI Top / Bottom Framebuffer Address (TFBL / BFBL)
        if (addr == 0xCC00201C || addr == 0xCC00201E || addr == 0xCC002024 || addr == 0xCC002026) {
            if (addr == 0xCC00201C && val > 0xFFFF) {
                mmioStore[0x201C >> 1] = (uint16_t)(val >> 16);
                mmioStore[0x201E >> 1] = (uint16_t)(val & 0xFFFF);
            } else {
                mmioStore[(addr & 0xFFFF) >> 1] = (uint16_t)val;
            }
            // Decodifica l'indirizzo solo quando la parte bassa è scritta o per scritture a 32 bit
            if (addr == 0xCC00201E || (addr == 0xCC00201C && val > 0xFFFF)) {
                if (currentFramebufferAddress == 0) {
                    uint32_t hi = mmioStore[0x201C >> 1];
                    uint32_t lo = mmioStore[0x201E >> 1];
                    uint32_t wordAddr = ((hi & 0x00FF) << 16) | lo;
                    uint32_t decoded = wordAddr << 5;
                    // Su Wii reale MEM1 è 24 MB (0x01800000)
                    if (decoded < 0x01800000 && decoded >= 0x10000) {
                        currentFramebufferAddress = decoded;
                        frameReady = true;
                        Log("[VI] Framebuffer address registrato da MMIO a 0x" + HexStr(currentFramebufferAddress));
                    }
                }
            }
            return;
        }

        // 4. DI Command Register: azzera immediatamente bit 0 (comando completato subito)
        if (addr == 0xCD006434 || addr == 0xCC006434) {
            mmioStore[(addr & 0xFFFF) >> 1] = val & ~0x00000001;
            return;
        }

        mmioStore[(addr & 0xFFFF) >> 1] = val;
    }

    // ================= ESECUZIONE NATIVA & FALLBACK CPU =================
    bool Step(PPCContext* ctx);

    void Execute(PPCContext* ctx, uint32_t startAddr, uint64_t maxInstructions = 0) {
        ctx->pc = startAddr;
        uint64_t count = 0;
        while (ctx->pc != 0) {
            if (!Step(ctx)) break;
            count++;
            if (maxInstructions && count >= maxInstructions) break;
        }
    }

    inline void DispatchIndirectBranch(PPCContext* ctx, uint32_t targetAddress) {
        uint8_t top = (targetAddress >> 24);
        if (top == 0x80 || top == 0x00 || top == 0xC0) {
            uint32_t idx = (targetAddress & (MEM1_SIZE - 1)) >> 2;
            if (idx < (MEM1_SIZE / 4)) {
                auto func = dispatchTable[idx];
                if (func) {
                    func(ctx);
                    return;
                }
            }
        }
        ctx->pc = targetAddress;
        uint32_t stopLr = ctx->lr;
        uint64_t maxSteps = 10000000;
        while (ctx->pc != 0 && maxSteps-- > 0) {
            uint8_t curTop = (ctx->pc >> 24);
            if (curTop == 0x80 || curTop == 0x00 || curTop == 0xC0) {
                uint32_t idx = (ctx->pc & (MEM1_SIZE - 1)) >> 2;
                if (idx < (MEM1_SIZE / 4)) {
                    auto func = dispatchTable[idx];
                    if (func) {
                        func(ctx);
                        return;
                    }
                }
            }
            if (!Step(ctx)) break;
            if (ctx->pc == stopLr) break;
        }
    }
};

extern NativeWiiRuntime g_Memory;

// Helper CR
inline void UpdateCRField(PPCContext* ctx, uint32_t fieldIndex, int32_t val1, int32_t val2) {
    uint32_t c = 0;
    if (val1 < val2) c |= 8;
    else if (val1 > val2) c |= 4;
    else c |= 2;
    if (ctx->xer & 0x80000000) c |= 1;
    ctx->SetCRField(fieldIndex, c);
}

inline void UpdateCRFieldLogical(PPCContext* ctx, uint32_t fieldIndex, uint32_t val1, uint32_t val2) {
    uint32_t c = 0;
    if (val1 < val2) c |= 8;
    else if (val1 > val2) c |= 4;
    else c |= 2;
    if (ctx->xer & 0x80000000) c |= 1;
    ctx->SetCRField(fieldIndex, c);
}

inline void UpdateCR0(PPCContext* ctx, uint32_t res) {
    uint32_t cr0 = 0;
    if ((int32_t)res < 0) cr0 |= 0x8;
    else if ((int32_t)res > 0) cr0 |= 0x4;
    else cr0 |= 0x2;
    if (ctx->xer & 0x80000000) cr0 |= 0x1;
    ctx->SetCRField(0, cr0);
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

// Helper globale per dequantizzare psq_l
inline void DequantizeLoad(PPCContext* ctx, uint32_t fD, uint32_t addr, bool w, uint32_t gqrIdx) {
    const auto& gqr = ctx->gqr[gqrIdx];
    float scale = (float)(1 << gqr.l_scale);

    auto ReadComponent = [&](uint32_t a) -> float {
        switch (gqr.l_type) {
            case 0: {
                uint32_t u = g_Memory.Read32(a);
                float f;
                std::memcpy(&f, &u, 4);
                return f;
            }
            case 4: return (float)g_Memory.Read8(a) / scale;
            case 5: return (float)g_Memory.Read16(a) / scale;
            case 6: return (float)(int8_t)g_Memory.Read8(a) / scale;
            case 7: return (float)(int16_t)g_Memory.Read16(a) / scale;
            default: return 0.0f;
        }
    };

    ctx->f[fD].ps[0] = ReadComponent(addr);
    if (!w) {
        uint32_t stride = (gqr.l_type == 0 || gqr.l_type == 5 || gqr.l_type == 7) ? 4 : 1;
        ctx->f[fD].ps[1] = ReadComponent(addr + stride);
    } else {
        ctx->f[fD].ps[1] = 1.0f;
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

inline bool NativeWiiRuntime::Step(PPCContext* ctx) {
    uint8_t top = (ctx->pc >> 24);
    if (top == 0x80 || top == 0x00 || top == 0xC0) {
        uint32_t idx = (ctx->pc & (MEM1_SIZE - 1)) >> 2;
        if (idx < (MEM1_SIZE / 4)) {
            auto func = dispatchTable[idx];
            if (func) {
                func(ctx);
                ctx->pc = ctx->lr;
                return true;
            }
        }
    }

    uint32_t instr = Read32(ctx->pc);
    if (instr == 0) {
        ctx->pc += 4;
        return true;
    }

    uint32_t op = (instr >> 26) & 0x3F;
    uint32_t rD = (instr >> 21) & 0x1F;
    uint32_t rA = (instr >> 16) & 0x1F;
    uint32_t rB = (instr >> 11) & 0x1F;
    int16_t simm = (int16_t)(instr & 0xFFFF);
    uint16_t uimm = (uint16_t)(instr & 0xFFFF);
    uint32_t xo10 = (instr >> 1) & 0x3FF;
    bool rc = (instr & 1);

    uint32_t next_pc = ctx->pc + 4;

    switch (op) {
        case 14: // addi / li
            ctx->r[rD] = (rA == 0 ? 0 : ctx->r[rA]) + (int32_t)simm;
            break;
        case 15: // addis / lis
            ctx->r[rD] = (rA == 0 ? 0 : ctx->r[rA]) + ((uint32_t)uimm << 16);
            break;
        case 18: { // b, bl, ba, bla
            int32_t li = (instr & 0x03FFFFFC);
            if (li & 0x02000000) li |= 0xFC000000;
            uint32_t target = (instr & 2) ? li : (ctx->pc + li);
            if (instr & 1) ctx->lr = ctx->pc + 4;
            next_pc = target;
            break;
        }
        case 16: { // bc
            int32_t bd = (instr & 0xFFFC);
            if (bd & 0x8000) bd |= 0xFFFF0000;
            uint32_t target = (instr & 2) ? bd : (ctx->pc + bd);
            uint32_t bo = (instr >> 21) & 0x1F;
            uint32_t bi = (instr >> 16) & 0x1F;
            if (EvaluateCondition(ctx, bo, bi)) {
                if (instr & 1) ctx->lr = ctx->pc + 4;
                next_pc = target;
            }
            break;
        }
        case 19: {
            if (xo10 == 16) { // blr / bclr
                uint32_t bo = (instr >> 21) & 0x1F;
                uint32_t bi = (instr >> 16) & 0x1F;
                if (EvaluateCondition(ctx, bo, bi)) {
                    uint32_t target = ctx->lr;
                    if (instr & 1) ctx->lr = ctx->pc + 4;
                    next_pc = target;
                }
            } else if (xo10 == 528) { // bctr / bctrl
                uint32_t bo = (instr >> 21) & 0x1F;
                uint32_t bi = (instr >> 16) & 0x1F;
                if (EvaluateCondition(ctx, bo, bi)) {
                    if (instr & 1) ctx->lr = ctx->pc + 4;
                    next_pc = ctx->ctr;
                }
            } else if (xo10 == 193) { // crxor
                uint32_t crD = (instr >> 21) & 0x1F;
                uint32_t crA = (instr >> 16) & 0x1F;
                uint32_t crB = (instr >> 11) & 0x1F;
                uint32_t bA = (ctx->cr >> (31 - crA)) & 1;
                uint32_t bB = (ctx->cr >> (31 - crB)) & 1;
                uint32_t res = bA ^ bB;
                ctx->cr = (ctx->cr & ~(1u << (31 - crD))) | (res << (31 - crD));
            }
            break;
        }
        case 21: { // rlwinm
            uint32_t sh = (instr >> 11) & 0x1F;
            uint32_t mb = (instr >> 6) & 0x1F;
            uint32_t me = (instr >> 1) & 0x1F;
            ctx->r[rA] = PPC_rlwinm(ctx->r[rD], sh, mb, me);
            if (rc) UpdateCR0(ctx, ctx->r[rA]);
            break;
        }
        case 20: { // rlwimi
            uint32_t sh = (instr >> 11) & 0x1F;
            uint32_t mb = (instr >> 6) & 0x1F;
            uint32_t me = (instr >> 1) & 0x1F;
            ctx->r[rA] = PPC_rlwimi(ctx->r[rA], ctx->r[rD], sh, mb, me);
            if (rc) UpdateCR0(ctx, ctx->r[rA]);
            break;
        }
        case 23: { // rlwnm
            uint32_t mb = (instr >> 6) & 0x1F;
            uint32_t me = (instr >> 1) & 0x1F;
            ctx->r[rA] = PPC_rlwinm(ctx->r[rD], ctx->r[rB] & 31, mb, me);
            if (rc) UpdateCR0(ctx, ctx->r[rA]);
            break;
        }
        case 24: // ori
            ctx->r[rA] = ctx->r[rD] | uimm;
            break;
        case 25: // oris
            ctx->r[rA] = ctx->r[rD] | ((uint32_t)uimm << 16);
            break;
        case 26: // xori
            ctx->r[rA] = ctx->r[rD] ^ uimm;
            break;
        case 27: // xoris
            ctx->r[rA] = ctx->r[rD] ^ ((uint32_t)uimm << 16);
            break;
        case 28: // andi.
            ctx->r[rA] = ctx->r[rD] & uimm;
            UpdateCR0(ctx, ctx->r[rA]);
            break;
        case 29: // andis.
            ctx->r[rA] = ctx->r[rD] & ((uint32_t)uimm << 16);
            UpdateCR0(ctx, ctx->r[rA]);
            break;
        case 10: { // cmpli
            uint32_t crfD = (instr >> 23) & 7;
            UpdateCRFieldLogical(ctx, crfD, ctx->r[rA], uimm);
            break;
        }
        case 11: { // cmpi
            uint32_t crfD = (instr >> 23) & 7;
            UpdateCRField(ctx, crfD, (int32_t)ctx->r[rA], (int32_t)simm);
            break;
        }
        case 7: // mulli
            ctx->r[rD] = (uint32_t)((int32_t)ctx->r[rA] * (int32_t)simm);
            break;
        case 8: // subfic
            ctx->r[rD] = (uint32_t)simm - ctx->r[rA];
            break;
        case 32: { // lwz
            uint32_t ea = (rA == 0 ? 0 : ctx->r[rA]) + (int32_t)simm;
            ctx->r[rD] = Read32(ea);
            break;
        }
        case 33: { // lwzu
            uint32_t ea = ctx->r[rA] + (int32_t)simm;
            ctx->r[rD] = Read32(ea);
            ctx->r[rA] = ea;
            break;
        }
        case 36: { // stw
            uint32_t ea = (rA == 0 ? 0 : ctx->r[rA]) + (int32_t)simm;
            Write32(ea, ctx->r[rD]);
            break;
        }
        case 37: { // stwu
            uint32_t val = ctx->r[rD];
            ctx->r[rA] += (int32_t)simm;
            Write32(ctx->r[rA], val);
            break;
        }
        case 34: { // lbz
            uint32_t ea = (rA == 0 ? 0 : ctx->r[rA]) + (int32_t)simm;
            ctx->r[rD] = Read8(ea);
            break;
        }
        case 35: { // lbzu
            uint32_t ea = ctx->r[rA] + (int32_t)simm;
            ctx->r[rD] = Read8(ea);
            ctx->r[rA] = ea;
            break;
        }
        case 38: { // stb
            uint32_t ea = (rA == 0 ? 0 : ctx->r[rA]) + (int32_t)simm;
            Write8(ea, (uint8_t)ctx->r[rD]);
            break;
        }
        case 39: { // stbu
            uint32_t ea = ctx->r[rA] + (int32_t)simm;
            Write8(ea, (uint8_t)ctx->r[rD]);
            ctx->r[rA] = ea;
            break;
        }
        case 40: { // lhz
            uint32_t ea = (rA == 0 ? 0 : ctx->r[rA]) + (int32_t)simm;
            ctx->r[rD] = Read16(ea);
            break;
        }
        case 41: { // lhzu
            uint32_t ea = ctx->r[rA] + (int32_t)simm;
            ctx->r[rD] = Read16(ea);
            ctx->r[rA] = ea;
            break;
        }
        case 42: { // lha
            uint32_t ea = (rA == 0 ? 0 : ctx->r[rA]) + (int32_t)simm;
            ctx->r[rD] = (uint32_t)(int32_t)(int16_t)Read16(ea);
            break;
        }
        case 44: { // sth
            uint32_t ea = (rA == 0 ? 0 : ctx->r[rA]) + (int32_t)simm;
            Write16(ea, (uint16_t)ctx->r[rD]);
            break;
        }
        case 45: { // sthu
            uint32_t ea = ctx->r[rA] + (int32_t)simm;
            Write16(ea, (uint16_t)ctx->r[rD]);
            ctx->r[rA] = ea;
            break;
        }
        case 46: { // lmw
            uint32_t ea = (rA == 0 ? 0 : ctx->r[rA]) + (int32_t)simm;
            for (uint32_t reg = rD; reg < 32; ++reg) {
                ctx->r[reg] = Read32(ea);
                ea += 4;
            }
            break;
        }
        case 47: { // stmw
            uint32_t ea = (rA == 0 ? 0 : ctx->r[rA]) + (int32_t)simm;
            for (uint32_t reg = rD; reg < 32; ++reg) {
                Write32(ea, ctx->r[reg]);
                ea += 4;
            }
            break;
        }
        case 48: { // lfs
            uint32_t ea = (rA == 0 ? 0 : ctx->r[rA]) + (int32_t)simm;
            uint32_t u = Read32(ea);
            float f;
            std::memcpy(&f, &u, 4);
            ctx->f[rD].ps[0] = f;
            ctx->f[rD].ps[1] = f;
            break;
        }
        case 50: { // stfs
            uint32_t ea = (rA == 0 ? 0 : ctx->r[rA]) + (int32_t)simm;
            float f = ctx->f[rD].ps[0];
            uint32_t u;
            std::memcpy(&u, &f, 4);
            Write32(ea, u);
            break;
        }
        case 54: { // lfd
            uint32_t ea = (rA == 0 ? 0 : ctx->r[rA]) + (int32_t)simm;
            uint64_t hi = Read32(ea);
            uint64_t lo = Read32(ea + 4);
            ctx->f[rD].u64 = (hi << 32) | lo;
            break;
        }
        case 52: { // stfd
            uint32_t ea = (rA == 0 ? 0 : ctx->r[rA]) + (int32_t)simm;
            Write32(ea, (uint32_t)(ctx->f[rD].u64 >> 32));
            Write32(ea + 4, (uint32_t)ctx->f[rD].u64);
            break;
        }
        case 56: { // psq_l
            bool w = (instr >> 15) & 1;
            uint32_t gqrIdx = (instr >> 12) & 7;
            int16_t d = (int16_t)(instr & 0xFFF);
            if (d & 0x800) d |= 0xF000;
            DequantizeLoad(ctx, rD, (rA == 0 ? 0 : ctx->r[rA]) + d, w, gqrIdx);
            break;
        }
        case 60: { // psq_st
            bool w = (instr >> 15) & 1;
            uint32_t gqrIdx = (instr >> 12) & 7;
            int16_t d = (int16_t)(instr & 0xFFF);
            if (d & 0x800) d |= 0xF000;
            QuantizeStore(ctx, rD, (rA == 0 ? 0 : ctx->r[rA]) + d, w, gqrIdx);
            break;
        }
        case 31: {
            switch (xo10) {
                case 0: { // cmp
                    uint32_t crfD = (instr >> 23) & 7;
                    UpdateCRField(ctx, crfD, (int32_t)ctx->r[rA], (int32_t)ctx->r[rB]);
                    break;
                }
                case 32: { // cmpl
                    uint32_t crfD = (instr >> 23) & 7;
                    UpdateCRFieldLogical(ctx, crfD, ctx->r[rA], ctx->r[rB]);
                    break;
                }
                case 266: // add
                    ctx->r[rD] = ctx->r[rA] + ctx->r[rB];
                    if (rc) UpdateCR0(ctx, ctx->r[rD]);
                    break;
                case 10: // addc
                    ctx->r[rD] = ctx->r[rA] + ctx->r[rB];
                    break;
                case 40: // subf
                    ctx->r[rD] = ctx->r[rB] - ctx->r[rA];
                    if (rc) UpdateCR0(ctx, ctx->r[rD]);
                    break;
                case 8: // subfc
                    ctx->r[rD] = ctx->r[rB] - ctx->r[rA];
                    break;
                case 104: // neg
                    ctx->r[rD] = (uint32_t)(-(int32_t)ctx->r[rA]);
                    if (rc) UpdateCR0(ctx, ctx->r[rD]);
                    break;
                case 444: // or / mr
                    ctx->r[rA] = ctx->r[rD] | ctx->r[rB];
                    if (rc) UpdateCR0(ctx, ctx->r[rA]);
                    break;
                case 28: // and
                    ctx->r[rA] = ctx->r[rD] & ctx->r[rB];
                    if (rc) UpdateCR0(ctx, ctx->r[rA]);
                    break;
                case 316: // xor
                    ctx->r[rA] = ctx->r[rD] ^ ctx->r[rB];
                    if (rc) UpdateCR0(ctx, ctx->r[rA]);
                    break;
                case 124: // nor
                    ctx->r[rA] = ~(ctx->r[rD] | ctx->r[rB]);
                    if (rc) UpdateCR0(ctx, ctx->r[rA]);
                    break;
                case 24: { // slw
                    uint32_t sh = ctx->r[rB] & 0x3F;
                    ctx->r[rA] = (sh < 32) ? (ctx->r[rD] << sh) : 0;
                    if (rc) UpdateCR0(ctx, ctx->r[rA]);
                    break;
                }
                case 536: { // srw
                    uint32_t sh = ctx->r[rB] & 0x3F;
                    ctx->r[rA] = (sh < 32) ? (ctx->r[rD] >> sh) : 0;
                    if (rc) UpdateCR0(ctx, ctx->r[rA]);
                    break;
                }
                case 792: { // sraw
                    uint32_t sh = ctx->r[rB] & 0x1F;
                    ctx->r[rA] = (uint32_t)((int32_t)ctx->r[rD] >> sh);
                    if (rc) UpdateCR0(ctx, ctx->r[rA]);
                    break;
                }
                case 824: { // srawi
                    uint32_t sh = (instr >> 11) & 0x1F;
                    ctx->r[rA] = (uint32_t)((int32_t)ctx->r[rD] >> sh);
                    if (rc) UpdateCR0(ctx, ctx->r[rA]);
                    break;
                }
                case 954: // extsb
                    ctx->r[rA] = (uint32_t)(int32_t)(int8_t)ctx->r[rD];
                    if (rc) UpdateCR0(ctx, ctx->r[rA]);
                    break;
                case 922: // extsh
                    ctx->r[rA] = (uint32_t)(int32_t)(int16_t)ctx->r[rD];
                    if (rc) UpdateCR0(ctx, ctx->r[rA]);
                    break;
                case 23: { // lwzx
                    uint32_t ea = (rA == 0 ? 0 : ctx->r[rA]) + ctx->r[rB];
                    ctx->r[rD] = Read32(ea);
                    break;
                }
                case 151: { // stwx
                    uint32_t ea = (rA == 0 ? 0 : ctx->r[rA]) + ctx->r[rB];
                    Write32(ea, ctx->r[rD]);
                    break;
                }
                case 87: { // lbzx
                    uint32_t ea = (rA == 0 ? 0 : ctx->r[rA]) + ctx->r[rB];
                    ctx->r[rD] = Read8(ea);
                    break;
                }
                case 215: { // stbx
                    uint32_t ea = (rA == 0 ? 0 : ctx->r[rA]) + ctx->r[rB];
                    Write8(ea, (uint8_t)ctx->r[rD]);
                    break;
                }
                case 279: { // lhzx
                    uint32_t ea = (rA == 0 ? 0 : ctx->r[rA]) + ctx->r[rB];
                    ctx->r[rD] = Read16(ea);
                    break;
                }
                case 407: { // sthx
                    uint32_t ea = (rA == 0 ? 0 : ctx->r[rA]) + ctx->r[rB];
                    Write16(ea, (uint16_t)ctx->r[rD]);
                    break;
                }
                case 790: { // lhbrx
                    uint32_t ea = (rA == 0 ? 0 : ctx->r[rA]) + ctx->r[rB];
                    uint16_t v = Read16(ea);
                    ctx->r[rD] = WII_BYTESWAP16(v);
                    break;
                }
                case 534: { // lwbrx
                    uint32_t ea = (rA == 0 ? 0 : ctx->r[rA]) + ctx->r[rB];
                    uint32_t v = Read32(ea);
                    ctx->r[rD] = WII_BYTESWAP32(v);
                    break;
                }
                case 918: { // sthbrx
                    uint32_t ea = (rA == 0 ? 0 : ctx->r[rA]) + ctx->r[rB];
                    Write16(ea, WII_BYTESWAP16((uint16_t)ctx->r[rD]));
                    break;
                }
                case 662: { // stwbrx
                    uint32_t ea = (rA == 0 ? 0 : ctx->r[rA]) + ctx->r[rB];
                    Write32(ea, WII_BYTESWAP32(ctx->r[rD]));
                    break;
                }
                case 371: { // mftb / mftbu
                    uint32_t tbr = (rB << 5) | rA;
                    if (tbr == 269) ctx->r[rD] = (uint32_t)(GetTimeBaseTicks() >> 32);
                    else ctx->r[rD] = (uint32_t)(GetTimeBaseTicks() & 0xFFFFFFFF);
                    break;
                }
                case 339: { // mfspr
                    uint32_t spr = (rB << 5) | rA;
                    if (spr == 8) ctx->r[rD] = ctx->lr;
                    else if (spr == 9) ctx->r[rD] = ctx->ctr;
                    else if (spr == 1) ctx->r[rD] = ctx->xer;
                    else if (spr == 268) ctx->r[rD] = (uint32_t)(GetTimeBaseTicks() & 0xFFFFFFFF);
                    else if (spr == 269) ctx->r[rD] = (uint32_t)(GetTimeBaseTicks() >> 32);
                    else if (spr >= 912 && spr <= 919) ctx->r[rD] = ctx->gqr[spr - 912].GetRaw();
                    else ctx->r[rD] = 0;
                    break;
                }
                case 467: { // mtspr
                    uint32_t spr = (rB << 5) | rA;
                    if (spr == 8) ctx->lr = ctx->r[rD];
                    else if (spr == 9) ctx->ctr = ctx->r[rD];
                    else if (spr == 1) ctx->xer = ctx->r[rD];
                    else if (spr >= 912 && spr <= 919) ctx->gqr[spr - 912].SetRaw(ctx->r[rD]);
                    break;
                }
                case 83: // mfmsr
                    ctx->r[rD] = 0x00002030;
                    break;
                case 146: // mtmsr
                    break;
                case 19: // mfcr
                    ctx->r[rD] = ctx->cr;
                    break;
                case 144: { // mtcrf
                    uint32_t crm = (instr >> 12) & 0xFF;
                    for (int i = 0; i < 8; ++i) {
                        if (crm & (1 << (7 - i))) {
                            ctx->SetCRField(i, (ctx->r[rD] >> (28 - i * 4)) & 0xF);
                        }
                    }
                    break;
                }
                case 26: { // cntlzw
                    uint32_t val = ctx->r[rD];
                    uint32_t lz = 0;
                    while (lz < 32 && !(val & (0x80000000 >> lz))) lz++;
                    ctx->r[rA] = lz;
                    if (rc) UpdateCR0(ctx, ctx->r[rA]);
                    break;
                }
                case 598: case 854: case 86: case 470: case 54: case 278: case 1014: case 982: case 210: case 595:
                    // cache / sync / mmu nops
                    break;
                default:
                    break;
            }
            break;
        }
        case 63: { // double fp ops / fmr
            uint32_t xo = (instr >> 1) & 0x3FF;
            if (xo == 72) { // fmr
                ctx->f[rD].d = ctx->f[rB].d;
            }
            break;
        }
        default:
            break;
    }

    ctx->pc = next_pc;
    return true;
}

} // namespace WiiPort
