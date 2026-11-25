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

    // Indirizzo FISICO dell'XFB corrente (0 = nessun framebuffer valido ancora registrato)
    uint32_t currentFramebufferAddress = 0;
    bool frameReady = false;
    uint32_t arenaLo = 0x80130000;   // inizio arena MEM1 libera (dopo il bss del DOL)

    // Dimensioni XFB correnti (aggiornate dinamicamente da VI/GX; YUYV 4:2:2, 2 byte per pixel)
    uint32_t xfbWidth  = 640;
    uint32_t xfbHeight = 480;
    inline uint32_t XfbBytes() const { return xfbWidth * xfbHeight * 2; }
    uint32_t xfbChangeLogCount = 0;
    uint32_t entryPoint = 0x80004000;
    uint32_t beamPos = 0x100;
    uint16_t dspCsr = 0x0020;
    bool dspDmaDone = false;
    bool dspMailReady = false;
    uint32_t ipcPpcMsg = 0;
    uint32_t ipcArmMsg = 0;
    uint32_t ipcCtrl = 0x00000036;
    uint32_t ipcProcessedMsg = 0;
    uint32_t mmioStore[0x8000] = {};

    NativeWiiRuntime() {
        mem1 = (uint8_t*)malloc(MEM1_SIZE);
        mem2 = (uint8_t*)malloc(MEM2_SIZE);
        dispatchTable = (void(**)(PPCContext*))calloc(MEM1_SIZE / 4, sizeof(void*));
        std::memset(mem1, 0, MEM1_SIZE);
        std::memset(mem2, 0, MEM2_SIZE);
        std::memset(mmioStore, 0, sizeof(mmioStore));
        gx.BindMemory(mem1, MEM1_SIZE, mem2, MEM2_SIZE, [this](uint32_t xfbAddr, const char* src) {
            this->SetXfbAddress(xfbAddr, src);
        });
    }

    ~NativeWiiRuntime() {
        free(mem1);
        free(mem2);
        free(dispatchTable);
    }

    void RegisterFunction(uint32_t ppcAddr, void (*nativeFunc)(PPCContext*)) {
        uint8_t top = (ppcAddr >> 24) & 0xF8;
        if (top == 0x80 || top == 0x00 || top == 0xC0 || top == 0xE0) {
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
        std::memset(mmioStore, 0, sizeof(mmioStore));
        currentFramebufferAddress = 0;
        frameReady = false;
        xfbWidth = 640;
        xfbHeight = 480;
        xfbChangeLogCount = 0;
        dspCsr = 0x0020;
        dspDmaDone = false;
        dspMailReady = false;
        ipcPpcMsg = 0;
        ipcArmMsg = 0;
        ipcCtrl = 0x00000036;
        ipcProcessedMsg = 0;
        gx.ResetState();

        for (const auto& sec : dol.sections) {
            snprintf(buf, sizeof(buf), "[RUNTIME] Copia sezione @ 0x%08X (size: %u)", sec.address, sec.size);
            Log(buf);
            uint8_t top = (sec.address >> 24) & 0xF8;
            if (top == 0x00 || top == 0x80 || top == 0xC0 || top == 0xE0) {
                uint32_t offset = sec.address & (MEM1_SIZE - 1);
                if (offset + sec.size <= MEM1_SIZE) {
                    std::memcpy(mem1 + offset, sec.data.data(), sec.size);
                }
            } else if (((sec.address >> 24) & 0xFC) == 0x90 || ((sec.address >> 24) & 0xFC) == 0xD0 || ((sec.address >> 24) & 0xFC) == 0x10) {
                uint32_t offset = sec.address & (MEM2_SIZE - 1);
                if (offset + sec.size <= MEM2_SIZE) {
                    std::memcpy(mem2 + offset, sec.data.data(), sec.size);
                }
            }
        }

        if (dol.bssAddress && dol.bssSize) {
            uint8_t top = (dol.bssAddress >> 24) & 0xF8;
            if (top == 0x00 || top == 0x80 || top == 0xC0 || top == 0xE0) {
                uint32_t offset = dol.bssAddress & (MEM1_SIZE - 1);
                if (offset + dol.bssSize <= MEM1_SIZE) {
                    std::memset(mem1 + offset, 0, dol.bssSize);
                }
            }
        }

        // Setup Low Memory OS Globals per compatibilità Broadway / libogc / RVL SDK
        Write32(0x80000020, 0x0D15EA5Eu);          // DVD / OS Boot magic
        Write32(0x80000024, 0x00000001u);          // Version
        Write32(0x80000028, (uint32_t)MEM1_SIZE);  // 128 MB RAM fisica
        Write32(0x8000002C, 0x00000002u);          // Wii retail
        Write32(0x80000030, (uint32_t)MEM1_SIZE);  // Arena / Physical Mem Size
        arenaLo = dol.bssAddress + dol.bssSize;
        if (arenaLo == 0) arenaLo = 0x80130000;
        arenaLo = (arenaLo + 0x1F) & ~0x1Fu;
        Write32(0x80000034, arenaLo);
        Write32(0x80000038, 0x80000000 + (uint32_t)MEM1_SIZE - 0x1000); // 128 MB ArenaHi
        Write32(0x800000CC, 0);                    // NTSC
        Write32(0x800000E4, 0);
        Write32(0x800000F0, (uint32_t)MEM1_SIZE);  // SimulatedMemSize
        Write32(0x800000F8, 0x0E7BE2C0u);          // Bus Clock 243 MHz
        Write32(0x800000FC, 0x2B73A840u);          // CPU Clock 729 MHz

        auto OverlapsDol = [&](uint32_t addr, uint32_t size) {
            for (const auto& s : dol.sections) {
                if (s.size == 0) continue;
                if (addr < s.address + s.size && addr + size > s.address) return true;
            }
            return false;
        };
        if (!OverlapsDol(0x80003100, 0x40)) {
            Write32(0x80003100, 0x01800000u);      // MEM1 physical 24 MB
            Write32(0x80003104, 0x01800000u);
            Write32(0x80003108, 0x01800000u);
            Write32(0x80003118, 0x04000000u);      // MEM2 physical 64 MB
            Write32(0x8000311C, 0x04000000u);
            Write32(0x80003120, 0x90000000u);      // MEM2 cached base
            Write32(0x80003124, 0x93400000u);
            Write32(0x80003128, 0x933E0000u);
            Write32(0x80003130, 0x933E0000u);
        }

        entryPoint = dol.entryPoint;
        snprintf(buf, sizeof(buf), "[RUNTIME] DOL caricato in memoria nativa con successo! EntryPoint: 0x%08X", entryPoint);
        Log(buf);
        return true;
    }

    // ================= XFB: validazione e traduzione indirizzi =================
    // Traduce un indirizzo guest (K0 0x80.., K1 0xC0.., fisico, MEM2 0x90../0xD0../0x10..)
    // in un puntatore host. Ritorna nullptr se l'intervallo [addr, addr+len) non sta nella RAM emulata.
    const uint8_t* GuestToHost(uint32_t addr, size_t len) const {
        uint8_t top1 = (addr >> 24) & 0xF8;
        if (top1 == 0x00 || top1 == 0x80 || top1 == 0xC0 || top1 == 0xE0) {
            uint32_t phys = addr & (MEM1_SIZE - 1);
            return (phys + len <= MEM1_SIZE) ? mem1 + phys : nullptr;
        }
        uint8_t top2 = (addr >> 24) & 0xFC;
        if (top2 == 0x90 || top2 == 0xD0 || top2 == 0x10) {
            uint32_t off = addr & (MEM2_SIZE - 1);
            return (off + len <= MEM2_SIZE) ? mem2 + off : nullptr;
        }
        return nullptr;
    }

    // Host pointer dell'XFB corrente, oppure nullptr se non ce n'e' uno valido
    const uint8_t* GetXfbHost() const {
        if (currentFramebufferAddress == 0) return nullptr;
        return GuestToHost(currentFramebufferAddress, XfbBytes());
    }

    // Registra un nuovo XFB solo se e' plausibile (allineato a 32 byte e interamente dentro la RAM).
    // Supporta MEM1 e MEM2, double buffering via registro VI, VIDEO_SetNextFramebuffer e GX_CopyDisp.
    bool SetXfbAddress(uint32_t guestAddr, const char* source) {
        if (guestAddr == 0) return false;
        const bool isViReg = (std::strcmp(source, "registro VI") == 0);
        const bool isSetNext = (std::strcmp(source, "VIDEO_SetNextFramebuffer") == 0);
        const bool lowMem = ((guestAddr & 0x1FFFFFFFu) < 0x10000u);
        const uint32_t alignMask = isSetNext ? 0x07u : 0x1Fu;
        if (lowMem || (guestAddr & alignMask) != 0 || GuestToHost(guestAddr, XfbBytes()) == nullptr) {
            if (!isViReg && xfbChangeLogCount < 16) {
                ++xfbChangeLogCount;
                LogConsole(std::string("[VI] XFB scartato (") + source + "): indirizzo non valido " + HexStr(guestAddr));
            }
            return false;
        }
        uint8_t top2 = (guestAddr >> 24) & 0xFC;
        uint32_t norm = (top2 == 0x90 || top2 == 0xD0 || top2 == 0x10)
            ? (0x90000000u | (guestAddr & (MEM2_SIZE - 1)))
            : (guestAddr & (MEM1_SIZE - 1));
        if (norm != currentFramebufferAddress) {
            currentFramebufferAddress = norm;
            if (xfbChangeLogCount < 16) {
                ++xfbChangeLogCount;
                LogConsole(std::string("[VI] XFB (") + source + ") = " + HexStr(guestAddr) + " (norm " + HexStr(norm) + ")");
            }
        }
        frameReady = true;
        return true;
    }

    // ================= MEMORY ACCESS PRIMITIVES =================
    inline uint32_t Read32(uint32_t addr) {
        if ((addr & 0xFF000000) == 0xCC000000 || (addr & 0xFF000000) == 0xCD000000) {
            return HandleMMIO_Read(addr);
        }
        uint8_t top = (addr >> 24) & 0xF8;
        if (top == 0x00 || top == 0x80 || top == 0xC0 || top == 0xE0) {
            uint32_t offset = addr & (MEM1_SIZE - 1);
            return WII_BYTESWAP32(*reinterpret_cast<uint32_t*>(mem1 + offset));
        }
        uint8_t top2 = (addr >> 24) & 0xFC;
        if (top2 == 0x90 || top2 == 0xD0 || top2 == 0x10) {
            uint32_t offset = addr & (MEM2_SIZE - 1);
            return WII_BYTESWAP32(*reinterpret_cast<uint32_t*>(mem2 + offset));
        }
        return 0;
    }

    inline void Write32(uint32_t addr, uint32_t val) {
        if ((addr & 0xFFFFFF80u) == 0xCC008000u) {
            gx.WriteWord(val);
            return;
        }
        if ((addr & 0xFF000000) == 0xCC000000 || (addr & 0xFF000000) == 0xCD000000) {
            HandleMMIO_Write(addr, val);
            return;
        }
        uint8_t top = (addr >> 24) & 0xF8;
        if (top == 0x00 || top == 0x80 || top == 0xC0 || top == 0xE0) {
            uint32_t offset = addr & (MEM1_SIZE - 1);
            *reinterpret_cast<uint32_t*>(mem1 + offset) = WII_BYTESWAP32(val);
            return;
        }
        uint8_t top2 = (addr >> 24) & 0xFC;
        if (top2 == 0x90 || top2 == 0xD0 || top2 == 0x10) {
            uint32_t offset = addr & (MEM2_SIZE - 1);
            *reinterpret_cast<uint32_t*>(mem2 + offset) = WII_BYTESWAP32(val);
            return;
        }
    }

    inline uint16_t Read16(uint32_t addr) {
        if ((addr & 0xFF000000) == 0xCC000000 || (addr & 0xFF000000) == 0xCD000000) {
            return (uint16_t)HandleMMIO_Read(addr);
        }
        uint8_t top = (addr >> 24) & 0xF8;
        if (top == 0x00 || top == 0x80 || top == 0xC0 || top == 0xE0) {
            uint32_t offset = addr & (MEM1_SIZE - 1);
            return WII_BYTESWAP16(*reinterpret_cast<uint16_t*>(mem1 + offset));
        }
        uint8_t top2 = (addr >> 24) & 0xFC;
        if (top2 == 0x90 || top2 == 0xD0 || top2 == 0x10) {
            uint32_t offset = addr & (MEM2_SIZE - 1);
            return WII_BYTESWAP16(*reinterpret_cast<uint16_t*>(mem2 + offset));
        }
        return 0;
    }

    inline void Write16(uint32_t addr, uint16_t val) {
        if ((addr & 0xFFFFFF80u) == 0xCC008000u) {
            gx.WriteHalf(val);
            return;
        }
        if ((addr & 0xFF000000) == 0xCC000000 || (addr & 0xFF000000) == 0xCD000000) {
            HandleMMIO_Write(addr, val);
            return;
        }
        uint8_t top = (addr >> 24) & 0xF8;
        if (top == 0x00 || top == 0x80 || top == 0xC0 || top == 0xE0) {
            uint32_t offset = addr & (MEM1_SIZE - 1);
            *reinterpret_cast<uint16_t*>(mem1 + offset) = WII_BYTESWAP16(val);
            return;
        }
        uint8_t top2 = (addr >> 24) & 0xFC;
        if (top2 == 0x90 || top2 == 0xD0 || top2 == 0x10) {
            uint32_t offset = addr & (MEM2_SIZE - 1);
            *reinterpret_cast<uint16_t*>(mem2 + offset) = WII_BYTESWAP16(val);
            return;
        }
    }

    inline uint8_t Read8(uint32_t addr) {
        if ((addr & 0xFF000000) == 0xCC000000 || (addr & 0xFF000000) == 0xCD000000) {
            return (uint8_t)HandleMMIO_Read(addr);
        }
        uint8_t top = (addr >> 24) & 0xF8;
        if (top == 0x00 || top == 0x80 || top == 0xC0 || top == 0xE0) {
            return mem1[addr & (MEM1_SIZE - 1)];
        }
        uint8_t top2 = (addr >> 24) & 0xFC;
        if (top2 == 0x90 || top2 == 0xD0 || top2 == 0x10) {
            return mem2[addr & (MEM2_SIZE - 1)];
        }
        return 0;
    }

    inline void Write8(uint32_t addr, uint8_t val) {
        if ((addr & 0xFFFFFF80u) == 0xCC008000u) {
            gx.WriteByte(val);
            return;
        }
        if ((addr & 0xFF000000) == 0xCC000000 || (addr & 0xFF000000) == 0xCD000000) {
            HandleMMIO_Write(addr, val);
            return;
        }
        uint8_t top = (addr >> 24) & 0xF8;
        if (top == 0x00 || top == 0x80 || top == 0xC0 || top == 0xE0) {
            mem1[addr & (MEM1_SIZE - 1)] = val;
            return;
        }
        uint8_t top2 = (addr >> 24) & 0xFC;
        if (top2 == 0x90 || top2 == 0xD0 || top2 == 0x10) {
            mem2[addr & (MEM2_SIZE - 1)] = val;
            return;
        }
    }

    uint32_t HandleMMIO_Read(uint32_t addr) {
        // CP Status Register: GP read idle + cmd idle (bits 2 & 3 = 0x0C), nessun underflow/overflow
        if (addr == 0xCC000000 || addr == 0xCC000002) return 0x0000000C;
        // PE Status / Token (GX_SetDrawSync / GX_DrawDone)
        if (addr == 0xCC00100A) return (uint32_t)(gx.peStatus | 0x000Cu);
        if (addr == 0xCC00100E) return (uint32_t)gx.peToken;
        // VI Vertical / Horizontal Beam Position (stabile su letture consecutive, avanza in polling)
        if (addr == 0xCC00202C) {
            static uint32_t s_beamSubtick = 0;
            if (++s_beamSubtick >= 8) {
                s_beamSubtick = 0;
                beamPos = 1u + (beamPos % 525u);
            } else if (beamPos == 0) {
                beamPos = 1u;
            }
            return beamPos;
        }
        if (addr == 0xCC00202E) {
            return 1u;
        }
        if (addr == 0xCC002002) return mmioStore[0x2002 >> 1] ? (mmioStore[0x2002 >> 1] & 0xFFFFu) : 0x00000001u;
        // DSP Control/Status & Mailbox: modella handshake reale CPU->DSP (0xCC005000) e DSP->CPU (0xCC005004/6)
        if (addr == 0xCC005000 || addr == 0xCC005002) return 0x0000;
        if (addr == 0xCC00500A) return (dspCsr & ~0x0601u) | 0x0020u;
        if (addr == 0xCC005004) {
            return dspMailReady ? 0x8071u : 0x0000u;
        }
        if (addr == 0xCC005006) {
            uint32_t ret = dspMailReady ? 0xFEEDu : 0x0000u;
            dspMailReady = false;
            dspDmaDone = false;
            return ret;
        }
        if (addr == 0xCC005012 || addr == 0xCC005020) return 0;
        if (addr == 0xCD8000C8 || addr == 0xCD8000C4 || addr == 0xCD8000C0 ||
            addr == 0xCD0000C8 || addr == 0xCD0000C4 || addr == 0xCD0000C0) return 0x00008000; // VI I2C ready/ACK
        if (addr == 0xCC006800 || (addr & 0xFFFFFF00) == 0xCD006800) return 0x00000000; // EXI channel ready
        // Hollywood IPC Registers (0xCD000000..0C, 0xCD800000..0C, legacy 0xCD006C00..0C)
        if (addr == 0xCD000000 || addr == 0xCD800000 || addr == 0xCD006C00) return ipcPpcMsg;
        if (addr == 0xCD000004 || addr == 0xCD800004 || addr == 0xCD006C04) return (ipcCtrl & ~0x09u) | 0x00000036u;
        if (addr == 0xCD000008 || addr == 0xCD800008 || addr == 0xCD006C08) {
            if (ipcArmMsg != 0) return ipcArmMsg;
            static uint32_t s_ipc_ack = 0;
            s_ipc_ack ^= 0x00000001u;
            return s_ipc_ack;
        }
        if (addr == 0xCD00000C || addr == 0xCD80000C || addr == 0xCD006C0C || addr == 0xCD00640C) return 0x00000036u;
        if (addr == 0xCD006404) return 0x00000030u; // HW Revision
        if (addr == 0xCD006434 || addr == 0xCC006434) return mmioStore[(addr & 0xFFFF) >> 1] & ~0x00000001; // DI command not busy (bit 0 = 0)
        if ((addr & 0xFFFFFF00) == 0xCD006400) {
            return mmioStore[(addr & 0xFFFF) >> 1] & ~0x00000001;
        }
        return mmioStore[(addr & 0xFFFF) >> 1];
    }

    void HandleMMIO_Write(uint32_t addr, uint32_t val) {
        if (addr == 0xCC00100A) {
            if (val & 0x04u) gx.peStatus &= ~0x0004u;
            if (val & 0x08u) gx.peStatus &= ~0x0008u;
            return;
        }
        if (addr == 0xCC00500A) {
            dspCsr = (uint16_t)(val & ~0x0601u);
            if (val & 0x0001u) {
                dspMailReady = false;
                dspDmaDone = false;
            }
            if ((val & 0x0800u) && !(val & 0x0004u) && !(val & 0x0001u)) {
                dspMailReady = true;
            }
            return;
        }
        if (addr == 0xCC005028) {
            dspDmaDone = true;   // Segnala completamento DMA ARAM
            dspMailReady = true; // Segnala mail di risposta DSP disponibile
            return;
        }
        if (addr == 0xCC005004 || addr == 0xCC005000 || addr == 0xCC005002) {
            dspDmaDone = false;
            dspMailReady = false;
            return;
        }
        // 1. WGPIPE: invio diretto di dati grafici al command processor
        if ((addr & 0xFFFFFF80u) == 0xCC008000u) {
            gx.WriteWord(val);
            return;
        }

        // 2. Hollywood Mailbox IPC (Starlet/IOS): supporto completo a specchio 0xCD000000, 0xCD800000 e 0xCD006C00
        if (addr == 0xCD000000 || addr == 0xCD800000 || addr == 0xCD006C00) {
            ipcPpcMsg = val;
            if (val != 0 && (val >= 0x80000000u || (val >= 0x00010000u && val < MEM1_SIZE) || ((val >> 24) == 0x10))) {
                ios.ProcessRequest(mem1, MEM1_SIZE, mem2, MEM2_SIZE, val);
                ipcArmMsg = val;
                ipcProcessedMsg = val;
            }
            return;
        }
        if (addr == 0xCD000004 || addr == 0xCD800004 || addr == 0xCD006C04) {
            if ((val & 0x01u) && ipcPpcMsg != 0 && ipcProcessedMsg != ipcPpcMsg) {
                ios.ProcessRequest(mem1, MEM1_SIZE, mem2, MEM2_SIZE, ipcPpcMsg);
                ipcArmMsg = ipcPpcMsg;
                ipcProcessedMsg = ipcPpcMsg;
            }
            ipcCtrl = (val & ~0x09u) | 0x36u;
            return;
        }

        // 2b. VI Horizontal Scaling Width (aggiorna larghezza XFB se valida)
        if (addr == 0xCC002048) {
            mmioStore[0x2048 >> 1] = (uint16_t)val;
            uint32_t w = (val & 0xFFu) * 16u;
            if (w >= 320 && w <= 720) xfbWidth = w;
            return;
        }

        // 3. VI Top / Bottom Framebuffer Address (TFBL 0xCC00201C / BFBL 0xCC002024)
        //    Registro a 32 bit: [hi16 @+0][lo16 @+2]. hi: bit 0-7 = FBB[23:16], bit 8-11 = XOFF, bit 12 = POFF.
        //    Con POFF=1 l'indirizzo e' FBB << 5, altrimenti FBB e' l'indirizzo diretto.
        if (addr == 0xCC00201C || addr == 0xCC00201E || addr == 0xCC002024 || addr == 0xCC002026) {
            const bool isTop = (addr == 0xCC00201C || addr == 0xCC00201E);
            const uint32_t base = isTop ? 0x201C : 0x2024;
            if ((addr == 0xCC00201C || addr == 0xCC002024) && val > 0xFFFF) {
                // scrittura a 32 bit: spezza in due meta' da 16 bit
                mmioStore[base >> 1]       = (uint16_t)(val >> 16);
                mmioStore[(base + 2) >> 1] = (uint16_t)(val & 0xFFFF);
            } else {
                mmioStore[(addr & 0xFFFF) >> 1] = (uint16_t)val;
            }
            // Decodifica su scrittura 32-bit o scrittura di una meta' se entrambe sono coerenti
            if (isTop && (addr == 0xCC00201E || val > 0xFFFF || (mmioStore[0x201E >> 1] & 0xFFFFu) != 0)) {
                uint32_t hi = mmioStore[0x201C >> 1] & 0xFFFFu;
                uint32_t lo = mmioStore[0x201E >> 1] & 0xFFFFu;
                uint32_t fbb = ((hi & 0x00FFu) << 16) | lo;
                uint32_t decoded = (hi & 0x1000u) ? (fbb << 5) : fbb;
                SetXfbAddress(decoded, "registro VI");
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
    bool Step(PPCContext* ctx, bool* outReturned = nullptr);

    void Execute(PPCContext* ctx, uint32_t startAddr, uint64_t maxInstructions = 0) {
        ctx->pc = startAddr;
        uint64_t count = 0;
        while (ctx->pc != 0) {
            bool returned = false;
            if (!Step(ctx, &returned)) break;
            if (returned && count > 0) break;
            count++;
            if (maxInstructions && count >= maxInstructions) break;
        }
    }

    inline void Call(uint32_t targetAddress, PPCContext* ctx) {
        DispatchIndirectBranch(ctx, targetAddress);
    }

    inline void DispatchIndirectBranch(PPCContext* ctx, uint32_t targetAddress) {
        if (targetAddress < 0x80003000u || targetAddress >= 0x88000000u || (targetAddress & 3u) != 0) {
            static uint32_t s_badBranchLogs = 0;
            if (s_badBranchLogs < 16) {
                ++s_badBranchLogs;
                Log("[WARN] Branch indiretto non valido: target=" + HexStr(targetAddress) +
                    " LR=" + HexStr(ctx->lr) + " r3=" + HexStr(ctx->r[3]) +
                    " r4=" + HexStr(ctx->r[4]) + " r30=" + HexStr(ctx->r[30]) +
                    " r31=" + HexStr(ctx->r[31]));
            }
            return;
        }
        uint8_t top = (targetAddress >> 24) & 0xF8;
        if (top == 0x80 || top == 0x00 || top == 0xC0 || top == 0xE0) {
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
        uint64_t maxSteps = 2000000000ULL;
        while (ctx->pc != 0 && maxSteps-- > 0) {
            uint8_t curTop = (ctx->pc >> 24) & 0xF8;
            if (curTop == 0x80 || curTop == 0x00 || curTop == 0xC0 || curTop == 0xE0) {
                uint32_t idx = (ctx->pc & (MEM1_SIZE - 1)) >> 2;
                if (idx < (MEM1_SIZE / 4)) {
                    auto func = dispatchTable[idx];
                    if (func) {
                        func(ctx);
                        return;
                    }
                }
            }
            bool returned = false;
            if (!Step(ctx, &returned)) break;
            if (returned) return;
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

// Helper CR / XER / FP
inline void UpdateCR1(PPCContext* ctx) {
    ctx->SetCRField(1, (ctx->fpscr >> 28) & 0xF);
}

inline void SetXERCarry(PPCContext* ctx, bool carry) {
    if (carry) ctx->xer |= 0x20000000u;
    else       ctx->xer &= ~0x20000000u;
}

inline bool GetXERCarry(const PPCContext* ctx) {
    return (ctx->xer & 0x20000000u) != 0;
}

inline void SyncSingleFP(PairedSingle& reg, double val) {
    reg.d = val;
    float f = (float)val;
    reg.ps[0] = f;
    reg.ps[1] = f;
}

inline int32_t DecodeGqrScale(uint8_t s) {
    int8_t signed6 = (int8_t)(s << 2);
    return (int32_t)(signed6 >> 2);
}

// Helper globale per dequantizzare psq_l / psq_lu / psq_lx / psq_lux
inline void DequantizeLoad(PPCContext* ctx, uint32_t fD, uint32_t addr, bool w, uint32_t gqrIdx) {
    const auto& gqr = ctx->gqr[gqrIdx & 7u];
    float factor = std::ldexp(1.0f, -DecodeGqrScale(gqr.l_scale));

    auto ReadComponent = [&](uint32_t a) -> float {
        switch (gqr.l_type) {
            case 0: {
                uint32_t u = g_Memory.Read32(a);
                float f;
                std::memcpy(&f, &u, 4);
                return f;
            }
            case 4: return (float)g_Memory.Read8(a) * factor;
            case 5: return (float)g_Memory.Read16(a) * factor;
            case 6: return (float)(int8_t)g_Memory.Read8(a) * factor;
            case 7: return (float)(int16_t)g_Memory.Read16(a) * factor;
            default: return 0.0f;
        }
    };

    float p0 = ReadComponent(addr);
    ctx->f[fD].ps[0] = p0;
    if (!w) {
        uint32_t stride = (gqr.l_type == 0) ? 4u : ((gqr.l_type == 5 || gqr.l_type == 7) ? 2u : 1u);
        ctx->f[fD].ps[1] = ReadComponent(addr + stride);
    } else {
        ctx->f[fD].ps[1] = 1.0f;
    }
    ctx->f[fD].d = (double)p0;
}

// Helper globale per quantizzare psq_st / psq_stu / psq_stx / psq_stux
inline void QuantizeStore(PPCContext* ctx, uint32_t fS, uint32_t addr, bool w, uint32_t gqrIdx) {
    const auto& gqr = ctx->gqr[gqrIdx & 7u];
    float factor = std::ldexp(1.0f, DecodeGqrScale(gqr.s_scale));

    auto ClampI = [](float v, int32_t lo, int32_t hi) -> int32_t {
        if (v >= (float)hi) return hi;
        if (v <= (float)lo) return lo;
        return (int32_t)v;
    };

    auto WriteComponent = [&](uint32_t a, float val) {
        switch (gqr.s_type) {
            case 0: {
                uint32_t u;
                std::memcpy(&u, &val, 4);
                g_Memory.Write32(a, u);
                break;
            }
            case 4:
                g_Memory.Write8(a, (uint8_t)ClampI(val * factor, 0, 255));
                break;
            case 5:
                g_Memory.Write16(a, (uint16_t)ClampI(val * factor, 0, 65535));
                break;
            case 6:
                g_Memory.Write8(a, (uint8_t)(int8_t)ClampI(val * factor, -128, 127));
                break;
            case 7:
                g_Memory.Write16(a, (uint16_t)(int16_t)ClampI(val * factor, -32768, 32767));
                break;
            default:
                break;
        }
    };

    WriteComponent(addr, ctx->f[fS].ps[0]);
    if (!w) {
        uint32_t stride = (gqr.s_type == 0) ? 4u : ((gqr.s_type == 5 || gqr.s_type == 7) ? 2u : 1u);
        WriteComponent(addr + stride, ctx->f[fS].ps[1]);
    }
}

inline bool NativeWiiRuntime::Step(PPCContext* ctx, bool* outReturned) {
    uint8_t top = (ctx->pc >> 24) & 0xF8;
    if (top == 0x80 || top == 0x00 || top == 0xC0 || top == 0xE0) {
        uint32_t idx = (ctx->pc & (MEM1_SIZE - 1)) >> 2;
        if (idx < (MEM1_SIZE / 4)) {
            auto func = dispatchTable[idx];
            if (func) {
                func(ctx);
                ctx->pc = ctx->lr;
                if (outReturned) *outReturned = true;
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
            if (instr & 1) {
                ctx->lr = next_pc;
                if (target != next_pc) {
                    DispatchIndirectBranch(ctx, target);
                    ctx->pc = next_pc;
                    return true;
                }
            }
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
                if (instr & 1) {
                    ctx->lr = next_pc;
                    if (target != next_pc) {
                        DispatchIndirectBranch(ctx, target);
                        ctx->pc = next_pc;
                        return true;
                    }
                }
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
                    if (instr & 1) {
                        ctx->lr = next_pc;
                        DispatchIndirectBranch(ctx, target);
                        ctx->pc = next_pc;
                        return true;
                    }
                    if (outReturned) *outReturned = true;
                    next_pc = target;
                }
            } else if (xo10 == 528) { // bctr / bctrl
                uint32_t bo = (instr >> 21) & 0x1F;
                uint32_t bi = (instr >> 16) & 0x1F;
                if (EvaluateCondition(ctx, bo, bi)) {
                    uint32_t target = ctx->ctr;
                    if (instr & 1) {
                        ctx->lr = next_pc;
                        DispatchIndirectBranch(ctx, target);
                        ctx->pc = next_pc;
                        return true;
                    }
                    next_pc = target;
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
        case 8: { // subfic
            uint64_t res64 = (uint64_t)(uint32_t)(int32_t)simm + (uint64_t)(uint32_t)(~ctx->r[rA]) + 1ull;
            ctx->r[rD] = (uint32_t)res64;
            SetXERCarry(ctx, (res64 >> 32) != 0);
            break;
        }
        case 12: // addic
        case 13: { // addic.
            uint64_t res64 = (uint64_t)ctx->r[rA] + (uint64_t)(uint32_t)(int32_t)simm;
            ctx->r[rD] = (uint32_t)res64;
            SetXERCarry(ctx, (res64 >> 32) != 0);
            if (op == 13) UpdateCR0(ctx, ctx->r[rD]);
            break;
        }
        case 17: // sc
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
        case 43: { // lhau
            uint32_t ea = ctx->r[rA] + (int32_t)simm;
            ctx->r[rD] = (uint32_t)(int32_t)(int16_t)Read16(ea);
            ctx->r[rA] = ea;
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
            SyncSingleFP(ctx->f[rD], (double)f);
            break;
        }
        case 49: { // lfsu
            uint32_t ea = ctx->r[rA] + (int32_t)simm;
            uint32_t u = Read32(ea);
            float f;
            std::memcpy(&f, &u, 4);
            SyncSingleFP(ctx->f[rD], (double)f);
            ctx->r[rA] = ea;
            break;
        }
        case 50: { // lfd
            uint32_t ea = (rA == 0 ? 0 : ctx->r[rA]) + (int32_t)simm;
            uint64_t hi = Read32(ea);
            uint64_t lo = Read32(ea + 4);
            ctx->f[rD].u64 = (hi << 32) | lo;
            break;
        }
        case 51: { // lfdu
            uint32_t ea = ctx->r[rA] + (int32_t)simm;
            uint64_t hi = Read32(ea);
            uint64_t lo = Read32(ea + 4);
            ctx->f[rD].u64 = (hi << 32) | lo;
            ctx->r[rA] = ea;
            break;
        }
        case 52: { // stfs
            uint32_t ea = (rA == 0 ? 0 : ctx->r[rA]) + (int32_t)simm;
            float f = (float)ctx->f[rD].d;
            uint32_t u;
            std::memcpy(&u, &f, 4);
            Write32(ea, u);
            break;
        }
        case 53: { // stfsu
            uint32_t ea = ctx->r[rA] + (int32_t)simm;
            float f = (float)ctx->f[rD].d;
            uint32_t u;
            std::memcpy(&u, &f, 4);
            Write32(ea, u);
            ctx->r[rA] = ea;
            break;
        }
        case 54: { // stfd
            uint32_t ea = (rA == 0 ? 0 : ctx->r[rA]) + (int32_t)simm;
            Write32(ea, (uint32_t)(ctx->f[rD].u64 >> 32));
            Write32(ea + 4, (uint32_t)ctx->f[rD].u64);
            break;
        }
        case 55: { // stfdu
            uint32_t ea = ctx->r[rA] + (int32_t)simm;
            Write32(ea, (uint32_t)(ctx->f[rD].u64 >> 32));
            Write32(ea + 4, (uint32_t)ctx->f[rD].u64);
            ctx->r[rA] = ea;
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
        case 57: { // psq_lu
            bool w = (instr >> 15) & 1;
            uint32_t gqrIdx = (instr >> 12) & 7;
            int16_t d = (int16_t)(instr & 0xFFF);
            if (d & 0x800) d |= 0xF000;
            uint32_t ea = ctx->r[rA] + d;
            DequantizeLoad(ctx, rD, ea, w, gqrIdx);
            ctx->r[rA] = ea;
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
        case 61: { // psq_stu
            bool w = (instr >> 15) & 1;
            uint32_t gqrIdx = (instr >> 12) & 7;
            int16_t d = (int16_t)(instr & 0xFFF);
            if (d & 0x800) d |= 0xF000;
            uint32_t ea = ctx->r[rA] + d;
            QuantizeStore(ctx, rD, ea, w, gqrIdx);
            ctx->r[rA] = ea;
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
                case 10: { // addc
                    uint64_t res64 = (uint64_t)ctx->r[rA] + (uint64_t)ctx->r[rB];
                    ctx->r[rD] = (uint32_t)res64;
                    SetXERCarry(ctx, (res64 >> 32) != 0);
                    if (rc) UpdateCR0(ctx, ctx->r[rD]);
                    break;
                }
                case 138: { // adde
                    uint64_t res64 = (uint64_t)ctx->r[rA] + (uint64_t)ctx->r[rB] + (GetXERCarry(ctx) ? 1ull : 0ull);
                    ctx->r[rD] = (uint32_t)res64;
                    SetXERCarry(ctx, (res64 >> 32) != 0);
                    if (rc) UpdateCR0(ctx, ctx->r[rD]);
                    break;
                }
                case 202: { // addze
                    uint64_t res64 = (uint64_t)ctx->r[rA] + (GetXERCarry(ctx) ? 1ull : 0ull);
                    ctx->r[rD] = (uint32_t)res64;
                    SetXERCarry(ctx, (res64 >> 32) != 0);
                    if (rc) UpdateCR0(ctx, ctx->r[rD]);
                    break;
                }
                case 234: { // addme
                    uint64_t res64 = (uint64_t)ctx->r[rA] + 0xFFFFFFFFull + (GetXERCarry(ctx) ? 1ull : 0ull);
                    ctx->r[rD] = (uint32_t)res64;
                    SetXERCarry(ctx, (res64 >> 32) != 0);
                    if (rc) UpdateCR0(ctx, ctx->r[rD]);
                    break;
                }
                case 40: // subf
                    ctx->r[rD] = ctx->r[rB] - ctx->r[rA];
                    if (rc) UpdateCR0(ctx, ctx->r[rD]);
                    break;
                case 8: { // subfc
                    uint64_t res64 = (uint64_t)ctx->r[rB] + (uint64_t)(uint32_t)(~ctx->r[rA]) + 1ull;
                    ctx->r[rD] = (uint32_t)res64;
                    SetXERCarry(ctx, (res64 >> 32) != 0);
                    if (rc) UpdateCR0(ctx, ctx->r[rD]);
                    break;
                }
                case 136: { // subfe
                    uint64_t res64 = (uint64_t)ctx->r[rB] + (uint64_t)(uint32_t)(~ctx->r[rA]) + (GetXERCarry(ctx) ? 1ull : 0ull);
                    ctx->r[rD] = (uint32_t)res64;
                    SetXERCarry(ctx, (res64 >> 32) != 0);
                    if (rc) UpdateCR0(ctx, ctx->r[rD]);
                    break;
                }
                case 200: { // subfze
                    uint64_t res64 = (uint64_t)(uint32_t)(~ctx->r[rA]) + (GetXERCarry(ctx) ? 1ull : 0ull);
                    ctx->r[rD] = (uint32_t)res64;
                    SetXERCarry(ctx, (res64 >> 32) != 0);
                    if (rc) UpdateCR0(ctx, ctx->r[rD]);
                    break;
                }
                case 232: { // subfme
                    uint64_t res64 = (uint64_t)(uint32_t)(~ctx->r[rA]) + 0xFFFFFFFFull + (GetXERCarry(ctx) ? 1ull : 0ull);
                    ctx->r[rD] = (uint32_t)res64;
                    SetXERCarry(ctx, (res64 >> 32) != 0);
                    if (rc) UpdateCR0(ctx, ctx->r[rD]);
                    break;
                }
                case 104: // neg
                    ctx->r[rD] = (uint32_t)(-(int32_t)ctx->r[rA]);
                    if (rc) UpdateCR0(ctx, ctx->r[rD]);
                    break;
                case 235: // mullw
                    ctx->r[rD] = (uint32_t)((int32_t)ctx->r[rA] * (int32_t)ctx->r[rB]);
                    if (rc) UpdateCR0(ctx, ctx->r[rD]);
                    break;
                case 75: { // mulhw
                    int64_t p = (int64_t)(int32_t)ctx->r[rA] * (int64_t)(int32_t)ctx->r[rB];
                    ctx->r[rD] = (uint32_t)((uint64_t)p >> 32);
                    if (rc) UpdateCR0(ctx, ctx->r[rD]);
                    break;
                }
                case 11: { // mulhwu
                    uint64_t p = (uint64_t)ctx->r[rA] * (uint64_t)ctx->r[rB];
                    ctx->r[rD] = (uint32_t)(p >> 32);
                    if (rc) UpdateCR0(ctx, ctx->r[rD]);
                    break;
                }
                case 491: { // divw
                    int32_t a = (int32_t)ctx->r[rA], b = (int32_t)ctx->r[rB];
                    ctx->r[rD] = (b == 0 || (a == INT32_MIN && b == -1)) ? 0u : (uint32_t)(a / b);
                    if (rc) UpdateCR0(ctx, ctx->r[rD]);
                    break;
                }
                case 459: // divwu
                    ctx->r[rD] = (ctx->r[rB] == 0) ? 0u : (ctx->r[rA] / ctx->r[rB]);
                    if (rc) UpdateCR0(ctx, ctx->r[rD]);
                    break;
                case 444: // or / mr
                    ctx->r[rA] = ctx->r[rD] | ctx->r[rB];
                    if (rc) UpdateCR0(ctx, ctx->r[rA]);
                    break;
                case 412: // orc
                    ctx->r[rA] = ctx->r[rD] | (~ctx->r[rB]);
                    if (rc) UpdateCR0(ctx, ctx->r[rA]);
                    break;
                case 28: // and
                    ctx->r[rA] = ctx->r[rD] & ctx->r[rB];
                    if (rc) UpdateCR0(ctx, ctx->r[rA]);
                    break;
                case 60: // andc
                    ctx->r[rA] = ctx->r[rD] & (~ctx->r[rB]);
                    if (rc) UpdateCR0(ctx, ctx->r[rA]);
                    break;
                case 316: // xor
                    ctx->r[rA] = ctx->r[rD] ^ ctx->r[rB];
                    if (rc) UpdateCR0(ctx, ctx->r[rA]);
                    break;
                case 284: // eqv
                    ctx->r[rA] = ~(ctx->r[rD] ^ ctx->r[rB]);
                    if (rc) UpdateCR0(ctx, ctx->r[rA]);
                    break;
                case 124: // nor
                    ctx->r[rA] = ~(ctx->r[rD] | ctx->r[rB]);
                    if (rc) UpdateCR0(ctx, ctx->r[rA]);
                    break;
                case 476: // nand
                    ctx->r[rA] = ~(ctx->r[rD] & ctx->r[rB]);
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
                    uint32_t sh = ctx->r[rB] & 0x3F;
                    int32_t val = (int32_t)ctx->r[rD];
                    if (sh == 0) {
                        ctx->r[rA] = (uint32_t)val;
                        SetXERCarry(ctx, false);
                    } else if (sh < 32) {
                        uint32_t lowMask = (1u << sh) - 1u;
                        SetXERCarry(ctx, val < 0 && (((uint32_t)val & lowMask) != 0));
                        ctx->r[rA] = (uint32_t)(val >> sh);
                    } else {
                        SetXERCarry(ctx, val < 0);
                        ctx->r[rA] = (val < 0) ? 0xFFFFFFFFu : 0u;
                    }
                    if (rc) UpdateCR0(ctx, ctx->r[rA]);
                    break;
                }
                case 824: { // srawi
                    uint32_t sh = (instr >> 11) & 0x1F;
                    int32_t val = (int32_t)ctx->r[rD];
                    if (sh == 0) {
                        ctx->r[rA] = (uint32_t)val;
                        SetXERCarry(ctx, false);
                    } else {
                        uint32_t lowMask = (1u << sh) - 1u;
                        SetXERCarry(ctx, val < 0 && (((uint32_t)val & lowMask) != 0));
                        ctx->r[rA] = (uint32_t)(val >> sh);
                    }
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
                case 20: // lwarx
                case 23: { // lwzx
                    uint32_t ea = (rA == 0 ? 0 : ctx->r[rA]) + ctx->r[rB];
                    ctx->r[rD] = Read32(ea);
                    break;
                }
                case 55: { // lwzux
                    uint32_t ea = ctx->r[rA] + ctx->r[rB];
                    ctx->r[rD] = Read32(ea);
                    ctx->r[rA] = ea;
                    break;
                }
                case 150: { // stwcx.
                    uint32_t ea = (rA == 0 ? 0 : ctx->r[rA]) + ctx->r[rB];
                    Write32(ea, ctx->r[rD]);
                    ctx->SetCRField(0, (ctx->xer & 0x80000000u) ? 0x3u : 0x2u);
                    break;
                }
                case 151: { // stwx
                    uint32_t ea = (rA == 0 ? 0 : ctx->r[rA]) + ctx->r[rB];
                    Write32(ea, ctx->r[rD]);
                    break;
                }
                case 183: { // stwux
                    uint32_t ea = ctx->r[rA] + ctx->r[rB];
                    Write32(ea, ctx->r[rD]);
                    ctx->r[rA] = ea;
                    break;
                }
                case 87: { // lbzx
                    uint32_t ea = (rA == 0 ? 0 : ctx->r[rA]) + ctx->r[rB];
                    ctx->r[rD] = Read8(ea);
                    break;
                }
                case 119: { // lbzux
                    uint32_t ea = ctx->r[rA] + ctx->r[rB];
                    ctx->r[rD] = Read8(ea);
                    ctx->r[rA] = ea;
                    break;
                }
                case 215: { // stbx
                    uint32_t ea = (rA == 0 ? 0 : ctx->r[rA]) + ctx->r[rB];
                    Write8(ea, (uint8_t)ctx->r[rD]);
                    break;
                }
                case 247: { // stbux
                    uint32_t ea = ctx->r[rA] + ctx->r[rB];
                    Write8(ea, (uint8_t)ctx->r[rD]);
                    ctx->r[rA] = ea;
                    break;
                }
                case 279: { // lhzx
                    uint32_t ea = (rA == 0 ? 0 : ctx->r[rA]) + ctx->r[rB];
                    ctx->r[rD] = Read16(ea);
                    break;
                }
                case 311: { // lhzux
                    uint32_t ea = ctx->r[rA] + ctx->r[rB];
                    ctx->r[rD] = Read16(ea);
                    ctx->r[rA] = ea;
                    break;
                }
                case 343: { // lhax
                    uint32_t ea = (rA == 0 ? 0 : ctx->r[rA]) + ctx->r[rB];
                    ctx->r[rD] = (uint32_t)(int32_t)(int16_t)Read16(ea);
                    break;
                }
                case 375: { // lhaux
                    uint32_t ea = ctx->r[rA] + ctx->r[rB];
                    ctx->r[rD] = (uint32_t)(int32_t)(int16_t)Read16(ea);
                    ctx->r[rA] = ea;
                    break;
                }
                case 407: { // sthx
                    uint32_t ea = (rA == 0 ? 0 : ctx->r[rA]) + ctx->r[rB];
                    Write16(ea, (uint16_t)ctx->r[rD]);
                    break;
                }
                case 439: { // sthux
                    uint32_t ea = ctx->r[rA] + ctx->r[rB];
                    Write16(ea, (uint16_t)ctx->r[rD]);
                    ctx->r[rA] = ea;
                    break;
                }
                case 535: { // lfsx
                    uint32_t ea = (rA == 0 ? 0 : ctx->r[rA]) + ctx->r[rB];
                    uint32_t u = Read32(ea);
                    float f; std::memcpy(&f, &u, 4);
                    SyncSingleFP(ctx->f[rD], (double)f);
                    break;
                }
                case 567: { // lfsux
                    uint32_t ea = ctx->r[rA] + ctx->r[rB];
                    uint32_t u = Read32(ea);
                    float f; std::memcpy(&f, &u, 4);
                    SyncSingleFP(ctx->f[rD], (double)f);
                    ctx->r[rA] = ea;
                    break;
                }
                case 599: { // lfdx
                    uint32_t ea = (rA == 0 ? 0 : ctx->r[rA]) + ctx->r[rB];
                    uint64_t hi = Read32(ea), lo = Read32(ea + 4);
                    ctx->f[rD].u64 = (hi << 32) | lo;
                    break;
                }
                case 631: { // lfdux
                    uint32_t ea = ctx->r[rA] + ctx->r[rB];
                    uint64_t hi = Read32(ea), lo = Read32(ea + 4);
                    ctx->f[rD].u64 = (hi << 32) | lo;
                    ctx->r[rA] = ea;
                    break;
                }
                case 663: { // stfsx
                    uint32_t ea = (rA == 0 ? 0 : ctx->r[rA]) + ctx->r[rB];
                    float f = (float)ctx->f[rD].d;
                    uint32_t u; std::memcpy(&u, &f, 4);
                    Write32(ea, u);
                    break;
                }
                case 695: { // stfsux
                    uint32_t ea = ctx->r[rA] + ctx->r[rB];
                    float f = (float)ctx->f[rD].d;
                    uint32_t u; std::memcpy(&u, &f, 4);
                    Write32(ea, u);
                    ctx->r[rA] = ea;
                    break;
                }
                case 727: { // stfdx
                    uint32_t ea = (rA == 0 ? 0 : ctx->r[rA]) + ctx->r[rB];
                    Write32(ea, (uint32_t)(ctx->f[rD].u64 >> 32));
                    Write32(ea + 4, (uint32_t)ctx->f[rD].u64);
                    break;
                }
                case 759: { // stfdux
                    uint32_t ea = ctx->r[rA] + ctx->r[rB];
                    Write32(ea, (uint32_t)(ctx->f[rD].u64 >> 32));
                    Write32(ea + 4, (uint32_t)ctx->f[rD].u64);
                    ctx->r[rA] = ea;
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
                case 512: // mcrxr
                    ctx->SetCRField((instr >> 23) & 7, (ctx->xer >> 28) & 0xF);
                    ctx->xer &= 0x0FFFFFFFu;
                    break;
                case 26: { // cntlzw
                    uint32_t val = ctx->r[rD];
                    uint32_t lz = 0;
                    while (lz < 32 && !(val & (0x80000000 >> lz))) lz++;
                    ctx->r[rA] = lz;
                    if (rc) UpdateCR0(ctx, ctx->r[rA]);
                    break;
                }
                case 598: case 854: case 86: case 470: case 54: case 278: case 1014: case 982: case 210: case 595:
                case 758: case 306:
                    // cache / sync / mmu nops
                    break;
                default:
                    break;
            }
            break;
        }
        case 4: { // Paired-Single SIMD + Indexed PSQ
            uint32_t fC = (instr >> 6) & 0x1F;
            uint32_t xo5 = (instr >> 1) & 0x1F;
            auto SetPS = [&](float p0, float p1) {
                ctx->f[rD].ps[0] = p0;
                ctx->f[rD].ps[1] = p1;
                ctx->f[rD].d = (double)p0;
                if (rc) UpdateCR1(ctx);
            };
            switch (xo5) {
                case 10: SetPS(ctx->f[rA].ps[0]*ctx->f[fC].ps[0] + ctx->f[rA].ps[1]*ctx->f[fC].ps[1], ctx->f[rB].ps[1]); break; // ps_sum0
                case 11: SetPS(ctx->f[rA].ps[0], ctx->f[rA].ps[0]*ctx->f[fC].ps[0] + ctx->f[rB].ps[1]); break; // ps_sum1
                case 12: SetPS(ctx->f[rA].ps[0]*ctx->f[fC].ps[0], ctx->f[rA].ps[1]*ctx->f[fC].ps[0]); break; // ps_muls0
                case 13: SetPS(ctx->f[rA].ps[0]*ctx->f[fC].ps[1], ctx->f[rA].ps[1]*ctx->f[fC].ps[1]); break; // ps_muls1
                case 14: SetPS(ctx->f[rA].ps[0]*ctx->f[fC].ps[0] + ctx->f[rB].ps[0], ctx->f[rA].ps[1]*ctx->f[fC].ps[0] + ctx->f[rB].ps[1]); break; // ps_madds0
                case 15: SetPS(ctx->f[rA].ps[0]*ctx->f[fC].ps[1] + ctx->f[rB].ps[0], ctx->f[rA].ps[1]*ctx->f[fC].ps[1] + ctx->f[rB].ps[1]); break; // ps_madds1
                case 18: SetPS(ctx->f[rA].ps[0]/ctx->f[rB].ps[0], ctx->f[rA].ps[1]/ctx->f[rB].ps[1]); break; // ps_div
                case 20: SetPS(ctx->f[rA].ps[0]-ctx->f[rB].ps[0], ctx->f[rA].ps[1]-ctx->f[rB].ps[1]); break; // ps_sub
                case 21: SetPS(ctx->f[rA].ps[0]+ctx->f[rB].ps[0], ctx->f[rA].ps[1]+ctx->f[rB].ps[1]); break; // ps_add
                case 23: SetPS(ctx->f[rA].ps[0]>=0.0f ? ctx->f[fC].ps[0] : ctx->f[rB].ps[0], ctx->f[rA].ps[1]>=0.0f ? ctx->f[fC].ps[1] : ctx->f[rB].ps[1]); break; // ps_sel
                case 24: SetPS(1.0f/ctx->f[rB].ps[0], 1.0f/ctx->f[rB].ps[1]); break; // ps_res
                case 25: SetPS(ctx->f[rA].ps[0]*ctx->f[fC].ps[0], ctx->f[rA].ps[1]*ctx->f[fC].ps[1]); break; // ps_mul
                case 26: SetPS(1.0f/std::sqrt(ctx->f[rB].ps[0]), 1.0f/std::sqrt(ctx->f[rB].ps[1])); break; // ps_rsqrte
                case 28: SetPS(ctx->f[rA].ps[0]*ctx->f[fC].ps[0] - ctx->f[rB].ps[0], ctx->f[rA].ps[1]*ctx->f[fC].ps[1] - ctx->f[rB].ps[1]); break; // ps_msub
                case 29: SetPS(ctx->f[rA].ps[0]*ctx->f[fC].ps[0] + ctx->f[rB].ps[0], ctx->f[rA].ps[1]*ctx->f[fC].ps[1] + ctx->f[rB].ps[1]); break; // ps_madd
                case 30: SetPS(-(ctx->f[rA].ps[0]*ctx->f[fC].ps[0] - ctx->f[rB].ps[0]), -(ctx->f[rA].ps[1]*ctx->f[fC].ps[1] - ctx->f[rB].ps[1])); break; // ps_nmsub
                case 31: SetPS(-(ctx->f[rA].ps[0]*ctx->f[fC].ps[0] + ctx->f[rB].ps[0]), -(ctx->f[rA].ps[1]*ctx->f[fC].ps[1] + ctx->f[rB].ps[1])); break; // ps_nmadd
                case 6: { // psq_lx
                    bool w = (instr >> 10) & 1; uint32_t iGqr = (instr >> 7) & 7;
                    DequantizeLoad(ctx, rD, (rA == 0 ? 0 : ctx->r[rA]) + ctx->r[rB], w, iGqr);
                    break;
                }
                case 7: { // psq_stx
                    bool w = (instr >> 10) & 1; uint32_t iGqr = (instr >> 7) & 7;
                    QuantizeStore(ctx, rD, (rA == 0 ? 0 : ctx->r[rA]) + ctx->r[rB], w, iGqr);
                    break;
                }
                default: {
                    switch (xo10) {
                        case 0: case 32: { // ps_cmpu0 / ps_cmpo0
                            uint32_t crfD = (instr >> 23) & 7; float a = ctx->f[rA].ps[0], b = ctx->f[rB].ps[0];
                            ctx->SetCRField(crfD, (a < b) ? 8u : (a > b) ? 4u : (a == b) ? 2u : 1u);
                            break;
                        }
                        case 64: case 96: { // ps_cmpu1 / ps_cmpo1
                            uint32_t crfD = (instr >> 23) & 7; float a = ctx->f[rA].ps[1], b = ctx->f[rB].ps[1];
                            ctx->SetCRField(crfD, (a < b) ? 8u : (a > b) ? 4u : (a == b) ? 2u : 1u);
                            break;
                        }
                        case 40:  SetPS(-ctx->f[rB].ps[0], -ctx->f[rB].ps[1]); break; // ps_neg
                        case 72:  SetPS(ctx->f[rB].ps[0], ctx->f[rB].ps[1]); break;   // ps_mr
                        case 136: SetPS(-std::fabs(ctx->f[rB].ps[0]), -std::fabs(ctx->f[rB].ps[1])); break; // ps_nabs
                        case 264: SetPS(std::fabs(ctx->f[rB].ps[0]), std::fabs(ctx->f[rB].ps[1])); break;   // ps_abs
                        case 528: SetPS(ctx->f[rA].ps[0], ctx->f[rB].ps[0]); break; // ps_merge00
                        case 560: SetPS(ctx->f[rA].ps[0], ctx->f[rB].ps[1]); break; // ps_merge01
                        case 592: SetPS(ctx->f[rA].ps[1], ctx->f[rB].ps[0]); break; // ps_merge10
                        case 624: SetPS(ctx->f[rA].ps[1], ctx->f[rB].ps[1]); break; // ps_merge11
                        default: break;
                    }
                    break;
                }
            }
            break;
        }
        case 59: { // single-precision fp ops
            uint32_t fC = (instr >> 6) & 0x1F;
            uint32_t xo5 = (instr >> 1) & 0x1F;
            float res = (float)ctx->f[rD].d;
            switch (xo5) {
                case 18: res = (float)ctx->f[rA].d / (float)ctx->f[rB].d; break;
                case 20: res = (float)ctx->f[rA].d - (float)ctx->f[rB].d; break;
                case 21: res = (float)ctx->f[rA].d + (float)ctx->f[rB].d; break;
                case 22: res = std::sqrt((float)ctx->f[rB].d); break;
                case 24: res = 1.0f / (float)ctx->f[rB].d; break;
                case 25: res = (float)ctx->f[rA].d * (float)ctx->f[fC].d; break;
                case 28: res = (float)ctx->f[rA].d * (float)ctx->f[fC].d - (float)ctx->f[rB].d; break;
                case 29: res = (float)ctx->f[rA].d * (float)ctx->f[fC].d + (float)ctx->f[rB].d; break;
                case 30: res = -((float)ctx->f[rA].d * (float)ctx->f[fC].d - (float)ctx->f[rB].d); break;
                case 31: res = -((float)ctx->f[rA].d * (float)ctx->f[fC].d + (float)ctx->f[rB].d); break;
                default: break;
            }
            SyncSingleFP(ctx->f[rD], (double)res);
            if (rc) UpdateCR1(ctx);
            break;
        }
        case 63: { // double fp ops
            uint32_t fC = (instr >> 6) & 0x1F;
            uint32_t xo5 = (instr >> 1) & 0x1F;
            switch (xo5) {
                case 18: ctx->f[rD].d = ctx->f[rA].d / ctx->f[rB].d; if (rc) UpdateCR1(ctx); break;
                case 20: ctx->f[rD].d = ctx->f[rA].d - ctx->f[rB].d; if (rc) UpdateCR1(ctx); break;
                case 21: ctx->f[rD].d = ctx->f[rA].d + ctx->f[rB].d; if (rc) UpdateCR1(ctx); break;
                case 22: ctx->f[rD].d = std::sqrt(ctx->f[rB].d); if (rc) UpdateCR1(ctx); break;
                case 23: ctx->f[rD].d = (ctx->f[rA].d >= 0.0) ? ctx->f[fC].d : ctx->f[rB].d; if (rc) UpdateCR1(ctx); break;
                case 25: ctx->f[rD].d = ctx->f[rA].d * ctx->f[fC].d; if (rc) UpdateCR1(ctx); break;
                case 26: ctx->f[rD].d = 1.0 / std::sqrt(ctx->f[rB].d); if (rc) UpdateCR1(ctx); break;
                case 28: ctx->f[rD].d = ctx->f[rA].d * ctx->f[fC].d - ctx->f[rB].d; if (rc) UpdateCR1(ctx); break;
                case 29: ctx->f[rD].d = ctx->f[rA].d * ctx->f[fC].d + ctx->f[rB].d; if (rc) UpdateCR1(ctx); break;
                case 30: ctx->f[rD].d = -(ctx->f[rA].d * ctx->f[fC].d - ctx->f[rB].d); if (rc) UpdateCR1(ctx); break;
                case 31: ctx->f[rD].d = -(ctx->f[rA].d * ctx->f[fC].d + ctx->f[rB].d); if (rc) UpdateCR1(ctx); break;
                default: {
                    switch (xo10) {
                        case 0: case 32: { // fcmpu / fcmpo
                            uint32_t crfD = (instr >> 23) & 7;
                            double a = ctx->f[rA].d, b = ctx->f[rB].d;
                            ctx->SetCRField(crfD, (a < b) ? 8u : (a > b) ? 4u : (a == b) ? 2u : 1u);
                            break;
                        }
                        case 12: SyncSingleFP(ctx->f[rD], (double)(float)ctx->f[rB].d); if (rc) UpdateCR1(ctx); break; // frsp
                        case 14: case 15: { // fctiw / fctiwz
                            double v = (xo10 == 15) ? std::trunc(ctx->f[rB].d) : std::nearbyint(ctx->f[rB].d);
                            int32_t iv = (v >= 2147483647.0) ? INT32_MAX : (v <= -2147483648.0) ? INT32_MIN : (int32_t)v;
                            ctx->f[rD].u64 = 0xFFF8000000000000ull | (uint32_t)iv;
                            break;
                        }
                        case 40:  ctx->f[rD].d = -ctx->f[rB].d; if (rc) UpdateCR1(ctx); break; // fneg
                        case 72:  ctx->f[rD].d = ctx->f[rB].d; ctx->f[rD].ps[0] = ctx->f[rB].ps[0]; ctx->f[rD].ps[1] = ctx->f[rB].ps[1]; if (rc) UpdateCR1(ctx); break; // fmr
                        case 136: ctx->f[rD].d = -std::fabs(ctx->f[rB].d); if (rc) UpdateCR1(ctx); break; // fnabs
                        case 264: ctx->f[rD].d = std::fabs(ctx->f[rB].d); if (rc) UpdateCR1(ctx); break; // fabs
                        case 583: ctx->f[rD].u64 = ctx->fpscr; break; // mffs
                        case 711: ctx->fpscr = (uint32_t)ctx->f[rB].u64; break; // mtfsf
                        default: break;
                    }
                    break;
                }
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
